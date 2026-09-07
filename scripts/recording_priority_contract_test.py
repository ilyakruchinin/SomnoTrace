#!/usr/bin/env python3
"""Contracts for bounded recording priority over History/card readers."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
STORAGE = (ROOT / "main/sd_storage.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/sd_storage.h").read_text(encoding="utf-8")
HISTORY = (ROOT / "main/touch_history.c").read_text(encoding="utf-8")


def function(source: str, name: str) -> str:
    match = re.search(
        rf"^[\w][\w\s*]*\b{name}\s*\([^;{{}}]*\)\s*\{{",
        source,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"missing function {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(source) and depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return source[match.end():cursor - 1]


begin = function(STORAGE, "sd_storage_recording_begin")
assert "__atomic_add_fetch(&s_recording_waiters, 1, __ATOMIC_RELEASE)" in begin
assert "SD_RECORDING_PRIORITY_WAIT_MS" in begin
assert "esp_timer_get_time() >= deadline_us" in begin
assert "__atomic_sub_fetch(&s_recording_waiters, 1, __ATOMIC_RELEASE)" in begin
assert begin.index("__atomic_add_fetch(&s_recording_waiters, 1, __ATOMIC_RELEASE)") < begin.index("__atomic_add_fetch(&s_recording, 1, __ATOMIC_RELEASE)")
assert "bool sd_storage_recording_pending(void);" in HEADER

acquire = function(STORAGE, "sd_storage_lease_acquire")
upload_case = acquire[acquire.index("case SD_LEASE_UPLOAD:"):]
assert "sd_storage_recording_pending()" in upload_case

cancelled = function(HISTORY, "history_operation_cancelled")
assert "sd_storage_recording_pending()" in cancelled
assert cancelled.index("sd_storage_recording_pending()") < cancelled.index(
    "operation->should_cancel"
)

print("recording priority contract passed")
