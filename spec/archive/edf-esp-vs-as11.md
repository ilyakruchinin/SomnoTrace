# ESP EDF Export vs AS11 Reference — Achieving Closest Data

- **Status:** Implemented / investigated
- **Author(s):** Ilya Kruchinin, Cascade (AI assistant)
- **Created:** 2026-07-06
- **Supersedes:** `edf-as11-comparison-20260629.md` (kept as historical archive)
- **Related specs:** `as11-summary-spool-and-mask-events.md`, `session-storage-and-export.md`, `local-data-files.md`

## 1. Goal

Generate EDF files on the ESP that are as close to byte-identical as possible
to what the AS11 writes natively to its own SD card. The ESP captures data
over BLE independently; the AS11 writes from its internal data path. This
document summarises how the alignment is achieved and what residual
differences remain (and why they are not fixable).

## 2. Data flow

```
AS11 internal sensor → AS11 internal processing
  ├── SD card (native EDF)        ← high-resolution internal data
  └── BLE notification (5 Hz)     ← coarser data, phys×100 int16
        ↓
ESP32 (BLE central)
  ├── .snt files (SD, raw int16)  ← captured from BLE
  └── EDF export (post-session)   ← converted from .snt → EDF
```

The ESP never has access to the AS11's internal high-resolution data. It can
only work with what the AS11 exposes via BLE notifications. The EDF export
process (`edf_gen.c`) converts `.snt` files into ResMed-compatible EDF files,
applying time alignment, scale conversion, and channel mapping to match the
AS11's EDF format as closely as possible.

## 3. Session lifecycle & time alignment

| Stage | Start | End | Notes |
|-------|-------|-----|-------|
| .snt capture | TherapyStart | TherapyStop | `session_writer.c` — raw BLE data |
| BRP/PLD/SA2 EDF | MaskOn | TherapyStop | Skip pre-MaskOn samples; header stamped with MaskOn NTP time |
| EVE/CSL EDF | TherapyStart | — | Header stamped with TherapyStart NTP time |
| STR EDF | Per-day | Per-day | One record per day, from Summary spool |

### MaskOn alignment

The AS11 starts BRP/PLD/SA2 recording at **MaskOn** (when the user puts on
the mask), not at TherapyStart. The .snt capture starts at TherapyStart, so
the EDF export must skip pre-MaskOn samples:

1. `edf_gen_generate()` reads `events.snt` to find the MaskOn event timestamp
2. Converts to NTP: `maskon_ntp_ms = maskon_as11_ms + clock_drift_ms`
3. Computes per-stream skip samples:
   - BRP (25 Hz): `skip = round(offset_ms / 40)`
   - SA2 (1 Hz): `skip = round(offset_ms / 1000)`
   - PLD (0.5 Hz): `skip = round(offset_ms / 2000)`
4. `convert_snt_to_edf()` seeks past skipped samples before the record loop

**Fallback:** If MaskOn is not found in `events.snt` (BLE notification
missed), EDF starts at TherapyStart with skip=0.

### Clock drift

The AS11's internal clock drifts relative to NTP. The ESP measures this via
`GetDateTime` RPC at connect time (`clock_drift_ms = ntp_ms - as11_ms`).
All EDF header timestamps use NTP-corrected time. Summary spool timestamps
are already NTP-synced and do not need drift correction.

### Record count

EDF record count uses **floor division**: `total_records = total_samples /
spr[0]`. The partial last 60-second record is dropped, matching AS11 (which
never zero-pads trailing partial records).

## 4. Scale conversion

BLE notifications deliver PLD/BRP/SA2 values as **int16 = physical × 100**
(confirmed by reverse-engineering, 2026-07-05). The EDF export converts:

```
phys = stored / 100.0
dig = dig_min + (phys - phys_min) × (dig_max - dig_min) / (phys_max - phys_min)
```

Each EDF signal has its own digital scale (`dig_max/phys_max`), so the raw
×100 value cannot be written directly. The conversion is per-signal in
`convert_snt_to_edf()`.

### Invalid sentinel

