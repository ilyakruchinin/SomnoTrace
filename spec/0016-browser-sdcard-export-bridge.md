# 0016 — Read-Only SD Export API & Browser `postMessage` Bridge

- **Status:** Implemented
- **Author(s):** Ilya Kruchinin
- **Created:** 2026-11-28
- **Last updated:** 2026-11-28
- **Related specs:** [0011-web-api-endpoints.md](0011-web-api-endpoints.md), [0014-web-interface-password.md](0014-web-interface-password.md)

## 1. Summary

SomnoTrace provides a two-layer mechanism enabling third-party web applications (such as `cpapclarity.com`, running over `https://`) and local scripts to read exported ResMed-compatible CPAP files from `/somnotrace/SDCARD` without disabling security controls or exposing admin credentials:

1. **Layer 1 — Read-Only SD Export API:** Two password-protected HTTP endpoints (`GET /api/sdcard/files` and `GET|HEAD /api/sdcard/file?path=...`) strictly jailed to `SDCARD/` on the SD card.
2. **Layer 2 — Browser `window.postMessage` Bridge (`?connect=<origin>`):** A popup/tab consent flow served directly by SomnoTrace that transfers requested `SDCARD/` files to the requesting web application using explicit-origin `window.postMessage` with zero-copy Transferable `ArrayBuffer`s.

## 2. Motivation / goals

- Allow HTTPS browser analysis tools (e.g. `cpapclarity.com`) to pull therapy data directly from a user's LAN SomnoTrace device (`http://somnotrace.local` or LAN IP) in 1 click without requiring manual SD downloads or external cloud uploads.
- Work universally across Chromium, Firefox (including HTTPS-Only Mode), and Safari/WebKit (macOS and iOS), where direct cross-scheme `https://` -> `http://` `fetch()` + CORS is blocked by Mixed Content rules or fails strict `connect-src 'self'` CSPs.
- Keep SomnoTrace authentication (`st_session` cookie and HTTP Basic auth) completely inside SomnoTrace so third-party websites never handle the user's device password or admin credentials.
- Prevent exposure of SomnoTrace internal directories (`.somnotrace/`, logs, oximetry raw stores, or NVS configuration).

## 3. Non-goals

- Cross-origin CORS preflight (`OPTIONS`) headers or read-only bearer tokens for direct HTTPS -> HTTP `fetch()`.
- Write access, deletion, or directory access outside `/somnotrace/SDCARD`.
- Embedding SomnoTrace in an `<iframe>` (blocked by browser Mixed Content rules on HTTPS parents).

## 4. Behaviour & Usage

### 4.1 Layer 1 — Read-Only SD Export API

Registered via `web_auth_register()` ([web_auth.c](../main/web_auth.c)), inheriting both session cookie authentication (`st_session`, `HttpOnly; SameSite=Strict`) and HTTP Basic authentication when a web password is configured. Implemented in [sdcard_api.c](../main/sdcard_api.c) and [sdcard_api_core.c](../main/sdcard_api_core.c).

#### `GET /api/sdcard/files[?dir=<relpath>]`

Streams a JSON array listing regular files under `/somnotrace/SDCARD` (or `/somnotrace/SDCARD/<relpath>` when `?dir=` is provided).

- **Execution:** Deferred to the PSRAM worker via `httpd_defer_dispatch()`, acquires `SD_LEASE_UPLOAD`, and walks directories iteratively up to depth `SDCARD_API_MAX_DEPTH` (`4`). Hidden entries starting with `.` are always skipped.
- **Response (`200 OK`, `application/json`, `Cache-Control: no-store`):**
  ```json
  [
    {"path":"STR.edf","size":32768,"mtime":1795624566},
    {"path":"Identification.json","size":184,"mtime":1795620966},
    {"path":"DATALOG/20261124/20261124_220000_BRP.edf","size":262144,"mtime":1795624566}
  ]
  ```
- **Errors:**
  - `400 Bad Request` (`{"error":"bad_path"}`) — invalid `?dir=` relative path.
  - `401 Unauthorized` — password enabled and client is not authenticated.
  - `404 Not Found` (`{"error":"not_found"}`) — requested `?dir=` subdirectory does not exist (returns `200 []` when `dir` is omitted and `SDCARD/` is empty/absent).
  - `503 Service Unavailable` (`{"error":"sd_not_ready"}` or `{"error":"sd_busy"}`) — SD card unavailable or `SD_LEASE_UPLOAD` busy.

