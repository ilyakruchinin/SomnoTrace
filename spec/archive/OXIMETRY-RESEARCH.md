# O2 Ring HR interference: protocol review and physical BLE experiments

**Date:** 2026-08-25  
**Status:** Investigation report; no SomnoTrace firmware changes made  
**Primary test device:** SleepHQ O2 Ring Pro / SHQO2Pro, serial `2572300897`, firmware `2D010003`

## 1. Executive summary

Two users' raw O2 Ring recordings contain a genuine, highly regular HR-only artifact approximately every 79 seconds. The artifact is present in the ring's raw Format A data and, for the Sleepyhead dataset, in the ViHealth CSV export byte-for-byte for SpO2 and HR. It is not introduced by SomnoTrace conversion, graphing, or upload.

SomnoTrace's current worn-ring probe cadence is approximately 81.4-81.6 seconds, not 60 seconds. The effective cadence consists of the explicit 60-second retry plus the task's 15-second outer delay, a four-second scan, and roughly two seconds of connection/GATT work. This occupies the same broad frequency range as the user artifact, but the stored 79-second feature is not phase-locked to the logged 81.5-second connections under the documented one-sample-per-second timeline. Direct one-connection/one-spike causation has therefore not been demonstrated.

The physical experiments in this report establish several important facts:

1. The ring exposes a standard Heart Rate service and subscribable Heart Rate Measurement characteristic, but it sends no Heart Rate notifications while worn. This path cannot detect removal.
2. A single persistent OxyII connection can query unauthenticated `cmd=0x04` reliably. In the test, 72/72 requests succeeded with a median response time of 40 ms.
3. `cmd=0x04` detected removal immediately: the first request after the operator marker returned state `0x00`, SpO2 `255`, HR `255`.
4. AUTH, SETUP, GET_INFO, large-MTU negotiation, and repeated GATT discovery are not required for contact detection on firmware `2D010003`.
5. A 12-minute recording made during one persistent connection with `cmd=0x04` every 10 seconds contained no 10-second periodic HR artifact and no affected-user-style sharp 79-second artifact.
6. A true 21-minute zero-Bluetooth-operation control also contained no sharp 79-second artifact.
7. OxyII `cmd=0x05` works on `2D010003` and returns the documented raw two-channel optical buffer. It provides a strong off-finger signature, but it is much more expensive than `cmd=0x04` and is not appropriate as the production contact detector.
8. The planned exact-35-second reconnect test was invalid: all scheduled connection attempts failed before HCI connection establishment because the cached BlueZ device object disappeared after the initial scan. No cadence conclusion can be drawn from that arm.
9. The randomized reconnect and longer persistent tests were stopped at the maintainer's request.

The best current automated candidate is one persistent, minimal OxyII connection with infrequent unauthenticated `cmd=0x04` polls, followed by the full identity and file-transfer setup only after state changes to off-finger. The safest data-integrity option remains no overnight connection at all, with sync triggered manually or near CPAP therapy termination.

## 2. Material reviewed

The investigation considered:

- `https://github.com/nglessner/o2ring-s-protocol`
- all current issues, including Issue #1 and Issue #5 with comments
- Discussion #6, including the `cmd=0x05` raw-buffer findings
- the referenced Plantucha/Tepna raw optical investigation
- the public Lepu BLE SDK patterns
- SomnoTrace's OxyII specification, implementation, host POCs, logs, and raw user recordings
- physical Linux/Bleak tests against the local SHQO2Pro

Relevant SomnoTrace paths:

- `main/oximeter_oxyii.c`
- `spec/0003-o2ring-ble-sync.md`
- `.ai/OXY/DESIGN.md`
- `.ai/OXY/POC/`
- `.ai/pulse-interference/`
- `.ai/pulse-interference/ble_lab.py`
- `.ai/pulse-interference/lab-results/`

## 3. Previously established user-data findings

### 3.1 Dark dataset

Seven Format A recordings were analyzed.

