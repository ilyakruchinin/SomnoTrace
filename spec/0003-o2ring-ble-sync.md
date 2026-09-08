# 0003 — O2 Ring BLE sync

- **Status:** Proposed
- **Author(s):** Ilya Kruchinin (@ilyakruchinin)
- **Created:** 2026-06-17
- **Last updated:** 2026-08-19 (sync-window / mandatory power-off)
- **Related specs:** `0001-airsense11-ble-sync.md`, `0002-edf-export.md`
- **Rationale / measurements:** `.ai/OXY/DESIGN.md`
- **Host POC (do not port verbatim):** `.ai/OXY/POC/`
- **Protocol study only (do not copy):** `.ai/OXY/o2ring-s-protocol/` (MIT; listed in `THIRD-PARTY-NOTICES.md`)

## 1. Summary

SomnoTrace pulls stored recordings from **one user-selected** Wellue
O2 Ring S (T8520) or SleepHQ O2 Ring Pro (SHQO2Pro) over BLE (OxyII)
and writes those files **byte-for-byte** under `.somnotrace/oximetry/`.
The AS11 BLE link stays up. No OSCAR/SleepHQ/SA2 conversion in this
spec — that is a later decision.

## 2. Motivation / goals

- Capture overnight oximetry from the household's chosen ring.
- Households may own two or more rings; this device pulls **only**
  the paired serial.
- Ring records autonomously; we download files, we do not stream a night.
- Work with both retail T8520 (`S8-AW`) and SleepHQ (`SHQO2Pro`).
- Never drop the AS11 connection (live therapy data).
- **Let the ring power off after sync.** Holding BLE through the
  post-removal END window prevents deep sleep and wastes ring battery.
- Keep the vendor file intact (SpO2, HR, **motion/flags**, trailer).

## 3. Non-goals

- Overnight live PPG / waveform decode (`cmd=0x03` body).
- Sitting on the ring all night, or polling it on a timer.
- Keep-awake / factory-reset / SET_CONFIG writes (`0xE3`, `0xEE`).
- A second protocol driver for legacy GATT `14839ac4-…`.
- Exporting to `SA2.edf`, OSCAR, or SleepHQ (loses motion + trailer;
  decide in a later spec).
- Pulling every OxyII ring in range.
- Pairing more than one serial on one SomnoTrace (v1 = one ring).
- Disconnecting AS11 to free the only BLE slot.
- Copying third-party source into this repo.

## 4. Behaviour

### 4.1 Inputs

- BLE adverts: mfg `0xF34E`, and/or name prefix `SHQO2Pro` / `S8-AW`
  (case-insensitive), and/or service UUID
  `e8fb0001-a14b-98f9-831b-4e2941d01248`.
- Optional retail recording-mode advert: name `T8520_`, mfg `0x036F`
  — **do not connect for files** on that advert. SHQO2Pro may never
  emit it (measured: stays OxyII while worn).
- User **selects one ring** via portal scan + pair (no SMP PIN).
  Identity is the ring **serial** from `GET_INFO`, not the BLE MAC.
- Wall clock (NTP local) for `SET_UTC_TIME` so new filenames are
  consistent. Does not rewrite already-stored files.

### 4.2 States / flow

```
unpaired → user scan → user picks one advert → connect + GET_INFO
         → store that serial as the only paired ring → disconnect
paired   → low-duty *passive* scan
         → ring absent from scan → clear "this presence served"
         → ring present + not yet served this presence
         → connect 2nd slot (AS11 stays up)
         → AUTH+SETUP + GET_INFO (serial must match) + one LIVE_B
         → [5]!=0x00 → disconnect, retry in 60 s (still recording)
         → [5]==0x00 (END window) → SET_UTC + GET_CONFIG + F4
         → F1 → pull new files → F4 → disconnect ring
         → mark served → **never reconnect while this advert lasts**
           (any GATT connection resets the ring's power-off timer)
         → advert stops (power-off) → clear served
         → advert STILL present at pull+130 s ⇒ END (≤ take-off+120 s,
           and pull ran inside it) must already be over ⇒ ring was
           put back on → clear served, resume 60 s worn probes
AS11 reconnect / pair / post_therapy_collect → pause ring I/O and scan
```

