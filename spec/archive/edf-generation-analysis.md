# EDF Generation Analysis — Fidelity, Correctness, and Complexity

- **Status:** Audit report
- **Author:** Cascade (AI assistant)
- **Created:** 2026-07-13
- **Scope:** `main/edf_gen.c` and its session/summary inputs
- **Related reports:** `edf-esp-vs-as11.md`, `edf-as11-comparison-20260629.md`, `as11-summary-spool-and-mask-events.md`

## 1. Executive summary

The EDF generator has a strong fidelity-oriented foundation. Its EDF header
layout, CRC model, record layouts, BRP/SA2/PLD metadata, PLD channel mapping,
digital scaling, invalid-value handling, and PSRAM SDMMC buffer handling are
deliberate and supported by prior AS11 comparisons. BLE sampling and
quantisation differences are correctly treated as source-data limitations.

The main current concern is the time-domain invariant: every SDCARD artifact
that participates in session grouping or display must use NTP-corrected local
time. This includes EDF headers, filenames, EVE/CSL annotation onsets, STR
Date/MaskOn/MaskOff values, and noon-day placement. The 2026-07-12 export
showed that NTP-stamped EDFs combined with AS11-clock STR intervals cause
importers to merge sessions.

No firmware source code was changed for this report.

## 2. Confirmed strengths

| Area | Assessment | Basis |
|---|---|---|
| EDF header and CRC format | Strong | Prior comparison found all STR signal definitions/header fields matched. The implementation writes EDF's interleaved signal-header layout and ResMed CRC words. |
| BRP conversion | Strong | De-interleaving, scaling, MaskOn alignment, 60-second records, and floor-dropping partial records are explicit. Prior aligned waveform correlation exceeded 0.999. |
| SA2 invalid handling | Strong | Pulse/SpO2 `-1` passthrough had byte-level validation. |
| PLD mapping and scale | Strong | The nine exported channels and their mapping from the 12-channel capture are documented and validated; remaining variation is BLE precision/timing. |
| STR statistics and mode | Strong | The summary-spool model, SessionMode duration treatment, active-profile mode selection, and record CRC are deliberate and previously compared. |
| NTP event alignment | Sound design; needs regression capture | EVE/CSL subtract NTP session start from corrected event time: `AS11 reportTime + drift - NTP start`. |
| SDMMC waveform writes | Important protection | The PSRAM record buffer handles the demonstrated multi-sector internal-RAM DMA corruption mode. |

## 3. Intentional, unavoidable differences

- The ESP only receives BLE data, not the AS11's internal high-resolution
  signal path. PLD RespRate, MinVent, and ramping MaskPress cannot be made
  byte-identical by exporter changes.
- Native AS11 files preserve AS11 clock time. SomnoTrace intentionally emits
  NTP-corrected time, so timestamp fields and header CRC1 can differ.
- Before a fresh Summary spool arrives, current-day STR uses live event data.
  Its MaskOff value can differ from the final AS11 Summary by seconds or by
  minute rounding.

## 4. Required timing model

| Artifact / value | Source clock | Required emitted clock |
|---|---|---|
| BRP/PLD/SA2 headers, IDs, names, DATALOG day | ESP session metadata | NTP |
| EVE/CSL headers | ESP session metadata | NTP |
| EVE/CSL annotation onset | AS11 `reportTime` | `AS11 + clock_drift_ms` |
| STR PeriodStart, Date, MaskOn, MaskOff | Summary boundary data | NTP |
| Raw `.snt` events | AS11 `reportTime` | Preserve raw; convert at export |

For the 2026-07-12 capture, AS11 was about 458 seconds ahead of NTP. BRP
files were named in NTP (`23:36:58`, `23:57:35`), while STR exposed AS11
intervals (`23:44–00:03`, `00:05–01:48`). An importer therefore assigned the
second BRP to the first STR interval. Corrected NTP STR intervals resolve the
same files as distinct sessions.

### 4.1 Documentation conflict

Two older archive reports state that Summary spool timestamps are already
NTP-synchronised. That conflicts with the observed grouping behavior and the
current requirement to correct STR boundaries. This claim requires a permanent
binary regression fixture containing raw Summary fields, AS11 event times,
ESP session metadata, and the native AS11 STR record. Until then, the latest
capture is the stronger operational evidence.

## 5. Findings and recommendations

### 5.1 High: write failures are not propagated — **FIXED**

All EDF and JSON write paths now use `write_all()` which checks every `fwrite`
return value. `edf_write_header()` returns `-1` on short writes. Every call
site checks the return and propagates `ESP_FAIL` with an artifact-specific
log message.

