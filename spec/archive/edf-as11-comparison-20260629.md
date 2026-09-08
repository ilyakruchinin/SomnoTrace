# SomnoTrace — EDF vs AS11 Reference Comparison (2026-06-29)

> **⚠ PARTIALLY OUTDATED** — Sections 3.1–3.3 and the recommendation
> (Section 5) describe a ~13 s capture-start offset that has since been
> **fixed**. Recording now starts at TherapyStart (not BLE reconnect),
> reducing the offset to ~5–9 s (AS11's internal processing delay).
> See `as11-summary-spool-and-mask-events.md` §4 for the fix details.
> Sections 2, 3.4, 3.5, 3.6, and 4 remain valid.

Analysis of ESP-generated EDF files against AS11 reference exports for
session `20260629_232738` (ESP) vs `20260629_233520` / `20260629_233506`
(AS11).

Clock drift: **-449 007 ms** (NTP = AS11 + drift; AS11 = NTP + 449.007 s).

---

## 1. Summary

| File | Header match | Data match | Graph correlation | Status |
|------|-------------|------------|-------------------|--------|
| SA2  | start_time -13 s, CRC1 differs | **byte-identical** | N/A | done |
| EVE  | start_time +1 s, CRC1 differs  | **byte-identical** | N/A | done |
| CSL  | start_time +1 s, CRC1 differs  | **byte-identical** | N/A | done |
| BRP  | start_time -13 s, CRC1 differs | 74.5 % bytes differ | **1.0000** | done |
| PLD  | start_time -13 s, CRC1 differs | 33.8 % bytes differ | **>0.99** | done |
| STR  | **identical** | 1 value off by 1 | N/A | done |
| Identification.json | — | **byte-identical** | — | done |
| CurrentSettings.json | structure OK | 6× `.0` formatting | — | done |

---

## 2. Byte-identical files

### SA2.edf (data)

All 242 data bytes match exactly. The `invalid_passthrough` fix works:
`-1` sentinel values for Pulse/SpO2 (no oximeter connected) are now
written verbatim instead of being scaled to `0`.

### EVE.edf and CSL.edf (data)

Both 64-byte data sections are byte-identical. Event annotations and
calibration data match AS11 exactly.

### Identification.json

Byte-for-byte identical (768 bytes).

### STR.edf (header)

All 78 signal definitions and all header fields match exactly. This
includes patient ID (CRC words), recording ID, start date/time, and
every signal label/transducer/unit/phys_min/phys_max/dig_min/dig_max/
prefilter/spr/reserved field.

---

## 3. Remaining differences

### 3.1 Header start_time (13 s therapy-start detection lag) — ⚠ OUTDATED

> **Fixed:** Recording now starts at TherapyStart event, not BLE reconnect.
> The ~13 s offset described below was from the old pre-TherapyStart
> recording behavior. Current offset is ~5–9 s (AS11 internal delay
> after TherapyStart before BRP recording begins). See
> `as11-summary-spool-and-mask-events.md` §4.

| File group | ESP header time | AS11 header time | Delta |
|------------|----------------|-----------------|-------|
| BRP / SA2 / PLD | 23.35.07 | 23.35.20 | -13 s |
| EVE / CSL       | 23.35.07 | 23.35.06 | +1 s  |

**Root cause (original):** ESP was recording from BLE reconnect time
(~7.5 min before TherapyStart) due to stream setup overhead. The fix
is to record from TherapyStart (session_writer_start) to TherapyStop
(session_writer_stop), matching the AS11's session lifecycle.

**CRC1 (H1) differs** as a direct consequence, since CRC1 covers header
bytes 0x19–0xFF which include the start_time field. CRC2 (H2, signal
header blocks) matches in all files.

### 3.2 BRP.edf — waveform capture offset (74.5 % byte difference) — ⚠ OUTDATED

> **Fixed:** The 13.4 s offset was from pre-TherapyStart recording.
> Current offset is ~5–9 s (AS11 internal delay). The correlation
> results below were from the old offset and are no longer representative.

The byte difference was entirely due to the 13.4 s capture-start offset
(335 samples × 0.04 s). When aligned:

| Signal | Correlation | Max error | Mean error | Lag |
|--------|------------|-----------|------------|-----|
| Flow.40ms | **1.0000** | 2 / 2500 (0.08 %) | 1.2 | 335 samples (13.40 s) |
| Press.40ms | **0.9993** | 2 / 2500 (0.08 %) | 1.2 | 335 samples (13.40 s) |

The waveforms overlay almost perfectly. Max error is 2 digital units
out of ±2500 full-scale — within rounding noise of the int16 scaling.

### 3.3 PLD.edf — waveform capture offset (33.8 % byte difference) — ⚠ OUTDATED

> **Fixed:** Same root cause as 3.2 — pre-TherapyStart recording.
> Current offset is ~5–9 s (2–4 PLD samples at 2 s interval).

Same 14 s capture-start offset (7 samples × 2 s). ESP's first 7 samples
captured the pressure ramp-up (0 → 540), while AS11 started recording
after pressure was already stable (359 → 530).

| Signal | Correlation | Mean error | Notes |
|--------|------------|------------|-------|
| MaskPress.2s | 0.9947 | 5.0 | Ramp-up in ESP's first 7 samples |
| RespRate.2s | 0.9956 | 2.1 | |
| TidVol.2s | 0.9983 | 0.4 | |
| MinVent.2s | 0.9945 | 1.7 | |
| Leak.2s | 0.8574 | 2.3 | Noisy small values |
| Press / EprPress | — | 0.0 | Both reach same steady-state (570 / 520) |
| Snore / FlowLim | — | 0.0 | Sparse / invalid values, aligned |

### 3.4 STR.edf — Flow.95 off by 1

| Signal | ESP | AS11 | Delta |
|--------|-----|------|-------|
| Flow.95 [35] | 649 | 650 | -1 |
| Crc16 [77] | differs | differs | (consequence of Flow.95) |

77 of 78 signal values are identical. The single off-by-1 on `Flow.95`
is a 1-LSB rounding difference in the spool percentile computation
(`spool_to_edf(raw, 5, 1)` where raw × 5 = 649 vs AS11's 650). Not
safely fixable without the raw spool byte and knowledge of AS11's
exact rounding mode.

### 3.6 PLD.edf — BLE quantisation (confirmed 2026-07-05)

> **Investigation complete.** Reverse-engineered by comparing raw 5 Hz
> `.snt` data with AS11 `PLD.edf` for sessions `20260705_233502` and
> `20260706_030134`. No decimation or phase alignment issue exists.
> The `.snt` data aligns at offset=0 and the `÷100` scale conversion is
> correct. Residual differences are a fundamental BLE data-path limitation.

The AS11 sends PLD values via BLE at **phys×100** (integer physical units).
Some channels have **coarser BLE resolution** than the AS11's internal
SD-card EDF, causing systematic small differences:

| Channel | BLE resolution | AS11 EDF resolution | Mismatch rate | Typical diff |
|---------|---------------|---------------------|---------------|-------------|
| RespRate | 1.0 bpm | 0.2 bpm (dig 450/phys 90) | ~20 % | ±0.2–1.0 bpm |
| MinVent | 0.01 L/min | 0.125 L/min (dig 240/phys 30) | ~24 % | ±0.02–0.12 |
| MaskPress | 0.01 cmH2O | 0.02 cmH2O (dig 2000/phys 40) | ~25–31 % | ±0.02–0.16 (ramp only) |
| Press | 0.01 cmH2O | 0.02 cmH2O | <0.1 % | — |
| EprPress | 0.01 cmH2O | 0.02 cmH2O | <0.1 % | — |
| Leak | 0.01 L/s | 0.02 L/s | ~3–6 % | ±0.02 |
| TidVol | 0.01 L | 0.02 L | ~3–6 % | ±0.02 |
| Snore | 0.01 | 0.02 | <1 % | — |
| FlowLim | 0.01 | 0.01 | <1 % | — |

**Key findings:**

1. **No phase shift** — best alignment is at offset=0 for all channels.
   The ESP correctly captures every 10th 5 Hz notification and the
   MaskOn skip calculation is accurate.

2. **Scale conversion is correct** — `stored / 100.0` matches AS11
   physical values. Confirmed by raw int16 ratio analysis across all
   9 PLD channels (average ratio = BLE_scale / EDF_scale, consistent
   with phys×100).

3. **RespRate/MinVent** — BLE sends integer bpm and 0.01 L/min
   resolution respectively, but AS11 internal EDF stores finer
   resolution (0.2 bpm, 0.125 L/min).  This is a deliberate AS11
   design choice: coarser data over BLE, finer on SD.  Not fixable.

4. **MaskPress** — differences concentrate during pressure ramp-up
   (first ~20 s).  BLE notification and SD write sample at slightly
   different moments within the 2 s window during rapid changes.
   Converges to ≤0.04 cmH2O once pressure stabilises.

5. **Other channels** — Press, EprPress, Snore, FlowLim match ≥94 %
   (most ≥99 %).  Small Leak/TidVol differences are sub-LSB
   quantisation noise.

**Conclusion:** These differences are a fundamental limitation of the
BLE data path.  The AS11 internally stores higher-resolution data on
its SD card than it exposes via BLE.  No firmware change can recover
the lost precision.  The generated PLD.edf is as accurate as the BLE
data allows.

### 3.5 CurrentSettings.json — `.0` formatting

Structure is correct: `{"FlowGenerator":{"SettingProfiles":{...}}}`.
Content is semantically identical. 6 numeric values differ in
formatting:

| Field | ESP | AS11 |
|-------|-----|------|
| StartPressure (AutoSet) | `7` | `7.0` |
| MaxPressure (AutoSet) | `20` | `20.0` |
| MinPressure (AutoSet) | `5` | `5.0` |
| StartPressure (AutoSetForHer) | `5` | `5.0` |
| StartPressure (CPAP) | `4` | `4.0` |
| HeatedTubeTemperature | `18` | `18.0` |

cJSON drops the `.0` suffix for integer-valued doubles. AS11's
serializer preserves it. This is cosmetic — all consumers parse JSON
numbers correctly regardless of trailing `.0`.

**Possible fix (not implemented):** Post-process the JSON string to
add `.0` to integer-valued floats matching known field names. Adds
fragility for no semantic benefit.

---

## 4. Fixes applied in this session

All fixes were applied to `main/edf_gen.c`:

1. **SA2 invalid marker** — Added `invalid_passthrough` field to
   `edf_signal_def_t`. When a raw `.snt` sample equals `-1` (the
   invalid sentinel), it is written to the EDF as `-1` directly,
   bypassing physical → digital scaling. Enabled for SA2 Pulse/SpO2.
   Also applies to padding of partial records for passthrough signals.

2. **Clock drift correction** — EDF header datetime and recording ID
   now use `start_epoch_ms - clock_drift_ms` so the header timestamp
   is in the AS11 clock domain.

3. **CurrentSettings.json nesting** — The `SettingProfiles` object is
   now wrapped under a `FlowGenerator` key to match AS11's structure,
   using `cJSON_AddItemReferenceToObject` to avoid double-free.

4. **Dead code removal** — Removed `edf_finalise_crc()` (52 lines):
   no callers remain after CRC computation was moved into
   `edf_write_header()` (in-memory buffer CRC before writing to disk).

---

## 5. Recommendation — ⚠ UPDATED

The ~13 s capture-start offset described in §3.1–3.3 has been **fixed**
(recording now starts at TherapyStart). Remaining differences are:

- **Cosmetic** — JSON `.0` formatting, CRC1 from ~5–9 s time offset
  (TherapyStart→MaskOn gap, now corrected in Stage 2 — see below).
- **Not safely fixable** — STR `Flow.95` 1-LSB rounding without raw
  spool data.
- **PLD BLE quantisation (§3.6)** — Confirmed 2026-07-05: no phase or
  scale error.  Residual PLD differences (RespRate ~20 %, MinVent ~24 %,
  MaskPress ramp ~25 %) are caused by the AS11 sending coarser data via
  BLE than it stores on SD internally.  Not fixable by firmware.
- **EDF record count** — Now uses **floor division** (drops partial last
  record, matching AS11). Was ceiling division (zero-padded).
- **EDF start alignment** — Stage 2 (EDF export) now skips pre-MaskOn
  samples from .snt files and stamps BRP/PLD/SA2 headers with MaskOn
  NTP time, matching AS11 behaviour. EVE/CSL remain at TherapyStart.
  See `as11-summary-spool-and-mask-events.md` §4 for details.
- **STR MaskOff** — Now uses MaskOn+duration (MaskOn for 0-duration),
  matching AS11 exactly.

The generated EDF files are as close to identical to AS11 as physically
possible given the independent capture path.
