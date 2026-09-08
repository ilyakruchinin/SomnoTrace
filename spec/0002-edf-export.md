# 0002 — EDF export

- **Status:** Draft
- **Author(s):** Ilya Kruchinin (@ilyakruchinin)
- **Created:** 2026-06-17
- **Last updated:** 2026-06-29
- **Related specs:** `0001-airsense11-ble-sync.md`, `0003-o2ring-ble-sync.md`

## 1. Summary

Mapping pulled therapy and oximetry data to EDF/EDF+ files in the format
the ResMed AirSense 11 writes to its SD card.  Output goes to
`/somnotrace/SDCARD/` (ResMed-compatible tree) for OSCAR compatibility.

## 2. Motivation / goals

- Produce byte-identical EDF files to what the AS11 firmware writes.
- Support multi-day `STR.edf` (summary) from Summary spool data.
- Include the current (in-progress) noon-day in `STR.edf`.
- Export `BRP.edf`, `PLD.edf`, `SA2.edf`, `EVE.edf` per session.

## 3. Non-goals

- Re-encoding or transcoding therapy waveforms (raw streams are passed
  through with header generation only).

## 4. Behaviour

### 4.1 Inputs

- **Summary spool** (`/somnotrace/.sessions/summaries/YYYYMMDD.spool`):
  per-day protobuf records pulled via BLE `StartSpool` with a 30-day
  lookback.  Each record contains scalars (AHI, indices, enums) and
  metric submessages (percentiles for pressure, flow, leak, etc.).
- **Settings JSON** (`<session>_settings.json`): from BLE `Get` RPC for
  `SettingProfiles`, captured during post-therapy collection.
- **Stream data** (`<session>_brp.snt`, `_pld.snt`, `_sa2.snt`): raw
  therapy waveforms recorded during the session.
- **Events** (`<session>_events.snt`): JSON-line event notifications
  (MaskOn, TherapyStop, etc.).
- **Respiratory events** (`<session>_resp_events.bin`): protobuf spool
  from `TherapyEvents-RespiratoryEvents`.

### 4.2 States / flow

1. **Post-therapy collection** (`post_therapy_collect`):
   - Pull Summary spool (30-day lookback) → per-day `.spool` files.
   - Pull respiratory events spool → `.bin` file.
   - Get device identification → `_ident.json`.
   - Get current settings → `_settings.json`.
2. **EDF generation** (`edf_gen_generate`):
   - Generate `BRP.edf`, `PLD.edf`, `SA2.edf` from stream data.
   - Generate `EVE.edf` from respiratory events.
   - Generate `STR.edf` from all available Summary spool day records.
   - Generate `Identification.json` from ident data.

### 4.3 Outputs / data formats

#### 4.3.1 Directory structure

```
/somnotrace/SDCARD/
  DATALOG/
    <YYYYMMDD>/          (noon-day folder)
      <ts>_BRP.edf       (Flow.40ms, Press.40ms)
      <ts>_PLD.edf       (9 × 0.2s channels)
      <ts>_SA2.edf       (Pulse.1s, SpO2.1s)
      <ts>_EVE.edf       (annotations + events)
      <ts>_STR.edf       (daily summary, multi-record)
  SETTINGS/
    <ts>_SETTINGS.edf    (settings snapshot)
  Identification.json
```

#### 4.3.2 STR.edf — summary record scaling

The AS11 stores summary metrics in the protobuf spool as **fixed-point
integers**.  Its firmware STR writer divides each value by a
field-specific **logical scale** before writing the EDF digital value.
The physical value is then `digital / edf_output_scale`.

Verified against live BLE data (spool vs `Get` RPC):

| Field group          | logical_scale | Conversion      | Example                    |
|----------------------|---------------|-----------------|----------------------------|
| Pressure (cmH2O)     | 2             | `raw / 2`       | 1178 → 589 → 11.78 cmH2O   |
| Flow (L/s)           | 0.2           | `raw * 5`       | 52 → 260 → 0.52 L/s        |
| Humidity/Temp/Power  | 10            | `raw / 10`      | 1160 → 116 → 11.6 mg/L     |
| SpO2 (%)             | 100           | `raw / 100`     | 97 → 0.97 → 97%            |
| Minute Ventilation   | 12.5          | `raw * 2 / 25`  | 650 → 52 → 6.5 L/min       |
| Respiratory Rate     | 20            | `raw / 20`      | 1280 → 64 → 12.8 bpm       |
| Tidal Volume         | 2             | `raw / 2`       | 50 → 25 → 0.5 L            |
| Indices (AHI etc)    | 10            | `raw / 10`      | 80 → 8 → 0.8 events/hr     |
| Duration/enums/SAU   | 1             | no conversion   | —                          |

