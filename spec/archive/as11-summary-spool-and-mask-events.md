# AS11 Summary Spool SessionModeEntries & Mask Event Analysis

- **Status:** Implemented
- **Author(s):** Cascade (AI assistant), Ilya Kruchinin
- **Created:** 2026-07-05
- **Last updated:** 2026-07-05
- **Related specs:** 0002-edf-export.md

## 1. Summary

Findings from comparing firmware-generated EDF files against AS11 native
exports. Covers: the true meaning of Summary spool `SessionModeEntries`
field 2 (per-session duration, not therapy mode), MaskOff calculation,
pressure stabilisation signalling via RPC events, `_SNC` variable
tracking for Summary spool update detection, EDF record count calculation,
and STR MaskOff alignment for 0-duration sessions.

## 2. SessionModeEntries — "mode" field is per-session duration

### Discovery

The Summary spool protobuf field 6 (`SessionModeEntries`) contains
repeated wrapper submessages, each with:

- **Sub-field 1** (varint): MaskOn timestamp (epoch ms, rounded to minute)
- **Sub-field 2** (varint): **Per-session duration in minutes** (NOT therapy mode)

The firmware originally interpreted sub-field 2 as a therapy mode value.
This produced garbage Mode values in STR.edf because the values (1, 2, 149,
444, etc.) are actually session durations in minutes.

### Verification

Compared all 18 session entries from the Jun 29 spool record and the
single entry from Jul 4 against the AS11's own STR.edf MaskOn/MaskOff
values. **100% match** across all entries:

```
MaskOff = MaskOn + duration   (when duration > 0)
MaskOff = MaskOn              (when duration == 0)
```

The AS11 writes MaskOff = MaskOn for 0-duration sessions (not -1).
Previously the firmware used -1, which caused MaskOn/MaskOff array
misalignment in STR.edf for multi-session days with 0-duration entries.

### Spool timestamp reference

Summary spool timestamps (PeriodStart, PeriodEnd, ClockB, session entry ts)
are in **NTP-synced UTC epoch milliseconds**, not the AS11's internal
drifting clock. Verified by matching ClockB exactly to events.snt
TherapyStop timestamps. The `clock_drift_ms` (from GetDateTime RPC) applies
only to the AS11's internal clock display, not to spool data.

## 3. MaskOff in STR.edf

### Problem

The Summary spool does not contain explicit MaskOff timestamps. Only
MaskOn timestamps and per-session durations are available.

### Solution

MaskOff is **reliably calculated** as `MaskOn + duration` from the spool's
SessionModeEntries. This matches the AS11's own STR.edf exactly.

The firmware's previous approach (`DurationMin / SessionCount` with a
`mode != 0` check) was wrong for multi-session days because it assumed
equal session durations. The fix uses the per-session duration directly
from each session entry.

### Current-day fallback

For the current (incomplete) day where no Summary spool exists yet,
MaskOff is derived from `events.snt` (live EventNotification messages).
The firmware receives `MaskOff` events via the `UsageEvents-TherapyStatusEvents`
subscription and writes them to `events.snt`. If no MaskOff event is
received (short sessions), `TherapyStop` is used as a fallback.

## 4. Pressure Stabilisation Signal

### Available events

The `SystemActivityEvents-FrequentActivityEvents` event selector
(provided by `SubscribeEvent`) includes:

- **`PressureStart`** — pressure begins ramping
- **`PressureStop`** — pressure stops (~15s after TherapyStop)
- `TherapyStarted`, `CooldownStarted`, `StandbyStarted`
- `WarmupStarted`, `RampDownStarted`, `RampDownCompleted`

### AS11 BRP/PLD/SA2 recording start

The AS11 starts BRP/PLD/SA2 EDF recording at **MaskOn**, not at
TherapyStart. The 5-9s gap previously attributed to "internal AS11
processing delay" is actually the time between TherapyStart (when the
device starts the therapy session) and MaskOn (when the user puts on the
mask). Archive analysis with NTP-corrected event timestamps confirms:

| Session | TherapyStart (NTP) | MaskOn (NTP) | BRP start (NTP) | Gap T→M |
|---------|-------------------|--------------|-----------------|---------|
| Jul 5 S1| 23:35:02 | 23:35:09.6 | 23:35:09 | 7.6s |
| Jul 6 S2| 03:01:32 | 03:01:41.6 | 03:01:41 | 9.6s |

BRP/PLD/SA2 EDF start time = MaskOn time (truncated to whole seconds by
EDF datetime format). The pressure ramp from start pressure (~4-7 cmH2O)
to target (~10-11 cmH2O) is visible in the first 2-16 seconds of data,
confirming that recording starts at MaskOn, not at PressureStart (which
fires ~2s after MaskOn, mid-ramp) or after pressure stabilises.

PLD signals that require computation (RespRate, TidVol, MinVent) show
zeros for the first 7-8 samples (~14-16s) after MaskOn — this is normal
(zeros in the data, not a different EDF start). Snore and FlowLim are
sparse and may show zeros for much longer.

