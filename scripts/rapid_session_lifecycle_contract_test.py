#!/usr/bin/env python3
"""Contracts for repeated therapy START/STOP without FD or writer retention."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
WRITER = (ROOT / "main/session_writer.c").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")


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
    return source[match.end(): cursor - 1]


request = function_body(WRITER, "sw_request_finalize")
start = function_body(WRITER, "session_writer_start")
send_open = function_body(WRITER, "storage_queue_send_open")
finish = function_body(WRITER, "storage_finish_and_dispatch")
post = function_body(WRITER, "sw_post_task")
worker = function_body(WRITER, "sw_storage_task")
stream = function_body(WRITER, "session_writer_try_stream_data_raw")
notify = function_body(WRITER, "session_writer_on_notification")

# A stop is terminal storage work, never a slow post-pipeline job.  Regular
# work preserves one queue slot so the sole BLE consumer never waits at STOP.
assert "SW_CMD_FINALIZE" in request
assert "SW_STORAGE_TERMINAL_RESERVE 1" in WRITER
assert "xQueueSend(s_storage_q, &fin, 0)" in request
assert "xQueueSend(s_storage_q, &fin, portMAX_DELAY)" not in request
assert "xQueueSend(s_post_q" not in request
assert "storage_finalize(" not in request
assert "free(s)" not in request
assert "s->end_time_us = esp_timer_get_time()" in request
assert request.find("s_active != s") < request.find("s->finalize_requested"), (
    "an obsolete borrowed pointer is dereferenced before active-owner validation"
)

# The lifecycle gate orders old FINALIZE before new OPEN, and OPEN is visible
# to the worker before any producer can discover the new pointer.
assert request.find("xSemaphoreTake(s_active_mutex") < request.find(
    "xQueueSend(s_storage_q, &fin"
)
assert start.find("xSemaphoreTake(s_active_mutex") < start.find(
    "storage_queue_send_open(&cmd"
)
assert start.find("storage_queue_send_open(&cmd") < start.find("s_active = s")
assert "uxQueueSpacesAvailable(s_storage_q)" in send_open
assert ">\n                SW_STORAGE_TERMINAL_RESERVE" in send_open
assert "xQueueSend(s_storage_q, cmd, 0)" in send_open

# Post work owns only copied values.  It cannot retain seven FILE handles,
# request a second FINALIZE, or free a writer object.
post_job = re.search(
    r"/\* ── Post-stop pipeline job.*?typedef struct \{(?P<body>.*?)"
    r"\}\s*sw_post_job_t;",
    WRITER,
    re.DOTALL,
)
assert post_job, "missing post-job definition"
post_fields = post_job.group("body")
assert "session_writer_t" not in post_fields
assert "session_dir" in post_fields and "session_id" in post_fields
assert "SW_CMD_FINALIZE" not in post
assert "free(s)" not in post
assert "post_wait_for_storage_quiet()" in post

# Storage closes/finalises first, copies metadata second, and frees the sole
# live writer owner last.  Queue overflow degrades to a durable day rebuild.
assert finish.find("storage_finalize(s, cmd)") < finish.find(
    "xQueueSend(s_post_q"
) < finish.rfind("free(s)")
finalize = function_body(WRITER, "storage_finalize")
assert "storage_finish_and_dispatch(finished, &cmd)" in worker

# Producer/event paths pin active->fill, preventing a BATCH/EVENT from landing
# behind FINALIZE and dereferencing a freed session.
assert "active_session_lock()" in stream
assert "session_writer_get_active()" not in stream
assert "active_session_lock()" in notify
assert "session_writer_get_active()" not in notify
assert "active_session_try_lock(&lifecycle_sampled)" in worker
assert "if (!lifecycle_sampled) continue" in worker
try_lock = function_body(WRITER, "active_session_try_lock")
assert "xSemaphoreTake(s_active_mutex, 0)" in try_lock
assert "xSemaphoreTake(s_active->fill_mutex, 0)" in try_lock
assert "portMAX_DELAY" not in try_lock
assert "producer_commit(" not in worker

# Both real therapy-start publishers must win the display lifecycle gate before
# starting recording or alert state. If OTA has already committed its restart,
# the event/heuristic is abandoned instead of reopening the race window.
gate_call = "if (!bsp_display_set_therapy_active(true))"
assert gate_call in stream
assert stream.index(gate_call) < stream.index("session_writer_start()")
assert "therapy recovery ignored: restart already committed" in stream
assert gate_call in notify
therapy_start = notify[notify.index("if (ev_type == AS11_EV_THERAPY_START)"):]
assert therapy_start.index(gate_call) < therapy_start.index(
    'crash_diag_note_activity("therapy_start")'
) < therapy_start.index("therapy_alert_on_therapy_start()") \
  < therapy_start.index("session_writer_start()")
assert "TherapyStart ignored: restart already committed" in therapy_start

# The worker refuses to operate on a command whose writer does not own the
# open descriptors.  OPEN is rejected even when it repeats the same pointer.
assert "if (file_owner != NULL)" in worker
for command in ("BATCH", "EVENT", "FINALIZE"):
    assert f"lifecycle violation: {command} owner mismatch" in worker

# A failed regular enqueue leaves the periodic commit due so it retries after
# the worker has drained capacity.
assert "SW_CMD_COMMIT" in worker and "s->commit_queued = true" in worker
barrier = worker[worker.index("case SW_CMD_COMMIT:"):worker.index("case SW_CMD_FINALIZE:")]
assert barrier.index("storage_commit(cmd.s)") < barrier.index("last_commit_us = esp_timer_get_time()")
assert "xQueuePeek" not in worker
# Actual EVENT -> EVENT -> COMMIT queue execution lives in session_checkpoint_barrier_test.py.

# Exhaust the compact admission model, including the former 23-queued-items
# counterexample.  Whenever a session is published active, one terminal slot
# must remain available regardless of regular sends, drains and restarts.
QUEUE_LEN = 24
RESERVE = 1
states = {(0, False)}  # (queued commands, active session published)
for _ in range(100):
    next_states = set(states)
    for queued, active in states:
        if queued:
            next_states.add((queued - 1, active))
        if active and QUEUE_LEN - queued > RESERVE:
            next_states.add((queued + 1, active))       # BATCH/EVENT
        if active:
            assert queued < QUEUE_LEN, "active session lost its FINALIZE slot"
            next_states.add((queued + 1, False))       # FINALIZE
        elif QUEUE_LEN - queued > RESERVE:
            next_states.add((queued + 1, True))        # OPEN + publish
    states = next_states
    for queued, active in states:
        assert 0 <= queued <= QUEUE_LEN
        assert not active or queued < QUEUE_LEN

# Full-after-STOP must drain twice before OPEN can consume a slot; the new
# session can then STOP immediately without blocking or dropping FINALIZE.
queued, active = 23, True
queued, active = queued + 1, False
queued -= 1
assert QUEUE_LEN - queued == RESERVE and not active
queued -= 1
assert QUEUE_LEN - queued > RESERVE
queued, active = queued + 1, True
assert queued < QUEUE_LEN
queued, active = queued + 1, False
assert queued == QUEUE_LEN and not active

assert "rapid_session_lifecycle_contract_test.py" in HOST_TEST

print("rapid session lifecycle contract passed")
