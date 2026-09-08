# SomnoTrace — Data Format & Visualization Architecture (v5)

v5: Eliminates the 4 MB PSRAM session buffer. All data is served from SD
with a small recent-buffer for the not-yet-flushed tail. Removes the 10-hour
session length limit and frees ~4 MB of PSRAM. One universal read path for
both active and historical sessions.

---

## 1. Incoming Data

| Stream | Signals | Rate | 8h raw binary |
|--------|---------|------|---------------|
| BRP | `PatientFlow`, `MaskPressure` | 25 Hz | 2,812 KB |
| SA2 | `HeartRate`, `SpO2` | 1 Hz | 113 KB |
| PLD | 12 analytics signals | 0.5 Hz | 338 KB |
| Events | Apneas, status, etc. | discrete | ~20 KB |
| **Total** | | | **~3.2 MB** |

JSON parsed on BLE arrival and discarded. Never stored.

---

## 2. PSRAM: Small Ring + Recent Buffer

No full-session PSRAM arrays. Data flows directly from BLE → small ring
buffers → SD flush every 60 s. A recent buffer holds the last flush window
(~60 s) for live HTTP serving of data not yet on SD.

```
PSRAM budget:
  Ring buffer (30s LCD):            3 KB
  Recent buffer (~60s, all streams): ~50 KB
    BRP: 60s × 25 Hz × 2 ch × 2 B =  6 KB
    SA2: 60s × 1 Hz × 2 ch × 2 B  =  240 B
    PLD: 60s × 0.5 Hz × 12 ch × 2 B = 720 B
    Events:                          ~2 KB
    (ring-buffered, overwritten each flush)
  Flush buffer (static):           6 KB
  ──────────────────────────────────────
  Total:                           ~59 KB
  PSRAM remaining:              ~8,133 KB (~7.9 MB)  ✓
```

**No session length limit.** Data accumulates on SD indefinitely; the
recent buffer only holds the tail between flushes.

### Why not a full-session PSRAM buffer?

v4 allocated 4 MB for 10 hours of columnar arrays. This was an
over-optimisation:

- **Latency gain was negligible:** PSRAM read = 0 ms vs SD read = 0.2–12 ms.
  Wi-Fi round-trip adds 10–40 ms either way, so total HTTP latency is
  15–50 ms (PSRAM) vs 15–62 ms (SD) — imperceptible to users.
- **10-hour hard limit was dangerous:** sleep sessions can exceed 10 h.
  v4 truncated data beyond 10 h despite it being safely on SD.
- **Two read paths added complexity:** separate code for PSRAM (active)
  vs SD (historical). v5 has one universal SD read path.
- **Half the PSRAM was wasted:** 4 MB consumed by arrays that duplicated
data already flushed to SD.

### Gap Handling

BLE will drop packets. Mark missing samples with `INT16_MIN` (`-32768`):

- Ring/recent buffers: sentinel inserted when expected sample is missing
- L0 files: sentinel preserved (maintains gap positions)
- MinMax: skip sentinels; all-sentinel bucket → sentinel pair
- EDF: map sentinel to EDF missing-data value
- Browser: render as gaps (discontinuous line), not as zero

---

## 3. SD Card: Durable Storage + Historical Access

### File Layout

```
/somnotrace/sessions/
  20260624_2215/
    session.json           ← metadata (start, end, clock drift, signal list)
    brp.snt                ← L0: Flow + MaskPressure interleaved, 25 Hz
    brp_mm.snt             ← L1: 1-second MinMax (BRP only)
    sa2.snt                ← L0: HeartRate + SpO2 interleaved, 1 Hz
    pld.snt                ← L0: 12 PLD channels interleaved, 0.5 Hz
    events.snt             ← therapy events
    BRP.edf                ← generated post-session
    SA2.edf
    PLD.edf
```

**Per-group files, not per-channel.** 5 data files = 5 `f_sync` calls per
flush (~15 ms) vs 16+ for per-channel (~48 ms).

