#!/usr/bin/env python3
"""Structural contract for recoverable Waveshare 7B OTA boots."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "main/main.c").read_text(encoding="utf-8")
DEFAULTS = (ROOT / "sdkconfig.7b.defaults").read_text(encoding="utf-8")
PARTITIONS = (ROOT / "partitions.csv").read_text(encoding="utf-8")


def function(name: str) -> str:
    match = re.search(
        rf"^[\w][\w\s*]*\b{name}\s*\([^;{{}}]*\)\s*\{{",
        MAIN,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"missing function {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(MAIN) and depth:
        if MAIN[cursor] == "{":
            depth += 1
        elif MAIN[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return MAIN[match.end():cursor - 1]


assert "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in DEFAULTS
assert re.search(r"app0,\s+app,\s+ota_0", PARTITIONS)
assert re.search(r"app1,\s+app,\s+ota_1", PARTITIONS)

gate = function("confirm_pending_ota_image")
assert "ESP_OTA_IMG_PENDING_VERIFY" in gate
assert "esp_ota_mark_app_valid_cancel_rollback()" in gate
assert "esp_ota_mark_app_invalid_rollback_and_reboot()" in gate
assert gate.index("ESP_OTA_IMG_PENDING_VERIFY") < gate.index(
    "esp_ota_mark_app_valid_cancel_rollback()"
)

app = function("app_main")
for readiness in ("display_ready", "recording_runtime_ready", "as11_ready"):
    assert readiness in app
assert app.index("bsp_display_enable_touch_services") < app.index(
    "confirm_pending_ota_image"
)
assert app.index("confirm_pending_ota_image") < app.index(
    "session_writer_enable_deferred_export"
)

print("OTA rollback contract passed")
