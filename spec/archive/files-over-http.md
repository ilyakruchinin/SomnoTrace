# SomnoTrace — Files over HTTP (EZShare-compatible)

Plan for serving SD card session files over HTTP with an EZShare-compatible
protocol. This enables verification that session data is stored correctly and
allows pulling files with existing EZShare client tools.

---

## 1. Background: EZShare Protocol

### What is EZShare?

EZShare WiFi SD cards are consumer WiFi SD cards that run a tiny HTTP server.
They are widely used in the CPAP community to pull ResMed SD card data
wirelessly. Multiple open-source clients exist (Python, Node.js, PowerShell,
bash). The protocol is simple HTTP with HTML directory listings.

### Observed protocol (from reverse-engineering and open-source clients)

**Base URL:** `http://<ip>/` (default `http://192.168.4.1/`, hostname
`ezshare.card`)

**Directory listing:**

```
GET /dir?dir=A:           ← root directory (FAT drive A:)
GET /dir?dir=A:\DCIM      ← subdirectory
GET /dir?dir=A:\DATALOG   ← arbitrary path
```

Returns an HTML page with a `<pre>` tag containing one entry per line. Each
line has a timestamp, a size (or `<DIR>`), and an `<a>` link:

```html
<pre>
2021-07-19 10:23:45    &lt;DIR&gt;    <a href="/dir?dir=A:\DATALOG\20210719">20210719</a>
2021-07-19 10:24:01       12345    <a href="/download?fname=2071A5~1.EDF&fdir=..\DATALOG\20210719">2071A5~1.EDF</a>
</pre>
```

- **Directories:** `<a href="/dir?dir=A:\path\subdir">subdir</a>`
- **Files:** `<a href="/download?fname=FILENAME.EXT&fdir=..\path">FILENAME.EXT</a>`