| Recording | Duration | Best periodic HR component | Folded peak-to-trough |
|---|---:|---:|---:|
| `20260817000413` | 8.13 h | no stable 79 s artifact | 0.73 bpm |
| `20260818000155` | 8.23 h | no stable 79 s artifact | 1.06 bpm |
| `20260818232905` | 8.30 h | no stable 79 s artifact | 0.83 bpm |
| `20260819233032` | 7.96 h | 79.0 s | 5.70 bpm |
| `20260820235432` | 7.91 h | 78.9 s | 7.33 bpm |
| `20260822000835` | 8.11 h | 78.9 s | 4.15 bpm |
| `20260823001702` | 8.14 h | 78.7 s | 8.32 bpm |

The affected nights contain a narrow HR rise with no corresponding periodic SpO2 or motion change. The onset closely follows the introduction of SomnoTrace's O2 Ring BLE support on August 19, which is strong circumstantial evidence but not event-level proof.

### 3.2 Sleepyhead dataset

The recording `20260821232015_2D010003` contains a stable approximately 79.1-second HR artifact with about 4.27 bpm folded peak-to-trough.

The vendor CSV and raw Format A file contain exactly the same 29,827 SpO2 and HR samples. Motion in the CSV is exactly the raw motion byte multiplied by two. The artifact therefore originated in the ring's stored output.

### 3.3 Maintainer recordings

The maintainer's supplied overnight files do not contain a comparable sharp feature. Their periodic components in the relevant band are generally around 1-2 bpm peak-to-trough and are not stable at approximately 79 seconds.

## 4. Upstream protocol implications

### 4.1 `cmd=0x04` is the explicit contact-state query

The verified reply contains:

- state `0x00`: no finger contact
- state `0x01`: finger present
- state `0x03`: file handle open
- live SpO2, HR, motion, battery, and PPG count

Local testing had already shown that `cmd=0x04` replies without AUTH. The physical experiments in this report reconfirmed that behavior over a 12-minute connection.

### 4.2 Live queries and file transfer can share one connection

Issue #5 documents that `cmd=0x04` and file commands can coexist on the same connection when requests are serialized. This was verified upstream on firmware `2D010001` and `2D010002`, including while worn and recording internally.

This supports a design where SomnoTrace keeps one minimal status connection and upgrades that same connection to a download session only after contact is lost.

### 4.3 Discussion #6 and `cmd=0x05`

Discussion #6 identifies `cmd=0x05` as a raw two-channel optical drain buffer:

- reply begins with a little-endian record count
- up to 102 records
- each record is two 32-bit channels plus one motion byte
- the 102-record response is a cap, not a fixed sample rate
- frequent draining is needed to avoid losing backlog

It is not a passive broadcast and has no known explicit contact-state field. It requires a connection, CCCD subscription, repeated writes, and large replies.

Discussion #6 also mentions a vendor-confirmed hourly HR artifact in older firmware, reportedly fixed around May 27, 2026. That issue demonstrates that derived HR firmware artifacts have existed, but it is distinct from the approximately 79-second feature examined here.

### 4.4 No known universal passive off-finger advertisement

Retail T8520 units may advertise differently by state:

- worn/recording: `T8520_*`, manufacturer `0x036F`
- syncable: `S8-AW`, manufacturer `0xF34E`

That state flip should be used when it is reliable.

The tested SHQO2Pro does not provide a known equivalent transition. Previous SomnoTrace ESP32 passive measurements found the same SHQO2Pro name, `0xF34E:00`, Heart Rate UUID, and approximately 58 ms advertisement interval while worn and through END. Silence occurs only at power-off, too late for download.

The Linux passive-scan repetition was inconclusive because of host-controller/tooling limitations:

- BlueZ Advertisement Monitor accepted registration but did not deliver reports without a separate discovery owner.
- legacy `hcitool` passive scanning was rejected by the Intel controller with HCI `Command Disallowed`/I/O error.
- active scanning confirmed the expected advertisement, but active scanning was not treated as a strict passive reproduction.

This does not overturn the earlier ESP32 passive evidence.

## 5. Physical test harness

A safe experiment harness was created at:

- `.ai/pulse-interference/ble_lab.py`

It supports:

- advertisement observation
- true idle/no-operation controls
- GATT inventory and standard Heart Rate subscription
- one persistent unauthenticated `cmd=0x04` connection
- fixed/random reconnect schedules
- raw `cmd=0x05` collection
- JSONL event logs
- automatic HCI btsnoop capture

