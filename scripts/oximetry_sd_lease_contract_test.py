#!/usr/bin/env python3
"""Structural contracts for O2 Ring FATFS arbitration."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
STORE = (ROOT / "main/oximeter_store.c").read_text(encoding="utf-8")
OXYII = (ROOT / "main/oximeter_oxyii.c").read_text(encoding="utf-8")
LEGACY = (ROOT / "main/oximeter_legacy.c").read_text(encoding="utf-8")
HTTP = (ROOT / "main/oximetry_http.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/oximeter_store.h").read_text(encoding="utf-8")


def function_body(name: str, source: str) -> str:
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


# A transaction is admitted only on a mounted card and uses EXPORT: this
# excludes destructive format/unmount while still allowing therapy recording.
assert "bool ox_store_begin_io(uint32_t timeout_ms);" in HEADER
assert "void ox_store_end_io(void);" in HEADER
begin = function_body("ox_store_begin_io", STORE)
assert begin.index("sd_storage_is_ready()") < begin.index(
    "sd_storage_lease_acquire(SD_LEASE_EXPORT, timeout_ms)"
)
assert begin.count("sd_storage_is_ready()") == 2
assert "sd_storage_lease_release(SD_LEASE_EXPORT)" in begin
assert "sd_storage_lease_release(SD_LEASE_EXPORT)" in function_body(
    "ox_store_end_io", STORE
)

# Short standalone metadata/diagnostic operations own leases themselves. This
# covers boot fallback, pairing, forget and the diagnostics endpoint.
for name in (
    "ox_store_ensure_dirs",
    "ox_store_load_paired",
    "ox_store_save_paired",
    "ox_store_delete_paired",
    "ox_store_conversion_diagnostics_json",
):
    body = function_body(name, STORE)
    assert body.index("ox_store_begin_io(0)") < body.index("ox_store_end_io()"), name

# A complete pull owns one lease from before its first store/index operation
# through conversion/finalisation. BLE discovery and file-list exchange happen
# first, so passive scanning never pins the card gate.
for source, list_call in (
    (OXYII, "oxyii_get_file_list(names, 32)"),
    (LEGACY, "parse_file_list(file_list, names, 32)"),
):
    pull = function_body("do_pull_and_mark", source)
    acquire = pull.index("ox_store_begin_io(0)")
    release = pull.rindex("ox_store_end_io()")
    assert pull.index(list_call) < acquire < pull.index("ox_store_index_check", acquire)
    assert release < pull.rindex("return pull_ok;")
    assert "do_scan(" not in pull[acquire:release]

for source in (OXYII, LEGACY):
    scan = function_body("do_scan", source)
    assert "ox_store_begin_io" not in scan
    assert "sd_storage_lease_acquire" not in scan

# Boot reconciliation and legacy conversion run under one transaction, rather
# than relying on a stale pre-I/O mounted check.
migration = function_body("canonical_migration_task", OXYII)
assert migration.index("ox_store_begin_io(5000)") \
       < migration.index("oximetry_canonical_reconcile()") \
       < migration.index("oximetry_canonical_migrate_all_legacy()") \
       < migration.index("ox_store_end_io()")

# Canonical HTTP enumeration and downloads retain the lease until all FATFS
# objects are closed. Every return after the file handler acquires must release.
for name in (
    "oximetry_recordings_handler",
    "oximetry_recording_handler",
    "oximetry_uploads_handler",
):
    body = function_body(name, HTTP)
    assert body.index("ox_store_begin_io(0)") < body.index("ox_store_end_io()")

file_handler = function_body("oximetry_file_handler", HTTP)
held = file_handler[file_handler.index("ox_store_begin_io(0)"):]
for index, return_match in enumerate(re.finditer(r"\breturn\b", held)):
    prefix = held[:return_match.start()]
    # The first return is the acquire-failed branch; every later return must
    # have released the transaction since the last acquire.
    if index == 0:
        continue
    assert prefix.rfind("ox_store_end_io()") > prefix.rfind("ox_store_begin_io(0)"), \
        "file handler return can strand O2 SD lease"

print("Oximetry SD lease contract passed")