The AS11 marks "no data" samples with `-1` (int16). For signals with
`invalid_passthrough = true` (SA2 Pulse/SpO2), the sentinel is written to
the EDF verbatim instead of being scaled. This matches AS11 behaviour.

## 5. Per-file summary

### BRP.edf — Breath waveform (25 Hz)

- **Channels:** Flow, MaskPressure (2ch, 25 Hz)
- **Alignment:** MaskOn-based start, floor-division record count
- **Match:** Correlation >0.999, max error 2 digital units out of ±2500
- **Gotcha — SDMMC DMA zeroing:** `record_buf` must be allocated in PSRAM
  (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`). The ESP32-S3 SDMMC driver
  silently writes zeros for multi-sector DMA writes from internal RAM
  buffers. FATFS bypasses its window buffer for full-sector-aligned chunks.
  Symptom: Flow channel all zeros, Press channel only last partial sector
  survives. PSRAM buffers are bounced through an aligned internal buffer
  and write correctly.

### SA2.edf — Oximetry (1 Hz)

- **Channels:** HeartRate, SpO2 (2ch, 1 Hz)
- **Alignment:** MaskOn-based start
- **Match:** Byte-identical data when oximeter is not connected (all `-1`
  sentinels pass through correctly)
- **Decimation:** SA2 is decimated at capture time (BLE sends 1 Hz, .snt
  stores 1 Hz, EDF writes 1 Hz — no decimation needed in export)

### PLD.edf — Per-breath stats (0.5 Hz)

- **Channels:** 9 of 12 .snt channels (drops TgtVent, IERatio, Ti)
- **Channel map:** .snt [0,1,2,3,4,5,6,9,10] → EDF [MaskPress, Press,
  EprPress, Leak, RespRate, TidVol, MinVent, Snore, FlowLim]
- **Alignment:** MaskOn-based start, offset=0 confirmed by reverse-engineering
- **Scale:** `stored / 100.0` — confirmed correct for all 9 channels
- **Gotcha — BLE quantisation:** See §6 below.

### EVE.edf — Respiratory events

- **Source:** `resp_events.bin` (protobuf spool, not .snt)
- **Header time:** TherapyStart (not MaskOn)
- **Records:** `1 + event_count` (1 for "Recording starts" + one annotation
  per event)
- **Events included:** Respiratory events only (types 2–6: Hypopnea, Central
  Apnea, Obstructive Apnea, Apnea, Arousal). Code 0 is the protobuf3 UNKNOWN
  default; code 1 is a sentinel that never appears in real spool data.
- **Match:** Byte-identical data (onsets, durations, and labels confirmed across
  3 sessions against AS11 reference exports, 2026-07-08)

### CSL.edf — CSR events

- **Source:** Same `resp_events.bin` as EVE.edf
- **Header time:** TherapyStart
- **Events included:** CSR events only (types 7–8: CSR start/end). Note: CSR
  codes are extrapolated from the +1 pattern; no CSR events have been observed
  in test sessions to confirm.
- **Gotcha — event filtering:** EVE.edf and CSL.edf both read from the same
  `resp_events.bin`. A `csl_mode` flag in `generate_eve_edf()` filters
  events: CSL.edf includes only CSR events, EVE.edf includes only
  respiratory events. Without this filtering, CSL.edf would contain all
  respiratory events (Issue 1, fixed 2026-07-05).

### STR.edf — Daily summary

- **Source:** Per-day Summary spool files (`.sessions/summaries/YYYYMMDD.spool`)
- **Records:** One per day, 77 signals + Crc16
- **MaskOff:** Calculated as `MaskOn + duration_min` from spool
  `SessionModeEntries` (sub-field 2 is per-session duration in minutes,
  NOT therapy mode). For 0-duration sessions, `MaskOff = MaskOn` (not -1).
- **Mode:** Derived from `ActiveTherapyProfile` in `settings.json`, not
  from spool `SessionModeEntries`.
- **Gotcha — Flow.95 1-LSB rounding:** One STR signal (`Flow.95` index 35)
  is off by 1 digital unit (ESP=649, AS11=650). This is a rounding
  difference in the spool percentile computation. Not safely fixable
  without the raw spool byte and AS11's exact rounding mode.
- **Gotcha — current-day MaskOff:** Before the Summary spool is pulled,
  MaskOff is derived from live `events.snt`. If no MaskOff event was
  received (very short sessions), TherapyStop is used as fallback. The
  current-day STR record is overwritten when the spool is pulled after
  therapy stop.

### JSON files

- **Identification.json:** Byte-identical (nested AS11 format with CRC words)
- **CurrentSettings.json:** Semantically identical. cJSON drops `.0` suffix
  for integer-valued doubles (e.g. `7` vs `7.0`). Cosmetic — all JSON
  parsers handle both forms. Not fixable without post-processing the JSON
  string, which adds fragility for no semantic benefit.

## 6. PLD BLE quantisation (confirmed 2026-07-05)

Reverse-engineered by comparing raw 5 Hz `.snt` data with AS11 `PLD.edf`
for sessions `20260705_233502` and `20260706_030134`.

**No decimation or phase alignment issue exists.** The .snt data aligns at
offset=0 and the `÷100` scale conversion is correct. Residual differences
are a fundamental BLE data-path limitation: the AS11 sends coarser data via
BLE than it stores on SD internally.

| Channel | BLE resolution | AS11 EDF resolution | Mismatch | Typical diff |
|---------|---------------|---------------------|----------|-------------|
| RespRate | 1.0 bpm | 0.2 bpm | ~20 % | ±0.2–1.0 bpm |
| MinVent | 0.01 L/min | 0.125 L/min | ~24 % | ±0.02–0.12 |
| MaskPress | 0.01 cmH2O | 0.02 cmH2O | ~25–31 % | ±0.02–0.16 (ramp only) |
| Leak | 0.01 L/s | 0.02 L/s | ~3–6 % | ±0.02 |
| TidVol | 0.01 L | 0.02 L | ~3–6 % | ±0.02 |
| Press | 0.01 cmH2O | 0.02 cmH2O | <0.1 % | — |
| EprPress | 0.01 cmH2O | 0.02 cmH2O | <0.1 % | — |
| Snore | 0.01 | 0.02 | <1 % | — |
| FlowLim | 0.01 | 0.01 | <1 % | — |

**Details:**

- **RespRate/MinVent:** BLE sends integer bpm and 0.01 L/min respectively,
  but AS11 internal EDF stores finer resolution (0.2 bpm, 0.125 L/min).
  Deliberate AS11 design choice: coarser over BLE, finer on SD. Not fixable.
- **MaskPress:** Differences concentrate during pressure ramp-up (first
  ~20 s). BLE notification and SD write sample at slightly different
  moments within the 2 s window during rapid changes. Converges to ≤0.04
  cmH2O once pressure stabilises.
- **Other channels:** Near-perfect match (≥94 %, most ≥99 %).

## 7. Known limitations — not fixable

| Issue | Cause | Impact |
|-------|-------|--------|
| PLD BLE quantisation | AS11 sends coarser data via BLE than SD | ~20–25 % of PLD samples differ by sub-unit amounts |
| STR Flow.95 off-by-1 | Spool percentile rounding mode unknown | 1 LSB in 1 of 77 STR signals |
| CurrentSettings.json `.0` | cJSON drops `.0` for integer doubles | Cosmetic only |
| CRC1 header difference | Header timestamp differs by seconds from AS11 | Expected — ESP time is NTP-corrected, AS11 clock drifts |
| Current-day MaskOff (pre-spool) | Fallback to TherapyStop if MaskOff event missed | Low — overwritten when spool is pulled |

## 8. Verification history

| Date | Sessions | Result |
|------|----------|--------|
| 2026-06-29 | `20260629_232738` | Initial comparison: SA2/EVE/CSL byte-identical, BRP/PLD >0.99 correlation, STR 1 value off |
| 2026-07-05 | `20260705_233502`, `20260706_030134` | MaskOn alignment confirmed, PLD BLE quantisation reverse-engineered, CSL event filtering fixed |

## 9. Changelog

- 2026-07-06: Created. Consolidates findings from
  `edf-as11-comparison-20260629.md`, `as11-summary-spool-and-mask-events.md`,
  and the 2026-07-05 PLD reverse-engineering investigation.
