#!/usr/bin/env python3
"""Static contracts for the shared asynchronous Wi-Fi scan service."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/net_provision.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/net_provision.c").read_text(encoding="utf-8")
UI = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")


def require(pattern: str, text: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


require(r"NETPROV_SCAN_MAX_APS\s+20", HEADER, "scan result must remain bounded")
require(r"void\s+netprov_scan_get_snapshot\s*\(", HEADER,
        "public snapshot API missing")
require(r"esp_err_t\s+netprov_scan_request\s*\(", HEADER,
        "public asynchronous request API missing")
require(r"NETPROV_SCAN_BLOCK_NOT_INITIALIZED", HEADER,
        "pre-init/QEMU blocked state missing")
require(r"NETPROV_SCAN_BLOCK_RECORDING", HEADER,
        "recording safety precondition missing")

require(r"s_radio_gate\s*=\s*xSemaphoreCreateBinary", SOURCE,
        "shared Wi-Fi radio gate missing")
require(r"netprov_try_connect.*?xSemaphoreTake\(s_radio_gate.*?"
        r"try_connect_radio_locked.*?xSemaphoreGive\(s_radio_gate\)", SOURCE,
        "connect/failover is not serialized with user scans")
require(r"netprov_start_portal.*?xSemaphoreTake\(s_radio_gate.*?"
        r"esp_wifi_set_mode\(WIFI_MODE_APSTA\).*?xSemaphoreGive\(s_radio_gate\)",
        SOURCE, "portal mode transition is not serialized with user scans")
require(r"WIFI_EVENT_STA_DISCONNECTED.*?user_scan_running\(\).*?"
        r"s_rescan_requested\s*=\s*true", SOURCE,
        "disconnect recovery can collide with a user scan")
require(r"!s_portal_mode\s*&&\s*!netprov_is_link_up\(\)", SOURCE,
        "user scan must not race an in-progress asynchronous reconnect")
require(r"err\s*=\s*esp_wifi_scan_start", SOURCE,
        "scan-start errors must be observed")
require(r"esp_wifi_clear_ap_list\s*\(", SOURCE,
        "driver-owned scan results need explicit error/empty cleanup")
require(r"memcmp\(aps\[j\]\.ssid.*?records\[i\]\.ssid", SOURCE,
        "duplicate SSIDs must be collapsed")
require(r"qsort\(aps,\s*result_count.*?scan_ap_rssi_desc", SOURCE,
        "touch/browser results must be strongest-first")
require(r"snapshot\.generation\s*!=\s*s_browser_delivered_scan_generation",
        SOURCE, "browser polling needs non-consuming generation delivery")
require(r'"\{\\"scanning\\":true\}"', SOURCE,
        "browser /scan loading response changed")

for removed in ("s_scan_running", "s_scan_done", "s_scan_json"):
    if removed in SOURCE:
        raise AssertionError(f"legacy racy/one-shot scan state remains: {removed}")

require(r"make_touch_button\(section,\s*616,\s*14,\s*134,\s*48,"
        r"\s*\"Scan\"", UI, "Connectivity header Scan geometry changed")
require(r"s_wifi_scan_row.*?LV_OBJ_FLAG_HIDDEN", UI,
        "nearby-network row must be absent from the initial frame")
require(r"NETPROV_SCAN_RUNNING.*?NETPROV_SCAN_READY", UI,
        "QEMU async scan fixture is missing")
require(r"netprov_scan_get_snapshot\(snapshot\)", UI,
        "hardware UI must consume a copied netprov snapshot")
require(r"wifi_scan_use_cb.*?lv_textarea_set_text\(s_wifi_ssid,\s*network->ssid\)"
        r".*?open_keyboard_sheet\(s_wifi_password", UI,
        "Use must populate SSID and open secure-network password editing")
require(r"layout_connectivity_rows.*?s_keyboard_target == s_wifi_password.*?return;",
        UI, "periodic scan layout must preserve keyboard editing geometry")
if "esp_wifi_scan_" in UI:
    raise AssertionError("LVGL layer must not call the ESP Wi-Fi scan driver")

use_body = re.search(r"static void wifi_scan_use_cb\([^)]*\)\s*\{(.*?)\n\}",
                     UI, re.MULTILINE | re.DOTALL)
if not use_body:
    raise AssertionError("Wi-Fi scan Use callback missing")
for forbidden in ("netprov_save_config", "esp_wifi_connect", "esp_restart"):
    if forbidden in use_body.group(1):
        raise AssertionError(f"Wi-Fi scan Use must not invoke {forbidden}")

print("netprov async scan contracts: PASS")