The `fdir` parameter uses backslash-separated relative paths with `..\` to
escape the DCIM root. Filenames are 8.3 short names (FAT32 limitation of the
original firmware).

**File download:**

```
GET /download?fname=2071A5~1.EDF&fdir=..\DATALOG\20210719
```

Returns the raw file bytes with `Content-Type: application/octet-stream`.

**Other endpoints (not needed for us):**
- `/photo` — image gallery view (filters by extension)
- `/config` — admin/config page
- `/client?command=version` — firmware version string

### Open-source clients that consume this protocol

| Project | Language | Purpose |
|---------|----------|---------|
| [nekromant/ezshare](https://github.com/nekromant/ezshare) | Python | `pip install ezshare` — list + download + recursive sync |
| [adrianRfeeger/ezShareCPAP](https://github.com/adrianRfeeger/ezShareCPAP) | Python | GUI app for CPAP data sync |
| [JCOvergaar/CPAP-data-from-EZShare-SD](https://github.com/JCOvergaar/CPAP-data-from-EZShare-SD) | Python | CLI for ResMed CPAP data |
| [dwbrott/ezfetch](https://github.com/dwbrott/ezfetch) | PowerShell | Windows CPAP data fetcher |
| [Biorn1950/EzShare-SdcardWifi-Downloader](https://github.com/Biorn1950/EzShare-SdcardWifi-Downloader) | Bash | Recursive download script |
| [StevenLimCA/ezShareDownloader](https://github.com/StevenLimCA/ezShareDownloader) | Node.js | Recursive download with cheerio HTML parsing |
| [hms-homelab/hms-cpap](https://github.com/hms-homelab/hms-cpap) | C++ | Home Assistant integration via HTTP polling |

All of these parse the same HTML `<pre>` directory listing format and download
via `/download?fname=...&fdir=...`.

---

## 2. SomnoTrace Adaptation

### Why EZShare-compatible?

We don't need to replicate the EZShare protocol exactly — we're not an SD card
in a camera. But following the same `/dir` + `/download` pattern means:

1. **Existing tools work out of the box** — `pip install ezshare` can list and
   pull our session files without modification.
2. **Simple to implement** — just HTML generation and static file serving on
   top of ESP-IDF's `httpd`.
3. **Human-browsable** — open in any browser, click through directories.
4. **Verification** — curl/wget/browser to inspect stored session data during
   development.

### Key differences from real EZShare

| Aspect | Real EZShare | SomnoTrace |
|--------|-------------|------------|
| Filesystem | FAT32, 8.3 names | FATFS (exFAT/FAT32), long names OK |
| Path format | `A:\DCIM\subdir`, `..\` escapes | Forward-slash POSIX paths under `/somnotrace/` |
| WiFi mode | AP-only (192.168.4.1) | STA mode (joins existing WiFi) |
| Directory listing | HTML `<pre>` with `<a>` links | Same format, POSIX paths |
| File download | `/download?fname=...&fdir=...` | `/download?path=/somnotrace/sessions/...` |
| Root | `A:` (drive letter) | `/` (filesystem root) |

### Design decision: path style

Real EZShare uses FAT drive letters (`A:\`) and backslash paths with `..\`
escapes. This is an artifact of the card's firmware. We'll use **forward-slash
POSIX paths** which are natural for FATFS on ESP-IDF and simpler to parse.

The `/dir` endpoint will accept a `dir` query parameter with a POSIX path:
`/dir?dir=/somnotrace/sessions`. This is a superset of the EZShare protocol —
existing clients that parse the HTML links will follow whatever path format
the links contain, so they'll work with POSIX paths too.

---

## 3. HTTP Endpoints

### 3.1 Directory Listing

```
GET /dir?dir=/somnotrace/sessions
GET /dir?dir=/somnotrace/sessions/20260624_2215
```

**Response:** `200 OK`, `Content-Type: text/html`

```html
<html><body><pre>
2026-06-24 22:15:00    &lt;DIR&gt;    <a href="/dir?dir=/somnotrace/sessions/20260624_2215">20260624_2215</a>
2026-06-24 22:15:01       2812    <a href="/download?path=/somnotrace/sessions/20260624_2215/brp.snt">brp.snt</a>
2026-06-24 22:15:01        225    <a href="/download?path=/somnotrace/sessions/20260624_2215/brp_mm.snt">brp_mm.snt</a>
2026-06-24 22:15:01        113    <a href="/download?path=/somnotrace/sessions/20260624_2215/sa2.snt">sa2.snt</a>
2026-06-24 22:15:01        338    <a href="/download?path=/somnotrace/sessions/20260624_2215/pld.snt">pld.snt</a>
2026-06-24 22:15:01         42    <a href="/download?path=/somnotrace/sessions/20260624_2215/events.snt">events.snt</a>
2026-06-24 22:15:01        512    <a href="/download?path=/somnotrace/sessions/20260624_2215/session.json">session.json</a>
</pre></body></html>
```

**Root listing** (`GET /dir?dir=/` or `GET /dir`):

```html
<html><body><pre>
2026-06-24 22:15:00    &lt;DIR&gt;    <a href="/dir?dir=/somnotrace">somnotrace</a>
</pre></body></html>
```

**Path traversal protection:** Reject paths containing `..` components. Only
absolute paths under the SD mount point are served.

### 3.2 File Download

```
GET /download?path=/somnotrace/sessions/20260624_2215/brp.snt
```

**Response:** `200 OK`

```
Content-Type: application/octet-stream
Content-Length: 2812

