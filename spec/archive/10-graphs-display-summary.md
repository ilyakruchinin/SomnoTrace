# Noon-day graph loading and display architecture

- **Status:** Proposed
- **Author(s):** Cascade (AI pair programmer), for review by @ilyakruchinin
- **Created:** 2026-07-20
- **Last updated:** 2026-07-20
- **Related specs:** `spec/0009-web-interface.md`, `spec/0010-web-ui-architecture-and-design.md`, `spec/0011-web-api-endpoints.md`, `spec/archive/local-data-files.md`, `spec/archive/files-over-http.md`, `spec/archive/http-server-concurrency-and-socket-exhaustion.md`

## 1. Summary

The dashboard should make a selected noon-day feel immediate while preserving a correct, stable view as data quality improves. The browser first obtains a sorted noon-day manifest for completed sessions, fixes the chart's full time extent from the first sample of the first session to the last sample of the last session, and progressively fills that fixed extent with L1 overview data. Progressive display is the chosen behavior because the manifest makes the final extent known before graph bytes arrive; no arriving session may move or resize the time axis. After every available L1 file has completed or failed, the browser streams each L0 file in the background. L0 availability changes the source used to build the current viewport, but never changes the viewport itself or recreates the chart.

The recommended transport is one sequential, fixed-length HTTP stream per file, with a small browser-side scheduler. The internal portal is plain HTTP; HTTPS/TLS is not part of this path. Parallel HTTP Range requests should not be the normal dashboard path: the current ESP-IDF HTTP server executes handlers synchronously on one worker task, so parallel requests do not provide parallel SD reads or sends. They consume sockets, add seeks and request overhead, and can delay useful bytes. Standard Range support should remain for interoperability, recovery, diagnostics, and a possible future viewport-priority optimization.

Rendering must be viewport-aware and pixel-aware. L1 and L0 are not two arbitrary line arrays: L1 is intended to contain one-second min/max aggregates of L0; newly generated v2 L1 must do so exactly, while historical v1 may contain the documented all-negative-bucket defect. At overview scales, use a custom thin-stroke min/max renderer that looks like one compressed waveform, not a filled band and not two boundary lines. At close scales, render a normal raw L0 polyline. This produces a mathematically consistent transition and avoids pretending that `(min + max) / 2` is a real breathing sample. Session boundaries and missing samples are always represented as line breaks.

## 2. Goals

1. Minimize perceived time from selecting a date to seeing useful noon-day data.
2. Initially fit the complete noon-day extent on screen.
3. Display every session in the noon-day without drawing lines between sessions.
4. Load all L1 overview files before starting background L0 retrieval.
5. Replace overview data with detail only where detail improves the current viewport.
6. Preserve the exact visible `[from, to]` range across every data arrival and redraw.
7. Support desktop and mobile zoom, pan, reset, and value inspection.
8. Maximize sustained file-transfer throughput without exhausting sockets or turning sequential SD access into competing seeks.
9. Keep full completed-session files in browser memory/cache so later zoom and pan do not require timestamp-specific APIs.
10. Make loading and tier decisions observable in development builds so regressions can be measured rather than inferred from graph appearance.

## 3. Non-goals

- Reimplement SleepHQ source code or visual design. The screenshots are behavioral references only.
- Add HTTPS/TLS to the internal web interface.
- Add a bespoke JSON API that returns graph points for arbitrary timestamp windows.
- Add more on-device storage tiers before measurements show L1 and L0 are insufficient.
- Change the stored clinical values or vertical physical scale as part of this work.
- Optimize unrelated log streaming, SMB upload, SleepHQ upload, or EDF generation.
- Treat a full-day L0 line as inherently better than an L1-derived compressed stroke when the display has far fewer pixels than samples.

## 4. Current implementation and root causes

### 4.1 Current data flow

The dashboard in `main/portal.html` currently:

1. Requests `/api/sessions?date=YYYYMMDD`.
2. Fetches every `brp_mm.snt` file.
3. Merges sessions into one pair of JavaScript arrays.
4. Creates a uPlot chart.
5. Fetches each `brp.snt` file.
6. Replaces the merged source and calls `setData()`.

The raw file endpoint in `main/session_graph.c` supports GET, HEAD, and byte ranges. The server configuration in `main/net_provision.c` allows ten open HTTP sockets, but ESP-IDF's normal `esp_http_server` still invokes ordinary file handlers synchronously on one worker task.

The live log viewer is already a WebSocket implementation, not SSE. `main/portal.html` connects to `/api/logs/ws`; `main/log_stream.c` performs a short WebSocket handshake and pushes frames from a separate forwarder task with `httpd_ws_send_frame_async`. An open log tab therefore does not hold the HTTP worker inside a long-running handler. It does consume one of the ten HTTP sockets, and the current single-viewer replacement path records the new fd without explicitly closing the displaced connection, so lifecycle and socket accounting still require testing and cleanup.

The SD card in `main/sd_storage.c` is requested in four-bit mode with all D0-D3 pins assigned and `SDMMC_FREQ_HIGHSPEED` as the maximum. A failed four-bit mount is retried in one-bit mode. Actual negotiated width and frequency are not currently exposed as structured portal diagnostics.

### 4.2 Correctness problems

#### 4.2.1 Session breaks are not stable

`mergeSessions()` inserts `null` into both X and Y arrays. uPlot expects a monotonic, finite X array; discontinuity should be represented by null Y values at finite timestamps. More importantly, `decimateForDisplay()` takes every Nth element across the globally merged array. It usually skips the one-element separator, reconnecting the last visible point of one session to the first visible point of the next.

