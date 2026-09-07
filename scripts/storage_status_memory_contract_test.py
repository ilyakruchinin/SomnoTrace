#!/usr/bin/env python3
"""Contracts that keep status rendering off scarce internal/DMA memory."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
DISPLAY = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")
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


start_match = re.search(
    r"static void start_storage_refresh\(void\).*?"
    r"(?=static void alert_config_task)",
    DISPLAY,
    re.DOTALL,
)
worker_match = re.search(
    r"static void storage_status_task\(void \*arg\).*?"
    r"(?=static void start_storage_refresh)",
    DISPLAY,
    re.DOTALL,
)
assert start_match and worker_match, "missing storage refresh functions"
start = start_match.group(0)
worker = worker_match.group(0)
summary = function_body(SCHED, "upload_sched_summary")

# The physical UI worker is created once at boot with its stack in PSRAM.
assert re.search(
    r"s_storage_worker_task\s*=\s*psram_task_create\(\s*"
    r"storage_status_task,\s*\"ui_storage\",\s*8192",
    DISPLAY,
), "storage status worker must have a persistent 8 KiB PSRAM stack"
assert "xTaskNotifyGive(s_storage_worker_task)" in start
assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" in worker

# A status getter must be a bounded RAM snapshot. The old implementation did
# a full O2 directory reconciliation with a 4,864-byte stack frame and could
# overwrite the 4 KiB transient ui_storage stack.
for forbidden in (
    "upload_ox_reconcile",
    "upload_ox_scan",
    "heap_caps_malloc",
    "opendir",
    "fopen",
):
    assert forbidden not in summary, f"status summary performs work: {forbidden}"
assert "s_summary_pending" in summary
assert "storage_status_memory_contract_test.py" in HOST_TEST

print("storage status memory contract passed")
