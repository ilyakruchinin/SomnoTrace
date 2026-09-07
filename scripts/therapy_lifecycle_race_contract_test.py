#!/usr/bin/env python3
"""Contracts for AirSense ingress, OTA, and restart therapy arbitration."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/bsp_display.h").read_text(encoding="utf-8")
SMALL = (ROOT / "main/bsp_display.c").read_text(encoding="utf-8")
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
for display in (SMALL,):
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

# Native OTA closes final publication races atomically, and releases the short
# reservation before entering the existing therapy-aware reboot worker.
for display in (SMALL,):
    commit = function_body(display, "bsp_display_try_reserve_maintenance_commit")
    for prerequisite in ("s_therapy_start_claims == 0", "s_therapy_start_waiters == 0",
                         "s_as11_notifications_pending == 0", "s_therapy_safe_maintenance"):
        assert prerequisite in commit
    assert "s_therapy_safe_restart_reserving = true" in commit
    assert "s_therapy_safe_maintenance = false" in commit

print("Original-screen therapy lifecycle gates and notification accounting passed")
