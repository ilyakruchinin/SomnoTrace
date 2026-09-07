#!/usr/bin/env python3
"""Contracts for AirSense ingress, OTA, and restart therapy arbitration."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/bsp_display.h").read_text(encoding="utf-8")
SMALL = (ROOT / "main/bsp_display.c").read_text(encoding="utf-8")
TOUCH = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")
AS11 = (ROOT / "main/as11_ble.c").read_text(encoding="utf-8")
NET = (ROOT / "main/net_provision.c").read_text(encoding="utf-8")


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
    return source[match.end():cursor - 1]


for declaration in (
    "void bsp_display_note_as11_notification_queued(void);",
    "void bsp_display_note_as11_notification_processed(void);",
    "bool bsp_display_try_begin_therapy_safe_maintenance(void);",
    "bool bsp_display_therapy_safe_maintenance_should_abort(void);",
    "void bsp_display_end_therapy_safe_maintenance(void);",
):
    assert declaration in HEADER

# A restart cannot reserve or commit while a raw notification is waiting for
# decryption/dispatch. This closes the pre-JSON queue gap for TherapyStart.
for display in (SMALL, TOUCH):
    reserve = function_body(display, "bsp_display_try_reserve_therapy_safe_restart")
    commit = function_body(display, "bsp_display_try_commit_therapy_safe_restart")
    queued = function_body(display, "bsp_display_note_as11_notification_queued")
    processed = function_body(display, "bsp_display_note_as11_notification_processed")
    begin = function_body(display, "bsp_display_try_begin_therapy_safe_maintenance")
    abort = function_body(display, "bsp_display_therapy_safe_maintenance_should_abort")
    end = function_body(display, "bsp_display_end_therapy_safe_maintenance")
    assert "s_as11_notifications_pending == 0" in reserve
    assert "s_as11_notifications_pending == 0" in commit
    assert "s_as11_notifications_pending++" in queued
    assert "s_as11_notifications_pending--" in processed
    assert "s_as11_notifications_pending == 0" in begin
    assert "s_therapy_start_claims == 0" in begin
    assert "s_therapy_safe_maintenance = true" in begin
    assert "s_therapy_start_claims > 0" in abort
    assert "therapy_active" in abort or "s_state.therapy" in abort
    assert "s_therapy_safe_maintenance = false" in end

gap = function_body(AS11, "gap_event")
notify_case = gap[gap.index("case BLE_GAP_EVENT_NOTIFY_RX"):
                  gap.index("case BLE_GAP_EVENT_L2CAP_UPDATE_REQ")]
assert notify_case.index("bsp_display_note_as11_notification_queued()") \
       < notify_case.index("heap_caps_malloc(notif_len") \
       < notify_case.index("os_mbuf_copydata") \
       < notify_case.index("xQueueSend(s_notif_queue")
assert "if (lifecycle_accounted)" in notify_case
assert notify_case.count("bsp_display_note_as11_notification_processed()") >= 3
worker = function_body(AS11, "notif_proc_task")
assert worker.index("handle_notify(item.data, item.len)") \
       < worker.index("bsp_display_note_as11_notification_processed()")

# OTA uses a separate cancellable maintenance gate. It never makes the live
# therapy publisher wait, and both upload modes check it between flash steps.
upload = function_body(NET, "ota_upload_handler")
flash = function_body(NET, "ota_flash_task")
url_handler = function_body(NET, "ota_url_handler")
url_task = function_body(NET, "ota_url_task")
assert upload.index("bsp_display_try_begin_therapy_safe_maintenance()") \
       < upload.index("xTaskCreate(ota_flash_task")
assert flash.index("bsp_display_therapy_safe_maintenance_should_abort()") \
       < flash.index("esp_ota_write(")
assert "ctx->aborted_by_therapy = true" in flash
assert "therapy started; update cancelled" in upload
assert upload.count("bsp_display_end_therapy_safe_maintenance()") >= 5
assert url_handler.index("bsp_display_try_begin_therapy_safe_maintenance()") \
       < url_handler.index("xTaskCreate(ota_url_task")
assert url_task.index("ota_native_should_abort()") \
       < url_task.index("esp_https_ota_perform(")
assert url_task.count("bsp_display_end_therapy_safe_maintenance()") >= 2
assert url_task.index("bsp_display_end_therapy_safe_maintenance()") \
       < url_task.index("ota_schedule_reboot()")

print("therapy lifecycle race contract passed")

# Native OTA closes final publication races atomically, and releases the short
# reservation before entering the existing therapy-aware reboot worker.
for display in (SMALL, TOUCH):
    commit = function_body(display, "bsp_display_try_reserve_maintenance_commit")
    for prerequisite in ("s_therapy_start_claims == 0", "s_therapy_start_waiters == 0",
                         "s_as11_notifications_pending == 0", "s_therapy_safe_maintenance"):
        assert prerequisite in commit
    assert "s_therapy_safe_restart_reserving = true" in commit
    assert "s_therapy_safe_maintenance = false" in commit
abort = function_body(NET, "ota_native_should_abort")
assert "p.cancel_requested" in abort and "bsp_display_therapy_safe_maintenance_should_abort()" in abort
commit = function_body(NET, "ota_native_commit_begin")
assert commit.index("bsp_display_try_reserve_maintenance_commit()") < commit.index("s_ota_progress.cancellable = false")
assert "if (!allowed) bsp_display_cancel_therapy_safe_restart()" in commit
