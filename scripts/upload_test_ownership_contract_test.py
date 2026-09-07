#!/usr/bin/env python3
"""Keep web and touchscreen upload probes serialized on the scheduler."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "components/uploader/uploader.h").read_text()
UPLOADER = (ROOT / "components/uploader/uploader.c").read_text()
SCHEDULER = (ROOT / "components/uploader/upload_sched.c").read_text()
WEB = (ROOT / "main/net_provision.c").read_text()
PORTAL = (ROOT / "main/portal.html").read_text()

if "uploader_test_connection" in HEADER or "uploader_test_connection" in UPLOADER:
    raise AssertionError("a caller can still run an upload probe outside the scheduler")
if "upload_sched_uploading" in HEADER or "upload_sched_uploading" in SCHEDULER:
    raise AssertionError("racy check-then-probe API is still exposed")
if not re.search(
    r"uploader_test_request\(const char \*backend, uint32_t \*generation_out\).*?"
    r"s_test\.state = UPLOAD_TEST_QUEUED.*?"
    r"if \(generation_out\) \*generation_out = generation",
    SCHEDULER,
    re.DOTALL,
):
    raise AssertionError("test generation is not assigned with the queued scheduler state")
if not re.search(
    r"upload_test_send.*?uploader_test_request\(backend_id, &generation\).*?"
    r"202 Accepted",
    WEB,
    re.DOTALL,
):
    raise AssertionError("web POST does not enqueue a generation-tagged scheduler test")
if not re.search(
    r'"/api/uploads/test-status".*?upload_test_status_handler', WEB, re.DOTALL
):
    raise AssertionError("web status route is missing")
if not re.search(
    r"pollUploadConnection\(id, generation, attempt\).*?"
    r"d\.generation !== generation.*?"
    r"d\.state === 'queued' \|\| d\.state === 'running'",
    PORTAL,
    re.DOTALL,
):
    raise AssertionError("portal does not fence and poll the queued test generation")

scripts = re.findall(r"<script>(.*?)</script>", PORTAL, re.DOTALL)
with tempfile.TemporaryDirectory(prefix="somno-portal-js-") as tmp:
    js = Path(tmp) / "portal.js"
    js.write_text("\n".join(scripts))
    subprocess.run(["node", "--check", str(js)], check=True)

print("Web and touchscreen upload tests share scheduler ownership")