**Pairing (required before any watch/pull):**

1. Portal starts an oximeter scan (high duty; AS11 stays up).
2. Results list every OxyII advert: name, RSSI, addr (display only).
   Two `SHQO2Pro` units in the same room both appear.
3. User picks **one**. Firmware connects, runs enough handshake to
   `GET_INFO`, reads serial + firmware, disconnects.
4. That **serial** is the only paired identity (NVS + `paired.json`).
   Replacing a pair overwrites the previous serial; old `files/` stay.
5. Forget: erase NVS + `paired.json`. Do **not** delete `files/`.

Watch/connect only if unpaired is false. After connect, `GET_INFO`
serial **must** equal the paired serial; otherwise disconnect and do
not pull. Advert name / last_addr are hints (MAC rotates on factory
reset). If two rings advertise the same prefix, serial is the gate.

**Sync window (the only *pull* trigger):**

SHQO2Pro advertises the same OxyII packet on-finger and off-finger
(measured `adv_deep.py`, 2026-08-19): name `SHQO2Pro`, mfg `0xF34E:00`,
Heart Rate UUID `0000180d-…`, ~58 ms interval. Take-off does **not**
flip any AD field. After take-off the ring counts down 9→0, shows END
for ~2 min, then vibrates and **powers off** (advertising stops).
Measured window X with no connection: take-off ~150 s → last advert
+273.8 s → SILENCE (~120 s of END). Button after power-off does
nothing until the next wear.

SomnoTrace must:

1. Low-duty **passive** scan (listen only; no scan requests).
2. If the paired ring is advertising and this presence is not yet
   served: connect on the 2nd slot (AS11 stays up).
3. Probe only: AUTH + SETUP + `GET_INFO` + one `cmd=0x04` LIVE_B.
   Do **not** send F4 / SET_UTC / GET_CONFIG / F1 on a probe.
   Measured: LIVE_B replies even with no AUTH; `0x01` on-finger
   (SpO2/HR live), `0x00` off-finger.
4. `GET_INFO` serial must match or disconnect immediately.
5. LIVE_B `[5]`:
   `0x00` = no finger (END / BLE sync — **pull**),
   `0x01` = finger present (recording — **never pull**),
   `0x03` = file handle open (**never pull now**).
   Only `0x00` continues. Any other value or no reply →
   **disconnect immediately, do not pull, do not mark served.**
   While still advertising and on-finger, probe again after **60 s**
   (still lands well inside the ~120 s END window; a pull takes
   ~4–30 s, leaving ≥60 s margin).
6. Off-finger (`0x00`) only: SET_UTC + GET_CONFIG + F4, then F1 →
   pull new / unfinalised files. Promote only if trailer magic
   `48 12 5a da` is at `file_size-44`. Incomplete files stay in
   `inbox/`.
7. **Always disconnect** (F4 if a file was open, then GAP disconnect).
8. Mark this presence **served** and **never reconnect while this
   advert continues** (measured: any GATT connection resets the
   ring's ~120 s END power-off timer). There is **no documented safe
   power-off opcode** (`0xEE` stays dead until USB; `0xE3` wipes
   recordings; SDK RESET is uncaptured). Do not invent one.
9. Re-worn detection is passive: END ends at take-off + ≤120 s and
   the pull ran inside that window, so if the advert is **still
   present at pull + 130 s** the ring must be back on a finger.
   Clear served and resume 60 s worn probes.
10. When scan no longer sees it, clear served.

**Not a trigger:**

- A wall-clock timer that connects without a LIVE_B gate.
- Advert-name / mfg / interval flip. SHQO2Pro stays `SHQO2Pro` /
  `0xF34E:00` while worn **and** through END (measured). Retail
  T8520 *may* flip `T8520_*` / `0x036F` → `S8-AW` / `0xF34E`; treat
  that as an extra hint, never the only gate, and **never connect**
  on the recording-mode advert.
- Sitting on LIVE_B / overnight PPG. One probe per 30 s while worn,
  then drop the link. Never hold the connection.

**Mandatory disconnect:** after every pair, every pull (success,
zero new files, or error), and on AS11 preemption. Never leave the
ring connected "in case more data arrives." A held link blocks the
END→deep-sleep transition and drains the ring. Issue #5 (live PPG
overnight) is explicitly out of scope for the same reason.