### File Header (32 bytes)

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 0x534E5442 "SNTB"                  */
    uint8_t  version;          /* format version (1)                  */
    uint8_t  tier;             /* 0 = L0 raw, 1 = L1 MinMax          */
    uint8_t  n_channels;      /* channels per record                 */
    uint8_t  sample_bytes;    /* 2 (int16)                           */
    uint16_t sample_hz_x10;   /* rate × 10 (250 = 25 Hz)            */
    uint16_t reserved;
    int64_t  start_epoch_ms;  /* session start (AS11 device clock)   */
    uint32_t sample_count;    /* records written (updated each flush) */
    uint32_t reserved2;
} sntb_header_t;              /* 32 bytes */
```

Seek to any time: `offset = 32 + (time_ms / sample_period_ms) * n_channels * 2`

### Record Layout: Interleaved

```
BRP record (4 bytes): [flow:int16] [press:int16]
SA2 record (4 bytes): [hr:int16]   [spo2:int16]
PLD record (24 bytes): [ch0:int16] ... [ch11:int16]
```

### L1 MinMax — BRP Only

SA2 (1 Hz) and PLD (0.5 Hz) have ≤1 sample per 1-second bucket — L1 is
meaningless for them. Their L0 files are small enough to read directly
(113 KB and 338 KB for 8h).

L1 record: `[flow_min, flow_max, press_min, press_max]` — 8 bytes/second.
225 KB for 8 hours.

---

## 4. Periodic SD Flush (Every 60 Seconds)

| File | 60s Size | 8h Total |
|------|----------|----------|
| `brp.snt` | 6.0 KB | 2,812 KB |
| `brp_mm.snt` | 480 B | 225 KB |
| `sa2.snt` | 240 B | 113 KB |
| `pld.snt` | 720 B | 338 KB |
| `events.snt` | variable | ~20 KB |
| **Total** | **~7.5 KB** | **~3.5 MB** |

Write: 0.35 ms. Sync: ~15 ms (5 × `f_sync`). Total: **~15 ms** = 0.025% of
the 60-second interval. BLE on Core 0; flush on Core 1.

### Flush Buffer: Static, Not VLA

The interleave buffer is allocated once at boot — not on the stack:

```c
/* Allocated once in PSRAM at boot (included in budget above) */
static int16_t s_flush_buf[1500 * 2];  /* max 60s × 25 Hz × 2 ch = 6 KB */