The OxyII command allow-list in the experimental paths permits only:

- `0x04` live/contact query
- `0x05` raw optical drain

It cannot send file, clock, configuration, factory-reset, or other administrative commands. File retrieval after recordings used the existing POC only after the ring was removed.

BlueZ was started with `--experimental` using:

- `/etc/systemd/system/bluetooth.service.d/experimental.conf`

This enabled Advertisement Monitor API testing. It did not alter SomnoTrace firmware.

## 6. Physical experiment results

### 6.1 GATT inventory and standard Heart Rate candidate

Result file:

- `lab-results/20260825-000011-gatt.jsonl`
- `lab-results/20260825-000011-gatt.btsnoop`

The ring exposed:

- Battery Service `0x180F`
- Device Information `0x180A`
- Heart Rate Service `0x180D`
- Heart Rate Measurement `0x2A37`, notify
- Body Sensor Location `0x2A38`, read
- OxyII service `E8FB0001-...`
- the legacy `14839AC4-...` service
- DFU service `0xFE59`

Subscription to Heart Rate Measurement succeeded. The CCCD write was acknowledged. During more than 60 seconds worn, the ring emitted zero Heart Rate Measurement notifications. A Battery Level notification was received, proving the GATT notification path was operational.

Conclusion: the standard Heart Rate service is advertised and present but dormant. It cannot be used as an unsolicited contact/removal stream in the tested state.

### 6.2 One persistent connection with `cmd=0x04` every 10 seconds

Results:

- `lab-results/20260825-000518-persistent.jsonl`
- `lab-results/20260825-000518-persistent.btsnoop`
- pulled recording: `persistent-pull/20260825000451.bin`

Connection behavior:

- one connection remained stable for approximately 12 minutes
- 72/72 `cmd=0x04` requests succeeded
- 60 replies were on-finger state `0x01`
- 12 replies were off-finger state `0x00`
- median response time: 40.0 ms
- response range: 23.8-57.4 ms
- connection establishment: approximately 0.57 s under BlueZ
- first off-finger response followed the operator marker by approximately 38 ms

The off-finger replies initially reported:

- state `0x00`
- SpO2 `255`
- HR `255`
- motion `0`

The final reply reported SpO2/HR `0`, still with state `0x00`.

Stored recording analysis:

- 743 samples
- no 10-second component: amplitude approximately 0.03-0.04 bpm, folded peak-to-trough approximately 0.13-0.15 bpm
- 79-second amplitude approximately 0.44-0.49 bpm
- no affected-user-style narrow 79-second artifact

The short duration permits only about nine 79-second cycles, so this is encouraging but not a definitive overnight safety result.

### 6.3 Raw `cmd=0x05` on-finger capture

Results:

- `lab-results/20260825-002259-ppg.jsonl`
- `lab-results/20260825-002259-ppg.ppg.bin`

At 0.5-second poll spacing:

- 120/120 replies succeeded
- initial replies were capped at 922 bytes / 102 records while backlog drained
- steady replies alternated roughly 65-85 records
- empty request payload worked; `{07,01}` was unnecessary

This confirms Discussion #6 on firmware `2D010003`.

### 6.4 Raw `cmd=0x05` contact transition

Results:

- `lab-results/20260825-002637-ppg.jsonl`
- `lab-results/20260825-002637-ppg.ppg.bin`

Steady-state channel statistics after excluding initial capped backlog:

| State | Channel 0 median | Channel 0 SD | Channel 1 median | Channel 1 SD |
|---|---:|---:|---:|---:|
| On finger | 1,726,678 | 29,808 | 1,790,329 | 20,845 |
| Off finger | 3,482,459 | 211 | 897 | 29 |

Off-finger behavior is unmistakable:

- channel 0 saturates at a high, nearly flat level
- channel 1 collapses to a near-zero, nearly flat level

Record count and reply cadence did not identify contact; the channel values did.

Conclusion: raw optical data can infer contact reliably, but it is far more expensive than the explicit `cmd=0x04` state and offers no production advantage for this purpose.

### 6.5 Zero-Bluetooth-operation control