**Required correction:** keep sessions separate in the data model, decimate each session separately, and add finite-X/null-Y break records only when constructing a temporary render array.

#### 4.2.2 Viewport selection is ignored during decimation

`getDashDisplayData(range)` accepts a range but decimates the entire noon-day. When zoomed in, only a small subset of those globally sampled points falls inside the viewport. This makes L0 look like L1 or worse, even when the full L0 file is present.

**Required correction:** first intersect each session with the viewport using index arithmetic or binary search, then decimate only the visible records to a target derived from plot width.

#### 4.2.3 L1 midpoint is not a real sample

L1 stores `[flow_min, flow_max]` for each one-second bucket. The current parser converts it to `(min + max) / 2`. That midpoint is neither the mean nor a sample from L0, and it can suppress the breathing amplitude. It cannot be expected to match a later raw line seamlessly.

**Required correction:** preserve min and max as separate values and feed them to the custom compressed-stroke renderer. Do not label a midpoint as raw breathing.

#### 4.2.4 Global subsampling is not suitable medical waveform decimation

Taking every Nth sample aliases periodic waveforms, may miss peaks, and shifts apparent morphology depending on the first selected index. It also causes visible changes when source length or range changes.

**Required correction:** use deterministic, viewport-anchored pixel buckets. Aggregate min/max for wide views; use raw samples when the visible sample count is sufficiently small.

#### 4.2.5 Data replacement can reset or move the viewport

`uPlot.setData()` resets scales by default unless explicitly told not to. Calling `setData()` and relying on scale hooks can also overwrite application viewport state. Recreating the chart guarantees a visible redraw and may derive a different full range from a different tier.

**Required correction:** chart lifetime, viewport state, and data state must be separate. Capture `[from, to]`, call `setData(data, false)`, then restore exactly that range while scale hooks are suppressed. Do not destroy the chart when L0 arrives.

#### 4.2.6 Full-day L0 may legitimately look no more detailed than L1

An eight-hour L0 session contains about 720,000 samples. A roughly 900-pixel chart cannot display them individually. Feeding 3,000 globally subsampled L0 values does not create visible high resolution and is not proof of a successful tier switch.

**Required correction:** choose the representation from viewport resolution, not from a fixed statement that L0 must always be plotted. At wide views, L1 is already sufficient to produce pixel-level min/max buckets. At close views, L0 must be used and raw samples become visibly different.

#### 4.2.7 Missing samples are converted to zero and v1 has sentinel ambiguity

The current parser maps the stored invalid marker to zero. A missing-data interval therefore appears as a clinically meaningful zero-flow line instead of a gap.

Current SNT v1 writers insert `-1` for missing BRP records and v1 L1 generation also uses `-1` for an all-missing bucket. However, `-1` is a legitimate captured BRP flow value, as the EDF conversion code already documents. SNT v1 therefore cannot distinguish every genuine `-1` flow sample from an inserted missing marker. There is also an L1 defect: maxima are initialized to `-1`, so an all-negative one-second flow bucket can retain `-1` as a false maximum.

**Decision:**

- Decode historical SNT v1 conservatively. For BRP L0, treat a record as a definite full gap when both flow and pressure are `-1`; do not discard every isolated flow value of `-1`, because negative flow is valid. A single channel equal to `-1` remains inherently ambiguous and should be retained unless surrounding records establish a writer-inserted gap run. For BRP L1, an all-missing channel is represented by `min == max == -1`; an isolated bound of `-1` is not by itself a missing bucket.
- Introduce SNT v2 for newly written files and use `INT16_MIN` (`-32768`) as the unambiguous missing marker across L0 and L1.
- Initialize all min/max accumulators with `INT16_MAX`/`INT16_MIN`, independent of the on-disk sentinel.
- Make parsers select the sentinel from the file version; do not guess from sample values.
- Map missing records to null Y values and preserve a discontinuity through every aggregation level.
- Keep the format-version change separate and covered by v1/v2 compatibility fixtures containing synthetic, non-patient data.

#### 4.2.8 Date changes and stale requests are not isolated

There is no per-load generation token and no `AbortController`. A slow response for a previously selected day can update shared dashboard state after the user selects another date.

**Required correction:** every date selection owns a load generation and abort controller. All asynchronous callbacks must verify the generation before mutating state.

#### 4.2.9 Current interaction behavior does not match requirements

The current double-click resets instead of zooming in. There is no right-double-click zoom-out implementation. Touch handlers only support panning when pan mode is active; default touch drag does not create a zoom selection. Touch listeners are passive, so they cannot reliably prevent page scrolling while manipulating the chart.

### 4.3 Transfer problems

#### 4.3.1 Parallel ranges do not create server-side parallelism

The normal ESP-IDF HTTP server has one synchronous request worker. Four or eight browser range requests can be open, but the server still services their handlers one at a time. For one SD card and one Wi-Fi interface this generally adds:

- HEAD and Range request/response overhead;
- socket pressure against `max_open_sockets = 10` and the global lwIP limit;
- multiple file opens and seeks;
- chunk reassembly and failure cases in JavaScript;
- delayed completion if one queued range fails;
- potential interference from status polling or an open log stream.

It does not produce four simultaneous SD reads or four simultaneous TCP sends.

#### 4.3.2 The current range merger accepts incomplete data

Failed range parts are set to null, but the remaining parts are concatenated and parsed as if they formed a complete file. A missing middle range shifts all subsequent records and can still produce apparently valid but wrong data.

