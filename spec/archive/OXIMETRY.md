# Oximetry architecture and design

- **Status:** Architecture proposal; no implementation in this document
- **Scope:** Multi-oximeter acquisition, storage, normalization, Web UI, SMB, and SleepHQ
- **Prepared:** 2026-08-23
- **Primary existing spec:** `spec/0003-o2ring-ble-sync.md`

## 1. Executive decision summary

SomnoTrace should treat oximetry as an independent recording domain, not as a special case inside the current O2 Ring S driver or CPAP EDF exporter.

The recommended architecture has four deliberately separate representations:

1. **Device protocol** — how a driver discovers a device and acquires data: stored-object download, periodic polling, or a continuous BLE stream.
2. **Immutable source evidence** — the exact vendor file when one exists. For live-only devices, the durably committed canonical capture is the source of truth; a raw notification journal is retained only when the driver requires it for re-decoding or audit.
3. **Canonical oximetry dataset** — a device-neutral manifest plus compact SNT-derived binary tracks. The Web UI reads only this representation; it never parses vendor formats.
4. **Destination artifacts and state** — backend-specific upload plans and independently persisted success/failure state for SMB and SleepHQ.

Key decisions:

- **Keep raw vendor data byte-for-byte.** It is source evidence and may be the only representation SleepHQ accepts.
- **Do not put a serial number in canonical recording paths.** Use an opaque stable `device_key` for acquisition namespaces and a globally unique `recording_id` for final recordings. Store serial/model/firmware as metadata.
- **Use filesystem discovery as the durable work queue.** Atomic manifests describe work, but a reboot-time reconciler always rediscovers incomplete downloads, staged conversions, ready recordings, and upload work. No volatile state transition can permanently strand data.
- **Build the canonical format by evolving SomnoTrace's existing SNT model**, rather than introducing CBOR, Arrow, Protobuf, or a new compression dependency. A JSON manifest supplies semantics; simple fixed-width binary tracks supply compact data and browser-friendly parsing.
- **Keep oximetry recordings independent from CPAP sessions.** Associate them at query/display time by absolute UTC interval overlap. Do not force a one-to-one foreign key.
- **Make upload state per recording generation and per backend.** SMB success must not imply SleepHQ success, and vice versa.
- **Use a distinct SleepHQ oximetry workflow.** It must select the O2/Viatom import type and send a source format SleepHQ explicitly supports. Do not send the canonical SomnoTrace format or fabricate a legacy VLD file unless an exact, tested exporter is available.
- **Do not make raw PPG mandatory.** The core model supports optional plethysmography, perfusion index, quality/status, battery, and alarms, but the minimum useful dataset remains SpO2, pulse, motion/activity, and validity.

## 2. Goals and non-goals

### 2.1 Goals

- Preserve current Wellue O2 Ring S post-wear file download behavior.
- Add legacy Wellue/Viatom and unrelated oximeters without changing the Web UI data model.
- Support all three acquisition styles:
  - autonomous device recording followed by file download;
  - periodic spot measurement/polling;
  - long-running notification or stream capture.
- Preserve all information the source provides, including motion/status and optional waveform data.
- Render oximetry alongside therapy on the existing absolute-time uPlot dashboard.
- Survive power loss or reboot during download, conversion, live capture, manifest update, and upload.
- Upload only new or changed recordings, independently to SMB and SleepHQ.
- Retain enough provenance to re-run a corrected decoder later.
- Avoid device-specific JavaScript.
- Stay suitable for an ESP32-S3 with FATFS/SD, constrained internal RAM, 8 MB PSRAM, two BLE connection slots, and concurrent AS11 capture.

### 2.2 Non-goals

- A clinical diagnostic interpretation engine.
- A universal medical interchange format replacing EDF/EDF+ or IEEE 11073.
- Lossy conversion of unknown source semantics to make an unsupported backend accept them.
- Storing patient names or other added identifiers.
- Replacing the existing ResMed-compatible `SDCARD/` export tree.
- Combining CPAP and oximetry into one physical file merely because they overlap in time.

## 3. Current-state audit

### 3.1 Existing O2 Ring S flow

The existing implementation is one concrete protocol and acquisition policy in one module:

- `main/oximeter_oxyii.c` implements OxyII BLE framing, pairing, serial verification, END-window probing, time setting, file listing, and file download.
- `main/oximeter_store.c` owns `.somnotrace/oximetry/inbox`, `files`, `paired.json`, and `index.json`.
- Pair identity is the serial returned by `GET_INFO`, correctly not the rotating BLE address.
- The background task passively watches the paired ring, probes `LIVE_B`, and pulls only when the ring is off-finger.
- A successful file is expected to contain O2 Ring S trailer magic `48 12 5a da` at `file_size - 44`.
- Raw files are currently stored under `files/<serial>/<source-name>.bin`.
- Only pairing/status controls are exposed to the Web UI. There is no recording catalog, normalized format, graph path, or oximetry upload path.

### 3.2 Important implementation/spec divergence

The architecture must not assume the present download path is already crash-safe:

1. `spec/0003-o2ring-ble-sync.md` requires keeping an incomplete `.part` and continuing from its durable length on the next presence. The active `oxyii_pull_file()` violates that requirement by calling `ox_store_part_remove()` unconditionally and restarting at offset zero.
2. `ox_store_promote()` computes `finalised` from trailer magic, but the write/remove/index promotion path is not conditional on that result: it still writes a final `.bin`, removes the `.part`, and indexes the source when `finalised` is false. A failed completion check therefore does not leave the source in the inbox as the spec describes.
3. `oxyii_pull_file()` returns `ESP_OK` after the transfer loop even when the loop ended on a timeout or short write; the caller can mark the device presence served despite incomplete source data.
4. `index.json` is rewritten in place, not temp-file + `fsync` + rename. A torn update can erase the only catalog.
5. The `.part` namespace is only the source filename, not `(driver, device, source object)`, so future devices can collide.

These are implementation issues for a later task, but the new design's invariants explicitly remove their architectural causes.

### 3.3 Current O2 Ring S time behavior

`oxyii_time_payload()` calls `localtime_r()`. Despite the opcode name `SET_UTC_TIME`, the ring receives SomnoTrace's configured local wall time. The protocol research indicates the ring stores those fields verbatim and uses them for its display and subsequent filenames.

The command is sent during off-finger file preparation, after the just-finished recording. Therefore:

- it cannot correct the timestamp already embedded in the recording being downloaded;
- it can improve the next recording's clock;
- it is not currently sent during pairing;
- the current path does not explicitly gate the write on `time_is_usable()`;
- no before/after ring-clock reading, measured offset, uncertainty, or sync provenance is retained with the recording.

The design retains local-wall synchronization for human-readable device behavior, but records enough metadata to convert to an unambiguous UTC timeline.

### 3.4 Reusable existing architecture

SomnoTrace already contains patterns worth reusing:

- The session writer's ordered durability commit and alternating CRC checkpoint slots.
- Immutable SNT streams with small fixed headers and `INT16_MIN` missing markers.
- Browser parsing of binary SNT into typed arrays, client caching, min/max overview tiers, HTTP HEAD/Range support, and uPlot rendering.
- The uploader's backend registry, per-backend statuses, cooldown ladder, and periodic self-healing scan.
- Atomic per-day upload-state files.
- Storage leases and the rule that live therapy capture outranks bulk work.
- Noon-day folders for aligning data shown in the dashboard.

The design generalizes these patterns rather than building a parallel stack from scratch.

## 4. Evidence from protocol and ecosystem research

### 4.1 O2 Ring S/OxyII versus legacy Wellue

The two referenced protocol projects describe materially different device families:

| Property | O2 Ring S / OxyII | Legacy Wellue O2 Ring |
|---|---|---|
| BLE service | `E8FB0001-A14B-98F9-831B-4E2941D01248` | `14839ac4-7d7e-415c-9a42-167340cf2339` |
| Stored format observed | Format A: 10-byte header, 3-byte 1 Hz records, 48-byte trailer | VLD v3: 40-byte header area, 5-byte records, about one record/4 s |
| Core stored fields | SpO2, pulse, motion/status | SpO2, pulse, invalid, motion, vibration |
| Invalid representation | Source-specific: zero and/or `0xff` cases | `0xff` plus invalid field |
| File addressing | Byte offset | Block number |
| Live data | SpO2/pulse/motion/battery header and unvalidated high-rate PPG body | Real-time sensor command |
| Device time | Explicit set command; fields stored verbatim | Structured recording time; no set command documented by the reference |