Results:

- `lab-results/20260825-003124-idle.jsonl`
- pulled recording: `idle-pull/20260825003057.bin`

The ring recorded for 1,297 samples (21 minutes 37 seconds) while the host issued no scan, connection, subscription, or command.

Periodic analysis:

| Period | Sinusoidal amplitude | Folded peak-to-trough |
|---|---:|---:|
| 10 s | 0.06 bpm | 0.20 bpm |
| 35 s | 0.18 bpm | 0.72 bpm |
| 79 s | 0.14 bpm | 0.66 bpm |
| 81.5 s | 0.56 bpm | 1.62 bpm |

No affected-user-style sharp 79-second artifact was present.

### 6.6 Exact-35-second reconnect arm: invalid

Results:

- `lab-results/20260825-005636-reconnect.jsonl`
- `lab-results/20260825-005636-reconnect.btsnoop`
- pulled recording: `fixed35-pull/20260825005419.bin`

The scheduler correctly generated ten deadlines approximately 35 seconds apart. However, every attempt failed immediately with:

- `BleakError("device 'dev_DB_A7_64_8C_1F_64' not found")`

There were:

- zero HCI connections
- zero CCCD subscriptions
- zero `cmd=0x04` writes

The cached `BLEDevice` object referenced a BlueZ D-Bus device path removed after the initial scan. The experiment therefore applied no 35-second GATT stimulus. Its recording must not be used to infer whether a shifted cadence changes HR artifacts.

The earlier interrupted 20-minute invocation also reached only the initial scan/prompt stage and sent no probes.

## 7. Interpretation

### 7.1 What is established

- The affected-user artifact is real and stored by the ring.
- The local ring does not show the same sharp artifact in a zero-interaction control.
- Repeated full GATT setup is not necessary to determine finger contact.
- One minimal persistent connection can determine contact quickly and reliably with `cmd=0x04`.
- Short-term 10-second polling on one connection did not imprint a 10-second periodic artifact in the stored HR data.
- Standard Heart Rate notifications do not provide an alternative stream on this firmware.
- Raw `cmd=0x05` is available but excessive for contact detection.

### 7.2 What remains unproven

- Whether current SomnoTrace reconnects directly or indirectly cause the users' 79-second artifact.
- Whether a valid 35-second reconnect schedule moves the artifact.
- Whether a randomized reconnect schedule produces event-locked HR changes.
- Whether one persistent connection remains harmless over an entire night.
- What ViHealth does when its ring tab is open.
- Whether affected physical units behave differently from the local `2D010003` unit.

### 7.3 Why the current firmware path is excessive

For every worn probe, SomnoTrace currently performs:

1. passive scan
2. connection
3. MTU exchange
4. OxyII service discovery
5. characteristic discovery
6. descriptor discovery
7. CCCD enable
8. AUTH
9. 200 ms delay
10. SETUP
11. GET_INFO
12. `cmd=0x04`
13. disconnect

The physical test shows that contact detection itself needs only:

1. an established connection
2. notification enable
3. `cmd=0x04`

The response then arrives in approximately 40 ms.

## 8. Recommendations

### 8.1 Safest option: no overnight GATT

For maximum confidence in medical-data integrity:

- never connect while the ring is worn
- use passive advertisement mode flips where supported
- initiate sync manually after removal, or
- initiate a short sync-watch window around CPAP therapy termination

This eliminates SomnoTrace as an overnight interference source.

Trade-off: SHQO2Pro does not expose a known passive removal flag, so fully automatic downloads can be missed if the ring powers off before a trigger.

### 8.2 Best current automatic candidate: one minimal persistent connection

For SHQO2Pro-like units without a useful advertisement transition:

1. Discover and verify the ring once at pairing or presence start.
2. Establish one connection, not one connection per poll.
3. Subscribe only to OxyII notifications.
4. Do not AUTH, SETUP, GET_INFO, GET_CONFIG, SET_UTC, F4, or negotiate file-transfer MTU during worn status monitoring.
5. Poll unauthenticated `cmd=0x04` every 15-30 seconds.
6. When state becomes `0x00`, perform GET_INFO once to verify serial.
7. Upgrade the same connection to the file-transfer sequence.
8. Pull files and disconnect immediately.