**Required correction:** a file is complete only if every expected byte was received and validated. Progressive rendering may use validated prefixes, but completion and cache promotion require exact length and header consistency.

#### 4.3.3 Browser caching is underused

Completed sessions are immutable, but the file handler does not provide a complete immutable-cache contract using ETag or Last-Modified plus long-lived `Cache-Control`. Returning to a previously viewed day should normally require no SD or Wi-Fi transfer.

#### 4.3.4 SD speed has not been separated from HTTP or Wi-Fi speed

The configured host requests four-bit high-speed mode, but perceived graph load also includes FATFS, HTTP response framing, the single HTTP worker, ESP-to-access-point Wi-Fi uplink, browser parsing, and rendering. The internal web interface is plain HTTP, so TLS is not a dashboard cost.

The client devices and internet connection exceeding 100 Mbit/s do not establish ESP32 uplink throughput. Measure the ESP's RSSI, channel/protocol, power-save mode, retransmission behavior where available, and sustained HTTP payload rate. The current Wi-Fi configuration enables AMPDU but uses deliberately reduced buffer counts, and no explicit `esp_wifi_set_ps()` policy is present. A/B test temporarily using `WIFI_PS_NONE` during foreground graph transfer and restoring the normal policy afterward; only increase Wi-Fi buffers if measurements show starvation and internal-RAM budget permits it.

The bottleneck must be measured before increasing sockets, Wi-Fi buffers, or SD buffers.

## 5. Proposed architecture

### 5.1 State model

Use one `NoonDayGraphController`-style state object rather than independent globals:

```text
loadGeneration
abortController
noonDay
phase: idle | manifest | overview | detail | ready | error
fullRange: { minMs, maxMs }
viewport: { minMs, maxMs }
userHasChangedViewport
sessions[] completed-only, sorted by startMs
  id
  startMs
  endMs
  clockDriftMs
  sampleCount
  l1: empty | loading | partial | ready | failed
  l0: empty | loading | partial | ready | failed
  l1Data: typed min/max records
  l0Data: interleaved typed records or validated prefix
chart
renderScheduled
suppressScaleEvents
```

Invariants:

1. `fullRange` is noon-day metadata, not a property of whichever tier most recently arrived.
2. `viewport` is application state and is never inferred from newly arrived data.
3. Sessions stay separate until a viewport render array is built.
4. A session's L0 readiness cannot change another session's source selection.
5. Data callbacks from an old `loadGeneration` cannot update the active chart.
6. The chart is created once per selected noon-day and survives tier changes.
7. Only completed sessions with final metadata and clock drift are included.
8. Sessions are expected not to overlap; if malformed metadata overlaps, the later/rightmost session wins in the overlap.

### 5.2 Noon-day manifest

Extend `/api/sessions?date=` or add `/api/noonday/manifest?date=`. One response should provide sessions sorted by `start_epoch_ms` and include enough data to avoid one HEAD request per file:

```json
{
  "date": "20260719",
  "sessions": [
    {
      "id": "20260719_223000",
      "state": "completed",
      "start_epoch_ms": 1784464200000,
      "end_epoch_ms": 1784493000000,
      "clock_drift_ms": -840,
      "clock_drift_valid": true,
      "brp_samples": 720000,
      "brp_mm_samples": 28800,
      "brp_bytes": 2880028,
      "brp_mm_bytes": 230428,
      "etag_brp": "...",
      "etag_brp_mm": "..."
    }
  ]
}
```

The endpoint must include only sessions whose completion metadata exists, whose state is completed, and whose final clock drift query has completed. Completion metadata should always serialize `clock_drift_ms` (including zero) plus an explicit `clock_drift_valid` boolean so unavailable drift cannot be confused with a legitimate zero. Active, growing, interrupted, or incomplete sessions are excluded from this dashboard architecture. Current SNT headers and `session.json` start/end values are already written in the ESP's NTP epoch domain. Use `start_epoch_ms` directly for graph placement; do not add `clock_drift_ms`, which records `NTP - AS11` for device-clock/EDF reconciliation and would double-shift dashboard timestamps if applied here. Compute each completed session's BRP sample end as `start_epoch_ms + (sample_count - 1) * 40 ms` and validate it against `end_epoch_ms`; do not rely on directory iteration order. Sort by `start_epoch_ms`. The noon-day full range is the first session start through the last BRP sample.

Sessions must not overlap in normal operation. If malformed metadata nevertheless overlaps, trim the earlier session's rendered and tooltip-visible interval at the later session's start so the rightmost/later session takes precedence.

The current fixed 8 KB JSON allocation should be replaced with bounded streaming or a dynamically sized response so a noon-day with many sessions cannot be silently truncated.

### 5.3 Initial chart creation

After the manifest arrives:

1. Abort the old controller and increment the generation.
2. Sort and validate sessions.
3. Set `fullRange` and initial `viewport` once.
4. Create the chart once with the fixed full X scale and empty data.
5. Show a lightweight status such as `Loading overview 0/N` without blocking navigation.
6. Start the L1 scheduler.

This lets the UI, controls, and time extent settle before graph bytes arrive. No later data arrival changes the chart dimensions or X scale.

### 5.4 L1 overview loading

Fetch all L1 files before beginning L0. Because the manifest fixes the full range first, progressively paint each validated L1 session into its final horizontal location as soon as it arrives. This is the selected UX; there is no atomic wait for all L1 files. Because the server is synchronous, use one active graph-file transfer initially. Benchmark a concurrency of two only after the single-stream design is measured; do not equate queued browser requests with throughput.

