#!/usr/bin/env python3
"""Contracts for atomic raw-recording versus card-reader ownership."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
STORAGE = (ROOT / "main/sd_storage.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/sd_storage.h").read_text(encoding="utf-8")
WRITER = (ROOT / "main/session_writer.c").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if not match:
        raise AssertionError(f"missing function: {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(source) and depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function: {name}")
    return source[match.end() : cursor - 1]


begin = function_body(STORAGE, "sd_storage_recording_begin")
acquire = function_body(STORAGE, "sd_storage_lease_acquire")
release = function_body(STORAGE, "sd_storage_lease_release_unchanged")
mutation_release = function_body(STORAGE, "sd_storage_lease_release")
assert mutation_release.index("sd_storage_content_changed()") < mutation_release.index(
    "sd_storage_lease_release_unchanged(role)")
start = function_body(WRITER, "session_writer_start")
discard = function_body(WRITER, "session_start_discard")

assert "bool sd_storage_recording_begin(void);" in HEADER
assert "s_destructive" in STORAGE

# Recording publishes waiter priority, waits only for a bounded reader handoff,
# and publishes its claim while holding the one arbitration mutex.
lock_at = begin.find("xSemaphoreTake(s_lease_mutex")
waiter_at = begin.find("__atomic_add_fetch(&s_recording_waiters, 1, __ATOMIC_RELEASE)")
destructive_check_at = begin.find("while (s_destructive == 0)")
reader_check_at = begin.find("if (s_uploading == 0)")
claim_at = begin.find("__atomic_add_fetch(&s_recording, 1, __ATOMIC_RELEASE)")
unlock_at = begin.rfind("xSemaphoreGive(s_lease_mutex")
assert -1 not in (
    lock_at, waiter_at, destructive_check_at, reader_check_at, claim_at,
    unlock_at,
)
assert lock_at < waiter_at < destructive_check_at < reader_check_at \
       < claim_at < unlock_at
assert "SD_RECORDING_PRIORITY_WAIT_MS" in begin
assert "__atomic_sub_fetch(&s_recording_waiters, 1, __ATOMIC_RELEASE)" in begin
assert "return claimed" in begin

# UPLOAD and DESTRUCTIVE publish their mutually-exclusive claim beneath that
# same mutex after acquiring the file-operation gate. A recording claim that
# wins first is observed before either role starts card I/O.
upload = acquire[acquire.find("case SD_LEASE_UPLOAD:") :]
upload_sem = upload.find("xSemaphoreTakeRecursive(s_export_sem")
upload_lock = upload.find("xSemaphoreTake(s_lease_mutex")
upload_check = upload.find("s_recording > 0 || sd_storage_recording_pending()")
upload_claim = upload.find("s_uploading++")
assert -1 not in (upload_sem, upload_lock, upload_check, upload_claim)
assert upload_sem < upload_lock < upload_check < upload_claim

destructive = acquire[
    acquire.find("case SD_LEASE_DESTRUCTIVE:") :
    acquire.find("case SD_LEASE_EXPORT:")
]
destructive_sem = destructive.find("xSemaphoreTakeRecursive(s_export_sem")
destructive_lock = destructive.find("xSemaphoreTake(s_lease_mutex")
destructive_check = destructive.find("s_recording > 0 || sd_storage_recording_pending() ||")
destructive_claim = destructive.find("s_destructive++")
assert -1 not in (
    destructive_sem, destructive_lock, destructive_check, destructive_claim
)
assert destructive_sem < destructive_lock < destructive_check < destructive_claim
assert "s_destructive--" in release
assert "s_uploading--" in release

# A writer is never allocated, published or queued without a successful raw-
# recording claim. Refusal therefore has no large session object to unwind;
# every later failure uses the centralized claim-aware discard helper.
claim_at = start.find("sd_storage_recording_try_begin()")
alloc_at = start.find("heap_caps_calloc(1, sizeof(session_writer_t)")
open_at = start.find("storage_queue_send_open(&cmd")
publish_at = start.find("s_active = s")
assert -1 not in (claim_at, alloc_at, open_at, publish_at)
assert claim_at < alloc_at < open_at < publish_at
refused_claim = start[claim_at:alloc_at]
assert "return NULL" in refused_claim
assert "batch_pool_create" not in refused_claim
assert start.count("session_start_discard(s)") == 4
for cleanup in (
    "batch_pool_destroy(s)",
    "vSemaphoreDelete(s->fill_mutex)",
    "sd_storage_recording_end()",
    "free(s)",
):
    assert cleanup in discard, f"start discard misses {cleanup}"

# Compact interleaving model: because all three claim transitions execute
# under the same mutex, no possible winner can overlap recording with either
# a reader or destructive owner.
states = {(0, 0, 0)}  # recording, uploading, destructive
for _ in range(12):
    next_states = set(states)
    for recording, uploading, destructive_count in states:
        if not uploading and not destructive_count:
            next_states.add((recording + 1, uploading, destructive_count))
        if not recording and not destructive_count:
            next_states.add((recording, uploading + 1, destructive_count))
        if not recording and not uploading and not destructive_count:
            next_states.add((recording, uploading, destructive_count + 1))
        if recording:
            next_states.add((recording - 1, uploading, destructive_count))
        if uploading:
            next_states.add((recording, uploading - 1, destructive_count))
        if destructive_count:
            next_states.add((recording, uploading, destructive_count - 1))
    states = next_states
    for recording, uploading, destructive_count in states:
        assert not (recording and uploading)
        assert not (recording and destructive_count)
        assert not (uploading and destructive_count)

print("SD recording arbitration contract passed")

# Exercise the production state transitions and deadline, not just a model of
# them: an old reader outlives one attempt, intent prevents reacquisition, then
# the retained start claims the card when that reader actually releases it.
import subprocess
import tempfile


def production_function(name: str) -> str:
    match = re.search(r"^(?:static )?[\w\s*]+\b" + name + r"\([^;]*?\)\s*\{", STORAGE, re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (STORAGE[end] == "{") - (STORAGE[end] == "}")
        end += 1
    return STORAGE[match.start():end] + "\n"


fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "sd_storage.h"
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
#define SD_RECORDING_PRIORITY_WAIT_MS 500U
#define SD_RECORDING_PRIORITY_POLL_MS 5U
#define ESP_LOGW(...) ((void)0)
typedef uint32_t TickType_t;
static int s_export_sem=2,s_lease_mutex=1;
static int s_recording,s_recording_waiters,s_uploading,s_destructive;
static uint32_t s_recording_intents,s_content_generation=7;
static int mutex_depth,export_depth;
static bool mutex_busy,s_mounted=true,cache_valid;
static uint64_t cached_free;
bool sd_storage_get_cached_free(uint64_t *out,uint64_t *total)
{ (void)total; *out=cached_free; return cache_valid; }
static int64_t now_us,release_at;
static void lease_init_once(void) {}
static int xSemaphoreTake(int sem, TickType_t ticks)
{ if(mutex_busy){assert(ticks==0);return 0;}assert(sem==1 && !mutex_depth);++mutex_depth;return pdTRUE; }
static void xSemaphoreGive(int sem)
{ assert(sem==1 && mutex_depth==1);--mutex_depth; }
static int xSemaphoreTakeRecursive(int sem,TickType_t ticks)
{ (void)ticks;assert(sem==2);++export_depth;return pdTRUE; }
static void xSemaphoreGiveRecursive(int sem)
{ assert(sem==2 && export_depth>0);--export_depth; }
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(TickType_t ticks)
{
    assert(!mutex_depth && sd_storage_recording_pending());
    now_us+=(int64_t)ticks*1000;
    if(release_at && now_us>=release_at) {
        release_at=0;sd_storage_lease_release(SD_LEASE_UPLOAD);
    }
}
'''
fixture += re.search(r"^#define SD_FLOOR_BYTES[^\n]*", STORAGE, re.M).group(0) + "\n"
fixture += "".join(production_function(name) for name in (
    "sd_storage_content_generation", "sd_storage_content_changed",
    "sd_storage_recording_intent_begin", "sd_storage_recording_intent_end",
    "sd_storage_recording_pending", "sd_storage_recording_active",
    "sd_storage_recording_begin", "sd_storage_recording_end",
    "sd_storage_recording_try_begin", "sd_storage_reserve_for_recording_cached",
    "sd_storage_lease_acquire", "sd_storage_lease_release_unchanged",
    "sd_storage_lease_release"))
fixture += r'''
int main(void) {
    assert(sd_storage_lease_acquire(SD_LEASE_UPLOAD,0));
    sd_storage_recording_intent_begin();assert(sd_storage_recording_pending());
    assert(!sd_storage_recording_begin());
    assert(now_us==500000 && !s_recording_waiters && s_uploading==1);
    assert(sd_storage_recording_pending()); /* no gap between retries */
    assert(!sd_storage_lease_acquire(SD_LEASE_UPLOAD,0));
    assert(!sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE,0));
    assert(export_depth==1 && !mutex_depth && !sd_storage_recording_active());
    /* No force-release: old reader must close before recording may claim. */
    now_us=650000;sd_storage_lease_release(SD_LEASE_UPLOAD);
    assert(sd_storage_recording_begin() && now_us==650000);
    assert(sd_storage_recording_pending() && sd_storage_recording_active());
    sd_storage_recording_intent_end();assert(!sd_storage_recording_pending());
    assert(!sd_storage_lease_acquire(SD_LEASE_UPLOAD,0));
    sd_storage_recording_end();assert(!sd_storage_recording_active());
    assert(sd_storage_content_generation()==8 && !export_depth && !mutex_depth);
    /* Multiple pending owners and cancellation remain paired and saturate. */
    sd_storage_recording_intent_begin();sd_storage_recording_intent_begin();
    sd_storage_recording_intent_end();assert(sd_storage_recording_pending());
    sd_storage_recording_intent_end();sd_storage_recording_intent_end();
    assert(!sd_storage_recording_pending() && !s_recording_intents);
    assert(sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE,0));
    sd_storage_recording_intent_begin();
    assert(!sd_storage_recording_begin()); /* destructive lease is not revoked */
    sd_storage_lease_release_unchanged(SD_LEASE_DESTRUCTIVE);
    sd_storage_recording_intent_end();
    assert(sd_storage_lease_acquire(SD_LEASE_UPLOAD,0));
    int64_t start=now_us;release_at=now_us+300000;
    assert(sd_storage_recording_begin());
    assert(now_us-start==300000 && !sd_storage_recording_pending());
    sd_storage_recording_end();
    assert(sd_storage_lease_acquire(SD_LEASE_UPLOAD,0));
    start=now_us;assert(!sd_storage_recording_begin());
    assert(now_us-start==500000 && !sd_storage_recording_pending());
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    assert(!s_recording && !s_uploading && !s_destructive && !export_depth && !mutex_depth);
    start=now_us;mutex_busy=true;
    assert(!sd_storage_recording_try_begin() && !s_recording);mutex_busy=false;
    s_lease_mutex=0;assert(!sd_storage_recording_try_begin());s_lease_mutex=1;
    s_export_sem=0;assert(!sd_storage_recording_try_begin());s_export_sem=2;
    assert(sd_storage_lease_acquire(SD_LEASE_UPLOAD,0));
    sd_storage_recording_intent_begin();
    assert(!sd_storage_recording_try_begin() && sd_storage_recording_pending());
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    assert(sd_storage_recording_try_begin() && sd_storage_recording_pending());
    sd_storage_recording_intent_end();sd_storage_recording_end();
    assert(now_us==start && !mutex_depth && !export_depth);
    assert(sd_storage_reserve_for_recording_cached());
    cache_valid=true;cached_free=SD_FLOOR_BYTES-1;
    assert(!sd_storage_reserve_for_recording_cached());
    cached_free=SD_FLOOR_BYTES;assert(sd_storage_reserve_for_recording_cached());
    s_mounted=false;assert(!sd_storage_reserve_for_recording_cached());
    puts("Production SD arbitration: retained intent spans 500ms retry, excludes new readers/maintenance, waits for real release, balances stop/success and owner references");
}
'''
with tempfile.TemporaryDirectory(prefix="somno-recording-admission-") as temp:
    path = Path(temp)
    (path / "test.c").write_text(fixture)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "main"), "-I" + str(ROOT / "scripts/test_include"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
