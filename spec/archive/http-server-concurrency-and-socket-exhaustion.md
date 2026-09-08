# HTTP Server Concurrency & Socket Exhaustion — Root Cause and Fix Plan

- **Status:** Proposed (design only, no code written)
- **Author(s):** Cascade (AI pair programmer), for review by @ilyakruchinin
- **Created:** 2026-07-08
- **Last updated:** 2026-07-08
- **Related specs:** `spec/archive/nvs-flash-proxy-task-for-psram-stacks.md`
  (separate issue, hit in the same debugging session — see §5 for how they
  interact)

## 1. Summary

The web portal becomes totally unresponsive ("runs out of sockets") whenever
a client keeps a `/api/logs/stream` (SSE) connection open. This is **not**
caused by, and is **not fixed by**, the NVS/flash-proxy proposal — it's a
distinct architectural issue: ESP-IDF's `esp_http_server` processes all
connections on a **single control task**, and `logs_stream_handler` runs an
unbounded `while (true)` loop directly inside that task, blocking it for the
entire lifetime of the SSE connection. Increasing `max_open_sockets` or
`max_uri_handlers` will not help — those only affect how many TCP
connections can be *accepted*, not how many can be *processed concurrently*
(that's always 1 in the current design).

## 2. Root cause

`@main/log_stream.c:303-383` (`logs_stream_handler`):

```c
static esp_err_t logs_stream_handler(httpd_req_t *req)
{
    ...
    while (true) {
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz,
                                            pdMS_TO_TICKS(2000), LOG_LINE_MAX);
        if (!item) {
            httpd_resp_send_chunk(req, ": keepalive\n\n", -1);   // every 2 s
            continue;
        }
        ... httpd_resp_send_chunk(...) per log line ...
    }
    // returns only when the client disconnects
}
```

`esp_http_server` (as configured here via `httpd_config_t` in
`@main/net_provision.c:1434-1444`) has exactly one worker/control task
(`config.stack_size = 8192`, single task, `select()`-based). That task calls
this handler function directly and synchronously; the handler does not
return until the SSE client disconnects. While it's blocked here:

- Other **already-open** sockets are not serviced — status polls, action
  POSTs, and log downloads all queue up silently.
- New connection attempts are still accepted by lwIP up to
  `config.max_open_sockets = 7` (and the global `CONFIG_LWIP_MAX_SOCKETS=16`
  ceiling shared with the FTP server / SMB / SleepHQ upload clients / mDNS),
  but `esp_http_server` never gets back to its `select()` loop to notice and
  process them — from the browser's perspective, everything just hangs.
- This exactly matches both symptoms reported: log download appearing to
  hang "for a long, long time," and the server appearing to "run out of
  sockets and become unresponsive" once enough clients pile up behind the
  one blocked worker.

`logs_stream_handler` does already guard against *multiple* simultaneous SSE
viewers (`s_sse_task` check), so this isn't "N streams competing" — it's
"1 stream, forever, blocking everything else."

## 3. Why raising `max_open_sockets`/`max_uri_handlers` doesn't fix it

- `max_uri_handlers` only bounds how many distinct routes can be
  *registered*; irrelevant to concurrency.
- `max_open_sockets` bounds how many TCP connections lwIP will let the httpd
  *accept* at once. Raising it (e.g. 7 → 12, bounded by
  `CONFIG_LWIP_MAX_SOCKETS`) means more clients can queue up instead of
  being flat-out refused, which is a reasonable defensive tweak — but none
  of those queued connections get processed any faster, because there is
  still only one worker task pulling requests off that queue. It delays the
  failure mode, it doesn't fix it.

## 4. Proposed fixes (ranked)

### 4.1 Recommended: offload the SSE stream via `httpd_req_async_handler_*`

ESP-IDF's `esp_http_server` provides exactly this escape hatch (confirmed
present in this build — `httpd_req_async_handler_begin`/`_complete` are
linked into `somnotrace.elf` already, unused). Pattern:

1. `logs_stream_handler` calls `httpd_req_async_handler_begin(req, &async_req)`
   and returns `ESP_OK` immediately — this hands the underlying request/
   socket off and frees the main control task to go back to `select()` and
   service other connections right away.
2. A dedicated task (spawned once per SSE connection, or a small persistent
   pool) owns `async_req` and runs the existing send loop against it,
   calling `httpd_resp_send_chunk`-equivalent APIs on the async handle.
3. `httpd_req_async_handler_complete(async_req)` when the client disconnects,
   releasing the slot back to the server.