A transport driver and a source decoder must consequently be independent concepts. A Wellue-branded device is not a sufficient format identifier.

### 4.2 Fields found beyond SpO2/pulse/movement

Other consumer and Bluetooth-standard oximeters may expose:

- perfusion index / pulse amplitude index;
- signal quality or measurement qualification;
- no-contact, sensor-displaced, inadequate-signal, and low-perfusion flags;
- normal, fast, and slow SpO2/pulse estimates;
- raw or processed plethysmography waveform;
- vibration/alarm state and threshold events;
- battery percentage/status;
- spot-check timestamps and stored-record access;
- device counters useful for detecting dropped notifications.

The Bluetooth Pulse Oximeter Service (PLXS, service `0x1822`) is especially important for future interoperability. Its continuous and spot-check characteristics standardize SpO2/pulse and can include measurement status, device/sensor status, timestamp, and pulse amplitude index. A future PLXS driver should fit the architecture without introducing new canonical fields.

BerryMed/BCI-style and some fingertip oximeters stream high-rate pleth samples with lower-rate numerical measurements. This is why the canonical model needs multiple tracks and cannot assume all channels share one sample rate.

### 4.3 SleepHQ evidence and uncertainty

The public SleepHQ API documentation confirms OAuth and generic import/file/process endpoints. The current SomnoTrace SleepHQ backend creates a CPAP import, uploads a ResMed tree, and calls `process_files`.

A public community client for Viatom data uses a different selection step: it requests the next import with the O2 type enabled (`GET .../teams/{team}/imports?o2=true`), then uploads the original file with filename/path/content hash and processes that import. This supports the user's statement that oximetry requires explicit type selection.

However, unauthenticated public documentation does not establish all of the following:

- the exact current O2 import creation/reservation contract;
- which O2 Ring S and legacy formats are accepted;
- whether an O2 import can safely contain multiple nights;
- whether it can be combined with a CPAP import;
- how to poll processing to terminal success;
- whether content-hash deduplication is guaranteed;
- the stable mapping from device model/source format to importer profile.

These must be verified against an authenticated test account before implementation. The architecture therefore treats SleepHQ device/import selection as an adapter profile, not a hardcoded guess.

## 5. Conceptual architecture

```text
BLE coordinator / therapy lifecycle / schedule
                    |
                    v
        +-------------------------+
        | Oximeter acquisition mgr|
        +-------------------------+
          | driver capability + policy
          v
 +------------------+      +------------------+
 | Protocol drivers |      | Source decoders  |
 | OxyII, legacy,   |----->| Format A, VLD,   |
 | PLXS, BCI, ...   |      | PLXS stream, ... |
 +------------------+      +------------------+
          | source artifact       | canonical samples
          v                       v
 +------------------+      +---------------------+
 | Inbox / live     |----->| Canonical dataset  |
 | capture writer   |      | manifest + SNT data|
 +------------------+      +---------------------+
          ^                       |
          |                       +--> Web catalog/API/uPlot
          |                       |
          +---- boot reconciler <-+--> upload planner
                                         |        |
                                         v        v
                                       SMB     SleepHQ
```

### 5.1 Layer responsibilities

#### BLE/resource coordinator

- Owns scan/connection arbitration across AS11 and oximeters.
- Gives AS11 reconnect and live therapy notifications highest priority.
- Knows whether a driver can be interrupted and resumed.
- Prevents a periodic or file-transfer operation from starving therapy capture.
- Makes the two-connection limit explicit instead of relying only on buffer contention.

#### Protocol driver

A driver owns only device-facing behavior:

- discovery match and confidence;
- identity/model/firmware retrieval;
- capabilities;
- safe time read/set operations;
- stored-object enumeration/open/read/close;
- live/spot stream start, packet parsing boundaries, sequence counters, and stop;
- safe disconnect and retry behavior.

A driver does **not** choose final folders, render graphs, or know SMB/SleepHQ.

#### Acquisition manager

- Selects the policy implied by driver capabilities and user configuration.
- Creates inbox jobs or live recording staging areas.
- Persists progress and calls storage commits.
- Emits immutable source artifacts or canonical live samples.
- Applies therapy/battery/BLE priority rules.

#### Source decoder

- Identifies and validates one source format.
- Converts source-specific invalid markers and flags without discarding the raw values.
- Emits canonical channels, source status, timing evidence, and source-provided summaries.
- Is versioned independently from the BLE driver.

#### Canonical writer

- Writes binary tracks and manifests.
- Implements ordered durability and final verification.
- Never exposes a recording as `ready` before all required output is durable.

#### Catalog/reconciler

- Scans filesystem state at boot and periodically.
- Repairs or restarts incomplete acquisition/conversion work.
- Builds an in-memory catalog; any optional disk catalog is a cache, not authority.
- Enqueues newly ready or changed recording generations for each backend.

#### Upload planner/backends

- Converts a recording into backend-specific immutable artifact sets.
- Persists state separately per backend and recording generation.
- Handles remote transaction recovery and terminal acknowledgement.

## 6. Driver and capability model

A driver descriptor should declare behavior rather than forcing all oximeters through one state machine.

### 6.1 Acquisition capabilities

- `stored_objects`: device lists and returns completed recordings.
- `random_access`: object reads can resume at an offset/block.
- `completion_marker`: source has a checksum/trailer/length rule.
- `spot_poll`: host can request a measurement periodically.
- `continuous_notify`: device can notify lower-rate numerical data.
- `waveform_notify`: device can stream pleth data.
- `device_records_while_disconnected`: normal autonomous wearable behavior.
- `stream_required_for_history`: no post-session retrieval exists.
- `read_clock`, `set_clock`, and declared clock basis.
- `stable_serial`: identity is available independently of BLE address.
- `interruptible` and `resume_semantics`.

### 6.2 Acquisition policies

#### A. Stored-object pull

Used by O2 Ring S and likely legacy Wellue:

1. Detect a driver-specific safe sync window.
2. Verify stable device identity.
3. Enumerate source objects.
4. Reconcile against immutable local source fingerprints/keys.
5. Resume or start missing objects.
6. Validate and finalize source artifact.
7. Disconnect according to the driver's power behavior.
8. Let conversion and upload proceed asynchronously.

#### B. Periodic polling

For devices that expose only current measurements:

- Start a recording from a configured schedule, therapy start, or valid-contact transition.
- Poll at a driver-declared cadence.
- Timestamp each observation from device time when trustworthy, otherwise host UTC plus latency/uncertainty.
- Commit gaps explicitly; never forward-fill in storage.
- Stop after therapy end plus a grace period, configured schedule end, prolonged no-contact, or explicit user action.

#### C. Continuous stream

For devices that require a live BLE stream:

- Hold the second BLE connection only while needed.
- Buffer bounded sample batches in PSRAM and send them to a dedicated storage owner.
- Use device counters/sample cadence to place samples on a uniform timeline.
- Insert missing samples/status when counters jump or a reconnect occurs.
- Reconnect with bounded backoff without blocking AS11.
- Enforce per-session reconnect-attempt, reconnect-rate, and total connection-time budgets. Exceeding a budget forces a cooling-off disconnect and explicit gap/interruption so an unstable oximeter cannot churn BLE, starve AS11, or drain its battery indefinitely.
- Finalize an interrupted-but-valid recording after a reboot or unrecoverable disconnect.

A device can support more than one policy. User-visible configuration should select a safe default, while the driver can reject unsafe combinations such as OxyII live mode mixed with file transfer in one connection.

## 7. Identity and time model

### 7.1 Device identity

Use an opaque, path-safe `device_key`, for example a truncated SHA-256 over a versioned tuple:

```text
device_key = hash(identity_schema || driver_id || manufacturer || model || serial)
```

Properties:

- stable across BLE address rotation;
- does not expose the serial in directory listings or SMB paths;
- prevents collisions between different protocol families with the same short serial;
- supports historical data from more than one paired device.

Store a device registry entry under `devices/<device_key>.json` containing:

- manufacturer, marketed model, protocol model code;
- serial and serial source;
- driver ID and last known firmware;
- first/last seen times;
- BLE address/name hints;
- capability snapshot;
- configured friendly label;
- SleepHQ adapter profile if verified;
- most recent clock synchronization result.