**Fixed in:** `edf_gen.c` — `write_all()`, `edf_write_header()`,
`convert_snt_to_edf()`, `generate_str_edf()`, `generate_eve_edf()`,
`generate_identification()`, `edf_gen_generate()` (CurrentSettings).

### 5.2 High: exports are replaced in place — **FIXED**

All export artifacts now use atomic write: `open_atomic_file()` creates a
`<path>.tmp` file, `finalize_atomic_file()` performs `fflush` + `fsync` +
`fclose` + `rename`. On any error, `discard_atomic_file()` closes and unlinks
the temp file, leaving the previous valid file intact.

**Fixed in:** `edf_gen.c` — applied to BRP/SA2/PLD/STR/EVE/CSL EDFs,
Identification.json/.crc, CurrentSettings.json/.crc.

### 5.3 Medium: sample alignment truncates, while archived docs say round

MaskOn skip and MaskOff maximum counts use integer division, not rounding:
BRP uses `ms * 25 / 1000`, SA2 uses `ms / 1000`, and PLD uses `ms / 2000`.

**Impact:** data can start up to one sample early: 40 ms BRP, 1 s SA2, or 2 s
PLD. End trimming has the corresponding ambiguity.

**Recommendation:** Determine the native AS11 phase rule from raw references
(floor, nearest, ceil, or fixed phase), then document and test that rule at
both boundaries.

### 5.4 Medium: EVE/CSL short-session gate uses raw BRP duration

The annotation gate checks raw BRP samples, while waveform EDF duration is
calculated after MaskOn skipping and MaskOff trimming.

**Impact:** edge cases can produce header-only BRP/PLD/SA2 with EVE/CSL, or
suppress annotations under a different effective duration criterion than the
waveform data.

**Recommendation:** Verify native short-session behavior and base the decision
on the same effective MaskOn-to-MaskOff duration as waveform export.

### 5.5 Medium: summary-drift reconstruction is valid but overengineered — **OPTIMISED**

`resolve_summary_drift()` previously scanned every stream-day directory and
parsed every session metadata JSON for *every* Summary day — O(days ×
sessions) directory scans and JSON parses.

**Optimised:** Replaced with a two-phase approach: `build_session_drift_index()`
scans all session JSONs once and builds an in-memory array of
`session_drift_entry_t` (start_as11_ms, drift_ms, as11_day). `lookup_drift()`
then does a linear scan of the in-memory array per Summary day. This reduces
I/O from O(days × sessions) directory opens + JSON parses to O(sessions) once.

**Still open:** The ideal design would persist the drift index at capture time
rather than rebuilding it at export time. The intra-day clock-adjustment risk
remains (one day-level drift can be wrong if the clock changed mid-day).
These are lower-priority architectural improvements.

### 5.6 Medium: STR selects an arbitrary 30 spool files — **FIXED**

The STR generator now uses a two-pass approach: first collect all `*.spool`
filenames from `readdir`, sort them lexicographically (YYYYMMDD =
chronological), keep only the newest 30, then parse those. Output is
deterministic regardless of filesystem ordering.

**Fixed in:** `edf_gen.c` — `generate_str_edf()`.

### 5.7 Medium: EVE/CSL source behavior needs a fresh native comparison — **PARTIALLY FIXED**

Current code builds EVE/CSL from `events.snt`; archived reports still describe
`resp_events.bin`. **Sorting, deduplication, and `dataId` filtering are now
implemented**: events are sorted by `onset_sec`, duplicates (same onset +
label) are removed, and both EVE and CSL now filter to
`TherapyEvents-RespiratoryEvents` only. This ensures deterministic annotation
order and content regardless of `events.snt` line ordering from BLE
retransmits or `_SNC` spool replay.

**Still open:**
- No native EVE/CSL comparison has been done to validate counts, labels,
  onsets, durations, and bytes against AS11 output.

**Recommendation:** capture respiratory and CSR events, compare counts,
ordering, labels, onsets, durations, and bytes with native EVE/CSL, and add
an out-of-order/duplicate-notification fixture.

### 5.8 Low: JSON byte-fidelity patching is fragile

CurrentSettings is rendered by cJSON then string-edited to restore `.0` on a
fixed key list. The known output is useful, but a new key, nesting change, or
formatter change can bypass it.

**Recommendation:** either use a deterministic schema renderer when exact
JSON bytes are a requirement, or treat JSON formatting as semantic-only and
remove post-processing from the critical path.

### 5.9 Low: noon boundary remains timezone-dependent

NTP epoch values are correct only if the ESP timezone used by `localtime_r`
matches the therapy/import timezone. A mismatch can put a near-noon session
in the wrong DATALOG/STR day.

**Recommendation:** add an 11:55–12:05 local-time regression case and keep
the ESP timezone configured for the therapy location.