EVE/CSL EDF files start at TherapyStart (not MaskOn).

BRP ends at TherapyStop ≈ MaskOff time. BRP duration (nrecs × 60s)
matches STR Duration (±1 min rounding). The partial last 60s record is
**dropped** (floor division), not zero-padded.

The firmware was previously starting recording at BLE reconnect time
(~7.5 min before TherapyStart) due to stream setup overhead. The fix
is to record from TherapyStart (session_writer_start) to TherapyStop
(session_writer_stop), which is already the existing session lifecycle.

### Fix

**Stage 1 (session_writer.c — unchanged):** BRP/PLD/SA2 .snt capture
starts at TherapyStart and stops at TherapyStop. No changes.

**Stage 2 (edf_gen.c — EDF export alignment):** The EDF export process
now aligns to AS11 behaviour:

1. **MaskOn-based start:** `edf_gen_generate()` reads `events.snt` to
   find the MaskOn event timestamp. It converts to NTP
   (`maskon_ntp_ms = maskon_as11_ms + clock_drift_ms`) and computes
   per-stream skip samples:
   - BRP (25 Hz): `skip = round(offset_ms / 40)`
   - SA2 (1 Hz): `skip = round(offset_ms / 1000)`
   - PLD (0.5 Hz): `skip = round(offset_ms / 2000)`
   The `convert_snt_to_edf()` function seeks past these samples before
   the record loop, so the EDF data starts at MaskOn.

2. **MaskOn-based EDF header time:** BRP/PLD/SA2 EDF headers use
   `maskon_ntp_ms` (truncated to seconds) as the start date/time.
   EVE/CSL continue using `start_epoch_ms` (TherapyStart).

3. **Floor division for record count:** `total_records = total_samples /
   spr[0]` (was ceiling division). The partial last 60s record is
   dropped, matching AS11 (which never zero-pads trailing records).

4. **Fallback:** If MaskOn is not found in `events.snt` (e.g. BLE
   notification missed), the EDF starts at TherapyStart with skip=0.
   This produces the previous (slightly larger) EDF — safe fallback.

The `SystemActivityEvents-FrequentActivityEvents` subscription is
retained for event logging (PressureStart, PressureStop, etc.) but is
not used to gate data recording or EDF export.

## 5. _SNC Variable for Summary Update Detection

### Background

`_SNC` (RPC variable, var_reference.tsv line 711) is a counter that
increments when the AS11 updates its Summary spool data (nor:1:/Summary.bin).
After TherapyStop, the AS11 takes a few seconds to finalise and write the
updated Summary.

### Previous approach

The firmware polled the Summary spool every 3 seconds for up to 2 minutes,
checking ClockB (field 40) against the session end time. This works but
is inefficient — each poll is a full PullSpoolFragments RPC round.

### Implemented approach

`_SNC` can be used as a `dataId` in `SubscribeEvent`. The AS11 pushes an
`EventNotification` with `dataId":"_SNC"` and `event":"ValueChange"` when
the counter increments — no polling required.

Example notification (received ~230ms after TherapyStop):
```json
{"jsonrpc":"2.0","method":"EventNotification","params":{
  "subscriptionId":3,"dataId":"_SNC",
  "events":[{"reportTime":"2026-07-02T10:46:10.230Z",
             "event":"ValueChange","value":247}]
}}
```

Implementation:
1. `SubscribeEvent` includes `"_SNC"` as a fourth dataId (alongside the
   three event-profile selectors)
2. `session_writer_on_notification` detects `dataId":"_SNC"` + `ValueChange`
   and sets a module-level flag (`s_snc_changed`) with the new value
3. `post_therapy_wait_spool_current` checks `session_writer_snc_changed()`
   every 3 seconds (just a flag check — no BLE RPC) and pulls the spool
   when the flag is set
4. Fallback: if no `_SNC` notification arrives within 2 minutes (e.g.
   subscription not accepted, BLE dropped notification), pulls the spool
   blindly on the last attempt and proceeds with available data

## 6. STR.edf Mode field

### Fix (already applied)

Mode is derived from `ActiveTherapyProfile` in settings.json (via
`profile_name_to_mop()` → `MODE_MAP[]`), not from SessionModeEntries.
This matches the AS11's own export behavior, which uses the active
therapy profile (MOP setting) for the Mode field.

## 7. Acceptance criteria

- [x] Mode derived from settings.json ActiveTherapyProfile
- [x] MaskOff calculated as MaskOn + per-session duration from spool
- [x] SessionModeEntries sub-field 2 documented as duration_min
- [x] SystemActivityEvents-FrequentActivityEvents subscription added (for event logging)
- [x] BRP/PLD/SA2 .snt capture starts at TherapyStart (Stage 1 — unchanged)
- [x] BRP/PLD/SA2 EDF export starts at MaskOn (Stage 2 — skip pre-MaskOn samples)
- [x] BRP/PLD/SA2 EDF header timestamp uses MaskOn NTP time
- [x] EVE/CSL EDF header timestamp uses TherapyStart NTP time
- [x] EDF record count uses floor division (partial last record dropped, matching AS11)
- [x] _SNC push notification subscription for Summary update detection
- [x] _SNC subscription verified end-to-end with live session logs
- [x] Fallback timeout retained (2 min)
- [x] MaskOff = MaskOn for 0-duration sessions (matches AS11, fixes STR alignment)