The serial belongs in metadata because it establishes provenance and prevents accidental cross-device attribution. It does not belong in the canonical path. A portable recording manifest should also snapshot the relevant identity fields so the recording remains intelligible if the device registry is lost.

### 7.2 Recording identity

A recording ID must be unique and immutable. Recommended form:

```text
<UTC-start-basic>_<source-fingerprint-prefix>
```

Examples are illustrative only. The identity must not depend solely on a vendor filename because clocks can be wrong and two devices can generate the same name.

- For downloaded sources, derive the suffix from `(device_key, source object key, full source content hash)`. A displayed hash prefix is only shorthand: creation must collision-check the full identity and add a persisted disambiguator if the path already belongs to different content.
- For live recordings, allocate a sufficiently large random/counter suffix when capture starts and persist it immediately.
- Keep the recording ID unchanged if better clock evidence later changes canonical start time.
- A changed decoder creates a new **generation** of the same recording, not a new medical recording.

### 7.3 Time metadata

Every recording manifest should distinguish:

- raw device timestamp text/fields;
- declared device clock basis: `utc`, `local_wall`, `offset_time`, `host_arrival`, or `unknown`;
- IANA/POSIX timezone configuration used to interpret local wall time;
- UTC offset selected, including DST ambiguity resolution;
- canonical `start_epoch_ms` and `end_epoch_ms`;
- time source/provenance (`device_file`, `device_packet`, `host_ntp`, `host_as11_drift`, inferred filename, etc.);
- last clock sync before the recording and first sync after it;
- measured device-minus-host offset when available;
- correction applied to source time;
- uncertainty/confidence and reason;
- whether timing is uniform, explicitly timestamped, or reconstructed from packet counters.

Raw source bytes are never rewritten when correcting time. Re-running conversion can create a new canonical generation with updated timing metadata.

### 7.4 Clock synchronization policy

- Never set an oximeter clock unless `time_is_usable()` is true. The current OxyII path lacks this gate and must gain it during migration.
- Read the device clock before setting it when supported; record the offset.
- Set time at pairing and at the earliest driver-declared safe opportunity before a future recording.
- For O2 Ring S, retain the safe post-recording local-wall set so it prepares the next night; also set during pairing if protocol testing confirms it does not disturb data or power behavior.
- Capture a post-set readback when supported.
- Do not claim zero drift merely because a set command was acknowledged.
- Do not adjust stored source timestamps silently based on a later sync without retaining the applied model and uncertainty.

## 8. Storage layout

Recommended SD layout:

```text
.somnotrace/
  oximetry/
    devices/
      <device_key>.json

    inbox/
      <device_key>/
        <source_object_key>/
          acquire.json
          source.part

    staging/
      <recording_id>/
        recording.json.tmp              # recording identity + active-generation pointer
        source/
          <original-name>.<original-extension>
        generations/
          <generation>/
            manifest.json.tmp
            data/
              vitals.snt.tmp
              pleth.snt.tmp
              pleth_mm.snt.tmp
        capture.ckpt

    recordings/
      <noon-day-YYYYMMDD>/
        <recording_id>/
          recording.json                # points to one complete active generation
          source/
            <original-name>.<original-extension>
            live.rawlog                 # only when the driver requires it
          generations/
            <generation>/
              manifest.json
              data/
                vitals.snt
                pleth.snt                # optional
                pleth_mm.snt             # optional overview envelope
                events.jsonl             # optional sparse events

    quarantine/
      <reason>-<timestamp>-<source-key>/
        acquire.json
        source.part

    state/
      uploads/
        <noon-day-YYYYMMDD>.json
      catalog-cache.json                # optional/rebuildable
```

### 8.1 Why this layout

- `inbox` is keyed by device and source object, preventing filename collisions.
- `staging` contains everything necessary to restart conversion after reboot.
- `recordings` is organized by the same local noon-day concept used by therapy UI, but every sample remains absolute UTC.
- A recording is portable: source, canonical data, and manifests travel together.
- Immutable generation directories let Web/UI/upload readers finish against generation N while a corrected decoder builds N+1. `recording.json` is atomically changed only after the new generation is complete; old generations can be reclaimed later under an explicit policy.
- Serial numbers are metadata, not path components.
- Source files are not mixed into the ResMed-compatible `SDCARD/` tree.
- Derived backend exports need not be permanently stored unless expensive to regenerate. If cached, they should live below the recording and be fingerprinted by exporter version.

### 8.2 Filenames

- Preserve the vendor filename exactly in manifest metadata.
- Sanitize the physical source filename for FATFS path safety and collision handling; never trust BLE-provided text as a path.
- Include `original_name` and `source_object_key` in `acquire.json`/manifest.
- Keep the original extension when known (`.vld`, `.bin`); an extension is not a format identifier.
- Do not put a user-friendly device label, MAC, or serial in a final path.

### 8.3 Coexistence and migration from the current layout

The current flat `inbox/`, `files/<serial>/`, root `paired.json`, and root `index.json` remain read-only migration inputs until every source is accounted for:

1. Create/update `devices/<device_key>.json` from NVS plus `paired.json`; NVS remains the active pairing authority until the driver/config migration is separately complete.
2. Scan actual files under `files/<serial>/` rather than trusting `index.json` alone. Match index entries when available, but freshly validate every source's size, structure, and trailer.
3. Copy each valid source into a new staging recording, hash and compare the copy, convert it, then publish generation 1. Do not delete or move the old file as part of this migration.
4. Treat old `finalised=false` entries and files that fail fresh validation as incomplete/invalid evidence: retain them in place and expose a diagnostic/quarantine import action rather than silently publishing them.
5. Reconcile old flat `.part` files by paired identity and remote listing where possible. If identity is ambiguous, retain/quarantine them; never append them to a guessed device object.
6. Keep the old reader/import scan enabled across at least one release. `index.json` becomes a legacy hint, not new-system authority.
7. Removal of old `files/`, flat `inbox/`, `paired.json`, or `index.json` is a later, explicit user-approved cleanup operation only after a full migration report shows no unmatched bytes.

## 9. Canonical oximetry dataset

### 9.1 Format choice

Use a **JSON recording manifest plus SNT v3 fixed-record binary tracks**.

Why:

- SomnoTrace already writes, serves, and parses SNT.
- The browser already uses typed arrays, whole-file caching, range requests, and min/max tiers.
- Oximetry vitals are small: even four 16-bit fields at 1 Hz are about 230 KB for eight hours.
- Fixed-width integers make ESP writes and validation simple.
- A JSON manifest is human-debuggable and supplies channel semantics without adding a CBOR/Protobuf library.
- Multiple tracks solve mixed sample rates cleanly.

Do not use JSON/CSV for sample data, EDF as the internal graph format, Apache Arrow, or Gorilla compression in the first implementation. They add cost without solving a measured storage problem.

### 9.2 SNT v2/v3 coexistence and v3 requirements

SNT v3 is a parallel, version-detected format for new canonical oximetry tracks; it does not rewrite or invalidate existing therapy SNT v1/v2 files. Shared embedded/browser readers must dispatch by magic/version and support both for as long as historical therapy data exists. Therapy capture may adopt v3 only in a separate migration.

The current `Hz x 10` field cannot exactly represent rates such as 0.25 Hz. V3 must support:

- explicit version and header byte length, so a reader can skip extensions it understands only partially;
- fixed-width little-endian records;
- an exact rational cadence for uniform tracks, represented as a numerator/denominator (for example `period_num_us / period_den`), not rounded integer Hz or microseconds;
- `start_epoch_ms`;
- durable sample count;
- timing mode:
  - uniform records, or
  - explicit per-record epoch/delta timestamp for genuinely irregular polling;
- tier (`L0` raw canonical values or `L1` min/max envelope);
- record/channel count and sample width;
- missing sentinel `INT16_MIN` for scalar channels;
- flags indicating signed versus unsigned interpretation where needed;
- optional data CRC/finalized marker, or equivalent integrity fields in the manifest.

The exact packed header layout should be specified and tested in a separate implementation spec. This document defines semantics, not C struct ABI.

### 9.3 Track model

Each track has one cadence and one timing model.

#### Required `vitals.snt`

Channel descriptors in the manifest choose from:

| Semantic | Preferred storage | Unit/scale | Notes |
|---|---:|---|---|
| `spo2` | int16 | centi-percent | `9800` = 98.00%; missing is `INT16_MIN` |
| `pulse_rate` | int16 | centi-bpm | preserves future fractional standard values |
| `motion` | int16 | source-defined or normalized descriptor | never imply comparability without a documented transform |
| `sample_status` | uint16 bits | canonical bitset | present for every record |
| `perfusion_index` | int16 optional | centi-percent | optional PLXS/vendor PI |
| `signal_quality` | int16 optional | basis points or source scale | descriptor states mapping |
| `battery` | int16 optional | centi-percent | include only if cadence is meaningful |
| `source_status` | uint16 optional | raw source bitfield | preserves unknown/vendor bits |
| `vibration` | int16 optional | boolean/state | legacy VLD or alarm state |

Tracks include only available channels. Missing optional metrics are absent, not zero-filled.

#### Optional `pleth.snt`

- One or more int16 waveform channels in arbitrary/source physical units.
- Manifest records original bit depth, signedness, scale, sampling rate, and whether decode is verified.
- Unvalidated OxyII PPG must not be labeled as a clinically meaningful waveform until verified.

#### Optional `pleth_mm.snt`

- Min/max envelope at a lower exact rate, generated from valid L0 samples.
- Used for full-night overviews; raw PPG is loaded only for zoomed ranges.
- Min/max is preferred to mean or LTTB because it preserves excursions and matches the existing dashboard approach.

#### Optional `events.jsonl`

Sparse records such as:

- low-SpO2 alarm on/off;
- pulse alarm on/off;
- sensor removed/replaced;
- vibration periods;
- stream disconnect/reconnect;
- source-defined markers.

Each line contains UTC onset, optional duration, canonical type, source type/value, and confidence. Repeated per-sample alarm/status bits can remain in `vitals.snt`; JSONL is for sparse annotation and audit.

### 9.4 Canonical sample-status bits

Keep the required v1 mapping small: `NO_CONTACT`, `INVALID_MEASUREMENT`, and `TRANSPORT_GAP`. Reserve optional stable meanings for drivers with verified evidence:

- `SPO2_UNAVAILABLE`
- `PULSE_UNAVAILABLE`
- `MOTION_ARTIFACT`
- `LOW_PERFUSION`
- `INADEQUATE_SIGNAL`
- `POOR_SIGNAL`
- `SENSOR_DISPLACED`
- `QUESTIONABLE_MEASUREMENT`
- `HOST_TIMESTAMPED`
- `ALARM_ACTIVE`

Rules:

- Canonical bits are set only when source semantics support the mapping.
- Unknown vendor bits remain in `source_status` and manifest decoder notes.
- A nonzero O2 Ring S status byte must not automatically be mapped to every canonical quality problem.
- Invalid numerical samples are stored as `INT16_MIN`; status explains why when known.
- The display uses gaps (`null`/`NaN`), never zero or forward-fill.
- Statistics exclude missing/invalid samples and state their validity policy.

### 9.5 Motion semantics

“Movement” is not standardized across devices. Store three pieces separately:

1. the source value (`motion` with its source scale descriptor);
2. any verified canonical interpretation (`activity_normalized`, only if a defensible mapping exists);
3. quality meaning such as `MOTION_ARTIFACT` in status.

The UI can graph a source motion index and label it by device. It must not compare “50” from OxyII with “50” from another vendor unless normalization is documented.

### 9.6 Manifest outline

Root `recording.json` is the small atomic publication point: it contains schema version, recording ID, immutable source identity/hash, and `active_generation`. The selected generation manifest carries the detailed device, time, track, conversion, summary, and integrity data below. This duplication should be minimized, but the generation must remain independently verifiable.

Illustrative generation manifest, not a finalized JSON schema:

```json
{
  "schema": "somnotrace.oximetry.generation/1",
  "recording_id": "...",
  "generation": 1,
  "state": "ready",
  "terminal_reason": "completed",
  "device": {
    "device_key": "...",
    "driver_id": "wellue_oxyii",
    "manufacturer": "Viatom/Wellue",
    "model": "T8520",
    "serial": "...",
    "firmware": "..."
  },
  "acquisition": {
    "mode": "stored_object",
    "started_epoch_ms": 0,
    "completed_epoch_ms": 0,
    "source_object_key": "...",
    "reconnects": 0,
    "dropped_packets": 0
  },
  "source": [{
    "original_name": "...",
    "stored_path": "source/...",
    "format_id": "wellue_oxyii_format_a",
    "size": 0,
    "sha256": "...",
    "validator_id": "...",
    "validator_version": 1
  }],
  "time": {
    "raw_device_start": "...",
    "device_clock_basis": "local_wall",
    "timezone": "...",
    "utc_offset_seconds": 0,
    "start_epoch_ms": 0,
    "end_epoch_ms": 0,
    "source": "device_filename",
    "correction_ms": 0,
    "uncertainty_ms": 0,
    "confidence": "high"
  },
  "tracks": [{
    "id": "vitals",
    "path": "data/vitals.snt",
    "timing": "uniform",
    "period_num_us": 1000000,
    "period_den": 1,
    "sample_count": 0,
    "sha256": "...",
    "channels": [
      {"semantic": "spo2", "storage": "i16", "scale": 0.01, "unit": "%"},
      {"semantic": "pulse_rate", "storage": "i16", "scale": 0.01, "unit": "bpm"},
      {"semantic": "motion", "storage": "i16", "scale": 1, "unit": "source_index"},
      {"semantic": "sample_status", "storage": "u16", "unit": "bitset"}
    ]
  }],
  "conversion": {
    "decoder_id": "wellue_oxyii_format_a",
    "decoder_version": 1,
    "source_fingerprint": "...",
    "firmware_version": "..."
  },
  "summary": {
    "computed": {},
    "source_reported": {}
  },
  "integrity": {
    "verified": true,
    "warnings": []
  }
}
```

Keep source-reported trailer summaries separate from SomnoTrace-computed summaries. Differences are valuable diagnostics and should not be overwritten.

## 10. Crash consistency and reconciliation

### 10.1 Fundamental invariant

A recording can always be recovered by scanning files. No persisted status may be the only evidence that work exists.

### 10.2 Downloaded source objects

`acquire.json` is atomically rewritten (`.tmp` -> flush -> `fsync` -> rename) and includes:

- driver/device/source object key;
- original name;
- expected size when known;
- remote revision/completion evidence when known;
- durable local length checkpoint;
- acquisition attempts and last error;
- final source hash once complete.

The file length is still authoritative. Resume algorithm:

1. Open/enumerate the remote object and obtain expected size/revision.
2. Inspect `source.part`.
3. If local length is greater than remote size, source identity/revision changed, or alignment is impossible, quarantine rather than append blindly.
4. Resume from the durable local offset/block when the driver supports it.
5. Commit chunks in bounded batches; update progress only after `fflush`/`fsync` according to policy.
6. Require exact expected length **and** driver validator success.
7. `fsync` the completed source, compute hash, and atomically rename it into staging.
8. Never create a final source file or remove the resumable partial after failed validation.

For O2 Ring S, length equality is insufficient; trailer magic and source structure are required. A timeout/short write is a failed acquisition, not a successful presence.

### 10.3 Conversion of immutable sources

Conversion is deterministic from `(source hash, decoder ID/version, time model version)`:

1. Ensure source artifact is durable in `staging/<recording_id>/source`.
2. Allocate the next immutable generation and write its tracks as `.tmp`.
3. Update track headers and counts.
4. Flush and `fsync` all tracks.
5. Validate file lengths, sample counts, timestamp bounds, and checksums/hashes.
6. Rename each track to its final name.
7. Write that generation's complete `manifest.json` with `state=ready` atomically **last**.
8. For a new recording, publish the staging directory into its start noon-day location using semantics verified on FATFS, then atomically write `recording.json`. For a re-conversion, atomically update `recording.json` to point to the complete new generation; never mutate the previously active generation in place.

After reboot:

- source present, no ready generation manifest: restart conversion from the beginning or a converter checkpoint;
- stale `.tmp`: ignore/remove only the derived temporary output, never the source;
- source present but its size/hash no longer matches durable acquisition metadata: retain it in quarantine as `source_corrupted` and do not derive new output;
- missing/torn `recording.json`: scan complete generation manifests and source identity to rebuild the pointer atomically; never infer readiness from track files alone;
- active generation with a missing/hash-invalid track: mark the recording damaged, stop serving/uploading that generation, fall back to a previously complete generation if policy permits, and regenerate from source; change the active pointer only after the replacement is complete;
- unknown/invalid source: move to quarantine with reason and keep bytes.

For expected overnight file sizes, restarting conversion is simpler and safer than a separate conversion journal. A checkpoint is justified only for large PPG sources after measurement proves restart cost is material.

### 10.4 Live capture

Live capture reuses the session writer's ordered commit principle:

```text
append complete sample batches
-> flush data
-> update durable sample counts
-> fsync affected tracks and any driver-required raw journal
-> write next alternating CRC checkpoint slot
-> fsync checkpoint
```

Recovery selects the newest valid checkpoint, truncates/ignores data beyond it, inserts an explicit interruption event, and either:

- resumes the same live recording if driver/session identity proves continuity; or
- finalizes it as `interrupted` and starts a new recording.

A terminal interrupted recording remains displayable and uploadable if its committed data is valid.

### 10.5 Catalog

Do not grow a single unbounded `index.json` as authority. Build an in-memory catalog by scanning manifests. A compact cache can accelerate boot, but corruption or deletion merely causes a rescan.

The scan should be bounded by configurable day windows for normal UI/upload work while still allowing a full maintenance rebuild.

## 11. Web API and uPlot design

### 11.1 Device neutrality

The browser receives canonical manifests/tracks only. Adding a source decoder must not require JavaScript changes unless it introduces a genuinely new canonical semantic.

### 11.2 Suggested API

- `GET /api/oximetry/recordings?date=YYYYMMDD`
  - recordings that overlap the local noon-day, including partial/interrupted terminal recordings;
  - ID, start/end, device label, available channels, quality summary, upload status.
- `GET /api/oximetry/recording?id=<recording_id>`
  - canonical manifest with sensitive operational fields filtered as appropriate.
- `HEAD|GET /api/oximetry/file?id=<recording_id>&track=vitals|pleth|pleth_mm|events`
  - binary stream with Content-Length, ETag from generation/hash, Range support, and bounded async worker.
- Optional `GET /api/day/timeline?date=YYYYMMDD`
  - union of CPAP and oximetry intervals to simplify dashboard loading.

IDs and track values must be allow-listed and resolved through the catalog; never concatenate untrusted query text into a path.

### 11.3 Loading strategy

For ordinary overnight vitals:

- fetch each ready `vitals.snt` once;
- parse into typed timestamp/value arrays;
- turn missing values into `NaN` or `null` according to the installed uPlot version;
- cache by `recording_id + generation + track hash`;
- perform min/max pixel-column envelopes client-side, as the dashboard already does.

For pleth:

- load `pleth_mm.snt` for full-night display;
- use Range-aligned block reads or a bounded raw window for deep zoom;
- do not download multi-megabyte raw waveform automatically on mobile.

Core 1 Hz/0.25 Hz vitals do not need server-side JSON expansion or a precomputed overview tier.

### 11.4 Dashboard behavior

Use absolute epoch milliseconds and the same shared x scale/cursor as therapy graphs.

Default panels when data exists:

1. SpO2 (%), with gaps for invalid samples.
2. Pulse rate (bpm).
3. Motion/activity (source index, device-labeled).
4. Perfusion index and/or quality when available.
5. Optional pleth waveform only on demand.

Behavior:

- List days that contain therapy **or** oximetry.
- Show oximetry even if no CPAP session exists.
- Align overlapping recordings by UTC, not filename prefix.
- If more than one oximeter recording overlaps, show device labels and allow selection; do not silently merge.
- Preserve gaps; `spanGaps` remains false.
- Visually distinguish questionable/invalid ranges using status overlays.
- Display time-confidence warnings when source time is uncertain.
- Report source-reported and computed summary statistics separately where useful.

### 11.5 Session association

Do not persist a hard CPAP session ID into the oximetry source model. Compute associations from overlap:

- recording interval intersects therapy interval;
- optionally rank by overlap duration and clock confidence;
- expose this as a rebuildable UI/catalog relation.

This handles a ring started before therapy, continued after therapy, multiple mask-on sessions, missing CPAP data, and oximetry-only nights.

## 12. Upload architecture

### 12.1 Generalization of current uploader

The present uploader is tightly coupled to ResMed EDF filename groups and a root bundle. Preserve its good scheduler/backend concepts but add a generic immutable upload-unit model:

```text
upload unit
  unit_id             recording_id
  generation          canonical/source generation
  local fingerprint   hash of backend artifact plan
  artifacts[]         role, path, size, hash, remote path/type
  backend state[]     independent SMB/SleepHQ state
```

Transport batching (one SMB/TLS session, several units per day) remains separate from success granularity (one recording generation).

Per-backend state should include:

- `pending | uploading | processing | ok | failed | unsupported`;
- attempts, last try, retry/cooldown time, and sanitized last error;
- artifact-plan fingerprint and config/profile generation;
- remote transaction/import ID;
- uploaded remote file IDs/hashes if available;
- final acknowledgement/status and time;
- whether a permanent configuration/profile error requires user action.

Persist state atomically per noon-day, independently from CPAP upload files or under a schema that can hold both typed units.

### 12.2 Reconciliation/idempotency

At boot and periodically:

1. Discover ready recording manifests.
2. Ask each enabled backend to produce an artifact plan.
3. Fingerprint the plan from role + content hashes + backend profile/version.
4. If no matching successful fingerprint exists, mark pending.
5. If files disappear or generation changes, invalidate only that backend unit.
6. Retry failed units through the existing cooldown ladder.

Mark `ok` only after the backend's complete success definition. An HTTP upload response alone is not necessarily final success.

### 12.3 SMB plan

Recommended default remote structure:

```text
<configured-base>/
  OXYMETRY/
    <noon-day>/
      <recording_id>/
        recording.json          # published last; points to active generation
        source/<vendor-file>
        generations/<generation>/
          manifest.json
          data/vitals.snt
          data/pleth.snt        # if present
          data/pleth_mm.snt     # if present
          data/events.jsonl     # if present
```

Decisions:

- Upload the portable recording package, including raw source and canonical files, by default.
- Do not expose the serial in paths; it remains in manifest metadata.
- Write remote files to a temporary name where SMB semantics allow, then rename after complete transfer. Probe/document target-server semantics rather than assuming atomic replace.
- If temporary rename is unavailable, upload immutable content to its final generation-specific name, publish that generation's manifest after its data, and still publish root `recording.json` last. An interrupted generation not selected by the root pointer is incomplete and is repaired/overwritten on retry; never mark it successful or present it as the committed package.
- Verify bytes written and preferably size/content hash; the current SMB path should not report success merely because the loop exited.
- Upload each generation's `manifest.json` after its data, then publish/replace root `recording.json` last. A root pointer therefore never selects an incomplete generation.
- Re-upload only when the package fingerprint/generation changes.
- A later optional setting may select `source only`, `canonical only`, or `complete package`, but `complete package` is the safest default.

### 12.4 SleepHQ plan

SleepHQ is not a generic file backup. Its adapter should have explicit profiles keyed by verified source format, for example conceptually:

```text
(driver_id, source_format_id, model/firmware constraints)
  -> supported SleepHQ O2 profile
  -> required original filename/path/content hash
  -> import selection parameter (observed community behavior: o2=true)
```

Rules:

- Use a separate O2 import transaction from CPAP unless authenticated testing proves a combined import is supported and preferable.
- Prefer the byte-exact vendor source format SleepHQ supports.
- Never upload SNT to SleepHQ unless SleepHQ documents support.
- Never label an O2 Ring S file as legacy VLD based only on brand.
- If a future source lacks a supported SleepHQ artifact, mark `unsupported`, while SMB and Web UI continue normally.
- An exporter is allowed only when its target format and all required semantics are understood and tested. The manifest records exporter ID/version and source generation.
- Persist the SleepHQ import ID **before** sending files.
- After reboot, inspect/resume or poll that import rather than opening a duplicate whenever the API permits.
- Supply a stable content hash/idempotency value.
- Call process/finalize only after all artifacts for that O2 unit are accepted.
- Poll import processing to a terminal accepted/complete state before marking `ok`; a processing failure remains retryable without pretending the recording was imported.