This is the architecturally correct fix: the main control task is never
blocked longer than it takes to hand off, regardless of how many SSE/slow
clients are connected (bounded only by `max_open_sockets`).

Same treatment should apply to `logs_download_handler`
(`@main/log_stream.c:387+`), which streams potentially multiple SD log files
chunk-by-chunk — it's shorter-lived than the SSE stream but still ties up
the single worker for its full duration; low-risk, moderate value to migrate
it too while touching this code.

### 4.2 Simpler alternative: second httpd instance for slow endpoints

Run a **second** `httpd_start()` instance on a separate port, dedicated only
to `/api/logs/stream` (and optionally `/api/logs/download`), leaving the
main portal/API server's single worker free of long-lived connections
entirely. Tradeoffs vs. §4.1:

- Much less code (no async request-handle lifetime management), still only
  a config-level change plus moving two handler registrations.
- Costs one more listening socket + one more small task stack (the second
  httpd instance's own control task, ~4-8 KB).
- Slightly worse UX: the frontend would need to know the second port (or a
  fixed offset like main port + 1), and the browser opens the SSE
  connection to a different origin/port than the page itself — same-origin
  is preserved if it's just a different port on the same host + CORS header
  already present (`Access-Control-Allow-Origin: *`, per
  `@main/log_stream.c:320`).

### 4.3 Simplest alternative: short polling instead of SSE

For a debug log viewer, sub-second push latency isn't a real requirement.
Replace the SSE stream with a plain polling endpoint:

- `GET /api/logs/recent?since=<cursor>` — returns any ring-buffer lines
  produced since `<cursor>`, plus a new cursor value, then **closes the
  connection immediately** (ordinary request/response, no long-lived
  handler).
- Frontend polls this every 1-2 s with `fetch()`, same pattern already used
  for `/api/status` polling elsewhere in `portal.html`.

This is the least code of any option here: no `httpd_req_async_handler_*`
lifetime management (§4.1), no second server instance (§4.2), no new task.
Each poll ties up the single httpd worker for microseconds instead of
seconds/minutes, so it can never cause the blocking behaviour in the first
place — the fix is structural by simply not having a long-lived handler at
all. Downsides: slightly more HTTP request overhead than a push model
(negligible at 1-2 s intervals for a single admin viewer), and loses the
"live tail -f" feel in exchange for near-real-time polling. For this
firmware's actual use case (occasional debugging via the web UI, not a
production log aggregation pipeline), this tradeoff is very likely worth
it — **recommend this as the default choice unless the live-tail UX is
considered a hard requirement**, in which case fall back to §4.1.

### 4.4 Best live-tail option: WebSocket + `httpd_ws_send_frame_async`

If live-tail UX **is** a requirement, WebSocket is a better fit than the
SSE + `httpd_req_async_handler_*` combination (§4.1), because ESP-IDF's
WS support is designed exactly for server-initiated push on long-lived
connections **without occupying the worker task**:

1. Enable `CONFIG_HTTPD_WS_SUPPORT=y` (currently not set; small code-size
   cost, no RAM cost when no WS connection is open).
2. Register the log endpoint as a WS URI (`.is_websocket = true`). The
   handler runs only for the handshake and for (rare) incoming frames —
   it returns immediately; the connection stays open but the worker task
   is **free**.
3. On connect, store the server handle + socket fd. A tiny forwarder task
   (or `httpd_queue_work` callbacks) drains the existing log ring buffer
   and pushes frames via `httpd_ws_send_frame_async(hd, fd, &frame)` —
   the officially supported API for sending WS frames from outside a
   handler context.
4. Frontend swaps `EventSource` for `WebSocket` — a ~10-line change in
   `portal.html`; message handling is otherwise identical (text frames
   instead of SSE `data:` events).

Why this beats §4.1 for this use case:

- `httpd_req_async_handler_begin/_complete` is designed for "finish a slow
  *response* later"; using it for an indefinite stream means manually
  managing an async request handle's lifetime forever, with edge cases
  around session close/purge. WS async send is the purpose-built path for
  indefinite push and handles connection close via the normal WS
  handshake/close machinery.
- The forwarder task is optional — frames can be queued from
  `httpd_queue_work` directly, meaning potentially zero new tasks.
- If a forwarder task is used, it only touches the PSRAM ring buffer and
  the WS API (no NVS/flash), so it can safely take a PSRAM stack via
  `psram_task_create`.

Downsides vs. short polling (§4.3): more code, a Kconfig flag, and a
frontend protocol change — §4.3 remains the recommendation when live-tail
is not a hard requirement.

### 4.5 Defensive, do-regardless-of-4.1–4.4