For perceived speed:

- choose a deterministic order, normally chronological;
- update progress after every completed L1 session;
- progressively render completed sessions into the already fixed full range;
- throttle chart updates to one animation frame;
- preserve blank gaps for sessions not yet loaded;
- do not wait for all L1 files before showing the first useful waveform;
- consider the overview phase complete only when every session is ready or has a recorded failure.

If an L1 file is absent or invalid, mark only that session failed and schedule its L0 as a fallback after the remaining L1 files. One bad session must not prevent the noon-day from displaying.

### 5.5 L0 background loading

After the L1 phase settles, stream complete L0 files one session at a time. Reprioritize pending sessions when the viewport changes:

1. sessions intersecting the viewport;
2. sessions adjacent to the viewport;
3. remaining sessions chronologically.

Do not abort a nearly complete transfer merely because the viewport moved. A simple policy is to finish the active file and reorder the pending queue.

As a full L0 file completes, retain its binary/typed representation in browser memory and promote it to the browser HTTP cache if immutable. Schedule a viewport render only if that session intersects the viewport and L0 changes the selected representation at the current resolution.

### 5.6 Optional progressive L0 parsing

A single `fetch()` response can be consumed with `ReadableStream.getReader()` instead of waiting for `arrayBuffer()`:

1. Parse and validate the fixed header as soon as enough bytes arrive.
2. Allocate the destination typed buffer from `sample_count` and record size.
3. Preserve up to `record_size - 1` carry bytes between network chunks.
4. Copy only complete records into the destination.
5. Track `loadedSampleCount` as a contiguous validated prefix.
6. At most every 100 ms or animation frame, schedule a render if the loaded prefix intersects the viewport.
7. Mark the file ready only when expected byte count, header count, and received count agree.

This can show L0 detail before the complete file arrives when the user is viewing the early part of a session. It does not help a later viewport until those sequential bytes arrive. Implement only after the simpler whole-stream state machine is correct; do not combine initial correctness work with frequent partial-buffer reallocations.

### 5.7 Per-session storage and zero-copy parsing

Avoid expanding an eight-hour file into ordinary JavaScript Number arrays. Keep the downloaded `ArrayBuffer` and use typed views over interleaved records where alignment permits. Generate timestamps only for points included in the current render output.

Suggested session representations:

```text
L1:
  startMs
  count
  Int16Array records: [flowMin, flowMax, pressMin, pressMax, ...]

L0:
  startMs
  count
  Int16Array records: [flow, pressure, ...]
```

This reduces mobile memory pressure and avoids a full-day timestamp array of hundreds of thousands of 64-bit JavaScript Numbers.

### 5.8 Viewport renderer

The renderer receives immutable inputs `(sessions, viewport, plotWidth)` and returns temporary uPlot arrays. It must not mutate controller state.

For each session:

1. Skip it if it does not intersect the viewport.
2. Convert viewport bounds directly to sample indices from NTP-domain `startMs` and sample period.
3. Choose a source/representation using visible resolution.
4. Aggregate that session independently.
5. Append finite timestamps and values.
6. Append a finite timestamp with null Y before the next session so no line can cross the boundary.

Target roughly one horizontal bucket per CSS pixel, with a configurable upper bound such as two output records per pixel. Device-pixel ratio should affect drawing sharpness, not multiply the semantic data target unnecessarily.

#### 5.8.1 Source selection

Use a pixel-based rule rather than an arbitrary four-hour threshold:

- **Raw L0 polyline:** L0 is available and visible raw samples are at or below approximately two samples per CSS pixel, subject to a safe point cap.
- **L0-derived compressed stroke:** L0 is available, raw samples exceed the point budget, and the viewport bucket width is below one second.
- **L1-derived compressed stroke:** viewport bucket width is at least one second, or L0 is not available.

For a 900-pixel chart, L1 has enough temporal resolution for a viewport around 15 minutes or wider. Four hours is about 16 seconds per pixel, so plotting raw L0 cannot reveal additional screen-resolvable detail. Keeping L1 at that width is correct and much cheaper. L0 should still be cached so zooming immediately selects it.

The exact crossover should be derived from `viewportDuration / plotWidth`, not hard-coded in hours.

#### 5.8.2 Single-line compressed rendering and mathematically consistent transition

The overview must not be a filled band and must not show two boundary lines. Use a custom thin-stroke renderer: aggregate each horizontal pixel bucket to min/max, draw a narrow vertical excursion for that bucket, and connect adjacent valid buckets with a single subtle center/continuity stroke. At dense overview scales this reads as one compressed waveform, matching the supplied SleepHQ reference. Stroke width and color remain the same as the raw line; no area fill is used.

Corrected v2 L1 is generated from exact L0 min/max values in each one-second bucket. If both tiers are aggregated into the same viewport-anchored pixel buckets, their extrema should agree except at bucket boundaries, the temporal-order information absent from L1, and missing-data edges. Historical v1 L1 can retain a false `-1` maximum in all-negative buckets and should not remain the preferred overview source indefinitely. Add a low-priority, atomic sidecar repair path that regenerates corrected v2 L1 from historical L0, writes a temporary file, validates it, renames it over the old sidecar, and changes its ETag. Prioritize the selected noon-day; migrate other completed sessions opportunistically rather than blocking boot. Until repaired L1 is available, display legacy L1 immediately and replace it in place when the corrected sidecar arrives. This preserves fast first paint and converges to a correct stable overview. A future L1 format may preserve extrema order, but it is not required for the initial custom renderer.