The production O2 adapter must not be implemented from the community-client inference alone. First run an authenticated contract test that verifies an O2 import workflow actually exists and documents its current contract, covering O2 Ring S source, legacy VLD source, duplicate hash, process status, and reboot between every remote step.

### 12.5 Independent endpoint outcomes

Example state after one night:

```json
{
  "recording_id": "...",
  "generation": 1,
  "backends": {
    "smb": {"status": "ok", "fingerprint": "..."},
    "sleephq": {"status": "failed", "attempts": 2, "last_error": "processing rejected"}
  }
}
```

SMB is never resent solely because SleepHQ failed. SleepHQ retries do not change SMB state.

## 13. O2 Ring S integration under the new architecture

The first implementation should preserve device-specific behavior from spec 0003 while moving responsibilities:

- `wellue_oxyii` driver: BLE discovery, serial verification, safe END probing, F1/F2/F3/F4, and local-wall clock set.
- `stored_object` acquisition policy: resumable inbox job keyed by device/source object.
- `wellue_oxyii_format_a` decoder: validates header/body/trailer, emits SpO2/pulse/motion/source status and source trailer summaries.
- canonical writer: generates recording package.
- generic Web UI and uploader discover the ready package.

Required corrections during that migration:

- resume at durable `.part` length;
- distinguish complete, incomplete, and invalid source files;
- propagate transfer timeout/short write as failure;
- never remove the only partial after failed validation;
- do not mark presence served until every selected source object is either already complete or newly validated;
- record device clock evidence and time provenance;
- reconcile existing `.bin` files regardless of old `index.json` state.

Existing raw files should not be moved destructively in the first migration. Import them into the new recording layout by verified copy/hash/atomic finalize, then leave the old tree until a later user-approved cleanup policy exists.

## 14. Legacy Wellue and future device integration

### 14.1 Legacy Wellue Driver (`oximeter_legacy.c`)

Implemented as an independent transport driver alongside OxyII:

- **GATT Service:** Primary service `14839ac4-7d7e-415c-9a42-167340cf2339`.
- **Characteristics:**
  - Write: `8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3` (all requests sent via ATT Write Request / write-with-response).
  - Notify: `0734594a-a8e7-4b1a-a6b1-cd5243059a57` (reassembled on lead byte `0x55` + CRC-8).
- **Control & Framing:**
  - `CMD_INFO` (`0x14`): JSON device info containing `SN`, `Model`, `CurBAT`, `FileList`.
  - `CMD_CONFIG` (`0x16`): JSON time synchronization `{"SetTIME":"YYYY-MM-DD,HH:MM:SS"}` when `time_is_usable()` is true.
  - `CMD_FILE_READ` (`0x03` / `0x04`): Chunked binary block download.
- **Decoder & Canonical Pipeline:**
  - Raw files are stored verbatim and validated by exact byte length matching `CMD_FILE_READ` total length (`ox_store_finalize_native()`).
  - Native VLD3 files (5-byte records, 4-second period) are converted via `oximetry_canonical_convert_vld3()` into SNT v3 tracks (SpO2, pulse rate, motion, vibration, invalid status).

### 14.1.1 Dual-Driver Dispatcher, Discovery & Pairing Architecture

The system supports both Gen1 (Legacy) and Gen2 (OxyII) devices seamlessly via `oximeter.c`:

#### A. BLE Advertisement Matching & Discovery (`gap_event`)
To ensure high-fidelity discovery during both UI scanning and low-duty background watch (`pull_task`), multi-criteria filtering is used:

1. **Gen1 (Legacy) Matching:**
   - **Service UUID Match:** Raw advertisement contains 128-bit Service UUID `14839ac4-7d7e-415c-9a42-167340cf2339`.
   - **Paired MAC Address Match:** Advertiser address matches `s_paired_addr`.
   - **Name Match with Gen2 Exclusion:** Device name matches Gen1 patterns (`"O2Ring"`, `"CMRing"`, `"KidsO2"`, `"Checkme"`, `"SleepU"`) **AND** Manufacturer ID is NOT `0xF34E` (Gen2 sync ID).
2. **Gen2 (OxyII) Matching:**
   - **Manufacturer ID Match:** Manufacturer Specific Data company ID equals `0xF34E` (idle/sync advert).
   - **Name Match:** Name starts with `"S8-AW"` or `"SHQO2PRO"` (excluding recording mode `0x036F` / `"T8520_"`).
   - **Paired MAC Address Match:** Advertiser address matches `s_paired_addr` (non-recording).
3. **Active Scanning Requirement:**
   - Both pairing scans and background watch (`do_scan()`) use **Active Scanning** (`passive = 0`). This causes the BLE radio to issue `SCAN_REQ` and receive Scan Response (`SCAN_RSP`) packets, which is essential because Gen1 rings place the 128-bit Service UUID in `SCAN_RSP` due to 31-byte advertising PDU limits.

#### B. Protocol Auto-Detection & Pairing Handshake
When the user pairs a device (or auto-detect mode `OX_DRIVER_AUTO` is used):

1. **Phase 1 — OxyII Probe:**
   - Initiates connection and requests ATT MTU exchange.
   - If the ring is Gen2: ATT MTU negotiates to 247+ bytes, OxyII service `e8fb0001-...` is discovered, AES-128 session authentication completes, device info is read, and the driver saves `driver = OX_DRIVER_OXYII` to NVS and `paired.json`.
   - If the ring is Gen1: ATT MTU exchange returns default 23 bytes (or service discovery fails with "service not found"). The probe identifies a protocol mismatch, terminates the link, and proceeds to Phase 2.
2. **Controller Settle Delay:**
   - A 500 ms delay is observed before Phase 2 to allow the BLE controller to finish disconnecting the previous link, preventing `BLE_HS_EBUSY` connection start collisions.
3. **Phase 2 — Legacy Fallback:**
   - Connects, discovers service `14839ac4-...`, queries `CMD_INFO` (0x14) JSON, synchronizes clock via `CMD_CONFIG` (0x16), and saves `driver = OX_DRIVER_LEGACY` to NVS and `paired.json`.
4. **Non-Destructive Probing:**
   - Probing attempts do not invoke `forget()` or erase persistent NVS storage. NVS and `paired.json` are committed only upon a successful pair.

#### C. Three-Tier Device Identity Model
1. **Tier 1 (Advertisement Filter):** Accepts packets matching UUID, known paired MAC, or candidate name patterns.
2. **Tier 2 (Candidate Selection):** Prioritizes the stored `s_paired_addr` if multiple rings are visible; falls back to first candidate if the MAC rotated (e.g. on hard reset).
3. **Tier 3 (Hardware Serial Number Gate):** Upon connection, reads `CMD_INFO` serial number. If it matches `s_serial`, sync proceeds and `s_paired_addr` is updated; if it mismatches, the connection is dropped immediately.

### 14.2 Standard PLXS

A future PLXS driver can map standardized measurement and device/sensor status bits to canonical channels. It may support:

- spot polling;
- continuous numerical notifications;
- stored spot records via Record Access Control Point;
- pulse amplitude index.

PLXS does not guarantee overnight storage or waveform, so capabilities are discovered per device.

### 14.3 Stream-only devices

A new stream-only driver must define:

- capture trigger/stop policy;
- measurement versus waveform cadence;
- sequence/gap detection;
- timestamp source and expected latency;
- no-contact behavior;
- reconnection continuity rule;
- whether a raw notification journal is required for re-decoding;
- storage estimate and retention policy.

The canonical/live writer and Web UI remain unchanged.

## 15. Resource, retention, and security considerations

### 15.1 BLE and task priority

- AS11 live therapy capture and reconnect outrank all oximeter bulk operations.
- A continuous oximeter stream can use the second slot, but must tolerate explicit coordinator preemption if required.
- File conversion, hashing, and upload should not run in BLE callbacks.
- Use bounded queues and PSRAM batches; raw capture owns SD writes through one storage worker.
- Bulk source conversion/upload respects storage leases and should defer under therapy load.

### 15.2 Storage estimates

Approximate eight-hour sizes before filesystem overhead:

- four int16 vital channels at 1 Hz: ~230 KB;
- four int16 vital channels every 4 s: ~58 KB;
- one int16 pleth channel at 100 Hz: ~5.8 MB;
- one uint8 raw pleth stream at 100 Hz: ~2.9 MB.

