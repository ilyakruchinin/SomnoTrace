#!/usr/bin/env python3
"""Focused static contracts for the 1024x600 first-run setup surface."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/first_run_setup_ui.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/first_run_setup_ui.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing first-run UI contract: {description}")


# The module is controller-driven and exposes a pointer-free bounded snapshot.
for callback in (
    "wifi_scan",
    "wifi_connect",
    "timezone_search",
    "timezone_select",
    "airsense_scan",
    "airsense_begin_pairing",
    "airsense_confirm_code",
    "card_retry",
    "configure_alerts",
    "configure_uploads",
    "finished",
    "wifi_set_static_ipv4",
    "ntp_server_set",
    "hostname_set",
    "time_advanced_set",
):
    require(HEADER, rf"\(\*{callback}\)\s*\(", f"{callback} controller callback")

for api in ("create", "destroy", "show", "hide", "update", "snapshot", "root"):
    require(HEADER, rf"first_run_setup_ui_{api}\s*\(", f"{api} API")

for maximum in (
    "FIRST_RUN_SETUP_UI_WIFI_RESULT_MAX",
    "FIRST_RUN_SETUP_UI_TIMEZONE_RESULT_MAX",
    "FIRST_RUN_SETUP_UI_AIRSENSE_RESULT_MAX",
):
    require(HEADER, rf"#define\s+{maximum}\s+6U", f"bounded {maximum}")

driver_headers = ("net_provision.h", "as11_ble.h", "sd_storage.h", "time_sync.h")
for driver in driver_headers:
    assert driver not in SOURCE, f"setup UI must not hard-couple to {driver}"

# Exact six-step rail and the product-specific setup rules.
for label in ("Wi-Fi", "Time & clock", "AirSense", "microSD card", "Alerts", "Uploads"):
    assert f'"{label}"' in SOURCE, f"missing rail label {label}"
assert "Skip setup" not in SOURCE, "generic whole-setup skip must not exist"
assert "Continue without recording" in SOURCE, "card needs explicit no-recording choice"
assert "Only 2.4 GHz networks are supported" in SOURCE, "Wi-Fi band limitation is missing"
assert "More > myAir App" in SOURCE, "machine-first AirSense instruction is missing"
assert "4-digit code" in SOURCE, "real AirSense code length is missing"
require(SOURCE, r"make_input\(\"4-digit code\".*?false,\s*4\)", "four-digit input cap")
require(SOURCE, r"strlen\(code\)\s*!=\s*4", "four-digit confirmation guard")
require(HEADER, r"const char\s+four_digit_code\[5\]", "leading-zero-safe code callback")

for state in (
    "WIFI_SCANNING", "WIFI_SELECT", "WIFI_PASSWORD", "WIFI_AUTH_FAILED",
    "AIRSENSE_SCANNING", "AIRSENSE_SELECT", "AIRSENSE_NOT_FOUND",
    "AIRSENSE_WAIT_CODE", "AIRSENSE_CODE_REJECTED", "AIRSENSE_PAIRED",
    "CARD_CHECKING", "CARD_READY", "CARD_MISSING", "CARD_UNREADABLE",
    "WIFI_HIDDEN", "WIFI_STATIC_ADDRESS", "WIFI_SCAN_BLOCKED",
    "TIME_ADVANCED",
):
    assert f"FIRST_RUN_SETUP_UI_{state}" in HEADER, f"missing state {state}"

# Touch geometry and typography are hardware contracts, not visual suggestions.
require(SOURCE, r"#define\s+UI_WIDTH\s+1024\b", "1024-pixel canvas")
require(SOURCE, r"#define\s+UI_HEIGHT\s+600\b", "600-pixel canvas")
for pattern, description in (
    (r"UI_HEADER_HEIGHT\s+60\b", "60-pixel setup header"),
    (r"UI_RAIL_X\s+16\b", "rail x coordinate"),
    (r"UI_RAIL_Y\s+62\b", "rail y coordinate"),
    (r"UI_RAIL_WIDTH\s+262\b", "rail width"),
    (r"UI_DETAIL_X\s+290\b", "detail x coordinate"),
    (r"UI_DETAIL_Y\s+62\b", "detail y coordinate"),
    (r"UI_DETAIL_WIDTH\s+718\b", "detail width"),
    (r"UI_DETAIL_HEIGHT\s+522\b", "panel height"),
):
    require(SOURCE, pattern, description)
require(SOURCE, r"#define\s+UI_TOUCH_TARGET_MIN\s+44\b", "44-pixel touch minimum")
require(SOURCE, r"if\s*\(height\s*<\s*UI_TOUCH_TARGET_MIN\)", "enforced touch minimum")
assert "somnotrace_space_grotesk" in SOURCE, "Space Grotesk is not compiled into the UI"
assert "somnotrace_ibm_plex_mono" in SOURCE, "IBM Plex Mono is not used for data"
require(HEADER, r"char\s+posix_tz\[64\]", "POSIX rule required by time_sync")
require(SOURCE, r"timezone_select\(.*?zone->id,.*?zone->posix_tz", "truthful timezone apply data")

# Exact audit surfaces and copy contracts.
for screen in (
    "SCREEN_WELCOME", "SCREEN_WIFI_SCAN", "SCREEN_WIFI_PASSWORD",
    "SCREEN_WIFI_STATIC", "SCREEN_WIFI_SCAN_BLOCKED", "SCREEN_TIMEZONE",
    "SCREEN_TIME_ADVANCED", "SCREEN_AIRSENSE_HOW", "SCREEN_AIRSENSE_SCAN",
    "SCREEN_AIRSENSE_NONE", "SCREEN_AIRSENSE_CODE",
    "SCREEN_AIRSENSE_REJECTED", "SCREEN_AIRSENSE_PAIRED", "SCREEN_CARD",
    "SCREEN_READY",
):
    assert screen in SOURCE, f"missing exact handoff screen {screen}"
for copy in (
    "SETUP CHECKLIST", "of 6 complete", "Not set up",
    "Optional · can wait", "Clock not set · UTC", "Card ready",
    "Card check runs automatically", "Hidden network", "saved networks",
    "Show", "Done", "Static network address", "Wi-Fi scan is unavailable",
    "Network time and device name", ".local", "Unavailable at pairing",
    "About %u nights", "Not configured",
):
    assert copy in SOURCE, f"missing handoff copy/state: {copy}"
assert "header_wifi" not in SOURCE, "the dedicated setup header must not show Wi-Fi"
require(SOURCE, r"lv_obj_set_size\(card_pill,\s*166,\s*36\)", "36-pixel card pill")
require(SOURCE, r"make_input\(\"somnotrace\".*?false,\s*10\)", "10-char durable hostname limit")

# The setup surface is lazy, maintains one detail tree, and is releasable.
require(SOURCE, r"static\s+first_run_setup_ui_t\s*\*\s*s_ui", "lazy singleton pointer")
require(SOURCE, r"first_run_setup_ui_create.*?heap_caps_calloc", "lazy snapshot allocation")
require(SOURCE, r"render_detail.*?lv_obj_clean\(s_ui->detail\)", "one reusable detail tree")
require(SOURCE, r"first_run_setup_ui_destroy.*?lv_obj_del.*?heap_caps_free", "full release path")
require(SOURCE, r"make_shell.*?LV_OBJ_FLAG_HIDDEN.*?render_all", "hidden first render")
require(SOURCE, r"first_run_setup_ui_show.*?render_all\(\).*?clear_flag", "complete first visible frame")
require(SOURCE, r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT", "PSRAM-only context")
assert SOURCE.count("heap_caps_calloc(") == 1, "no internal-RAM allocation fallback is allowed"
require(
    SOURCE,
    r"if\s*\(s_ui->rendered_key\s*!=\s*key\)\s*\{\s*"
    r"render_detail\(screen,\s*key\)",
    "detail rebuild only on a screen/structural transition",
)
render_all_body = re.search(
    r"static void render_all\(void\)\s*\{(.*?)\n\}", SOURCE, re.DOTALL
)
assert render_all_body and "lv_obj_clean" not in render_all_body.group(1), (
    "steady snapshot rendering must never clean the detail tree"
)

# User outcomes and resumable rail positions go through the durable service.
for transition in (
    "FIRST_RUN_SETUP_UPDATE_SELECT",
    "FIRST_RUN_SETUP_UPDATE_COMPLETE",
    "FIRST_RUN_SETUP_UPDATE_SKIP",
    "FIRST_RUN_SETUP_UPDATE_CONTINUE_WITHOUT_RECORDING",
):
    assert transition in SOURCE, f"missing durable transition {transition}"

# The source is compiled only for the two 1024x600 targets that have the fonts.
target_block = re.search(
    r"if\(CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B OR\s*"
    r"\(CONFIG_SOMNOTRACE_BOARD_QEMU AND NOT CONFIG_SOMNOTRACE_QEMU_DISPLAY_154\)\)"
    r"(.*?)endif\(\)", CMAKE, re.DOTALL
)
assert target_block, "missing 7B/QEMU CMake source block"
assert '"first_run_setup_ui.c"' in target_block.group(1), (
    "setup UI must be registered only in the 7B/QEMU source block"
)

print("first-run 1024x600 setup UI contract passed")