At close zoom, the display changes from a compressed thin stroke to a raw polyline because the user has requested resolution that L1 cannot contain. The viewport must remain fixed; only waveform detail changes.

#### 5.8.3 Stable bucket anchoring

Anchor buckets to absolute time or viewport pixel boundaries in a deterministic way. Do not anchor them to array index zero after each source replacement. Given the same viewport and width, rerendering must produce identical X coordinates regardless of which sessions finished loading.

#### 5.8.4 Missing data and session gaps

- Convert invalid samples to null, never zero.
- If a min/max bucket contains no valid samples, output null.
- Reset bucket state at every session boundary.
- Insert a finite-X/null-Y break between sessions even when sessions are only one sample period apart.
- Never globally subsample an array containing break markers.
- Validate overlapping session times; if overlap is possible, either use separate uPlot series per overlapping session or define an explicit priority instead of producing non-monotonic X values.

### 5.9 Seamless data update protocol

Every render update follows this sequence:

```text
savedViewport = controller.viewport
controller.suppressScaleEvents = true
chart.setData(renderedData, false)
chart.setScale("x", savedViewport)
controller.suppressScaleEvents = false
```

Additional requirements:

- Do not assign `dashXRange = null` on L0 arrival.
- Do not call `destroy()` or construct a new uPlot instance on tier changes.
- Do not derive `fullRange` from L0 after deriving it from L1.
- Ignore `setScale` hooks while applying programmatic restoration.
- Coalesce multiple chunk/session arrivals into one animation-frame render.
- If the user is actively dragging, defer the data swap until the gesture ends or render against the gesture's current viewport without changing it.
- Preserve cursor time and tooltip when possible; hide it only if the selected time becomes a gap.

## 6. Transport and server design

### 6.1 Recommended normal path

Use a single whole-file GET per session and tier:

```text
GET /api/session/file?date=YYYYMMDD&session=ID&type=brp_mm
GET /api/session/file?date=YYYYMMDD&session=ID&type=brp
```

The manifest supplies size and validation metadata, so the dashboard does not need a HEAD before each GET. The server should send:

- correct `Content-Length` for the whole or ranged response;
- `Accept-Ranges: bytes`;
- `ETag` or Last-Modified;
- immutable `Cache-Control` for completed sessions;
- `no-store` for active/growing files if that endpoint is used outside this completed-session dashboard;
- correct `Content-Range` and 206 status for valid ranges;
- 416 for invalid or unsatisfiable ranges.

Use sequential SD reads into a reusable PSRAM buffer. Benchmark 16 KB, 32 KB, and 64 KB buffers. The internal dashboard uses plain HTTP, so buffer size should be chosen from measured SD, TCP, and browser throughput rather than TLS record size. Avoid loading an entire multi-megabyte L0 file into ESP memory.

Use fixed-length response framing where supported rather than transfer-chunked framing when the file size is known. If ESP-IDF's public response API makes fixed-length streaming awkward, measure chunked overhead before introducing low-level socket sends; correctness and connection cleanup take priority.

### 6.2 Range policy

Keep byte-range support, but do not split every dashboard download into four or eight simultaneous ranges by default.

Range remains useful for:

- interrupted-download recovery;
- browser/proxy interoperability;
- diagnostics;
- fetching a header or tail;
- a future viewport-priority request by byte offset;
- very large future signals where whole-file retention is inappropriate.

A timestamp-range JSON graph API is not required for the proposed whole-file client architecture. The existing `/api/session/graph` endpoint can be deprecated after all consumers are audited, but should not be removed in the same change.

### 6.3 Why one stream is expected to be fastest

For the current server:

- one handler runs at a time;
- all requests read the same SD card;
- one Wi-Fi link sends all bytes;
- one sequential file read minimizes FATFS and SD seeks;
- one request minimizes HTTP headers and scheduler overhead;
- fewer sockets leave room for portal status and control requests.

Only reintroduce multi-range dashboard downloads if an instrumented A/B test on the target device shows a repeatable throughput improvement without increasing failures or UI latency.

### 6.4 Browser cache

Completed files are immutable. Use a stable ETag derived from session ID, type, size, and final sample count, and return a long-lived cache policy. The browser then avoids re-downloading data when switching dates or revisiting a session. Keep a bounded in-memory parsed-data LRU above the HTTP cache; evict old parsed arrays before risking mobile memory pressure.

Suggested policy:

- parsed noon-day cache: most recent one or two days, bounded by bytes;
- HTTP cache: immutable completed files, validated by ETag;
- active session: no-store and refresh from a separate active-session design.

### 6.5 SDMMC verification

The code requests the fastest standard ESP32-S3 SDMMC mode used here: four-bit bus and high-speed maximum. Verification must report what was actually negotiated, not merely what was requested.

Add one boot diagnostic containing:

- card identity/type;
- requested and actual clock;
- requested and actual bus width;
- whether the one-bit fallback path ran;
- card capacity;
- sequential read benchmark result in MB/s, either behind a diagnostic setting or a one-time development command.

Do not claim the SD card is the bottleneck until direct sequential read speed is compared with end-to-end HTTP payload speed.

### 6.6 HTTP and network instrumentation

Add development-only timing for:

```text
manifest response time
file open/seek time
SD fread bytes and elapsed time
HTTP send bytes and elapsed time
client time to first byte
client sustained payload MB/s
header parse time
first overview paint
all-overview-ready time
first L0 detail paint
all-L0-ready time
```