Settings fields [6-30] come from the `Get` RPC response (settings.json)
and are already in EDF digital units (e.g. pressures × 50, temperatures
× 10).  No additional scaling is applied to those.

#### 4.3.3 Current day in STR.edf

The AS11 **does include the current in-progress noon-day** in the Summary
spool.  The PeriodStart/PeriodEnd span the full noon-to-noon window even
while the day is still in progress, and DurationMin accumulates as
sessions are added.

`collect_summary_spool()` captures and stores it as `YYYYMMDD.spool`,
where the filename is derived from the raw AS11 PeriodStart (no clock
drift correction).  This is critical: applying drift correction before
computing the noon-day label can shift the day by one when the corrected
time falls before noon (e.g. 12:00 AS11 → 11:52 NTP → previous day).

`build_current_day_record()` remains as a fallback for when the spool
pull fails entirely — it synthesizes Date, MaskOn/MaskOff, MaskEvents,
and Duration from `events.snt` and session timing, with -1 sentinels for
all other fields.

### 4.4 Error handling & edge cases

- Missing summary spool for a day → that day is omitted from STR.edf.
- Spool pull failure → `build_current_day_record()` synthesizes a
  minimal record from session events.
- Missing settings JSON → settings fields [6-30] remain -1 sentinel.
- Spool filenames use raw AS11 PeriodStart for noon-day classification
  (no drift correction) — consistent with `edf_gen.c`'s day matching.

#### 4.3.4 STR enum mapping conventions

Settings fields [14-30] are enum values mapped from the AS11 `settings.json`
string labels to EDF digital integers.  The mapping follows a consistent
convention verified against three independent sources: AS11 native EDF
exports, OSCAR source code (`resmed_loader.cpp`), and airbreak-plus
(`resmed_config.py` ENUM_OPTIONS, `edf_signals.md`).

**Universal rule: EDF = raw enum + 1**

The AS11 EDF writes `raw_enum + 1` for most enum fields.  OSCAR recovers
the 0-indexed enum by decrementing by 1 for AS11 devices (`if (AS_eleven)
field--;`).  The raw enums match airbreak-plus `ENUM_OPTIONS` (0-indexed).

| Idx | EDF label | Short | Raw enum (airbreak-plus)            | JSON string → EDF value        |
|-----|-----------|-------|--------------------------------------|--------------------------------|
| 14  | S.AS.Comfort    | AFC | `{0:'Standard', 1:'Soft'}`     | Off→1, On→2                    |
| 15  | S.RampEnable    | RMA | `{0:'Off', 1:'On', 2:'Auto'}`  | Off→1, On→2, Auto→3            |
| 17  | S.EPR.ClinEnable| EPA | `{0:'Off', 1:'On'}`            | via `on_off_to_edf()`          |
| 18  | S.EPR.EPREnable | EPX | `{0:'Off', 1:'On'}`            | via `on_off_to_edf()`          |
| 20  | S.EPR.EPRType   | EPT | `{0:'Ramp Only', 1:'Full Time'}` | RampOnly→1, FullTime→2       |
| 21  | S.SmartStart    | SST | `{0:'Off', 1:'On'}`            | via `on_off_to_edf()`          |
| 22  | S.PtAccess      | —   | *(not in ENUM_OPTIONS)*        | Full/Advanced→1, Basic→2       |
| 23  | S.ABFilter      | ABF | `{0:'No', 1:'Yes'}`            | No→1, Yes→2                    |
| 25  | S.Tube          | TBT | `{0:'SlimLine', 1:'Standard', 2:'3m'}` | SlimLine→1, Standard→2, 15mmNonHeated→3, 19mmNonHeated→4 |
| 26  | S.ClimateControl| CCO | `{0:'Auto', 1:'Manual'}`       | Auto→1, Manual→2               |
| 27  | S.HumEnable     | HMX | `{0:'Off', 1:'On'}`            | via `on_off_to_edf()`          |
| 29  | S.TempEnable    | HTX | `{0:'Off', 1:'On', 2:'Auto'}`  | via `on_off_to_edf()`          |

**Exception 1: S.Mask [24] uses raw + 2**