**F3 budget (issue #1):** the ring can go silent on F3 mid-file
(per-connection throughput cap). Timeout → keep `.part`, F4,
disconnect, then the *next* presence (or a single resume reconnect
inside the same window if the file is still incomplete) continues
from `.part` size. Do not sit on the link waiting.

**Priority:** AS11 StreamData / reconnect / spool always preempts
ring F3. One F3 chunk then yield. If preempted, leave `.part` and
retry on the next watch cycle.

### 4.3 BLE / protocol

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`.

Connection table in the NimBLE GAP path (today `s_conn_handle` is a
singleton in `as11_ble.c`). Ring disconnect must **not** run AS11
`auto_reconnect_task`. Raise ACL/msys only as needed; extra buffers
are AS11 headroom.

Background scan: low duty (not 96/96). Pairing scan: high duty,
AS11 stays connected. SoftAP Wi-Fi scan remains the only reason to
call `as11_ble_disconnect()`.

GATT (OxyII only — ignore legacy service even if advertised):

| Role | UUID |
|---|---|
| Service | `e8fb0001-a14b-98f9-831b-4e2941d01248` |
| Write (write-without-response) | `e8fb0002-a14b-98f9-831b-4e2941d01248` |
| Notify | `e8fb0003-a14b-98f9-831b-4e2941d01248` |

Frame: `A5 | cmd | ~cmd | flag | seq | len_le16 | payload | crc8`.
CRC-8 poly `0x07`, init 0, no reflect, no xorout.
Fixture: `A5 E1 1E 00 02 00 00` → `BF`.

Connect sequence (clean-room; match behaviour, do not paste POC):

1. ATT Exchange MTU 517 (mandatory on fw `2D010002`; do it always).
2. Discover OxyII service + write/notify chars.
3. Enable notifications (CCCD `0x0001` LE).
4. `0xFF` AUTH — 16-byte payload:
   `XOR(derive_session_key("0000", unix_ts), MD5("lepucloud"))`.
   Key layout: bytes 0–7 = even bytes of that MD5; 8–11 = ASCII
   `"0000"` (or device serial if known); 12–15 = little-endian uint32 timestamp `(ts>>(i*8))&0xFF`.
   - On older firmware (`2D010002` / `2D010003`): no reply. Wait ~600ms timeout
     and proceed in plaintext mode via step 5 (`0x10`).
   - On newer firmware (`1.13.1.0` / BranchCode `2D010001`): ring replies with
     a 20-byte payload. Decrypt via `XOR(payload, MD5("lepucloud"))`. Bytes 4..19
     contain the 16-byte AES session key. Ring transitions to AES-128-ECB
     (PKCS7-padded) encryption for subsequent command request/response payloads.
     Step 5 (`0x10`) is skipped.
5. `0x10` payload `00` (legacy plaintext rings only).
6. `0xC0` SET_UTC_TIME: year LE16, mon, day, hour, min, sec, `0x00`.
   Send **local** wall clock. Ring stores fields verbatim; new
   filenames use that clock. Do not rewrite existing files.
7. `0x00` GET_CONFIG — consume 40-byte reply **before** any F3.
8. `0xF4` READ_FILE_END — always, clears leftover handle.
9. `0xF1` GET_FILE_LIST — `u8 count` + `N × 16` (14-byte ASCII name
   `YYYYMMDDhhmmss` + 2 zero pad).
10. Per new file:
    - `0xF2` 20-byte payload: 16-byte name slot + u32 LE type `0` (OXY).
      Reply first 4 bytes = file size.
    - `0xF3` u32 LE offset, append up to 512-byte chunks to
      `inbox/<name>.part`. Resume offset = existing `.part` size.
      Stop on empty payload or `offset >= size`.
    - `0xF4`.
    - Promote to `files/<serial>/` as the **exact F3 bytes**
      (header + body + trailer). Iff magic present, mark
      `finalised=true`; else keep `.part` / retry later. Never
      strip motion/flags or the trailer.

Optional status (not required to pull): `0xE1` GET_INFO (serial,
fw), `0xE4` GET_BATTERY (byte[1] = percent), `0x04` LIVE_SAMPLES_B
header: `[5]=state`, `[6]=spo2`, `[7]=motion`, `[8]=hr`, `[13]=bat`;
`255` = no finger.

Never send `0xE3` or `0xEE`.

Format A file:

- Header 10 B: `01 03 00 00 00 00 00 00 04 00`
- Body: 3 B/sample `[spo2, hr, flags]`, 1 Hz
- Trailer 48 B; sub-magic `48 12 5a da` at offset 4 of trailer
  (`file_size - 44`). Size match without magic ⇒ not finalised.

### 4.4 Storage

Add `SD_OXYMETRY_DIR` (`SD_APP_DIR "/oximetry"`) and mkdir it in
`sd_storage_init()`. Do **not** store pulls under `SDCARD/`.
Do **not** upload these files in v1.

```
/somnotrace/.somnotrace/oximetry/
  paired.json
  index.json
  inbox/<name>.part
  files/<serial>/<name>.bin          # exact bytes from the ring
  files/<serial>/<name>.meta.json    # optional bookkeeping only
```

`<name>.bin` is the vendor Format A blob: 10-byte header, 3-byte
samples `[spo2, hr, flags]` (flags carry **motion**), 48-byte trailer.
Do not transcode, strip, or wrap it.

`paired.json`: `serial` (required), `firmware`, `name_prefix`,
`last_addr` (hint). This is the configured ring.

`index.json`: array of `{name, serial, sha256, bytes, finalised}`.
Dedup key = `serial + name`. Re-pull if `finalised` is false.

`meta.json` is optional (pulled_at_ms, sample count, trailer stats).
The `.bin` is the source of truth.

NVS namespace `oximeter` (via `nvs_writer_run()`): `serial` (the
selection), name_prefix, last_addr. Forget: erase NVS + `paired.json`;
**keep** `files/` (including other serials from earlier pairs).

### 4.5 Export / upload

**Out of scope.** Do not write `SA2.edf`. Do not call
`uploader_on_export_complete` for oximetry. SleepHQ and OSCAR ingest
are a later spec; SA2 would drop motion and the trailer.

### 4.6 Code placement

- `main/oximeter.h` — scan / pair / forget / status / periodic tick
- `main/oximeter_oxyii.c` — clean-room codec + session
- `main/oximeter_store.c` — SD + index + inbox (no EDF)
- `as11_ble.c` — connection table + scan dispatcher only
- License header from `docs/source-header.txt` on every new file
- Never commit. Never copy `.ai/OXY/o2ring-s-protocol/*.py`

Portal (minimum): scan lists **all** OxyII rings in range; user
picks one to pair; forget; show paired serial/fw/name; last pull
time; last error. Battery optional. Unpaired ⇒ no background pull.

### 4.7 Error handling

- F1 timeout → send F4, retry once, then disconnect and wait.
- F2 silent (no reply ~4 s) → log MTU, disconnect, retry later
  (classic `2D010002` MTU gate).
- F3 timeout / empty mid-file → keep `.part`, F4, disconnect;
  next cycle resumes at `.part` size.
- Ring disconnect mid-pull → same as F3 timeout.
- AS11 preemption → abort F3 loop, F4 if possible, keep `.part`.
- Unfinalised after full size → keep `.part` / do not mark done;
  retry later.
- No SD → skip pull (same as other writers).
- Other rings in range: show on scan; after pair, `GET_INFO`
  mismatch → disconnect, no files written.

## 5. Acceptance criteria

- [ ] `MAX_CONNECTIONS=2`; AS11 never torn down for a ring pull.
- [ ] Ring disconnect does not trigger AS11 auto-reconnect.
- [ ] SHQO2Pro and `S8-AW` both match the watch filter.
- [ ] `T8520_*` / `0x036F` is not used to start a file session.
- [ ] Unpaired: never connects for a pull (scan-only if user asked).
- [ ] Pair stores serial from `GET_INFO`; a second pair replaces it.
- [ ] After pair, a different ring's advert does not yield a pull
      (serial mismatch → disconnect).
- [ ] Handshake + F1–F4 write a known night as the **same bytes**
      the ring sent into
      `.somnotrace/oximetry/files/<serial>/<name>.bin`.
- [ ] `.bin` still contains 3-byte samples (motion/flags intact)
      and the 48-byte trailer when finalised.
- [ ] Re-running pull does not duplicate a finalised file.
- [ ] Interrupted F3 resumes from `.part` without starting at 0.
- [ ] No `SA2.edf` / SDCARD write / uploader trigger from this path.
- [ ] CRC fixture and AUTH derivation have host or on-device tests
      (no patient data in fixtures).
- [ ] No third-party protocol source copied; header on new files.
- [ ] `0xE3` / `0xEE` never sent.
- [ ] Background path never holds the link; while worn it may
      probe LIVE_B at most once per 60 s, then disconnect.
- [ ] After pair or pull the ring GAP link is down (no leftover
      connection). The ring can finish END and power off.
- [ ] After a completed pull, firmware never reconnects while the
      same advert continues (a connection would reset the ring's
      power-off timer). Ring powers off on its own ~120 s after
      take-off. `0xE3` / `0xEE` never sent.
- [ ] If the advert persists past pull + 130 s, served is cleared
      (re-worn inference) and the next take-off pulls normally.
- [ ] After advertising stops, the next appearance starts a new
      pull (new session after take-off).
- [ ] Never pulls while LIVE_B `[5]==0x01` (on-finger) or on a
      `0x036F` / `T8520_*` recording advert. Pull only after
      `[5]==0x00` (off-finger END window).
- [ ] Probe never sends F4 / F1 / SET_UTC. Those run only after
      LIVE_B `[5]==0x00`.
- [ ] After a completed off-finger pull the GAP link is down so
      the ring can finish END and power off (~120 s window X).
- [ ] Watch scan is passive (no scan requests).

## 6. Security / privacy considerations

- Oximetry is personal health data. Raw `.bin` stays on-device
  under `.somnotrace/oximetry/`. v1 does **not** upload it.
- No real recordings in git / tests / issues.
- No bonding; anyone in range can speak OxyII. Physical possession
  of the ring is the trust model (same as the vendor app).
- Do not log serial in uploaded crash reports if avoidable.

## 7. Open questions

- How long SHQO2Pro keeps advertising after END if we never
  connect (must be long enough for one scan+pull; ~90 s observed).
- F3 budget while AS11 StreamData is hot (resume covers this).
- Later: SleepHQ / OSCAR ingest without throwing away motion.
- Later: more than one paired serial on one box.

## 8. Changelog

- 2026-06-17: Stub.
- 2026-08-19: Proposed. Dual-connect, both SKUs, pair-by-serial
  (one ring), raw files only. SA2/OSCAR/SleepHQ export withdrawn.
  Advert-flip trigger withdrawn.
- 2026-08-19: Sync window is the only connect trigger. Periodic
  reconnect forbidden. Disconnect after every session is mandatory
  so the ring can deep-sleep (battery).
- 2026-08-19: `adv_deep.py` — take-off does not change the advert.
  Window X ≈ 120 s then power-off. Probe = AUTH+SETUP+LIVE_B;
  F4 only after `[5]==0x00`. 30 s reconnect while worn is OK.
- 2026-08-19: SHQO2Pro keeps advertising if put back on after a
  pull (no advert gap). Measured: a GATT connection during END
  resets the ~120 s power-off timer, so after a pull the firmware
  never reconnects while the advert lasts. Re-worn is inferred
  passively (advert alive at pull + 130 s). No power-off command
  (`0xEE` / `0xE3` forbidden). Worn probe interval 60 s.