This supports simple uncompressed SNT for vitals. Initial waveform policy:

- For stored-object devices such as O2 Ring S, preserve the vendor source but do not start a live PPG connection merely because a waveform command exists; OxyII's waveform decode and multi-hour behavior are not yet validated.
- For a stream-required device, admit capture only after reserving worst-case SD space. Retain the canonical pleth track when waveform is a selected/required capability; retain a duplicate raw notification journal only when the driver explicitly requires re-decode/audit evidence.
- There is no implicit waveform auto-deletion in this proposal. A future size/age retention setting must be explicit; until then, low-space admission refuses optional waveform while preserving vitals/source integrity.

### 15.3 Retention

- Raw source and canonical vitals are source-of-truth health data and should not be auto-deleted merely because upload succeeded.
- Derived overview/export artifacts are regenerable and can be reclaimed first.
- Any future automatic retention deletion requires an explicit user policy and separate design.
- Quarantine files remain visible through diagnostics; do not retry malformed sources forever without cooldown.

### 15.4 Privacy/security

- Serial numbers and health data are sensitive local metadata. Avoid serials in URLs, routine logs, and remote paths.
- API responses should expose a friendly label/opaque key rather than full serial except in device settings/detail views.
- Never log raw sample payloads or upload credentials.
- Validate all source lengths, counts, arithmetic, and path components before allocation/use.
- Bound JSON manifest size, channel count, sample count, and track size.
- Treat SD manifests as untrusted after card removal/modification.
- SMB traffic may not provide confidentiality; the UI should state that SMB protection depends on the configured network/server. SleepHQ remains TLS-only.

## 16. Migration and implementation sequence

### Phase 0 — protocol fixtures and contracts

- Create synthetic/no-patient fixtures for OxyII Format A valid, incomplete, malformed, and edge invalid-marker cases.
- Create synthetic legacy VLD fixtures from public format documentation.
- Specify the SNT v3 header, rational-cadence encoding, v2/v3 detection/coexistence, and manifest JSON schemas.
- Authenticated SleepHQ contract test before writing production adapter logic.

### Phase 1 — storage/canonical foundation

- Introduce device registry, recording/generation manifests, staging/final layout, and reconciler.
- Implement generic canonical writer plus dual SNT v2/v3 readers and integrity checks.
- Implement low-space admission and the initial optional-waveform policy.
- Add unit tests for power loss at every ordered commit boundary.

### Phase 2 — O2 Ring S migration

- Split OxyII protocol/acquisition/storage responsibilities.
- Correct partial resume and completion semantics.
- Implement Format A decoder and import existing raw files non-destructively.
- Verify raw source hashes against device/vendor exports where test data is available privately.

### Phase 3 — Web UI

- Add catalog and track endpoints.
- Add SpO2, pulse, motion, and quality panels to the existing shared timeline.
- Test noon-day overlap, DST, wrong device clock, gaps, multiple recordings, and oximetry-only days.

### Phase 4 — generic upload units

- Extend/refactor scheduler without regressing current EDF groups.
- Add complete-package SMB upload with manifest-last remote commit.
- Add SleepHQ O2 profile only after contract verification.
- Test reboot after remote transaction creation, each file upload, process request, and process success.

### Phase 5 — legacy Wellue

- Add legacy BLE driver and VLD decoder independently.
- Compare canonical output against a trusted desktop parser using synthetic/non-patient fixtures.

### Phase 6 — live/polling foundation

- Add explicit BLE coordinator and live writer/checkpoint recovery.
- Implement PLXS first as the standards-based integration target.
- Add proprietary stream drivers only with measured protocol fixtures.

## 17. Verification and acceptance criteria

### Source acquisition

- [ ] Two devices with the same source filename cannot collide.
- [ ] Reboot at every chunk boundary resumes or safely restarts without losing a valid partial.
- [ ] Expected length without completion evidence is not accepted.
- [ ] Invalid source is retained in quarantine with a reason.
- [ ] A completed source is immutable and content-hashed.

### Conversion

- [ ] Every ready manifest references existing, length-valid, hash-valid tracks.
- [ ] Reboot during conversion always causes rediscovery and eventual completion/quarantine.
- [ ] Decoder/version changes create a new generation and invalidate backend plans.
- [ ] Missing/invalid data remains gaps, never plausible zero values.
- [ ] Raw motion/status bits remain recoverable.

### Time

- [ ] Canonical samples use absolute UTC.
- [ ] Local device time, timezone, correction, source, and uncertainty are retained.
- [ ] DST fallback cannot overwrite or merge recordings.
- [ ] Clock is never set from an unusable host time source.

### Web UI

- [ ] No vendor-format parser exists in JavaScript.
- [ ] Oximetry appears with and without CPAP data.
- [ ] Overlapping data uses one shared x-axis and preserves gaps.
- [ ] Multiple devices/recordings are labeled and not silently merged.
- [ ] Normal vitals loading is bounded and raw PPG is lazy.

### Uploads

- [ ] SMB and SleepHQ states are independent.
- [ ] Reboot cannot permanently strand a pending unit.
- [ ] A changed generation re-uploads only where required.
- [ ] SMB root `recording.json` selects a generation only after that generation's files and manifest complete.
- [ ] SleepHQ receives an explicitly selected, verified O2 profile/source format.
- [ ] SleepHQ is marked successful only after terminal processing success.
- [ ] Unsupported SleepHQ formats do not block local display or SMB.

### Regression/resource behavior

- [ ] AS11 live capture and reconnect remain highest priority.
- [ ] No filesystem or conversion work occurs in BLE callbacks.
- [ ] Memory/queue limits are measured with AS11 + Wi-Fi + Web UI active.
- [ ] Existing CPAP EDF upload behavior remains unchanged through migration.

## 18. Open questions requiring evidence

1. **SleepHQ authenticated O2 contract:** exact import selection/creation endpoint, supported format IDs/models, terminal status, deduplication behavior, and whether CPAP/O2 should share an import.
2. **O2 Ring S Format A motion/status semantics:** exact bits/scales must be confirmed before mapping beyond raw motion/source status.
3. **OxyII clock readback:** determine whether GET_INFO clock can reliably bracket SET time and whether pairing-time SET affects device power/recording behavior.
4. **Existing invalid `.bin` files:** audit whether any files were promoted without trailer completion under current behavior; migrate to ready only after fresh validation.
5. **SNT v3 irregular timing:** choose full epoch per record versus delta timestamps after representative spot-poll devices are selected.
6. **Live raw journal default:** decide per driver after measuring whether source notifications add useful re-decode value relative to storage cost.
7. **SMB remote verification:** determine server/library support for temporary rename and efficient post-upload verification.
8. **Multiple active oximeters:** architecture supports historical multiples; product policy may continue to allow only one active paired oximeter initially.
9. **SNT v2/v3 ABI and browser dispatch:** finalize the v3 packed header, rational cadence, version detection, and dual-parser test matrix before implementation; historical therapy v1/v2 remains untouched.

The SNT v3 ABI/dual-reader contract must be resolved before canonical conversion is implemented. The remaining uncertainties do not block a conservative O2 Ring S conversion that preserves unknown fields as raw/source semantics, but they should not be answered by guessing.

## 19. Rejected alternatives

### Parse vendor formats in JavaScript

Rejected because every new device would require browser code, duplicate embedded/source semantics, and expose raw parser attack surface in the UI. It also cannot solve upload or crash recovery.

### Convert everything to legacy VLD

Rejected because fields, cadence, invalid semantics, and provenance differ. It would lose data and may mislead SleepHQ about device type.

### Store only the canonical format

Rejected because decoder bugs become irreversible and SleepHQ may require original bytes.

### Store only vendor files and decode on each request

Rejected because it couples UI latency and memory to every format, repeats work, and makes stream-only devices awkward.

### Put serial in the recording path

Rejected because it leaks identity, complicates replacement/multi-device history, and is unnecessary for uniqueness. Serial stays in metadata.

### One global mutable index/state machine

Rejected because a torn/corrupt index can strand all work and an ever-growing JSON array is expensive. Per-job/per-recording manifests plus discovery self-heal.

### JSON/CSV canonical samples

Rejected because of size, parsing allocations, and loss of typed binary/range advantages.

### CBOR/Arrow/Protobuf/Gorilla in v1