void session_flush(recent_buffer_t *rb, flush_state_t *fs)
{
    uint32_t delta = rb->brp_count - fs->flushed_brp;
    if (delta == 0) return;

    /* L0: interleave from recent buffer into static buffer, append to SD */
    for (uint32_t i = 0; i < delta; i++) {
        uint32_t si = (fs->flushed_brp + i) % RECENT_BRP_CAP;
        s_flush_buf[i * 2 + 0] = rb->brp_flow[si];
        s_flush_buf[i * 2 + 1] = rb->brp_press[si];
    }
    f_write(fs->fd_brp, s_flush_buf, delta * 4, &bw);

    /* L1: compute 1-second MinMax, skip sentinels */
    uint32_t n_sec = delta / 25;
    for (uint32_t sec = 0; sec < n_sec; sec++) {
        uint32_t base = (fs->flushed_brp + sec * 25) % RECENT_BRP_CAP;
        int16_t fmn = INT16_MAX, fmx = INT16_MIN;
        int16_t pmn = INT16_MAX, pmx = INT16_MIN;
        for (int j = 0; j < 25; j++) {
            int16_t fv = rb->brp_flow[(base + j) % RECENT_BRP_CAP];
            int16_t pv = rb->brp_press[(base + j) % RECENT_BRP_CAP];
            if (fv != INT16_MIN) {
                if (fv < fmn) fmn = fv; if (fv > fmx) fmx = fv;
            }
            if (pv != INT16_MIN) {
                if (pv < pmn) pmn = pv; if (pv > pmx) pmx = pv;
            }
        }
        if (fmn == INT16_MAX) { fmn = fmx = INT16_MIN; }
        if (pmn == INT16_MAX) { pmn = pmx = INT16_MIN; }
        int16_t mm[4] = { fmn, fmx, pmn, pmx };
        f_write(fs->fd_brp_mm, mm, 8, &bw);
    }

    /* SA2, PLD: interleave and append (similar pattern) */
    /* ... */

    /* Update headers, sync all files */
    update_header_sample_count(fs->fd_brp, rb->brp_count);
    f_sync(fs->fd_brp);
    f_sync(fs->fd_brp_mm);
    f_sync(fs->fd_sa2);
    f_sync(fs->fd_pld);
    f_sync(fs->fd_events);

    fs->flushed_brp = rb->brp_count;
}
```

### Recent Buffer (Ring-Buffered)

The recent buffer is a set of per-stream ring buffers holding the last ~60 s
of data — the window between flushes. It is the only copy of data not yet
on SD. HTTP requests for the active session read from SD for flushed data
and from the recent buffer for the tail.

```c
typedef struct {
    int16_t brp_flow[RECENT_BRP_CAP];   /* 1500 samples = 60s @ 25 Hz */
    int16_t brp_press[RECENT_BRP_CAP];
    int16_t sa2_hr[RECENT_SA2_CAP];     /* 60 samples = 60s @ 1 Hz */
    int16_t sa2_spo2[RECENT_SA2_CAP];
    int16_t pld[RECENT_PLD_CAP][12];    /* 30 samples = 60s @ 0.5 Hz */
    uint32_t brp_count, sa2_count, pld_count;  /* total counts (monotonic) */
} recent_buffer_t;
```

Counts are monotonic (never wrap). Array indices are `count % CAP`. This
lets the flush function and HTTP handler determine which data is in the
recent buffer vs already on SD.

### Crash Recovery

1. Scan for incomplete sessions (`session.json` missing `end_time`)
2. Read `.snt` headers → `sample_count` = last committed count
3. Truncate to `32 + sample_count * record_size`
4. Mark as interrupted, generate EDF from what exists

Maximum data loss: **60 seconds**.

---

## 5. HTTP API

### Endpoints

| Endpoint | Method | Response | Purpose |
|----------|--------|----------|---------|
| `/` | GET | HTML | Single-page app (uPlot + UI) |
| `/api/sessions` | GET | JSON | List sessions on SD |
| `/api/session/:id` | GET | JSON | Session metadata |
| `/api/events/:id` | GET | Binary | Events for a session |
| `/api/data/:id/:signal` | GET | Binary | Signal data |

### Caching Semantics

Historical (completed) sessions are **immutable files on SD** — their data
never changes. The `Cache-Control` header tells the **browser** to reuse its
own cached copy. This uses **zero ESP32 RAM** and eliminates repeat requests
entirely:

| Session State | Cache-Control | Rationale |
|---------------|--------------|-----------|
| **Completed** | `public, max-age=86400` | Data is immutable; browser reuses |
| **Active** | `no-store` | Data is still growing; always fresh |

> [!NOTE]
> This is not ESP-side caching. No PSRAM or RAM is used. The browser
> simply doesn't re-request data it already has. For a user viewing the same
> session multiple times, panning back and forth, previously-fetched ranges
> are served from the browser's own disk cache — zero Wi-Fi traffic, zero
> ESP load.

For the **active session**, every request hits the ESP. Data older than the
last flush is read from SD; the most recent ~60 s (not yet flushed) is read
from the recent buffer. No caching, no stale data risk.

### Data Query

```
GET /api/data/20260624_2215/flow?from=0&to=28800&max=1000
```

### Tier Selection

```c
uint32_t range_samples = (to - from) * sample_hz;

if (range_samples <= max_points) {
    /* Raw L0, no downsampling — full waveform fidelity */
    serve_l0_raw(file, from, to, ch_idx);

} else if (range_samples / max_points < sample_hz) {
    /* Bucket < 1 second → L0 + on-demand MinMax */
    serve_l0_downsampled(file, from, to, max_points, ch_idx);

} else {
    /* Bucket ≥ 1 second → L1 MinMax re-bucket (BRP only) */
    serve_l1_rebucketed(mm_file, from, to, max_points, ch_idx);
}
```

### Full-Fidelity Waveform Viewing

Some users zoom to 10-second or shorter windows to inspect actual breathing
waveforms. This is handled naturally by the tier selection:

| Window | BRP Samples | Tier | What the User Sees |
|--------|------------|------|--------------------|
| 5 sec | 125 | L0 raw | Every sample, true waveform |
| 10 sec | 250 | L0 raw | Every sample, true waveform |
| 20 sec | 500 | L0 raw | Every sample, true waveform |
| 40 sec | 1,000 | L0 raw | Every sample (at max_points=1000) |
| 1 min | 1,500 | L0 + MinMax | Light downsampling, peaks preserved |
| 30 min | 45,000 | L1 re-bucket | Envelope view, peaks preserved |
| 8 hours | 720,000 | L1 re-bucket | Whole-night envelope |

At ≤40 seconds the user gets **every single sample** — no downsampling, no
MinMax, no information loss. The raw int16 values are sent directly. The
breathing waveform, pressure fluctuations, and any artifacts are visible
exactly as captured from the AS11.

### Binary Response Format

```
Content-Type: application/octet-stream
X-Signal-Scale: 0.01
X-Signal-Unit: L/min
X-Data-Tier: raw|minmax
X-Sentinel: -32768

