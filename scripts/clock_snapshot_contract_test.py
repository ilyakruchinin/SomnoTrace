#!/usr/bin/env python3
"""Contracts for atomic drift/captured-clock snapshots and provenance."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
TIME_SYNC = (ROOT / "main/time_sync.c").read_text(encoding="utf-8")
AS11 = (ROOT / "main/as11_ble.c").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")


def function_span(source: str, name: str) -> tuple[int, int]:
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
    return match.start(), cursor


def function_body(source: str, name: str) -> str:
    start, end = function_span(source, name)
    return source[start:end]


def assert_symbol_confined(
    source: str, symbol: str, allowed_functions: tuple[str, ...]
) -> None:
    spans = [function_span(source, name) for name in allowed_functions]
    declaration = re.search(rf"^static\b[^;]*\b{symbol}\b[^;]*;", source, re.MULTILINE)
    comments = [
        match.span()
        for match in re.finditer(r"//[^\n]*|/\*.*?\*/", source, re.DOTALL)
    ]
    allowed = spans + comments + ([declaration.span()] if declaration else [])
    for match in re.finditer(rf"\b{symbol}\b", source):
        if not any(start <= match.start() < end for start, end in allowed):
            line = source.count("\n", 0, match.start()) + 1
            raise AssertionError(f"unguarded {symbol} access at line {line}")


# Persisted drift is a four-field snapshot.  In particular, its int64_t
# values cannot be torn on the 32-bit S3 and provenance cannot lag a value.
assert "portMUX_TYPE s_drift_lock = portMUX_INITIALIZER_UNLOCKED" in TIME_SYNC
drift_load = function_body(TIME_SYNC, "drift_cache_load")
drift_store = function_body(TIME_SYNC, "drift_cache_store")
for body in (drift_load, drift_store):
    assert "portENTER_CRITICAL(&s_drift_lock)" in body
    assert "portEXIT_CRITICAL(&s_drift_lock)" in body
for symbol in ("s_drift_ms", "s_drift_at_ms", "s_drift_loaded", "s_drift_src"):
    assert symbol in drift_load and symbol in drift_store
    assert_symbol_confined(TIME_SYNC, symbol, ("drift_cache_load", "drift_cache_store"))

for reader in (
    "time_source_drift_age_ms",
    "time_sync_has_drift",
    "time_sync_peek_drift_snapshot",
    "time_sync_get_drift_snapshot",
    "time_sync_recover_from_as11",
):
    assert "drift_cache_load()" in function_body(TIME_SYNC, reader)
for writer in ("time_sync_save_drift", "load_drift_from_nvs", "load_drift_from_sd"):
    assert "drift_cache_store(" in function_body(TIME_SYNC, writer)


# The pre-stream AS11 clock and the corresponding wall clock/source are also
# published together.  A stale connection must never donate a measurement.
assert "portMUX_TYPE s_as11_clock_lock = portMUX_INITIALIZER_UNLOCKED" in AS11
capture_invalidate = function_body(AS11, "as11_clock_capture_invalidate")
capture_store = function_body(AS11, "as11_clock_capture_store")
capture_load = function_body(AS11, "as11_clock_capture_load")
for body in (capture_invalidate, capture_store, capture_load):
    assert "portENTER_CRITICAL(&s_as11_clock_lock)" in body
    assert "portEXIT_CRITICAL(&s_as11_clock_lock)" in body
assert_symbol_confined(
    AS11,
    "s_as11_clock_capture",
    (
        "as11_clock_capture_invalidate",
        "as11_clock_capture_store",
        "as11_clock_capture_load",
    ),
)
assert_symbol_confined(
    AS11,
    "s_as11_clock_generation",
    ("as11_clock_capture_invalidate", "as11_clock_capture_store"),
)
assert "++s_as11_clock_generation" in capture_invalidate
assert "generation == s_as11_clock_generation" in capture_store
assert "s_as11_clock_ms" not in AS11
assert "s_as11_clock_capture_ntp_ms" not in AS11

gap_disconnect = re.search(
    r"case BLE_GAP_EVENT_DISCONNECT:.*?case BLE_GAP_EVENT_ENC_CHANGE:",
    AS11,
    re.DOTALL,
)
assert gap_disconnect and "as11_clock_capture_invalidate();" in gap_disconnect.group()
assert "as11_clock_capture_invalidate();" in function_body(AS11, "as11_ble_forget")
assert "as11_clock_capture_invalidate();" in function_body(AS11, "as11_ble_disconnect")

reconnect = function_body(AS11, "reconnect_task")
recapture_at = reconnect.find("as11_clock_capture_invalidate()")
query_at = reconnect.find("as11_ble_get_datetime(&as11_ms)")
source_at = reconnect.find("time_source_get()", query_at)
wall_at = reconnect.find("time(NULL)", source_at)
store_at = reconnect.find("as11_clock_capture_store(", wall_at)
assert -1 not in (recapture_at, query_at, source_at, wall_at, store_at)
assert recapture_at < query_at < source_at < wall_at < store_at
assert "capture_generation = as11_clock_capture_invalidate()" in reconnect
assert "as11_clock_capture_store(capture_generation" in reconnect

get_drift = function_body(AS11, "as11_ble_get_clock_drift")
provenance_at = get_drift.find("capture.wall_source != TIME_SRC_NTP")
calculate_at = get_drift.find("capture.wall_ms - capture.as11_ms")
assert -1 not in (provenance_at, calculate_at) and provenance_at < calculate_at

# Pairing's intentional link teardown is another manual disconnect route.
assert re.search(
    r"as11_clock_capture_invalidate\(\);\s*"
    r"s_manual_disconnect = true;\s*"
    r"if \(s_conn_handle != BLE_HS_CONN_HANDLE_NONE\)",
    AS11,
)

assert "clock_snapshot_contract_test.py" in HOST_TEST

print("clock snapshot contract passed")
