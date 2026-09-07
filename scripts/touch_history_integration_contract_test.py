#!/usr/bin/env python3
"""Structural contract for the rich History BSP/QEMU integration."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
DISPLAY = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
CONTROLLER = (ROOT / "main/touch_history_controller.c").read_text(encoding="utf-8")


def require(text: str, pattern: str, label: str) -> None:
    if not re.search(pattern, text, re.DOTALL):
        raise AssertionError(f"missing rich History integration: {label}")


def function(name: str) -> str:
    match = re.search(rf"static\s+[^\n]+\s+{name}\([^\)]*\)\s*\{{", DISPLAY)
    if not match:
        match = re.search(rf"(?:bool|void|esp_err_t)\s+{name}\([^\)]*\)\s*\{{", DISPLAY)
    if not match:
        raise AssertionError(f"missing function {name}")
    start = match.start()
    depth = 0
    opened = False
    for index in range(match.end() - 1, len(DISPLAY)):
        if DISPLAY[index] == "{":
            depth += 1
            opened = True
        elif DISPLAY[index] == "}":
            depth -= 1
            if opened and depth == 0:
                return DISPLAY[start:index + 1]
    raise AssertionError(f"unterminated function {name}")


for unit in ("touch_history_ui.c", "touch_history_controller.c"):
    assert f'"{unit}"' in CMAKE, f"BSP targets omit {unit}"

build = function("build_history_page")
require(build,
        r"make_plain_container\(\s*history,\s*UI_PANEL_X,\s*UI_PANEL_Y,\s*"
        r"TOUCH_HISTORY_UI_WIDTH,\s*TOUCH_HISTORY_UI_HEIGHT\s*\)",
        "992x450 History host at page-local 16,4")
require(build, r"touch_history_ui_create\(\s*s_history_host,\s*&config,\s*"
               r"&s_history_ui\s*\)", "one retained rich History tree")

init = function("bsp_display_init")
require(init, r"touch_history_controller_create\(\s*&history_config,\s*"
              r"&s_history_controller\s*\)", "controller created before UI")
require(init, r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?deterministic_preview\s*=\s*true",
        "QEMU deterministic rich provider")

page = function("set_active_page")
require(page, r"previous_page\s*==\s*1.*?"
              r"touch_history_controller_set_active\(.*?false\)",
        "History deactivation on tab exit")
require(page, r"page\s*==\s*1.*?"
              r"touch_history_controller_set_active\(.*?true\)",
        "History activation and newest auto-load on tab entry")

changed = function("history_controller_changed")
assert "__atomic_store_n" in changed
assert "lv_" not in changed and "xTaskNotify" not in changed, (
    "worker callback must only schedule an LVGL-task apply"
)
apply = function("apply_history_controller_if_needed")
require(apply, r"s_active_page\s*!=\s*1.*?return", "visible-page apply guard")
require(apply, r"touch_history_controller_revision.*?"
               r"touch_history_controller_apply", "revision-gated LVGL apply")
update = function("update_ui")
require(update, r"active_tab\s*==\s*1.*?"
              r"apply_history_controller_if_needed", "LVGL task applies revisions")

route = function("history_route_card")
require(route, r"set_manage_section\(MANAGE_STORAGE\).*?set_active_page\(2\)",
        "card error route to Manage Storage")
therapy = function("bsp_display_set_therapy_active")
require(therapy, r"therapy_finished.*?touch_history_controller_refresh",
        "therapy stop invalidates/reloads History")
seed = function("bsp_display_qemu_seed_demo")
require(seed, r"touch_history_controller_refresh", "QEMU rich-model seed refresh")

for legacy in (
    "s_history_worker_task", "s_history_trace_worker_task",
    "history_trace_task", "queue_history_trace_load", "start_history_load",
    "refresh_history_widgets", "s_services.history", "s_qemu_history_traces",
):
    assert legacy not in DISPLAY, f"duplicate legacy History path remains: {legacy}"

require(CONTROLLER, r"xSemaphoreCreateMutex\(\).*?xQueueCreate\(",
        "internal FreeRTOS control objects")
require(CONTROLLER, r"vQueueDelete\(controller->queue\).*?"
                    r"vSemaphoreDelete\(controller->mutex\).*?"
                    r"heap_caps_free\(controller\)",
        "kernel object teardown after worker exit")

print("rich History BSP integration contract passed")
