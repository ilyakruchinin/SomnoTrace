#!/usr/bin/env python3
"""Contracts for leasing every uploader-side O2 card traversal."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "components/uploader/upload_sched.c").read_text(
    encoding="utf-8"
)


def function(name: str) -> str:
    match = re.search(
        rf"^[\w][\w\s*]*\b{name}\s*\([^;{{}}]*\)\s*\{{",
        SOURCE,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"missing function {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(SOURCE) and depth:
        if SOURCE[cursor] == "{":
            depth += 1
        elif SOURCE[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return SOURCE[match.end():cursor - 1]


backend = function("run_backend")
take = backend.index("uploader_lease_take(LEASE_WAIT_MS)")
bundle_scan = backend.index("upload_scan_bundle(&bundle)")
ox_scan = backend.index("upload_ox_reconcile(")
discovery_release = backend.index("uploader_lease_give()", ox_scan)
assert take < bundle_scan < ox_scan < discovery_release

day_scan = backend.index("upload_scan_day_groups(")
day_take = backend.rfind("uploader_lease_take(LEASE_WAIT_MS)", 0, day_scan)
day_release = backend.index("uploader_lease_give()", day_scan)
day_end = backend.index("be->day_end", day_release)
assert day_take < day_scan < day_release < day_end

ox_put = backend.index("be->put_oximetry(")
ox_take = backend.rfind("uploader_lease_take(LEASE_WAIT_MS)", 0, ox_put)
ox_mark = backend.index("upload_ox_mark(", ox_put)
ox_release = backend.index("uploader_lease_give()", ox_mark)
assert ox_take < ox_put < ox_mark < ox_release

# The final post-pass summary also reconciles the O2 tree and therefore needs
# its own short lease after backend transactions have completed.
run_pass = function("run_pass")
summary_alloc = run_pass.rindex("upload_ox_ref_t *ox_refs")
summary = run_pass[summary_alloc:]
assert summary.index("uploader_lease_take(LEASE_WAIT_MS)") < summary.index(
    "upload_ox_reconcile("
) < summary.index("uploader_lease_give()")

# Boot-time state loading can overlap an already-live maintenance HTTP server.
init = function("upload_sched_init")
assert init.index("uploader_lease_take(LEASE_WAIT_MS)") < init.index(
    "upload_ox_init()"
) < init.index("uploader_lease_give()")

print("uploader O2 lease contract passed")