OSCAR's AS11 mask handling subtracts 2 (not 1) with the comment
`// why be consistent?`.  Confirmed by AS11 native EDF data:
Pillows→2, FullFace→3, Nasal→4, Pediatric→5.

| Raw (airbreak-plus `MSK`) | EDF value | OSCAR (−2) | Label     |
|---------------------------|-----------|------------|-----------|
| 0                         | 2         | 0          | Pillows   |
| 1                         | 3         | 1          | Full Face |
| 2                         | 4         | 2          | Nasal     |
| 3                         | 5         | 3          | Pediatric (OSCAR maps to Unknown) |

**Exception 2: Mode [5] uses a custom remap table**

`MODE_MAP[] = {3,1,2,4,10,16,8,6,7,5,9}` — maps the MOP (ActiveTherapyProfile)
enum index to the EDF Mode value.  See `edf_signals.md` "STR enum export maps".
AutoSetForHer (MOP=11) is passed through as-is (not in the 11-entry table).

**Exception 3: S.EPR.EPRType [20] — OSCAR applies a net-zero transform for AS11**

OSCAR does `epr += 1; if (AS_eleven) epr--;`, so for AS11 the EDF value is
used as-is.  The `+1` is for AS10 devices where EDF values are 0-indexed.
Our raw+1 values happen to match OSCAR's option index directly.

**Exception 4: S.Tube [25] — OSCAR does NOT decrement for AS11**

OSCAR reads `S.Tube` without any `AS_eleven` adjustment.  Our raw+1 values
match the AS11 native EDF.  OSCAR's `RMS9_TubeType` channel is declared but
never assigned display options, so tube type is not shown in OSCAR's UI.

**Exception 5: HeatedTube [31] / Humidifier [32] — spool-derived remap**

These come from the Summary spool (not settings.json) and use custom remap
tables documented in `edf_signals.md`: `ZHT: [3,4,1,5,2]`, `HUC: [1,2,3]`.
In practice the spool values are copied directly and match AS11 native output.

**Scalar fields (no enum mapping):**

| Idx | EDF label       | Conversion        |
|-----|-----------------|-------------------|
| 6-13| Pressure fields | cmH2O × 50        |
| 16  | S.RampTime      | direct minutes    |
| 19  | S.EPR.Level     | cmH2O × 50        |
| 28  | S.HumLevel      | direct            |
| 30  | S.Temp          | °C × 10           |

**AS10 differences (for future EDF→session conversion support):**

The AS10 (AirSense 10) EDF uses **0-indexed** enum values (no +1 offset).
OSCAR detects the device generation and applies the decrement only for AS11.
If we ever need to parse AS10 EDF files:

- Most enum fields: use the value directly (0=Off, 1=On, etc.)
- S.Mask: use the value directly (0=Pillows, 1=Full Face, 2=Nasal)
- S.EPR.EPRType: OSCAR adds 1 to the EDF value (undoing the 0-index)
- Mode: AS10 uses the same MODE_MAP remap table
- S.Tube: AS10 EDF values are 0-indexed (0=SlimLine, 1=Standard, 2=3m)
- JSON labels differ: AS10 uses `Essentials` (Plus/On) instead of
  `PatientView` (Full/Basic), and tube types may use different strings

## 5. Acceptance criteria

- [x] STR.edf stats fields match AS11 firmware output (verified via
      live BLE spool vs Get RPC comparison, 2026-06-29).
- [x] Current in-progress noon-day included in Summary spool (verified
      via FTP fetch + Python protobuf parse, 2026-06-29).
- [x] Spool filename uses raw AS11 PeriodStart (no drift correction)
      — fixed in `post_therapy.c`.
- [x] STR.edf settings enum fields match AS11 firmware output (verified
      via AS11 native EDF comparison, OSCAR source, and airbreak-plus,
      2026-07-31).  See §4.3.4.
- [ ] Multi-day STR.edf opens correctly in OSCAR.
- [ ] BRP/PLD/SA2/EVE EDF files match AS11 SD card output.

## 6. Security / privacy considerations

- Handles personal medical data.  No real patient data in tests or
  fixtures.  All data stays on local SD card unless explicitly uploaded.

## 7. Open questions

- Should we also query `Summary-SpontTriggerPercentage` and
  `Summary-SpontCyclePercentage` via Get RPC?  These return
  `InvalidObject` errors and may require a different RPC method.
- TargetMinuteVentilation, IeRatio, and InspiratoryDuration metrics
  also return `InvalidObject` via Get RPC but are present in the spool.
  These fields are not yet mapped in the C STR signal definitions.