### 5.10 Low: filename collisions and fallbacks need fixtures

Export filenames are second-granular even though internal session IDs can have
a suffix. Same-second corrected MaskOn events could collide. Missing MaskOn
falls back to TherapyStart, which is safe but less AS11-like.

**Recommendation:** add fixtures for same-second starts, missing MaskOn,
missing MaskOff, no oximeter, zero-record sessions, and session boundaries
across midnight/noon.

## 6. Suggested verification matrix

| Priority | Fixture / test | Expected result |
|---|---|---|
| P0 | 2026-07-12 multi-session data | Seven distinct importer sessions; graphs and events show NTP time. |
| P0 | Raw Summary plus native STR | Establish authoritative Summary timestamp domain. |
| P1 | Write-error injection / full SD | Generation reports failure and preserves prior valid files. |
| P1 | Power-loss simulation during STR replacement | Previous STR remains importable. |
| P1 | MaskOn/MaskOff fractional sample boundaries | Confirm native sample phase and record count. |
| P1 | CSR and duplicate/out-of-order events | EVE/CSL match native event ordering/content. |
| P2 | More than 30 summary days | Deterministic newest-window STR output. |
| P2 | Timezone noon crossover | Correct DATALOG folder and STR date. |

## 7. Overall assessment

The generator is close to its stated goal where it has direct AS11 reference
coverage. The waveform conversion and ResMed-oriented EDF layout are not
overengineered; their complexity is justified by measured format and SDMMC
constraints. The time-domain work is also necessary, but the current
historical Summary drift lookup is more complex than the ideal design and
should eventually be replaced with persisted per-day/per-session time-domain
metadata.

The highest-value next work is reliability and regression coverage, not more
signal conversion logic: atomic exports, checked write failures, an
authoritative Summary timestamp fixture, and native validation of short
sessions and event ordering.

## 8. Remaining work plan

| # | Finding | Priority | Status | Next step |
|---|---|---|---|---|
| 5.1 | Write failures not propagated | High | **Fixed** | — |
| 5.2 | Exports replaced in place | High | **Fixed** | — |
| 5.3 | Sample alignment truncates vs round | Medium | Deferred | Need reference captures to determine AS11's exact phase rule (floor/nearest/ceil) at both MaskOn and MaskOff boundaries. Do not change until native behavior is established. |
| 5.4 | EVE/CSL short-session gate uses raw BRP duration | Medium | Open | Verify native AS11 short-session behavior. Base the gate on effective MaskOn-to-MaskOff duration, not raw BRP sample count. |
| 5.5 | Summary-drift reconstruction overengineered | Medium | **Optimised** | I/O reduced from O(days × sessions) to O(sessions) via in-memory drift index. Architectural: persist index at capture time — defer. |
| 5.6 | STR selects arbitrary 30 spool files | Medium | **Fixed** | — |
| 5.7 | EVE/CSL source behavior | Medium | **Partially fixed** | Sort/dedup/dataId filter done. Remaining: native EVE/CSL byte-level comparison. |
| 5.8 | JSON byte-fidelity patching fragile | Low | Open | Either use a deterministic schema renderer or accept semantic-only JSON. Low risk — current key list is stable. |
| 5.9 | Noon boundary timezone-dependent | Low | Open | Add 11:55–12:05 local-time regression fixture. Ensure ESP timezone matches therapy location. |
| 5.10 | Filename collisions and fallbacks | Low | Open | Add fixtures for same-second starts, missing MaskOn/MaskOff, no oximeter, zero-record sessions, midnight/noon boundaries. |

### Recommended order of next work

1. **5.4** — Verify and fix short-session gate. Needs a short-session capture
   with known AS11 behavior.
2. **5.3** — Determine sample phase rule from reference captures. Blocked on
   native AS11 data at fractional-sample boundaries.
3. **5.7 remaining** — Capture native EVE/CSL for byte-level comparison.
4. **5.5 remaining** — Persist drift index at capture time. Architectural
   improvement; defer until needed.
5. **5.8–5.10** — Low-priority fixtures and hardening. Address as regression
   coverage expands.

## 9. Changelog

- 2026-07-13: Initial audit; no firmware source changes.
- 2026-07-13: Fixed 5.1 (checked writes), 5.2 (atomic exports), 5.6
  (deterministic STR day selection), 5.7 partial (EVE/CSL sort + dedup).
  Updated remaining work plan.
- 2026-07-13: Fixed 5.5 (optimised drift lookup to in-memory index),
  5.7 remaining (tightened CSL `dataId` filter to `TherapyEvents-
  RespiratoryEvents` only). Updated remaining work plan.