Body (little-endian):
[t0:uint32]          ← epoch seconds of first point
[dt:uint16]          ← ms between points/buckets
[n:uint16]           ← number of points
[int16[n]]           ← if tier=raw
[int16[n*2]]         ← if tier=minmax (min,max pairs)
```

### End-to-End Latency

ESP-side data access is fast, but **Wi-Fi round-trip dominates**:

| Component | Time |
|-----------|------|
| Browser → ESP (Wi-Fi) | 5–20 ms |
| ESP reads + downsamples | 0.2–12 ms |
| ESP → Browser (Wi-Fi, ~4 KB) | 5–20 ms |
| Browser parse + uPlot render | 5–10 ms |
| **Total** | **15–62 ms** |

This is why additional pre-computed tiers beyond L1 don't help — shaving
10 ms off the ESP step barely affects the total when Wi-Fi adds 10–40 ms
of fixed overhead. Every scenario already feels instant.

### Streaming Downsample from SD (Low RAM)

```c
#define CHUNK_RECORDS  512

void serve_l0_downsampled(FILE *f, uint32_t start, uint32_t end,
                          uint16_t max_points, uint8_t ch_idx)
{
    uint32_t n_records = end - start;
    uint32_t bucket_size = n_records / max_points;
    int16_t chunk[CHUNK_RECORDS * 2];  /* 2 KB stack for BRP */
    int16_t mn = INT16_MAX, mx = INT16_MIN;
    uint32_t in_bucket = 0;

    fseek(f, 32 + start * 4, SEEK_SET);

    for (uint32_t rem = n_records; rem > 0; ) {
        uint32_t to_read = MIN(rem, CHUNK_RECORDS);
        fread(chunk, 4, to_read, f);
        for (uint32_t i = 0; i < to_read; i++) {
            int16_t v = chunk[i * 2 + ch_idx];
            if (v != INT16_MIN) {
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            if (++in_bucket >= bucket_size) {
                emit_minmax_pair(mn, mx);
                mn = INT16_MAX; mx = INT16_MIN;
                in_bucket = 0;
            }
        }
        rem -= to_read;
    }
}
```

Stack usage: **2 KB** regardless of range.

---

## 6. Client-Side Rendering

### uPlot + Zero-Copy TypedArrays

```javascript
async function loadSignal(sessionId, signal, from, to) {
    const maxPts = Math.min(window.innerWidth, 1200);
    const resp = await fetch(
        `/api/data/${sessionId}/${signal}?from=${from}&to=${to}&max=${maxPts}`
    );
    const tier  = resp.headers.get('X-Data-Tier');
    const scale = parseFloat(resp.headers.get('X-Signal-Scale'));
    const buf   = await resp.arrayBuffer();
    const hdr   = new DataView(buf);

    const t0 = hdr.getUint32(0, true);
    const dt = hdr.getUint16(4, true);
    const n  = hdr.getUint16(6, true);
    const raw = new Int16Array(buf, 8);

    const SENTINEL = -32768;
    const ts = new Float64Array(n);

    if (tier === 'minmax') {
        const lo = new Float32Array(n);
        const hi = new Float32Array(n);
        for (let i = 0; i < n; i++) {
            ts[i] = t0 + (i * dt) / 1000;
            lo[i] = raw[i*2]   === SENTINEL ? null : raw[i*2]   * scale;
            hi[i] = raw[i*2+1] === SENTINEL ? null : raw[i*2+1] * scale;
        }
        uplot.setData([ts, lo, hi]);  // filled band
    } else {
        const vals = new Float32Array(n);
        for (let i = 0; i < n; i++) {
            ts[i] = t0 + (i * dt) / 1000;
            vals[i] = raw[i] === SENTINEL ? null : raw[i] * scale;
        }
        uplot.setData([ts, vals]);  // line
    }
}
```

### Optional: Client-Side Pre-Fetch for Smooth Panning

When viewing a window, pre-fetch adjacent ranges in the background so panning
feels instant. This is **purely browser-side** — no ESP changes:

```javascript
async function loadAndPrefetch(sessionId, signal, from, to) {
    const data = await loadSignal(sessionId, signal, from, to);
    const span = to - from;

    // Background-fetch neighbors (browser cache will hold them)
    if (from + span < sessionEnd) {
        fetch(`/api/data/${sessionId}/${signal}?from=${from+span}&to=${to+span}&max=1200`);
    }
    if (from - span >= 0) {
        fetch(`/api/data/${sessionId}/${signal}?from=${from-span}&to=${to-span}&max=1200`);
    }
}
```

For completed sessions with `Cache-Control: max-age=86400`, the pre-fetched
responses stay in the browser's HTTP cache. Subsequent pans hit the cache, not
the ESP. For the active session, pre-fetching still helps (the response arrives
before the user pans), but the data won't be cached since it may change.

### Event Overlay

```javascript
const eventPlugin = {
    hooks: {
        draw: (u) => {
            const ctx = u.ctx;
            for (const evt of sessionEvents) {
                const x0 = u.valToPos(evt.startSec, 'x', true);
                const x1 = u.valToPos(evt.endSec,   'x', true);
                ctx.fillStyle = EVENT_COLORS[evt.type];
                ctx.globalAlpha = 0.2;
                ctx.fillRect(x0, u.bbox.top, x1 - x0, u.bbox.height);
                ctx.globalAlpha = 1.0;
            }
        }
    }
};
```

---

## 7. Post-Session Workflow

```
TherapyStop event received
    │
    ├─ 1. Final flush (remaining recent buffer → SD)
    ├─ 2. Capture NTP time + AS11 GetDateTime → clock_drift_ms
    ├─ 3. Write session.json (times, drift, signal list, end marker)
    │
    ├─ 4. Generate EDF from SD .snt files:
    │      Read L0 interleaved, de-interleave into EDF data records
    │      Apply clock_drift_ms to EDF header start time
    │      (STR.edf pulled via AS11 StartSpool, not self-generated)
    │
    ├─ 5. Upload EDF files (SMB / SleepHQ)
    │
    └─ 6. Recent buffer cleared; SD files are now immutable
           (served with Cache-Control: max-age=86400)
```

---

## 8. Architecture Diagram

```
                 BLE JSON-RPC
                     │
                     ▼
           ┌──────────────────┐
           │  JSON Parser     │
           │  (extract int16, │
           │   sentinel gaps) │
           └────────┬─────────┘
                    │
       ┌────────────┼──────────────┐
       ▼            ▼              ▼
  ┌────────┐  ┌──────────┐  ┌─────────┐
  │Ring buf│  │ Recent   │  │ Events  │
  │(3K LCD)│  │ buffer   │  │ buffer  │
  └────────┘  │ (~60s)   │  └────┬────┘
              └────┬─────┘       │
                   │  60s flush  │
                   ▼             │
              ┌────────┐   ┌─────▼────┐        ┌────────────┐
              │ L0 .snt│   │events.snt│        │  Browser   │
              │ + L1   │   └──────────┘        │  uPlot     │
              └────┬───┘                       │ TypedArray │
                   │     HTTP GET (binary)      └──────┬─────┘
                   │     active: recent buf + SD       │
                   ├──────────────────────────────────┘
                   │     historical: SD only (≤ 12 ms)
              ┌────▼────┐
              │EDF gen  │  (post-session)
              └─────────┘
```

---

## 9. Final Decision Summary

| Aspect | Decision | Rationale |
|--------|----------|-----------|
| Store JSON? | **No** | Parse on arrival, discard |
| PSRAM | **~60 KB (ring + recent + flush)** | No 10h limit; 7.9 MB free |
| SD files | **Per-group interleaved** (5 files) | Fewer f_sync (15 ms vs 48 ms) |
| SD flush | **Every 60s, ~7.5 KB** | 0.025% of interval |
| L1 sidecar | **BRP only, 1s MinMax** | SA2/PLD too low-rate |
| Additional tiers? | **No** | Wi-Fi latency dominates; L1 ≤ 12 ms |
| Gap handling | **INT16_MIN sentinel** | Throughout pipeline |
| Web transport | **HTTP GET + binary** | On-demand, no live push |
| Active session reads | **SD for flushed + recent buffer for tail** | One universal read path |
| Full-fidelity zoom | **≤ 40s window → raw L0** | Every sample, no loss |
| Charting | **uPlot + TypedArray** | Zero-copy, columnar-native |
| Downsampling | **MinMax, integer-only** | Preserves medical peaks |
| Crash safety | **Max 60s loss** | f_sync per flush |
| EDF source | **SD .snt files** | Works after crash |
| STR.edf | **From AS11 spool** | Not self-generated |
| Flush buffer | **Static 6 KB in PSRAM** | No VLA stack overflow |
| Session length | **Unlimited** | Data on SD, not PSRAM-limited |