Rejected for the baseline because they add dependencies and implementation complexity without a measured need. The manifest/SNT split is compact enough and aligns with current code.

### Treat oximetry as a CPAP EDF group

Rejected because acquisition timing, identity, source formats, and SleepHQ import behavior differ. Correlation by absolute time is more robust.

## 20. References

External sources were reviewed on 2026-08-23. Public reverse-engineered projects are protocol-study references only; implementation must remain clean-room as required by `CONTRIBUTING.md` and `THIRD-PARTY-NOTICES.md`.

### Protocols and formats

- O2 Ring S/OxyII protocol: https://github.com/nglessner/o2ring-s-protocol
- Legacy Wellue O2 Ring protocol and VLD v3: https://github.com/farolone/wellue-o2ring-protocol
- Bluetooth Pulse Oximeter Service 1.0.1: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/PLXS_v1.0.1/out/en/index-en.html
- Bluetooth Pulse Oximeter Profile 1.0.1: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/PLXP_v1.0.1/out/en/index-en.html
- BerryMed/BCI protocol reference: https://github.com/zh2x/BCI_Protocol
- OSCAR Viatom loader/source-format context: https://gitlab.com/CrimsonNape/OSCAR-code/-/blob/HEAD/viatom_loader.cpp

### SleepHQ

- Public API documentation/authentication: https://sleephq.com/api-docs/index.html
- Community Viatom/O2 API client showing O2 import selection: https://github.com/twack/sleephq_api_client
- Broader generated SleepHQ API client (generic imports): https://github.com/frohoff/sleephq-client
- Published compatible O2 devices (product context, not an API contract): https://shop.sleephq.com/products/sleephq-therapy-check

### Rendering/interchange context

- uPlot: https://github.com/leeoniya/uPlot
- HTTP Range requests: https://developer.mozilla.org/en-US/docs/Web/HTTP/Range_requests
- EDF+ specification: https://www.edfplus.info/specs/edfplus.html

### Relevant SomnoTrace files

- `spec/0003-o2ring-ble-sync.md`
- `main/oximeter_oxyii.c`
- `main/oximeter_store.c`
- `main/sd_storage.h`
- `main/session_writer.c`
- `main/session_graph.c`
- `main/portal.html`
- `components/uploader/upload_index.*`
- `components/uploader/upload_scan.*`
- `components/uploader/upload_sched.*`
- `components/uploader/uploader_smb.c`
- `components/uploader/uploader_sleephq.c`

## 21. Implementation progress summary

This section records the implementation completed after the architecture proposal was moved to this archive. The work covers stages 0 through 4 from the proposed implementation sequence. No commit or push was performed.

### Completed

#### Stage 0 — fixtures and SleepHQ contract

- Added `scripts/oximetry_contract_test.py`, which generates synthetic, non-patient oximetry payloads and performs the SleepHQ flow only when explicitly invoked with `--upload` and credentials supplied through environment variables.
- Authenticated contract testing confirmed:
  - OAuth token acquisition;
  - team discovery through `/api/v1/me`;
  - O2 import creation through `POST /api/v1/teams/<team>/imports?o2=true`;
  - multipart file upload;
  - `process_files`;
  - terminal import-status polling.
- A synthetic legacy VLD-shaped payload reached SleepHQ status `complete`.
- A synthetic OxyII Format A-shaped payload was rejected by SleepHQ as corrupted. The production design therefore preserves the original Format A source but creates a deterministic VLD3-derived SleepHQ artifact instead of sending Format A directly.
- Credentials were not embedded in source or written into the repository. Real patient/oximetry data was not used.

#### Stage 1 — canonical storage and recovery foundation

- Added `main/oximetry_canonical.c/.h`.
- Added canonical directories under `.somnotrace/oximetry/` for devices, inbox, staging, recordings, quarantine, and state.
- Added SNT v3 canonical vitals tracks with a fixed little-endian header, exact rational cadence fields, UTC start time, sample count, data length, CRC32, and explicit missing markers.
- Canonical Format A vitals currently contain SpO2, pulse rate, motion, canonical sample status, and original source-status fields.
- Added atomic JSON writes, source copying, track fsync, generation manifests, root `recording.json` publication, and staged-conversion replay during boot reconciliation.
- Added a rebuildable canonical catalog and safe recording/track path resolution.
- Added non-destructive migration scanning of existing `files/<serial>/*.bin` data.

#### Stage 2 — O2 Ring S integration

- O2 Ring S download now resumes from the durable `.part` length.
- Transfers fail closed on timeout, short write, overrun, zero-size response, or incomplete byte count.
- Raw promotion now requires the Format A trailer magic at `file_size - 44`, writes the final raw file atomically, and removes the partial only after successful finalization.
- Completed raw files are converted automatically into canonical recordings and a SleepHQ VLD3 export.
- Ring clock updates are blocked when `time_is_usable()` is false.
- Existing raw files are retained; migration does not destructively move or delete them.

#### Stage 3 — Web UI and API

- Added `main/oximetry_http.c/.h` and registered canonical endpoints:
  - `/api/oximetry/recordings`;
  - `/api/oximetry/recording`;
  - `/api/oximetry/file` with GET/HEAD and byte-range support;
  - `/api/oximetry/uploads`.
- Added a canonical-only oximetry panel to the dashboard with SpO2, pulse-rate, and motion graphs.
- The browser parses only SNT v3 canonical data; it does not parse vendor binary formats.
- Missing values are rendered as gaps rather than zeroes or forward-filled values.
- Oximetry is loaded independently and can be displayed on nights without CPAP data.

#### Stage 4 — SMB and SleepHQ upload integration

- Added `components/uploader/upload_ox.c/.h` for canonical recording discovery, fingerprints, persistent state, reconciliation, and per-backend status.
- Extended the uploader backend interface with oximetry transport callbacks.
- SMB uploads the portable oximetry package, including the raw source, canonical generation, manifests, and derived VLD3 export.
- SleepHQ creates an explicit O2 import, uploads the generated VLD3 artifact, uses filename-plus-content MD5 ordering, processes the import, and waits for terminal completion.
- SMB and SleepHQ states are independent and survive reboot through `.somnotrace/upload_state/oximetry.json`.
- Existing CPAP EDF upload behavior remains on its existing backend path.

#### Stage 5 — Legacy Wellue driver, dual-driver dispatcher & discovery remediation

- Added `main/oximeter_legacy.c` implementing full Gen1 transport (`14839ac4-...` service, all-WRITE_REQ chunks, `CMD_INFO` JSON parsing, `CMD_CONFIG` time sync, and native block download).
- Added exact-size promotion `ox_store_finalize_native()` in `main/oximeter_store.c` for raw Gen1 files and integrated `oximetry_canonical_convert_vld3()` for SNT v3 track generation.
- Added dual-driver orchestrator in `main/oximeter.c` supporting automatic protocol detection (`OX_DRIVER_AUTO`), probing OxyII first via ATT MTU negotiation and falling back cleanly to Legacy with non-destructive NVS management.
- Hardened BLE discovery in `gap_event` with multi-criteria matching (128-bit UUID, paired MAC address, and name patterns with Gen2 exclusion) and active scanning (`passive = 0`) to reliably receive `SCAN_RSP` packets in both UI and background watch loops.

### Verification completed

- `./scripts/build-dist.sh` completed successfully with ESP-IDF 5.5.1.
- `git diff --check` passed.
- Canonical contract tests (`scripts/oximetry_contract_test.py`) and VLD3 decoder tests (`scripts/vld3_decoder_test.c`) passed cleanly.
- The final firmware image was generated under `dist/` by the project build script.
- No credentials were found in the repository after a repository search.

### Current limitations and follow-up work

- Periodic polling, continuous streaming, PLXS, and live PPG capture are not implemented yet.
- Format A-to-VLD3 export is intentionally lossy in cadence/field representation for SleepHQ compatibility; the original source and canonical data remain available for SMB/local use.
- The SNT v3 ABI and canonical status-bit mappings should receive dedicated fixture tests before being treated as a stable public format.
- SleepHQ import IDs are persisted indirectly through the active uploader transaction, but full crash recovery between every individual remote API operation still needs hardware/integration testing.
- The dashboard currently displays the core canonical tracks; optional perfusion index, signal quality, battery, alarms, and pleth tracks are represented in the architecture but not yet populated by the O2 Ring S converter.