<raw file bytes>
```

Stream the file in chunks (e.g. 4 KB) to avoid loading the whole file into
RAM. Use `httpd_resp_send_chunk` in a loop.

**Missing file:** `404 Not Found` with a short HTML error body.

### 3.3 Session List (JSON, convenience endpoint)

```
GET /api/sessions
```

**Response:** `200 OK`, `Content-Type: application/json`

```json
[
  {
    "id": "20260624_2215",
    "path": "/somnotrace/sessions/20260624_2215",
    "start": "2026-06-24T22:15:00",
    "end": "2026-06-25T06:30:00",
    "state": "completed",
    "files": ["brp.snt", "brp_mm.snt", "sa2.snt", "pld.snt", "events.snt", "session.json"],
    "total_size": 4042
  }
]
```

This is a convenience endpoint for the web UI and automated tooling. It reads
`session.json` from each session directory to populate metadata. The
EZShare-compatible `/dir` endpoint is the primary access method.

### 3.4 Session Metadata (JSON)

```
GET /api/session/20260624_2215
```

Returns the contents of `session.json` for a specific session.

---

## 4. HTML Directory Listing Format (Specification)

The HTML response is intentionally minimal to keep parsing simple for both
browsers and automated tools:

```
<html><body><pre>
{timestamp}    {size_or_DIR}    <a href="{url}">{name}</a>
...
</pre></body></html>
```

**Fields:**
- `{timestamp}` — `YYYY-MM-DD HH:MM:SS` (modification time from `stat`)
- `{size_or_DIR}` — right-aligned file size in bytes, or `&lt;DIR&gt;` for
  directories
- `{url}` — `/dir?dir={path}` for directories, `/download?path={path}` for
  files
- `{name}` — filename or directory name (URL-encoded in the href)

**Sorting:** Directories first (alphabetical), then files (alphabetical).
Include `.` and `..` entries for browser navigation, matching EZShare
behaviour.

**Encoding:** UTF-8. HTML-encode special characters in filenames (`&`, `<`,
`>`).

---

## 5. Implementation Plan

### Phase 1: SD Card Initialization

**Component:** `sd_storage` (new component or part of existing main)

1. Initialize SDMMC 4-bit mode using ESP-IDF `sdmmc` driver:
   - CLK=IO16, CMD=IO15, D0-D3=IO17,IO18,IO13,IO14
   - Max frequency: 20 MHz (SDMMC freq default)
   - Mount FATFS at `/somnotrace`
2. Create directory structure on first boot:
   ```
   /somnotrace/sessions/
   ```
3. Log SD card info at boot: capacity, free space, mount point.

**Kconfig/sdkconfig:**
- `CONFIG_SDMMC_ENABLE` — enable SDMMC peripheral
- FatFS via VFS (already available in ESP-IDF)

### Phase 2: Session Data Writer

**Component:** `session_writer` (new component)

1. **Session start detection:**
   - Triggered by AS11 therapy start event (via BLE notification).
   - Create session directory: `/somnotrace/sessions/YYYYMMDD_HHMM/`
   - Open 5 data files: `brp.snt`, `brp_mm.snt`, `sa2.snt`, `pld.snt`,
     `events.snt`.
   - Write 32-byte `sntb_header_t` to each `.snt` file (see
     `local-data-files.md` §3).

2. **Recent buffer (in RAM, ~50 KB):**
   - Per-stream ring buffers holding last ~60 s of data.
   - BLE callback writes parsed int16 samples into ring buffers.
   - See `local-data-files.md` §2 (v5) for buffer layout.

3. **Periodic flush (every 60 s):**
   - Interleave from recent buffer → flush buffer → `f_write` to SD.
   - Compute L1 MinMax for BRP (1-second buckets).
   - `f_sync` all 5 files.
   - Update `sample_count` in each file header.

4. **Session stop:**
   - Final flush.
   - Capture NTP time + AS11 `GetDateTime` → `clock_drift_ms`.
   - Write `session.json` (start, end, drift, signal list, state).
   - Close all files.

5. **Crash recovery (on boot):**
   - Scan `/somnotrace/sessions/` for directories without `session.json`
     (or with `session.json` missing `end_time`).
   - Truncate `.snt` files to last committed `sample_count`.
   - Write `session.json` with `state: "interrupted"`.

### Phase 3: HTTP File Server

**Component:** extend `net_provision.c` or new `file_server.c`

1. Register `/dir` handler:
   - Parse `dir` query parameter (default: `/`).
   - Reject paths with `..` (traversal protection).
   - `opendir` + `readdir` to enumerate entries.
   - `stat` each entry for size and modification time.
   - Generate HTML `<pre>` listing.
   - Send as single `httpd_resp_send` (listings are small).

2. Register `/download` handler:
   - Parse `path` query parameter.
   - Reject paths with `..`.
   - `open` file, `stat` for size.
   - Set `Content-Type: application/octet-stream`.
   - Set `Content-Length` header.
   - Stream in 4 KB chunks via `httpd_resp_send_chunk`.
   - Close file on completion.

3. Register `/api/sessions` handler:
   - List session directories.
   - Read `session.json` from each.
   - Return JSON array.

4. Register `/api/session/:id` handler:
   - Read and return `session.json` for the given session ID.

### Phase 4: Testing & Verification

1. **Manual browser test:**
   - Navigate to `http://<esp-ip>/dir?dir=/somnotrace/sessions`
   - Click through session directories.
   - Download individual `.snt` files.