- Raise `config.max_open_sockets` from 7 towards `CONFIG_LWIP_MAX_SOCKETS`
  (16) minus headroom for FTP/SMB/SleepHQ/mDNS sockets — bounds how many
  clients queue instead of being refused outright while the real fix (4.1
  or 4.2) is rolled out incrementally.
- Add a hard maximum SSE session duration (e.g. auto-`httpd_resp_send_chunk`
  a close/disconnect after N minutes) so a forgotten open browser tab can't
  block things indefinitely even before §4.1/4.2 land. Cheap, one `if`
  around the existing loop's iteration counter/timestamp. Does not fix the
  underlying architecture, only bounds the blast radius.
- Audit `CONFIG_LWIP_MAX_SOCKETS=16` against actual worst-case concurrent
  usage: httpd (`max_open_sockets`) + FTP server session(s) + outbound SMB
  client + outbound SleepHQ HTTPS client + mDNS — if these can all be
  simultaneously active, confirm the total never exceeds 16, or raise it
  (small RAM cost per socket's lwIP control block, internal RAM only).

## 5. Interaction with the flash-proxy proposal

Independent concerns, but worth sequencing sensibly:

- If §4.1 (async handler + dedicated per-connection task) is implemented,
  that new task's stack placement should follow the same rules as the rest
  of the PSRAM work: log streaming/download only touches the SSE ring
  buffer (PSRAM) and SD-card log files (FATFS/SDMMC — not SPI-flash/NVS),
  so per the flash-proxy doc's non-goals (§3) and root-cause analysis
  (§4), **this new task is safe to give a PSRAM stack via
  `psram_task_create`** without needing the flash proxy at all — it never
  touches `nvs_*`/LittleFS.
- Do not sequence the flash-proxy work as a prerequisite for this — they're
  fully independent and can be done in either order.

## 6. Suggested implementation order

This is a user-facing bug (server hangs), independent of the NVS/PSRAM
work in `spec/archive/nvs-flash-proxy-task-for-psram-stacks.md` — do this
first, it's simpler and unblocks daily use of the web UI immediately.

1. Quick defensive win (§4.5): bump `max_open_sockets`, add SSE max-duration
   cutoff. Low risk, ships same day, do regardless of which option below is
   chosen.
2. **Recommended default: convert to short polling (§4.3)** — least code,
   structurally can't reintroduce the blocking bug, and the UX tradeoff is
   acceptable for a debug log viewer. Do this unless live-tail UX is a hard
   requirement.
3. If live-tail UX is required, use WebSocket + async frame send (§4.4)
   instead of §4.3 — purpose-built for indefinite push; prefer it over the
   §4.1 async-handler pattern, which is better suited to slow-but-finite
   responses.
4. Migrate `logs_download_handler` similarly (§4.1's async pattern, or
   leave as-is if step 2/3 already resolves perceived unresponsiveness —
   downloads are naturally bounded/short-lived, lower priority than the
   SSE stream).
5. §4.2 (second httpd instance) is superseded by §4.3 for this use case —
   only revisit if a future endpoint needs genuine independent concurrency
   that polling can't address.

## 7. Open questions

- Does `httpd_req_async_handler_begin` require `config.lru_purge_enable` or
  any other specific config flag beyond what's already set
  (`@main/net_provision.c:1434-1439`)? Needs a quick check against the
  ESP-IDF `async_handlers` example before implementation.
- How many async slots should be pre-provisioned (pool vs. one-task-per-
  connection spawned on demand)? Given `logs_stream_handler` already caps
  concurrent SSE viewers at 1 (`s_sse_task` guard), a single dedicated task
  reused per connection is likely sufficient — no pool needed initially.

## 8. Changelog

- 2026-07-08: Initial draft, written after observing the portal become
  unresponsive with SSE log streaming open, in the same session as the
  NVS/PSRAM bootloop work.
- 2026-07-08: Revised after review — added short-polling alternative
  (§4.3) as the recommended default (least code, structurally immune to
  the blocking bug, acceptable UX tradeoff for a debug log viewer),
  renumbered the defensive-tweaks section to §4.4, and revised §6 to make
  clear this is a user-facing bug fix that should be done independently
  of, and before, the NVS/PSRAM flash-proxy work.
- 2026-07-08: Second revision — added §4.4 (WebSocket +
  `httpd_ws_send_frame_async`) as the preferred option when live-tail UX
  is required, superseding §4.1's async-handler pattern for indefinite
  streams (WS async send is the purpose-built ESP-IDF API for
  server-initiated push and does not occupy the worker task).