Log one summary per file, not one line per chunk. Include session ID, tier, expected bytes, received bytes, cache hit/miss, and failure reason. Add Wi-Fi RSSI, protocol/channel, current power-save policy, and available TCP retransmission counters where the ESP-IDF/lwIP APIs expose them.

Also test with the log viewer closed and open. The current viewer uses an asynchronously pushed WebSocket and should not block the HTTP worker, but it consumes a socket and adds outbound traffic. Explicitly close any displaced WebSocket when enforcing the single-viewer policy, stop reconnecting when the Logs tab is intentionally closed, and verify repeated viewers cannot leak sockets. Long synchronous log-history/download requests remain separate finite handlers and should be included in contention tests.

## 7. Interaction and navigation UX

### 7.1 Unified pointer state machine

Replace separate mouse/touch code with Pointer Events where supported. One state machine should own `pointerdown`, `pointermove`, `pointerup`, and `pointercancel`.

Set an appropriate `touch-action` on the plot overlay while a chart gesture is active. Do not use passive touch listeners when `preventDefault()` is required. Preserve normal vertical page scrolling outside the chart.

### 7.2 Default zoom mode

Desktop:

- left-button drag shows a horizontal selection rectangle;
- release zooms to the selected time range;
- a movement threshold distinguishes click from drag;
- left double-click zooms in around the pointer time;
- mouse wheel zooms around the pointer time;
- single right-click does nothing;
- suppress the browser context menu only over the plot;
- two right-button clicks within the double-click interval zoom out around the pointer time.

Do not use `dblclick` alone for right-button detection because browser support for secondary-button double-click is inconsistent. Track right-button releases/context-menu events with time and distance thresholds; the first click is intentionally inert.

Mobile:

- one-finger horizontal drag shows a selection and zooms on release;
- a tap moves/shows the inspection cursor;
- optional two-finger pinch zooms continuously around the pinch center;
- gesture cancellation restores the pre-gesture viewport.

### 7.3 Pan mode

When pan mode is enabled:

- desktop left drag pans;
- mobile one-finger drag pans;
- pointer movement updates scale without rebuilding data on every pixel if the existing render buffer covers the range;
- renderer refresh is throttled to animation frames;
- on gesture end, build the exact new viewport representation.

The pan button must visibly show active state and expose `aria-pressed`.

### 7.4 Buttons

- Zoom in/out operate around viewport center unless a cursor time is active, in which case use the cursor as center.
- Reset restores exactly `fullRange` and does not destroy the chart.
- Buttons remain usable while L0 downloads.
- No control resets data-loading progress.

### 7.5 Tooltip and cursor

Add a dashboard-specific tooltip rather than reusing the memory-chart tooltip, which formats values as bytes and assumes second-based timestamps.

Behavior modeled on the supplied screenshot:

- thin vertical dashed crosshair at the selected time;
- compact floating label near the curve, kept inside plot bounds;
- `Breathing: 25.8 L/min` for a raw L0 point;
- time shown in the axis/cursor label with seconds at close zoom;
- hide the value in session or missing-data gaps;
- theme-aware colors with sufficient contrast;
- pointer-events disabled on the tooltip itself;
- mouse move updates continuously;
- touch tap selects and pins; tap outside or a new gesture clears it.

For compressed-stroke data, do not invent an exact value. Display `Breathing: -18.6 to 31.2 L/min`, or explicitly label a representative value as approximate. Once raw L0 is selected at the same cursor time, replace the range with the exact nearest sample without moving the cursor.

Use a nearest-valid-point search limited to the current session so the cursor cannot jump across a session gap.

## 8. Visual rendering

- Chart height remains 240 CSS pixels.
- Keep the fixed flow scale unless a later specification changes it.
- Y-axis has no `L/min` title; unit appears in tooltip.
- Axis labels remain compact and theme-aware.
- Horizontal and vertical grid lines use the same subtle stroke, width, and dash pattern.
- Cursor/crosshair must be visually distinct from the background grid.
- Compressed L1/L0 rendering is a single thin stroke with no filled band and no paired boundary lines.
- Use uPlot's high-DPI canvas support; do not increase semantic point count solely because device pixel ratio is high.
- Resize updates chart dimensions and rerenders the same viewport; it must not revert to a stale hard-coded height or reset X scale.

## 9. Failure handling

- Abort old requests immediately on date change.
- Ignore every response whose generation does not match the active controller.
- Validate magic, version, tier, channel count, sample size, expected length, and sample count.
- Reject incomplete multi-part/range results rather than concatenating holes.
- Keep successfully loaded sessions visible if another session fails.
- Show per-phase status without replacing the chart with an error if partial data exists.
- Retry a failed immutable file once using a single full GET; avoid retry storms.
- If L1 fails but L0 succeeds, derive the overview from L0 for that session.
- If L0 fails, retain L1 and mark detail unavailable for that session.
- If manifest times overlap or are invalid, log the affected session and preserve monotonic rendering rather than connecting it incorrectly.

## 10. Implementation plan

### Phase 0: Instrument and establish baseline

1. Add client performance marks and one server timing summary per file.
2. Record target-device results for one, three, and five sessions.
3. Record direct SD sequential-read throughput.
4. Record actual SD width/frequency and one-bit fallback.
5. Record Wi-Fi RSSI, protocol/channel, power-save policy, and ESP-to-client HTTP throughput.
6. Compare one GET with current 4-range behavior using identical files.
7. A/B test normal Wi-Fi power saving versus `WIFI_PS_NONE` during graph transfer.
8. Test with status polling and the log WebSocket enabled/disabled.

Deliverable: a table of time to first overview, all overview, first detail, all detail, payload MB/s, and failures.

