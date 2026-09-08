# NVS/Flash Proxy Task — Proposal to Unblock PSRAM Task Stacks

- **Status:** Proposed (design only, no code written)
- **Author(s):** Cascade (AI pair programmer), for review by @ilyakruchinin
- **Created:** 2026-07-08
- **Last updated:** 2026-07-08
- **Related specs:** `spec/archive/http-server-concurrency-and-socket-exhaustion.md`
  (separate issue hit in the same session — the web server becoming
  unresponsive under a long-lived SSE connection is a single-worker-task
  concurrency problem, **not fixed by this proposal**; see that doc)

## 1. Summary

We hit three consecutive bootloops while moving FreeRTOS task stacks to PSRAM
to save internal RAM. Root cause: on ESP32-S3, any code path that touches raw
SPI NOR flash (NVS, LittleFS on the `storage` partition, OTA) briefly disables
the instruction/data cache for **both** cores. PSRAM is accessed through that
same cache, so a task whose *stack* lives in PSRAM crashes
(`esp_task_stack_is_sane_cache_disabled` assert) the moment it — or anything
it calls — touches flash while cache is disabled.

This document proposes a **single dedicated "flash proxy" task** with a small,
permanently-internal stack that is the *only* code in the firmware allowed to
call `nvs_*`, `esp_partition_*`, or LittleFS stdio functions on the `storage`
partition. Every other task — including ones we want fully in PSRAM (BLE
pairing/reconnect, the uploader, and HTTP request handlers) — talks to it
through a FreeRTOS queue instead of calling flash APIs directly. This turns a
per-task, easy-to-get-wrong safety rule ("does this task's call graph ever
touch flash?") into a single structural guarantee ("only one file in the
codebase includes `nvs.h`").

## 2. Motivation / goals

- Get internal RAM usage as close to the essential minimum as possible
  (Wi-Fi/BLE driver buffers, DMA buffers, and now the proxy's own tiny stack).
- Remove the current fragility where *any* future code path that indirectly
  calls into NVS from a PSRAM-stack task (or the HTTP worker task) causes a
  hard crash loop — we've now hit this bug three separate times in one
  session (`reconnect_task`, `confirm_task`'s `nvs_save_pairing`, the
  uploader's LittleFS state save, and the HTTP `status` handler calling
  `as11_ble_is_paired()`).
- Make "is it safe to give this task a PSRAM stack?" a static, auditable
  property instead of something that depends on the task's entire transitive
  call graph, including future changes.

## 3. Non-goals

- Not proposing to change how **SD card** I/O works (SDMMC is a separate
  peripheral from SPI flash/PSRAM; it does not disable cache the same way,
  and PSRAM buffers already work fine there per the `edf_gen.c` SD/PSRAM
  fix). SD-card-writing tasks (`session_writer`, `edf_gen`) are out of scope.
- Not proposing to touch closed-source ESP-IDF driver-internal tasks (Wi-Fi
  driver task, NimBLE host task, `esp_timer` service task). We don't control
  their task-creation call sites, so they can't be migrated to this pattern;
  see §7.4.
- Not implementing anything yet — this is a design for review.

## 4. Root cause detail (why this is even necessary)

`spi_flash_disable_interrupts_caches_and_other_cpu()` runs before every raw
flash transaction (any `nvs_get_*`/`nvs_set_*`/`esp_partition_read/write`,
which LittleFS on the `storage` partition also goes through). ESP32-S3 maps
both flash and octal PSRAM through the same MMU/cache hardware, so while this
function has cache disabled:

- Any instruction fetch or stack push/pop for a task whose **stack** resides
  in PSRAM will fault — this is exactly what
  `esp_task_stack_is_sane_cache_disabled()` checks for and aborts on.
- The disable/enable window is narrowly scoped around the actual SPI
  transaction (not the whole `nvs_get_str()` call), so by the time a
  function does its final `memcpy` into a caller-supplied output buffer,
  cache is typically back on — but we should not rely on this timing detail
  being stable across ESP-IDF versions (see §5.3 for how the proposal avoids
  needing to rely on it).

The critical fact this proposal exploits: **a *blocked* task's PSRAM stack is
never touched while it's not running.** If Task A (PSRAM stack) asks Task B
(internal stack) to do the flash operation and then blocks on a queue/
semaphore, only Task B's stack is live during the cache-disabled window. Task
A is completely safe regardless of where its stack lives.

### 4.1 Cheaper alternative for the read-only BLE call sites

Three of the six call sites in §5.4 below are pure reads of rarely-changing
data: `as11_ble_is_paired()` and `as11_ble_get_paired_info()` are called on
**every** `/api/status` poll (i.e. from the HTTP worker task, at whatever
cadence the frontend polls), which means the proxy's queue round-trip +
context switch becomes a per-poll cost, not a per-pairing-event cost. This
is very likely fine in absolute terms (low microseconds), but it's the
wrong place to pay any avoidable latency, and it's unnecessary complexity
for data that only changes on pair/unpair/forget.

**Recommendation: cache paired-state in a plain in-RAM struct**, updated
only at the point where pairing succeeds, is forgotten, or is loaded once
at boot (all three are already low-frequency, proxy-appropriate events).
`as11_ble_is_paired()`/`_get_paired_info()` then become simple RAM reads —
no queue, no proxy, no NVS call at all on the hot path. This shrinks the
migration surface in §5.4 from 6 call sites to effectively 4 (the writes:
`nvs_save_pairing`, `as11_ble_forget`, plus `netprov_save_config`,
`device_settings_save`, `uploader` config save, and the boot-time loads),
and removes the latency question entirely rather than just hoping it's
small enough. Should be done regardless of whether the full proxy is
implemented — it's a strict improvement on its own.

## 5. Proposed design

### 5.0 Evaluate FIRST: `CONFIG_SPI_FLASH_AUTO_SUSPEND` (possible zero-code fix)

Before building any proxy, evaluate this Kconfig option (present in this
IDF v5.5.1 build, currently **not set**). When enabled — and when the
board's flash chip supports the suspend/resume command set — ESP-IDF keeps
the **cache enabled during flash erase/program operations** by
auto-suspending the flash operation whenever the CPU needs a cache fill.
If that holds for this hardware, the entire "PSRAM stack + flash op =
crash" restriction disappears, and every task can get a PSRAM stack with
**zero application code changes** — this whole proposal becomes
unnecessary.

Caveats that make this an *evaluation*, not a recommendation yet:

- Requires flash-chip support (SUS/resume commands). The boot log reports
  `spi_flash: detected chip: generic`, so the exact chip on the Waveshare
  ESP32-S3 module needs identifying (`esptool.py flash_id`) and checking
  against IDF's supported-chip list for auto-suspend.
- Known chip errata exist around suspend-during-erase on some parts;
  IDF gates the feature accordingly, but it needs a soak test, not just a
  boot test.
- Slightly increases flash wear/latency for erase operations (they get
  suspended/resumed repeatedly under load).
- Verify it actually removes the `esp_task_stack_is_sane_cache_disabled`
  assert path for **both** reads and writes in this IDF version — test by
  enabling the option and running an NVS write loop from a PSRAM-stack
  task.

**Test plan:** enable `CONFIG_SPI_FLASH_AUTO_SUSPEND=y`, temporarily
re-promote `reconnect_task` to `psram_task_create`, trigger repeated BLE
reconnects + config saves for an extended period. If no assert and no data
corruption: adopt it, close this proposal, and skip §5.1–§5.5 entirely. If
the chip is unsupported or the soak fails: proceed with the proxy below.

### 5.1 One proxy task, one queue, synchronous RPC

Add a new component, e.g. `main/flash_proxy.c` / `.h`:

```c
// flash_proxy.h
typedef enum {
    FP_NVS_GET_STR,
    FP_NVS_SET_STR,
    FP_NVS_GET_U8,
    FP_NVS_SET_U8,
    FP_NVS_COMMIT,       // commit + close in one op
    FP_NVS_ERASE_ALL,
    FP_LFS_READ_FILE,    // whole-file read, bounded size
    FP_LFS_WRITE_FILE,   // atomic tmp-write + rename, as uploader_state.c does today
} flash_op_t;

typedef struct {
    flash_op_t   op;
    const char  *ns_or_path;   // NVS namespace, or LittleFS path
    const char  *key;          // NVS key (unused for LFS ops)
    void        *buf;          // caller-owned in/out buffer (PSRAM is fine)
    size_t       len;          // in: buf capacity / data length; out: bytes written
    uint8_t      u8val;        // for FP_NVS_*_U8
    esp_err_t    result;       // out
    TaskHandle_t caller;       // for direct-to-task notify reply
} flash_req_t;

esp_err_t flash_proxy_init(void);          // creates queue + internal-stack task
esp_err_t flash_proxy_call(flash_req_t *req, TickType_t timeout);
```

`flash_proxy_call()` fills `req.caller = xTaskGetCurrentTaskHandle()`, sends
`req` (by pointer — the struct can live on the caller's own stack, PSRAM or
not, since it's just a plain memory write/queue copy, no flash involved) via
`xQueueSend`, then does `xTaskNotifyWait(..., timeout)`. The proxy task loops
on `xQueueReceive`, dispatches on `op`, performs the *actual* `nvs_*`/`fopen`
call using its **own** internal stack, sets `req->result`, and replies with
`xTaskNotifyGive(req->caller)`.

The proxy task itself:

```c
static void flash_proxy_task(void *arg) {
    flash_req_t *req;
    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            switch (req->op) {
                case FP_NVS_GET_STR: /* nvs_open/get_str/close */ ...
                ...
            }
            xTaskNotifyGive(req->caller);
        }
    }
}
```

Created once, at boot, with a small **internal-RAM** stack (~4–6 KB is ample
— NVS/LittleFS call chains are not deep) using plain `xTaskCreate` (never
`psram_task_create`). One task; NVS already serializes all access internally
via its own lock, so a single-threaded proxy doesn't introduce a new
bottleneck — it just makes the existing serialization explicit and visible.

### 5.2 Why a queue, not a literal socket

The user's framing was "communicate via a socket." A FreeRTOS queue +
task-notify round trip is the right-sized tool here and is what "socket" is
standing in for conceptually:

- No TCP/IP stack dependency (works before Wi-Fi is even up, e.g. loading
  saved BLE pairing credentials at boot).
- Deterministic, sub-microsecond dispatch — no serialization/parsing
  overhead.
- Trivial to reason about ownership: the request struct lives on the
  caller's stack for the duration of the call; no dynamic allocation needed
  in the common case.

A real loopback socket (lwIP `AF_INET`/`AF_UNIX`-style) would add stack
buffering, dispatch overhead, and — ironically — its own internal-RAM socket
buffers, for no benefit over a queue in a single-binary, single-address-space
firmware. Not recommended.

### 5.3 Buffer safety — bounce through an internal scratch buffer

To avoid depending on exactly when ESP-IDF re-enables cache inside
`nvs_get_str`/`esp_partition_read`, the proxy task should **not** pass the
caller's buffer pointer directly into `nvs_get_*`/`fread` if that pointer
might be in PSRAM. Instead:

1. Proxy reads into a small **static internal-RAM scratch buffer** it owns
   (sized to the largest value we store — a few hundred bytes for NVS
   strings, up to the uploader's ~4.5 KB state JSON for the LittleFS case,
   chunked if needed).
2. Only *after* the flash call returns (cache guaranteed re-enabled by
   normal control flow) does the proxy `memcpy` from its internal scratch
   into the caller's `req->buf`, which may be anywhere, including PSRAM.

This makes the design correct by construction rather than by an
implementation-detail assumption about ESP-IDF internals, at the cost of one
extra small `memcpy` per call — negligible given call frequency (pairing
once per BLE reconnect, config saves are human-interaction-rate, state saves
are per processed day).

### 5.3a Recommended simplification: function-shipping instead of op-codes

The op-enum RPC above (§5.1) has a hidden cost: real call sites are
*sequences*, not single ops. `nvs_save_pairing` sets four keys + commit;
`netprov_load_config` reads a hostname plus up to N SSID/password pairs.
With op-code RPC these become either many queue round-trips per logical
operation or an ever-growing list of bespoke compound ops.

**Better: ship the function, not the operation.** The proxy becomes a
generic "run this on an internal stack" service:

```c
// internal_call.h — ~100 lines total implementation
typedef esp_err_t (*internal_call_fn_t)(void *arg);
esp_err_t internal_call(internal_call_fn_t fn, void *arg, TickType_t timeout);
```

Call sites keep their existing logic intact, wrapped in a thunk:

```c
/* as11_ble.c — existing nvs_open/set x4/commit body moves into the thunk
 * unchanged; only the entry point changes. */
static esp_err_t save_pairing_thunk(void *arg) {
    const pairing_info_t *p = arg;
    /* ... existing nvs_open / nvs_set_str x4 / nvs_commit / nvs_close ... */
}

esp_err_t nvs_save_pairing(...) {
    pairing_info_t info = { ... };
    return internal_call(save_pairing_thunk, &info, pdMS_TO_TICKS(5000));
}
```

Why this is better than op-code RPC:

- **One queue hop per logical transaction**, regardless of how many NVS
  keys or file operations it contains.
- **No op-enum to maintain** — new storage needs never touch the proxy
  mechanism, they just write a new thunk.
- **Minimal migration diff** — existing function bodies move into thunks
  nearly verbatim instead of being decomposed into request structs.
- The buffer-safety argument (§5.3) still holds: the *thunk* executes
  entirely on the proxy's internal stack, so its locals and the full flash
  call chain are safe; caller-owned PSRAM data buffers are only touched by
  `memcpy`s that occur while cache is enabled.

What is lost vs. op-code RPC: the "only `flash_proxy.c` includes `nvs.h`"
compile-time guarantee (§7.2), since thunks live in their own subsystems.
Mitigation: confine `nvs.h` includes to one small `*_store.c` file per
subsystem whose documented contract is "every function here is a thunk,
only ever invoked via `internal_call`", plus the grep-based lint from
§7.2. Slightly weaker than a hard compile error, but the 5× reduction in
mechanism/migration complexity is worth it. **Recommend function-shipping
as the implementation approach; treat §5.1's op-code RPC as the fallback
if the weaker include-hygiene guarantee proves insufficient in practice.**

### 5.4 Call-site migration inventory

All current direct flash touches, grep-verified in this session:

| File | Function(s) | Called from | Type |
|---|---|---|---|
| `main/as11_ble.c` | `nvs_save_pairing`, `nvs_get_str_opt` (via `reconnect_task`, `confirm_task`), `as11_ble_is_paired`, `as11_ble_get_paired_info`, `as11_ble_forget` | BLE pairing tasks, HTTP `/api/status` & `/api/ble/*` handlers | NVS |
| `main/net_provision.c` | `netprov_load_config`, `netprov_save_config` | HTTP `/save`, `/api/status`, boot | NVS |
| `main/device_settings.c` | `device_settings_get_json`/`_load`, `_save` | HTTP `/api/device/settings` | NVS |
| `main/time_sync.c` | `time_sync_set_timezone`, `time_sync_get_timezone`, `time_sync_get_tz_name` | HTTP `/save`, boot, `/api/status` | NVS |
| `components/uploader/uploader.c` | `uploader_get_config`/`_save_config` (NVS "uploader" namespace) | HTTP `/api/uploads/config` | NVS |
| `components/uploader/uploader_state.c` | `uploader_state_load`, `uploader_state_save` | uploader task (`process_day`, `uploader_reset_state`), boot | LittleFS (`storage` partition) |

Every one of these is presently called **inline**, on whatever task happens
to invoke it — which today includes the **HTTP server worker task itself**
(that's what caused the mid-session crash: the `/api/status` handler calling
`as11_ble_is_paired()` while the httpd task had an unintended PSRAM stack).
Migrating all six call sites to route through `flash_proxy_call()` removes
flash access from every caller's execution context, including the HTTP
worker task's.

## 6. What this unlocks

Once no application task calls flash APIs directly, these become safe to
give PSRAM stacks (via `psram_task_create`), which is not true today:

- `as11_ble.c`: `reconnect_task`, `confirm_task` (currently reverted to
  internal stacks — this was the day-1 fix for the first bootloop).
- `components/uploader/uploader.c`: the `uploader` task itself (currently
  reverted to `xTaskCreatePinnedToCore` — the day-1 fix for the second
  crash).
- **The HTTP server worker task is explicitly NOT part of this saving** —
  §7.3 recommends keeping it internal permanently as defense-in-depth even
  after migration. Do not count its 8 KB in the total below.

### 6.1 Revised, realistic savings estimate

| Item | Change |
|---|---|
| `reconnect_task` → PSRAM stack | +8 KB internal RAM freed |
| `confirm_task` → PSRAM stack | +8 KB internal RAM freed |
| `uploader` task → PSRAM stack | +12 KB internal RAM freed |
| Flash proxy task's own internal stack (new) | −5 KB |
| Internal scratch buffer for buffer-safety bounce (§5.3, sized for the ~4.5 KB LittleFS state JSON) | −4.5 KB |
| **Net from task-stack migration** | **≈ 18.5 KB** |

The **larger** saving is not from task stacks at all — it's from finally
being able to lower `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (see §7.4.1).
That threshold currently forces *every* allocation up to 16384 B — not just
task stacks, but general heap allocations (cJSON buffers, HTTP response
buffers, BLE GATT buffers, etc.) — into internal RAM project-wide. Once the
proxy removes the need for that blanket floor, dropping it to ~4096 B (just
above the largest IDF-driver-owned stack we don't control, see §7.4) frees
up internal RAM for *all* medium-sized allocations across the whole
firmware, not just the handful of task stacks listed above. That number is
hard to estimate without instrumentation (depends on runtime allocation
patterns), but is very plausibly larger than the ~18.5 KB task-stack saving
itself — profile after implementation rather than trusting an a priori
estimate here.

## 7. Risks, caveats, and open questions

### 7.1 Single point of failure / deadlock hygiene

Every flash-touching call becomes a blocking RPC. Rule: **no task may hold a
mutex across a `flash_proxy_call()`** unless that mutex can never be needed
by the proxy task itself (it never will — the proxy never calls back into
application code). Still, always use a bounded timeout
(`flash_proxy_call(&req, pdMS_TO_TICKS(5000))`), not `portMAX_DELAY`, so a
bug in the proxy degrades a caller to an error return instead of hanging a
BLE or HTTP thread forever.

### 7.2 Maintainability — this is a structural invariant, not a lint rule (yet)

The entire value of this design rests on "nothing outside `flash_proxy.c`
calls `nvs_*`/`esp_partition_*`/LittleFS `fopen` on the `storage` partition
ever again." That's easy to violate by accident in future PRs. Suggested
guardrails:
- Keep `nvs.h`/`nvs_flash.h` `#include`s confined to `flash_proxy.c` only —
  remove them from `as11_ble.c`, `net_provision.c`, `device_settings.c`,
  `time_sync.c`, `uploader.c`, `uploader_state.c` once migrated, so a stray
  `nvs_get_str()` call becomes a compile error, not a runtime crash.
- A one-line CI/pre-commit grep (`grep -rl 'nvs_\(open\|get_\|set_\|commit\|erase\)' main/ components/ --exclude=flash_proxy.c`)
  as a cheap regression guard, if the project ever adds CI.

### 7.3 The HTTP worker task is a shared resource — treat it as higher risk

Even after full migration, **any** future handler (including third-party
middleware or something added carelessly later) that calls flash directly
would silently reintroduce the crash, and it'd be on the shared httpd
thread, affecting *all* requests. Recommendation: migrate the six call sites
above regardless (removes the fragility for BLE/uploader tasks, which is the
biggest win), but **keep the HTTP worker task's stack internal** as
defense-in-depth even after migration, rather than chasing the last ~8 KB.
This is a judgment call — flag for discussion rather than deciding
unilaterally here.

### 7.4 Tasks we don't control

The Wi-Fi driver task (`wifi driver task`, stack 6656 B per boot log), the
NimBLE host task, and the `esp_timer` service task are created inside
closed-source or vendored ESP-IDF components via their own internal
`xTaskCreate`/`xTaskCreatePinnedToCore` calls. We cannot apply
`psram_task_create` to them. Their stack placement is governed purely by the
global `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` /
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` Kconfig knobs. **Regardless of whether
this proxy is implemented, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` must stay
at or above the largest such driver stack size** (the httpd worker's
8192 B is the largest known internal-only requirement in our own code; IDF
driver tasks are all smaller, ~6656 B for Wi-Fi), or those tasks risk the
same crash class with no code-level fix available to us. This proxy pattern
narrows the *application-level* risk surface; it does not remove the need
for a safe global Kconfig floor.

#### 7.4.1 Critical prerequisite: `ALLOW_EXT_MEM` must be `y`, not `n`

`psram_task_create` (in `main/psram_task.c`) allocates the stack via
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` and passes it to
`xTaskCreateStaticPinnedToCore`. FreeRTOS's own
`xPortcheckValidStackMem` check (in
`freertos_tasks_c_additions.h`) will **reject** a PSRAM stack buffer
unless `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` — this is exactly what
caused the most recent bootloop in this session (the config was flipped to
`n` as a (mistaken) attempt to protect the httpd worker task, which instead
broke every `psram_task_create` call site with an assert at boot).

This means the two Kconfig knobs must move **together**, not independently:

- `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` (required for
  `psram_task_create` to work at all).
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` set high enough that plain
  `xTaskCreate`/`xTaskCreatePinnedToCore` calls we don't control (httpd
  worker at 8192 B, Wi-Fi driver at 6656 B) still land internal by default.
  **Once the proxy is fully in place, this can drop from 16384 to ~4096
  (just above the largest uncontrolled driver stack), not to 2048** — 2048
  is below the httpd worker's 8192 B stack size and would put it in PSRAM
  by default, silently reintroducing the exact crash this whole session has
  been fighting, since the httpd worker is explicitly being kept off this
  migration path (§7.3).

### 7.5 Open questions

- Should the proxy handle LittleFS writes as one atomic "read whole file /
  write whole file" RPC (simple, matches current `uploader_state.c` usage
  exactly), or a more general streaming/chunked API? Given current usage is
  always whole-file (≤4.5 KB), the simple whole-file RPC is likely
  sufficient and much easier to reason about — recommend this unless state
  size grows significantly.
- Is one proxy task enough, or should NVS and LittleFS be split into two
  tasks to avoid one slow LittleFS write (rename + fsync) blocking a
  time-sensitive NVS read (e.g. BLE reconnect reading pairing info)? Given
  current call frequency (LittleFS saves only happen once per processed
  day, NVS reads happen at BLE reconnect / HTTP request time), contention
  is expected to be negligible — recommend starting with one task and
  splitting only if profiling shows otherwise.

## 8. Suggested implementation phases (for a future session)

0. **Evaluate `CONFIG_SPI_FLASH_AUTO_SUSPEND` (§5.0) first** — identify
   the flash chip, enable the option, soak-test a PSRAM-stack task doing
   NVS writes. If it passes, stop here: no proxy needed at all.
1. Add the in-RAM paired-state cache (§4.1) — independent, ships first,
   removes 3 of 6 call sites and the hot-path latency question entirely.
   Worth doing even if §5.0 succeeds (removes NVS from the HTTP hot path).
2. Build the `internal_call` function-shipping service (§5.3a) as a
   standalone component (~100 lines); unit-test in isolation (e.g. via a
   temporary debug HTTP endpoint) before touching any existing call site.
3. Migrate `time_sync.c` and `device_settings.c` first (smallest, lowest-risk
   call sites, both already only called from contexts we fully control).
4. Migrate `net_provision.c` (`netprov_load_config`/`_save_config`) and
   `uploader.c`'s NVS config get/save.
5. Migrate `uploader_state.c` (LittleFS) and re-promote the `uploader` task
   to `psram_task_create`.
6. Migrate `as11_ble.c`'s remaining NVS call sites (writes + boot load);
   re-promote `reconnect_task` and `confirm_task` to `psram_task_create`.
7. Confine `nvs.h`/`nvs_flash.h` includes to the per-subsystem `*_store.c`
   thunk files (§5.3a) — or to `flash_proxy.c` alone if the op-code RPC
   fallback is chosen; add the grep lint from §7.2 either way.
8. Re-audit whether the HTTP worker task stack should move to PSRAM (§7.3)
   — separate decision, not bundled with this migration. Default:
   leave it internal.
9. Re-tune `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` from 16384 down to ~4096
   (§7.4.1) — **keep `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`**, do not
   flip it to `n`. Profile actual internal-RAM headroom gained afterward
   rather than relying on the pre-implementation estimate in §6.1.

## 9. Changelog

- 2026-07-08: Initial draft, written after three bootloop incidents during
  the RAM/endpoint optimization session (BLE reconnect NVS crash, uploader
  LittleFS crash, HTTP status-handler NVS crash via unintended PSRAM httpd
  stack).
- 2026-07-08: Revised after review — corrected savings estimate (§6.1,
  ~18.5 KB from task stacks, not 28-36 KB; larger saving is actually from
  lowering `ALWAYSINTERNAL` project-wide, not yet quantified), added
  in-RAM cache alternative for read-only BLE call sites (§4.1) to remove
  hot-path latency and shrink the migration surface, and documented the
  `ALLOW_EXT_MEM`/`ALWAYSINTERNAL` prerequisite relationship (§7.4.1) that
  directly explains the most recent bootloop in this session.
- 2026-07-08: Second revision — added §5.0 (`CONFIG_SPI_FLASH_AUTO_SUSPEND`
  as a possible zero-code fix to evaluate before building anything) and
  §5.3a (function-shipping `internal_call` service recommended over the
  op-code RPC, which is demoted to fallback — one queue hop per logical
  transaction, no op-enum, near-verbatim call-site migration).
