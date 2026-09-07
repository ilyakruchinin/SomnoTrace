#!/usr/bin/env python3
"""Static contracts for native first-run setup runtime integration."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = (ROOT / "main/first_run_setup_controller.c").read_text()
BSP = (ROOT / "main/bsp_display_7b.c").read_text()
MAIN = (ROOT / "main/main.c").read_text()
QEMU = (ROOT / "main/main_qemu.c").read_text()
CMAKE = (ROOT / "main/CMakeLists.txt").read_text()
CAPTURE = (ROOT / "scripts/capture-qemu-ui.py").read_text()


def require(source: str, pattern: str, message: str) -> None:
    if not re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


# Runtime is part of every relevant firmware target and owns one bounded,
# PSRAM-stack worker. Setup callbacks enqueue operations instead of blocking
# LVGL with radio, flash, or filesystem work.
require(CMAKE, r'"first_run_setup_controller\.c"',
        "setup controller is absent from the firmware component")
require(CONTROLLER, r"xQueueCreate\(1,\s*sizeof\(setup_operation_t\)\)",
        "setup operation queue is no longer explicitly bounded")
require(CONTROLLER,
        r"psram_task_create\(\s*setup_worker,\s*\"setup_worker\"",
        "setup worker stack is not PSRAM-backed")
for callback in (
    "wifi_scan_callback", "wifi_connect_callback",
    "timezone_search_callback", "timezone_select_callback",
    "time_advanced_callback", "airsense_scan_callback",
    "airsense_begin_callback", "airsense_confirm_callback",
    "card_retry_callback", "alerts_callback", "uploads_callback",
):
    require(CONTROLLER,
            rf"static esp_err_t {callback}\([^{{]+\)\s*\{{.*?queue_operation\(",
            f"{callback} no longer dispatches nonblocking work")
if re.search(r"\blv_[a-zA-Z0-9_]+\s*\(", CONTROLLER):
    raise AssertionError("setup controller calls LVGL outside display ownership")


# Each setup promise is backed by a real service operation. Unsupported
# persistent static IPv4 stays explicitly unavailable rather than succeeding
# in presentation state alone.
for token in (
    "netprov_scan_request", "netprov_try_connect", "netprov_save_config",
    "timezone_catalog_search", "time_sync_set_timezone",
    "time_sync_set_ntp_server", "netprov_set_mdns_name",
    "as11_ble_scan", "as11_ble_start_pair", "as11_ble_confirm_pair",
    "sd_storage_init", "session_writer_recover", "session_writer_init",
    "therapy_alert_save_config_json", "uploader_save_config",
):
    if token not in CONTROLLER:
        raise AssertionError(f"truthful setup service operation missing: {token}")
require(CONTROLLER, r"static_ipv4_supported\s*=\s*false",
        "unsupported static IPv4 is not reported truthfully")
require(CONTROLLER, r"FIRST_RUN_SETUP_UI_WIFI_SCAN_BLOCKED",
        "blocked Wi-Fi scans collapse into a generic/fake result")
require(CONTROLLER,
        r"airsense_confirm_callback.*?strlen\(four_digit_code\) != 4",
        "AirSense confirmation no longer preserves the four-digit contract")


# A fresh 7B enters the touch setup only after reconciliation and service
# initialization. Missing credentials/time must not force the legacy blocking
# portal/reboot while that setup surface is active.
reconcile_at = MAIN.index("reconcile_first_run_setup(&setup_facts)")
alerts_at = MAIN.index("therapy_alert_init()")
setup_at = MAIN.index("bsp_display_start_first_run_setup(sd_ret)")
if not reconcile_at < alerts_at < setup_at:
    raise AssertionError("native setup starts before reconciled services are ready")
if re.search(r"if\s*\(\s*!has_creds\s*\).*?s_softap_requested\s*=\s*true",
             MAIN, re.DOTALL):
    raise AssertionError("fresh 7B still auto-enters the blocking SoftAP")
require(MAIN, r"time_failed\s*&&\s*!native_setup_active",
        "missing time can still reboot an active native setup")


# The setup tree is lazy and opaque on LVGL's top layer. Only the display
# update path transfers generation-tagged snapshots, then destroys setup and
# reveals the already-built normal shell after durable completion.
require(BSP,
        r"first_run_setup_ui_create\(lv_layer_top\(\),\s*callbacks\)",
        "setup is not isolated above the normal navigation shell")
require(BSP,
        r"first_run_setup_controller_snapshot\(&live,\s*&generation\).*?"
        r"generation != s_first_run_setup_seen_generation.*?"
        r"first_run_setup_ui_update\(&live\)",
        "display task no longer applies bounded generation snapshots")
require(BSP,
        r"first_run_setup_controller_take_finished\(\).*?"
        r"first_run_setup_ui_destroy\(\).*?"
        r"first_run_setup_controller_stop\(\).*?"
        r"lv_obj_invalidate\(lv_scr_act\(\)\)",
        "finished setup does not release its UI and reveal the normal shell")
require(BSP,
        r"static bool s_backlight_force_on;\s*"
        r"static bool s_setup_backlight_force_on;",
        "setup and portal no longer have independent display visibility claims")
require(BSP,
        r"first_run_setup_controller_take_finished\(\).*?"
        r"s_setup_backlight_force_on\s*=\s*false.*?"
        r"bsp_display_apply_backlight_policy\(false\)",
        "finishing setup does not release only the setup visibility claim")
require(BSP,
        r"bool forced\s*=\s*s_backlight_force_on\s*\|\|\s*"
        r"s_setup_backlight_force_on",
        "backlight policy does not preserve overlapping setup and portal claims")


# QEMU still boots the completed dashboard by default, with a deterministic
# emulator-only touch route and capture scenario for a disposable setup run.
if QEMU.index("seed_finished_setup_preview()") > QEMU.index("bsp_display_init()"):
    raise AssertionError("QEMU default no longer seeds the completed preview")
require(BSP, r"QEMU first-run setup preview requested",
        "QEMU has no selectable setup preview route")
require(BSP, r"bsp_display_qemu_start_setup_preview.*?first_run_setup_reset\(\)",
        "QEMU setup preview is not deterministic/disposable")
if '"setup-wifi"' not in CAPTURE or "QEMU setup preview ready" not in CAPTURE:
    raise AssertionError("QEMU capture tool cannot select the setup preview")

print("first-run setup runtime contract: PASS")