#### `GET /api/sdcard/file?path=<relpath>` and `HEAD /api/sdcard/file?path=<relpath>`

Streams a single regular file from `/somnotrace/SDCARD/<relpath>` as `application/octet-stream` with `Cache-Control: no-store`.

- **Path validation (`sdcard_api_relpath_valid()`):**
  - Non-empty, length `< 192` bytes.
  - Rejects leading/trailing `/`, `\`, control characters (`< 0x20` or `0x7f`), empty segments (`//`), `.`, `..`, and any hidden `.`-prefixed path segment.
- **Streaming:** `HEAD` returns `Content-Length` only. Files `<= 2048` bytes are sent inline with `Content-Length`. Larger files set `Connection: close` and hand off to the shared async worker `session_graph_enqueue_file()`.
- **Errors:** `400` (`bad_path`), `401` (unauthenticated), `404` (`not_found`), `503` (`sd_not_ready`).

#### CLI / Script Example (`curl`)

```bash
# List all exported files (omit -u :password if web password is off)
curl -u :yourpassword http://somnotrace.local/api/sdcard/files

# List a single DATALOG day folder
curl -u :yourpassword 'http://somnotrace.local/api/sdcard/files?dir=DATALOG/20261124'

# Download one EDF file
curl -u :yourpassword -OJ 'http://somnotrace.local/api/sdcard/file?path=DATALOG/20261124/20261124_220000_BRP.edf'
```

---

### 4.2 Layer 2 — Browser `window.postMessage` Bridge (`?connect=<origin>`)

Implemented in [portal.html](../main/portal.html). When `/?connect=<origin>` is opened in a popup window or tab (`window.open()`), SomnoTrace enters connect mode and short-circuits before starting dashboard WebSockets or polling.

#### Step-by-step flow

1. **Origin validation (`parseConnectOrigin()`):**
   - Parses `connect` with `new URL(raw)`.
   - Permits `https:` origins, or `http://localhost` / `http://127.0.0.1` for local development.
   - Normalizes to `u.origin` (stripping any path/query/fragment) and rejects all other schemes or hosts (e.g. `http://evil.example`, `javascript:`, `file:`).
   - Verifies `window.opener` is present.
2. **Authentication (preserving `window.opener`):**
   - Checks same-origin `GET /api/auth/status`. Because `GET /` is public while `/api/auth/status` and `/api/sdcard/*` are same-origin `fetch()` calls, an existing `SameSite=Strict` cookie works immediately without showing login.
   - If a password is enabled and the browser is not signed in, `#auth-overlay` is shown over the consent dialog; signing in via `doLogin()` hides the login overlay inline without calling `location.reload()`, keeping `window.opener` intact.
3. **User consent:**
   - Displays `#connect-overlay` with the exact validated `targetOrigin` and `[ Cancel ]` / `[ Allow & Sync ]` buttons.
4. **Protocol v1 message exchange (`connectRunTransfer()`):**
   Every `postMessage` uses `targetOrigin` explicitly (never `'*'`), and incoming messages verify `event.origin === targetOrigin && event.source === window.opener`.
   - **SomnoTrace -> App (`st:manifest`):**
     ```json
     {
       "type": "st:manifest",
       "v": 1,
       "files": [
         { "path": "STR.edf", "size": 32768, "mtime": 1795624566 }
       ]
     }
     ```
   - **App -> SomnoTrace (`st:request`):** sent within 15 seconds; allows incremental sync by requesting only missing/updated `path` strings from the manifest:
     ```json
     {
       "type": "st:request",
       "paths": ["STR.edf", "DATALOG/20261124/20261124_220000_BRP.edf"]
     }
     ```
   - **SomnoTrace -> App (`st:file`, sequentially 1 file at a time):** fetches each requested path and transfers ownership of the `ArrayBuffer` zero-copy via the `postMessage(..., targetOrigin, [buf])` transfer list:
     ```json
     {
       "type": "st:file",
       "path": "DATALOG/20261124/20261124_220000_BRP.edf",
       "size": 262144,
       "mtime": 1795624566,
       "data": "<ArrayBuffer>"
     }
     ```
   - **SomnoTrace -> App (`st:done`):**
     ```json
     { "type": "st:done", "count": 2 }
     ```
     Popup then calls `window.close()`.
   - **SomnoTrace -> App (`st:error`):** sent on cancellation or failure (e.g. `user_cancelled`, `unknown_path`, `request_timeout`, `manifest_http_401`, `file_http_404`):
     ```json
     { "type": "st:error", "error": "user_cancelled" }
     ```