### Phase 1: Correct data and viewport model

1. Add load generation and cancellation.
2. Return sorted, completed-only manifest metadata, clock drift, and file sizes.
3. Exclude active, interrupted, and incomplete sessions.
4. Store sessions separately in typed arrays.
5. Implement conservative SNT v1 `-1` compatibility and SNT v2 `INT16_MIN` missing-data handling.
6. Fix L1 accumulator initialization and add atomic, low-priority v1-to-v2 L1 sidecar regeneration from historical L0.
7. Implement per-session viewport slicing, overlap precedence, and gap insertion.
8. Create one chart per selected day.
9. Preserve viewport with `setData(data, false)` and guarded scale hooks.
10. Fix resize to retain 240-pixel height and current range.

This phase should resolve connected sessions, apparent failure to use L0 while zoomed, and visible range movement.

### Phase 2: Deterministic multi-resolution renderer

1. Preserve L1 min/max pairs.
2. Implement absolute-time/pixel-anchored min/max aggregation.
3. Implement the custom no-fill, single compressed-stroke renderer.
4. Implement raw L0 polyline rendering at close resolution.
5. Add per-session source selection.
6. Add development diagnostics showing source and output point count per session.
7. Verify L1 and L0 bucket extrema agree for the same viewport buckets.

### Phase 3: Loading scheduler and transport simplification

1. Remove HEAD from the normal dashboard path by using manifest sizes.
2. Replace default multi-range fetching with one full GET.
3. Load L1 sessions first, updating the fixed chart progressively.
4. Start L0 only after the L1 phase settles.
5. Load L0 sequentially with viewport-aware pending priority.
6. Add immutable cache headers and client parsed-data LRU.
7. Retain standards-compliant Range support without using it by default.
8. Add an explicit `/favicon.ico` response or embedded favicon declaration so browsers do not create repeated avoidable 404 requests.
9. Re-run the Phase 0 benchmark before changing SD buffers, Wi-Fi buffers, power-save policy, or socket counts.
10. Close displaced log WebSockets and verify repeated log viewers cannot consume stale socket slots.

### Phase 4: Navigation and tooltip

1. Implement unified Pointer Events.
2. Add touch drag-to-zoom and pan.
3. Correct left double-click zoom-in.
4. Implement inert single right-click and right-double-click zoom-out.
5. Add pinch zoom if testing shows it does not conflict with page navigation.
6. Add dashboard breathing tooltip and crosshair.
7. Test mouse, trackpad, Android Chrome, and iOS Safari behavior.

### Phase 5: Optional progressive stream rendering

Only after Phases 1-4 are stable:

1. Parse L0 response streams incrementally.
2. Preallocate typed storage from the validated header.
3. Render validated prefixes at a throttled rate.
4. Measure whether first-detail latency improves enough to justify complexity.
5. If it does not materially improve UX, keep whole-response parsing.

## 11. Acceptance criteria

### Initial load

- [ ] Selecting a noon-day immediately cancels the previous day's work.
- [ ] Sessions are sorted chronologically regardless of filesystem order.
- [ ] Initial X range is first BRP sample of the first session through last BRP sample of the last session.
- [ ] The chart's initial full range does not change when L1 or L0 arrives.
- [ ] Each L1 session appears progressively in its final fixed horizontal location as soon as it is validated.
- [ ] Progressive L1 arrival never moves or resizes the time axis.
- [ ] L0 requests do not begin until all L1 sessions are ready or failed.
- [ ] Only completed sessions with final metadata and clock drift are included.
- [ ] Sessions are positioned directly from NTP-domain `start_epoch_ms`; `clock_drift_ms` is retained as metadata but is not added to graph timestamps.
- [ ] Active, interrupted, and incomplete sessions are excluded.
- [ ] One failed session does not hide successful sessions.

### Session boundaries and data integrity

- [ ] No line connects any two sessions at any zoom level.
- [ ] Decimation cannot remove a session separator.
- [ ] SNT v1 definite two-channel `-1` gap records render as gaps without dropping every legitimate isolated flow value of `-1`; remaining single-channel ambiguity is documented.
- [ ] Historical defective v1 L1 sidecars can be atomically regenerated as corrected v2 sidecars without blocking boot.
- [ ] SNT v2 `INT16_MIN` missing markers render as gaps without colliding with valid BRP flow.
- [ ] L1 maxima are initialized correctly for all-negative buckets.
- [ ] Malformed overlapping sessions give precedence to the later/rightmost session.
- [ ] Incomplete HTTP responses cannot be promoted to ready data.
- [ ] File header and byte-count validation failures are visible in diagnostics.

### Seamless resolution change

- [ ] L0 arrival never resets to the full noon-day unless the user was already at full range.
- [ ] L0 arrival never changes the viewport center or duration by even one sample period.
- [ ] The uPlot instance is not destroyed on tier changes.
- [ ] A close viewport switches to raw L0 when available.
- [ ] A wide viewport may remain L1 when L0 cannot improve pixel-level resolution.
- [ ] For identical pixel buckets, L1-derived and L0-derived extrema match within stored quantization and documented boundary behavior.
- [ ] Compressed views look like one thin waveform stroke, with no filled band and no paired boundary lines.
- [ ] Tier/source diagnostics confirm the selected representation during tests.

### Navigation