2. **curl verification:**
   ```bash
   # List root
   curl 'http://<esp-ip>/dir?dir=/'

   # List sessions
   curl 'http://<esp-ip>/dir?dir=/somnotrace/sessions'

   # Download a file
   curl -o brp.snt 'http://<esp-ip>/download?path=/somnotrace/sessions/20260624_2215/brp.snt'

   # Session list JSON
   curl 'http://<esp-ip>/api/sessions'
   ```

3. **EZShare client compatibility:**
   ```bash
   pip install ezshare
   ezshare-cli -l /somnotrace/sessions
   ezshare-cli -d /somnotrace/sessions/20260624_2215/brp.snt
   ```
   (May require minor path adaptation since we use POSIX paths instead of
   `A:\` drive letters. The HTML link parsing should work regardless.)

4. **Binary file verification:**
   - Download `.snt` files, parse the 32-byte header, verify `magic`,
     `sample_count`, and record layout.
   - Download `session.json`, verify metadata fields.

---

## 6. ESP-IDF Requirements

### sdkconfig.defaults additions

```ini
# SDMMC (4-bit mode)
CONFIG_SDMMC_ENABLE=y
CONFIG_SDMMC_PSRAM=y          # if PSRAM shares SDMMC pins (check)
CONFIG_FATFS=y
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_CODEPAGE=850
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_FS_LOCK=0
```

> **Note:** The Waveshare board routes SD on SDMMC peripheral (IO13-18),
> separate from SPI LCD (IO21, IO38-39). No pin conflict. See
> `docs/hardware/README.md` — "microSD (SDMMC 4-bit): CLK=IO16, CMD=IO15,
> D0-D3=IO17,IO18,IO13,IO14".

### HTTP server

The existing `httpd` instance in `net_provision.c` is reused. New handlers
are registered alongside the existing `/api/ble/*` endpoints.

### Memory

- Directory listing: ~2 KB stack buffer for HTML generation.
- File download: 4 KB chunk buffer (static or heap-allocated once).
- No PSRAM needed for the file server itself.

---

## 7. File Layout on SD

```
/somnotrace/
  sessions/
    20260624_2215/
      session.json           ← metadata
      brp.snt                ← L0: Flow + MaskPressure, 25 Hz
      brp_mm.snt             ← L1: 1-second MinMax (BRP only)
      sa2.snt                ← L0: HeartRate + SpO2, 1 Hz
      pld.snt                ← L0: 12 PLD channels, 0.5 Hz
      events.snt             ← therapy events
    20260625_2300/
      ...
```

### session.json format

```json
{
  "id": "20260624_2215",
  "start_epoch_ms": 1719258900000,
  "end_epoch_ms": 1719288600000,
  "start_iso": "2026-06-24T22:15:00+10:00",
  "end_iso": "2026-06-25T06:30:00+10:00",
  "clock_drift_ms": -1234,
  "state": "completed",
  "signals": ["flow", "mask_pressure", "heart_rate", "spo2", "pld_0", "..."],
  "as11_device": "E8:E0:7E:DD:0A:48",
  "as11_client_id": "33B9C8F313F1"
}
```

---

## 8. What is NOT included (deferred)

- **EDF export** — post-session generation from `.snt` files (separate spec).
- **SMB / SleepHQ upload** — separate component.
- **Web UI visualization** — uPlot rendering of session data (separate spec).
- **Data query / downsampling API** — `/api/data/:id/:signal` endpoint with
  tier selection (will be added later, builds on top of the file server).
- **File deletion / cleanup** — old session management (future).
- **Authentication** — the HTTP server is open on the local network (same as
  current provisioning portal).

---

## 9. Changelog

- 2026-06-24: Initial draft. Researched EZShare protocol from open-source
  clients and reverse-engineering notes. Designed EZShare-compatible `/dir`
  and `/download` endpoints with POSIX paths. Planned SD card initialization,
  session data writer, and HTTP file server implementation phases.
