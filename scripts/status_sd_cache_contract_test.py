#!/usr/bin/env python3
"""Keep frequently-polled status responses away from FATFS and SDMMC."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
NET = (ROOT / "main/net_provision.c").read_text(encoding="utf-8")
SD = (ROOT / "main/sd_storage.c").read_text(encoding="utf-8")
SD_HEADER = (ROOT / "main/sd_storage.h").read_text(encoding="utf-8")
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


status = function_body(NET, "netprov_build_status_json")
cached = function_body(SD, "sd_storage_get_cached_free")
queried = function_body(SD, "sd_storage_get_free")
reserve = function_body(SD, "sd_storage_reserve_for_recording")

# Status is polled every few seconds. It may serialize cached values, but must
# not open directories/files or call any API that can reach the card.
for forbidden in (
    "f_getfree",
    "sd_storage_get_free(",
    "session_writer_pending_export_json",
    "opendir(",
    "fopen(",
):
    assert forbidden not in status, f"/api/status performs SD I/O: {forbidden}"
assert "sd_storage_get_cached_free(&sd_free, &sd_total)" in status

# The cache reader itself must remain a bounded copy. A successful explicit
# capacity query is the producer that refreshes it.
for forbidden in ("f_getfree", "opendir(", "fopen("):
    assert forbidden not in cached, f"cache reader performs I/O: {forbidden}"
assert "capacity_cache_store(free, total)" in queried
assert "portENTER_CRITICAL(&s_capacity_lock)" in cached
assert "sd_storage_get_cached_free(&free_bytes, &total)" in reserve
assert "sd_storage_get_free(" not in reserve, (
    "therapy START performs a synchronous SD capacity query"
)
assert "sd_storage_get_cached_free" in SD_HEADER
assert "status_sd_cache_contract_test.py" in HOST_TEST

print("status SD cache contract passed")
