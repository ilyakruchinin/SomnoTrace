#!/usr/bin/env python3
"""Structural contracts for session-writer init/start failure resilience."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/session_writer.c").read_text(encoding="utf-8")


def function_body(name: str) -> str:
    match = re.search(
        rf"^[\w][\w\s*]*\b{name}\s*\([^;{{}}]*\)\s*\{{",
        SOURCE,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"missing function: {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(SOURCE) and depth:
        if SOURCE[cursor] == "{":
            depth += 1
        elif SOURCE[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function: {name}")
    return SOURCE[match.end():cursor - 1]


# Workers self-park before touching shared queues. Init observes that state, so
# deleting a partially-created worker cannot race queue/semaphore destruction.
storage_worker = function_body("sw_storage_task")
post_worker = function_body("sw_post_task")
assert storage_worker.index("vTaskSuspend(NULL)") \
       < storage_worker.index("xQueueReceive(s_storage_q")
assert post_worker.index("vTaskSuspend(NULL)") \
       < post_worker.index("xQueueReceive(s_post_q")

park = function_body("sw_wait_startup_parked")
assert "eTaskGetState(task) != eSuspended" in park
assert "vTaskDelay(1)" in park

unwind = function_body("session_writer_init_unwind")
assert unwind.index("s_ready = false") \
       < unwind.index("psram_task_delete(s_post_task)") \
       < unwind.index("psram_task_delete(s_storage_task)") \
       < unwind.index("vQueueDelete(s_post_q)") \
       < unwind.index("vQueueDelete(s_storage_q)") \
       < unwind.index("vSemaphoreDelete(s_active_mutex)")
for global_name in (
    "s_post_task", "s_storage_task", "s_post_q", "s_storage_q",
    "s_active_mutex",
):
    assert f"{global_name} = NULL" in unwind
assert "vTaskDelete(" not in unwind

init = function_body("session_writer_init")
storage_create = init.index("s_storage_task = psram_task_create(")
storage_park = init.index("sw_wait_startup_parked(s_storage_task)")
post_create = init.index("s_post_task = psram_task_create(")
post_park = init.index("sw_wait_startup_parked(s_post_task)")
ready = init.index("s_ready = true")
assert storage_create < storage_park < post_create < post_park < ready
assert ready < init.index("vTaskResume(s_storage_task)")
assert ready < init.index("vTaskResume(s_post_task)")
assert init.count("goto no_mem") == 3
failure_tail = init[init.index("no_mem:"):]
assert failure_tail.index("session_writer_init_unwind()") \
       < failure_tail.index("return ESP_ERR_NO_MEM")

# Storage ownership is admitted before any session/batch allocation. The
# temporary session owns that claim until publication or centralized discard.
start = function_body("session_writer_start")
claim = start.index("sd_storage_recording_try_begin()")
session_alloc = start.index("heap_caps_calloc(1, sizeof(session_writer_t)")
fill_mutex_alloc = start.index("xSemaphoreCreateMutex()", session_alloc)
batch_alloc = start.index("batch_pool_create(s)", fill_mutex_alloc)
assert start.index("sw_start_intent_begin()") < claim
assert "sd_storage_reserve_for_recording_cached()" in start
assert "sw_start_intent_end()" in start
assert claim < session_alloc < fill_mutex_alloc < batch_alloc
assert start.count("sd_storage_recording_try_begin()") == 1
assert start.index("s->recording_claim_held = true") \
       < fill_mutex_alloc

# The only pre-session allocation failure releases its local claim directly;
# every later allocation/arbitration/queue failure uses one complete cleanup.
alloc_failure = start[session_alloc:start.index("s->recording_claim_held = true")]
assert "sd_storage_recording_end()" in alloc_failure
assert start.count("session_start_discard(s)") == 4

discard = function_body("session_start_discard")
assert discard.index("batch_pool_destroy(s)") \
       < discard.index("vSemaphoreDelete(s->fill_mutex)") \
       < discard.index("sd_storage_recording_end()") \
       < discard.index("free(s)")
assert "s->recording_claim_held = false" in discard

# OPEN is still queued while holding the active-session gate, and the pointer
# is published only after successful admission. Duplicate starts return the
# healthy winner after releasing their own provisional storage claim.
active_lock = start.index("xSemaphoreTake(s_active_mutex", batch_alloc)
duplicate = start.index("if (s_active && s_active->active)", active_lock)
enqueue = start.index("storage_queue_send_open(&cmd, 0)", duplicate)
publish = start.index("s_active = s", enqueue)
unlock = start.index("xSemaphoreGive(s_active_mutex)", publish)
assert active_lock < duplicate < enqueue < publish < unlock

duplicate_branch = start[duplicate:start.index("s->active = true", duplicate)]
assert duplicate_branch.index("xSemaphoreGive(s_active_mutex)") \
       < duplicate_branch.index("session_start_discard(s)") \
       < duplicate_branch.index("return existing")

enqueue_failure = start[
    start.index("if (!storage_queue_send_open(&cmd, 0))"):
    publish
]
assert enqueue_failure.index("xSemaphoreGive(s_active_mutex)") \
       < enqueue_failure.index("session_start_discard(s)") \
       < enqueue_failure.index("return NULL")
assert "crash_diag_note_session(NULL)" in enqueue_failure

print("session writer start resilience contract passed")
