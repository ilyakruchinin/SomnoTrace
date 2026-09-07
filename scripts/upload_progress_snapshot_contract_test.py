#!/usr/bin/env python3
"""Keep concurrent upload progress readers off the mutable upload index."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SCHED = (ROOT / "components/uploader/upload_sched.c").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"^[\w][\w\s*]*\b{name}\s*\([^;{{}}]*\)\s*\{{",
        source,
        re.MULTILINE,
    )
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


snapshot = function_body(SCHED, "upload_sched_progress_snapshot")
refresh = function_body(SCHED, "refresh_index_progress_cache")
init = function_body(SCHED, "upload_sched_init")
invalidate = function_body(SCHED, "service_pending_invalidation")
scan = function_body(SCHED, "do_scan")
run_pass = function_body(SCHED, "run_pass")
sched_task = function_body(SCHED, "sched_task")

# Display/httpd/WebSocket callers may run concurrently with reset and scan.
# Their snapshot must be a bounded copy under the runtime lock, never a walk
# through upload_index's replaceable s_days pointers.
for forbidden in (
    "upload_index_",
    "opendir(",
    "fopen(",
    "heap_caps_malloc(",
    "malloc(",
):
    assert forbidden not in snapshot, f"progress snapshot performs work: {forbidden}"
assert "xSemaphoreTake(s_lock, portMAX_DELAY)" in snapshot
assert "dst->days_done = src->days_done" in snapshot
assert "dst->days_total = src->days_total" in snapshot
assert "out->max_days = progress_max_days" in snapshot

# Only the scheduler-side cache producer may aggregate the index. It computes
# into locals, then publishes the whole set while holding the snapshot lock.
assert "upload_index_backend_progress(" in refresh
assert "s_rt[i].days_done = done[i]" in refresh
assert "s_rt[i].days_total = total[i]" in refresh
assert "s_progress_max_days = max_days" in refresh
assert refresh.count("xSemaphoreTake(s_lock, portMAX_DELAY)") >= 2

# Prime after the boot-time index load, and refresh after every scheduler path
# that reconciles, uploads, or clears the in-memory index.
assert "refresh_index_progress_cache();" in init
assert "refresh_index_progress_cache();" in invalidate
assert scan.count("refresh_index_progress_cache();") >= 2
assert run_pass.count("refresh_index_progress_cache();") >= 2
assert re.search(
    r"upload_index_clear\(\);\s*refresh_index_progress_cache\(\);",
    sched_task,
), "reset publishes a stale progress cache"

assert "upload_progress_snapshot_contract_test.py" in HOST_TEST

print("upload progress snapshot contract passed")