Why 15-30 seconds:

- the measured END window is approximately two minutes
- a 30-second poll leaves several opportunities to detect removal
- traffic is one small request/reply per interval
- it is less intrusive than one-second live polling and much less intrusive than repeated reconnection/discovery

The local ten-second test was clean but too short to certify overnight use. Before production, run at least two full-night A/B recordings on an affected ring:

- no connection
- one persistent connection with 30-second `cmd=0x04`

### 8.3 If persistent connection is rejected: minimal reconnect probe

If an overnight held connection proves harmful or consumes too much battery:

- cache OxyII value/CCCD handles after pairing
- connect using the known address when valid
- skip MTU 517 for the small status frame
- skip service/characteristic/descriptor discovery when cached handles are valid
- write CCCD
- send only `cmd=0x04`
- disconnect immediately after the reply
- verify serial only before download or after an address/service change

Fallback to discovery if:

- cached handles fail
- Service Changed is indicated
- firmware version changes
- MAC changes after factory reset

This should reduce a current approximately 2.1-second probe session to a small fraction of a second. It still needs affected-ring testing because connection establishment itself may be the trigger.

### 8.4 Reduce cadence while worn; increase it near expected removal

An adaptive policy can reduce overnight exposure:

- while CPAP therapy is active: no ring connections, or very infrequent checks
- at therapy stop/mask-off: begin 15-30-second contact checks for several minutes
- if removed: pull immediately
- if still worn: fall back to a longer interval or manual sync

This is safer than connecting every approximately 81.5 seconds for the entire night.

### 8.5 Use passive mode transitions whenever available

For retail T8520 units that reliably expose:

- `T8520_*` / `0x036F` while recording
- `S8-AW` / `0xF34E` when syncable

SomnoTrace should never connect during the recording advertisement. Connect only after the passive transition.

### 8.6 Do not use these alternatives for contact detection

- Standard Heart Rate `0x2A37`: no notifications observed.
- `cmd=0x05`: reliable optical contact signature but excessive data and polling.
- RSSI: too variable and unrelated to contact.
- advertisement silence: occurs after the download window.
- masking stored HR around known probe times: modifies medical data and does not address non-phase-locked behavior.
- continuous raw PPG streaming: unnecessary, high traffic, and unvalidated overnight.

### 8.7 Preserve diagnostic observability

Any revised firmware should log:

- presence first seen
- connection start and completion
- whether handles were cached or discovered
- CCCD completion
- exact command writes and response times
- `cmd=0x04` state transitions
- disconnection reason
- poll schedule/deadline
- ring serial verification event

These timestamps are required to correlate future stored recordings without relying on inferred cadence.

## 9. Recommended implementation direction

The evidence now supports the following order:

1. Add an optional experimental persistent-status mode for affected-ring A/B testing.
2. Use one connection and unauthenticated `cmd=0x04` at 30-second intervals.
3. Perform identity verification and full setup only after off-finger detection.
4. Keep retail advertisement-flip handling as the preferred no-connection path.
5. Add a manual-sync fallback.
6. Do not deploy persistent mode as the only production strategy until an affected user completes full-night paired controls.
7. Capture ViHealth Android HCI traffic later to determine whether the vendor app uses a held connection, `cmd=0x04`, another command, or an advertisement transition.

## 10. Final conclusion

The physical tests substantially narrow the options. There is no useful connectionless standard Heart Rate stream, and raw optical polling is unnecessarily heavy. The current reconnect-and-full-handshake probe is far more intrusive than the protocol requires.

A single persistent connection with sparse unauthenticated `cmd=0x04` queries is technically viable and was clean in a short local stored recording. It offers immediate removal detection and eliminates repeated connect/discovery/auth/setup cycles. It is the strongest automatic candidate, but it still needs full-night testing on an affected physical ring.

For the strongest medical-data safety guarantee, eliminate overnight GATT entirely and sync manually or near CPAP therapy termination. If automatic overnight monitoring remains a requirement, minimize it to one connection and one small explicit state query per 15-30 seconds, with all expensive setup deferred until removal is confirmed.