## 8. Changelog

- 2026-07-05: Initial document. Captures findings from AS11 data stream
  investigation and code fixes.
- 2026-07-05: Added section 9 (unresolved items) and clarified _SNC polling.
- 2026-07-05: Switched _SNC from Get RPC polling to SubscribeEvent push
  notifications (confirmed by other developer's capture of _SNC
  ValueChange EventNotification).
- 2026-07-05: Archive analysis of AS11 exports confirms BRP starts at
  TherapyStart (5-9s after CSL), NOT at PressureStart. Reverted
  PressureStart gating on BRP/PLD. Removed pressure_started flag.
  Updated unresolved items.
- 2026-07-05: _SNC subscription verified working with live session logs.
  Fixed EDF sample dropping (ceiling division for record count).
  Fixed STR MaskOff for 0-duration sessions (MaskOff=MaskOn, not -1).
- 2026-07-06: Per-signal analysis of AS11 EDFs reveals BRP/PLD/SA2 start
  at MaskOn (not TherapyStart with internal delay). Pressure ramp is
  visible in first 2-16s of data. Implemented Stage 2 EDF export
  alignment: skip pre-MaskOn samples, use MaskOn NTP for header time,
  switch to floor division (drop partial last record). EVE/CSL remain
  at TherapyStart. Fallback to TherapyStart if MaskOn event missing.

## 9. Unresolved items — to revisit

These items remain unreliable or unverified after the current fixes.
They should be revisited when more data or AS11 protocol knowledge is
available.

### 9.1 BRP/PLD/SA2 EDF start alignment

Implemented: EDF export (Stage 2) now skips pre-MaskOn samples from
.snt files and stamps BRP/PLD/SA2 headers with MaskOn NTP time. The
.snt capture (Stage 1) still records from TherapyStart to TherapyStop
— no changes to session_writer.c.

**Status:** Implemented. Needs verification with a live session.

### 9.2 Current-day MaskOff (live, before spool pull)

For the current (incomplete) day where no Summary spool has been pulled
yet, MaskOff is derived from `events.snt` live event notifications.
The firmware maps `MaskOff` → MaskOff and `TherapyStop` → MaskOff
(fallback). If the AS11 doesn't send a `MaskOff` event (very short
sessions), `TherapyStop` is used, which may differ by a few seconds
from the AS11's own MaskOff value.

**Impact:** Low — the current-day STR record is overwritten when the
Summary spool is pulled after therapy stop (which uses the verified
`MaskOn + duration_min` calculation). The live STR record is only
visible briefly between therapy stop and EDF regeneration.

### 9.3 STR.edf Date (noon boundary timezone)

The noon-day boundary uses `localtime_r` (firmware's configured
timezone). If the firmware's timezone differs from the AS11's
configured timezone, the day boundary could shift, causing a session
to appear on the wrong day in STR.edf.

**Impact:** Config-dependent. If both devices use the same timezone,
this is correct.

**Possible fix:** Read the AS11's timezone setting via `Get` RPC and
use it for noon-day calculations. Low priority since timezone is
typically set correctly on both devices.

### 9.4 _SNC subscription — verified working

Confirmed end-to-end with live session logs (Jul 5, 2026):
1. `reconnect: SubscribeEvent response received` — subscription accepted ✓
2. `>>> _SNC ValueChange: 1103` — initial value at connect ✓
3. `>>> _SNC ValueChange: 1104` — during therapy (Summary updated mid-session) ✓
4. `>>> _SNC ValueChange: 1105` — after TherapyStop (Summary finalized) ✓
5. `spool_is_current: ... → FRESH` — spool pull triggered correctly ✓

The AS11 accepts `"_SNC"` as a dataId in `SubscribeEvent` alongside the
three event-profile selectors. Push notifications arrive reliably.

**Status:** Resolved. No further action needed.

### 9.5 BRP recording end time

Archive analysis confirms BRP ends at TherapyStop ≈ MaskOff time.
BRP duration (nrecs × 60s) matches STR Duration (±1 min rounding).
The firmware stops recording at TherapyStop (session_writer_stop),
which matches AS11 behavior.

**Status:** Resolved by archive analysis. No further action needed.

### 9.6 Short sessions without STR entries

Archive analysis found sessions that have BRP/PLD/SA2 files but are
NOT in STR.edf session entries (e.g., Jul 4 03:38 and 06:26 sessions).
These are likely very short or interrupted sessions that the AS11
doesn't count as real therapy sessions for Summary spool purposes.

**Impact:** None for EDF generation — the firmware records all sessions
and generates EDFs for them. The STR.edf only includes sessions that
the AS11 considers valid (with MaskOn/duration in Summary spool).

**Status:** Observed but not an issue. Expected behavior.