- [ ] Desktop left drag zooms to selection.
- [ ] Mobile one-finger drag zooms to selection in zoom mode.
- [ ] Pan mode works with mouse and touch.
- [ ] Left double-click zooms in around pointer position.
- [ ] Right double-click zooms out around pointer position.
- [ ] Single right-click has no graph action.
- [ ] Wheel, buttons, and reset retain correct bounds.
- [ ] Navigation remains usable while background L0 downloads.

### Tooltip

- [ ] Mouse hover displays breathing value and time with a visible crosshair.
- [ ] Raw L0 tooltip shows an exact value and unit.
- [ ] Overview tooltip shows an honest min/max range or explicitly approximate value.
- [ ] Tooltip does not jump across session gaps.
- [ ] Touch can select and dismiss the tooltip.
- [ ] Tooltip and cursor are readable in day and night themes.

### Performance

- [ ] Baseline and final metrics are captured on the same card, device, browser, and Wi-Fi conditions.
- [ ] Single-stream versus multi-range throughput is measured rather than assumed.
- [ ] Final design has no socket-exhaustion failures in one-, three-, and five-session noon-days.
- [ ] Revisiting an immutable cached day generates no file-body transfer when browser cache remains valid.
- [ ] Rendering and interaction stay responsive on a representative mobile device with an eight-hour L0 file cached.
- [ ] Server logs report actual SD bus mode and measured sequential read throughput.
- [ ] Wi-Fi measurements record RSSI, power-save policy, and sustained ESP-to-client payload throughput.
- [ ] Keeping one log WebSocket open does not materially reduce graph throughput or block graph requests.
- [ ] Replacing/reconnecting log viewers does not leak HTTP socket slots.
- [ ] The portal generates no repeated favicon 404 requests.

## 12. Test matrix

| Scenario | Sessions | View | Expected source | Key checks |
|---|---:|---|---|---|
| Short day | 1 | full | L1 compressed stroke | first paint, fixed extent |
| Typical day | 2-3 | full | L1 compressed stroke | progressive fill, visible gaps, no links |
| Fragmented day | 5+ | full | L1 compressed stroke | ordering, JSON completeness, sockets |
| Close zoom | any | 10-40 seconds | raw L0 | every sample, tooltip exact |
| Medium zoom | any | minutes | L0 min/max or L1 by pixels | stable transition |
| Wide zoom | any | hours | L1 compressed stroke | low CPU, no false expectation of raw detail |
| L1 missing | mixed | full | L0 fallback for one session | partial success |
| L0 missing | mixed | close | L1 retained | detail-unavailable status |
| Missing BLE records | 1 | close | raw L0 with gaps | no zero substitution |
| Date changed mid-load | any | any | new day only | abort/stale response isolation |
| Mobile portrait | any | all | resolution-dependent | drag zoom, pan, tooltip |
| Cache revisit | any | all | cached | zero repeated body bytes |

## 13. Recommendation on the open full-day L0 question

Do not use a fixed rule such as "L0 below four hours." Use screen resolution.

For wide views, L1 contains one-second min/max records and is already much denser than the display. Reaggregating L0 would consume CPU and memory without adding visible information. For close views, L0 is essential. Therefore:

1. always download/cache L0 in the background after L1 so future zoom is immediate;
2. select L0 for the visible viewport only when its sub-second information can reach the screen;
3. keep L1 for wide overviews even after L0 is available;
4. use matching pixel-bucket aggregation so the transition is stable.

This gives the best combination of perceived speed, waveform fidelity, mobile performance, and seamless behavior.

## 14. Security and privacy

Graph files contain personal medical data. Browser caching improves performance but leaves local copies on the viewing device. The portal should clearly remain local-network functionality, avoid shared/public cache semantics if authenticated multi-user deployment is introduced, and never include real patient files in tests or fixtures. ETags must not expose patient identity. Development timing logs should contain session IDs and byte counts only, not sample values.

## 15. Resolved decisions

1. L1 displays progressively because the completed-session manifest fixes the full range before L1 bytes arrive.
2. The internal portal is plain HTTP. HTTPS/TLS is outside scope.
3. ESP-to-client throughput remains unmeasured; benchmark before tuning SD, Wi-Fi, buffers, power saving, or sockets.
4. Live logs already use an asynchronously pushed WebSocket, not SSE. Keep it, but harden displaced-client closure and socket lifecycle.
5. Compressed overview rendering is a custom single thin stroke with no fill and no paired boundary lines.
6. Sessions do not overlap in valid data. If malformed data overlaps, the later/rightmost session takes precedence.
7. Only completed sessions with final metadata and clock drift are displayed. Active/growing sessions are excluded.
8. SNT v1 receives conservative `-1` compatibility and historical L1 sidecar repair. SNT v2 uses `INT16_MIN` and version-aware parsing.
9. Add favicon handling to avoid unnecessary browser 404 traffic.

## 16. Remaining measurements

1. Direct sequential SD read throughput on the target card.
2. Sustained ESP-to-client HTTP throughput, RSSI, channel/protocol, and power-save impact.
3. Single whole GET versus four queued Range requests under identical conditions.
4. Graph throughput with the log WebSocket disconnected and connected.
5. Mobile CPU/memory behavior for cached eight-hour L0 files.

## 17. Changelog

- 2026-07-20: Initial architecture proposal based on the current dashboard, SNT writer, raw file endpoint, HTTP server configuration, SDMMC setup, and supplied SleepHQ interaction reference.
- 2026-07-20: Resolved product decisions: progressive fixed-range L1 display, HTTP-only portal, completed sessions only, later-session overlap precedence, custom no-fill compressed stroke, existing WebSocket log architecture, versioned missing-data sentinel, Wi-Fi measurement plan, and favicon handling.
