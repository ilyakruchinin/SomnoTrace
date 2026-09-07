#!/usr/bin/env python3
"""Keep web and touchscreen upload probes on one transport claim."""

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

for api in ("uploader_test_connection", "uploader_test_request"):
    if api not in HEADER:
        raise AssertionError(f"missing upload test entry point: {api}")
if not re.search(
    r"uploader_test_connection\(.*?upload_sched_probe_begin\(\).*?"
    r"be->test\(cfg, msg, msg_len\).*?upload_sched_probe_end\(\)",
    UPLOADER,
    re.DOTALL,
):
    raise AssertionError("web probe does not hold the scheduler transport claim")
if not re.search(
    r"uploader_test_request\(const char \*backend, uint32_t \*generation_out\).*?"
    r"s_test\.state = UPLOAD_TEST_QUEUED.*?"
    r"if \(generation_out\) \*generation_out = generation",
    SCHEDULER,
    re.DOTALL,
):
    raise AssertionError("test generation is not assigned with the queued scheduler state")
if not re.search(
    r"case EV_TEST:.*?upload_sched_probe_begin\(\).*?"
    r"uploader_(?:smb|sleephq)_probe\(\).*?upload_sched_probe_end\(\)",
    SCHEDULER,
    re.DOTALL,
):
    raise AssertionError("native probe does not hold the scheduler transport claim")
if not re.search(
    r"upload_test_read_overrides.*?uploader_load_config\(out\).*?"
    r'cfg_str_from\(root, "smb_pass".*?cfg_str_from\(root, "shq_client_secret"',
    WEB,
    re.DOTALL,
):
    raise AssertionError("web probe does not merge unsaved form settings")
if not re.search(
    r"upload_test_send.*?uploader_test_connection\(backend_id, use, &ok, msg, sizeof\(msg\)\).*?"
    r'cJSON_AddBoolToObject\(root, "ok"',
    WEB,
    re.DOTALL,
):
    raise AssertionError("web POST does not run the bounded synchronous probe")
if not re.search(
    r'"/api/uploads/test-status".*?upload_test_status_handler', WEB, re.DOTALL
):
    raise AssertionError("web status route is missing")
if not re.search(
    r"function testUploadConnection\(id\).*?JSON\.stringify\(body\).*?"
    r"d\.message.*?d\.ok",
    PORTAL,
    re.DOTALL,
):
    raise AssertionError("portal does not submit and render the unsaved-settings probe")
if "pollUploadConnection(id, d.generation" in PORTAL:
    raise AssertionError("portal mixes synchronous results with scheduler generations")

scripts = re.findall(r"<script>(.*?)</script>", PORTAL, re.DOTALL)
with tempfile.TemporaryDirectory(prefix="somno-portal-js-") as tmp:
    js = Path(tmp) / "portal.js"
    js.write_text("\n".join(scripts))
    subprocess.run(["node", "--check", str(js)], check=True)

print("Web and touchscreen upload tests share one transport claim")