#### Web App Integration Example

Third-party sites must invoke `window.open()` synchronously inside a click handler, must not set a restrictive `Cross-Origin-Opener-Policy` (`same-origin` detaches `window.opener`), and must target a distinct device host (`somnotrace.local` or LAN IP — note that `CSP: upgrade-insecure-requests` rewrites same-host `http://` navigations, not cross-host ones):

```js
async function importFromSomnoTrace(deviceHost = 'http://somnotrace.local') {
  const stOrigin = new URL(deviceHost).origin;
  const files = [];

  return new Promise((resolve, reject) => {
    const popup = window.open(
      `${stOrigin}/?connect=${encodeURIComponent(location.origin)}`,
      'somnotrace_sync',
      'width=460,height=580'
    );
    if (!popup) return reject(new Error('Popup blocked'));

    function onMessage(event) {
      if (event.origin !== stOrigin || event.source !== popup) return;
      const msg = event.data;
      if (!msg || typeof msg !== 'object') return;

      if (msg.type === 'st:manifest') {
        const wanted = msg.files
          .filter((f) => needsImport(f.path, f.size, f.mtime))
          .map((f) => f.path);
        popup.postMessage({ type: 'st:request', paths: wanted }, stOrigin);
      } else if (msg.type === 'st:file') {
        files.push({ path: msg.path, size: msg.size, mtime: msg.mtime, data: msg.data });
      } else if (msg.type === 'st:done') {
        window.removeEventListener('message', onMessage);
        resolve(files);
      } else if (msg.type === 'st:error') {
        window.removeEventListener('message', onMessage);
        reject(new Error(msg.error || 'SomnoTrace transfer error'));
      }
    }

    window.addEventListener('message', onMessage);
  });
}
```

## 5. Acceptance criteria

- [x] `GET /api/sdcard/files` and `GET|HEAD /api/sdcard/file?path=...` are strictly jailed to `/somnotrace/SDCARD`, reject traversal and hidden entries, and are registered behind `web_auth_register()`.
- [x] `/?connect=<origin>` enforces `https://` (or `http://localhost` / `http://127.0.0.1`), requires `window.opener`, works with and without web password auth, transfers requested files sequentially using `Transferable` `ArrayBuffer`s with exact `targetOrigin`, and auto-closes.
- [x] Unit tests ([sdcard_api_test.c](../scripts/sdcard_api_test.c) and [connect_bridge_test.js](../scripts/connect_bridge_test.js)) pass in `scripts/run_host_tests.sh`.
- [x] Cross-scheme `https://` -> `http://` bridge verified in Chromium (CSP `connect-src 'self'; upgrade-insecure-requests`) and Firefox (HTTPS-Only Mode enabled).

## 6. Security / privacy considerations

- **No `.somnotrace/` exposure:** Both path validation and directory walking reject `.`-prefixed names, `..`, backslashes, and leading `/`, ensuring logs, oximetry internals, and uploader state are never reachable.
- **No wildcard `postMessage`:** Both manifest/file replies and incoming `st:request` verification pin the exact normalized `targetOrigin` shown to the user in the consent modal.
- **No credential leakage:** Password authentication happens inside SomnoTrace's own popup using `SameSite=Strict` cookies; the third-party app never handles passwords or tokens.

## 7. Open questions

- None for v1. Optional future extensions (not required for v1): a "Remember this site" preference or client-side uncompressed `.zip` download using Layer 1.

## 8. Changelog

- 2026-11-28: Initial specification and implementation (Layer 1 `/api/sdcard/*` + Layer 2 `?connect=` `window.postMessage` bridge v1).
