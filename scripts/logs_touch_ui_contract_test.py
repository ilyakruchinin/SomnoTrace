#!/usr/bin/env python3
"""Structural runtime contracts for the lazy Rev B3 native Logs surface."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
BSP = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")
CONTROLLER = (ROOT / "main/touch_logs_controller.c").read_text(encoding="utf-8")
UI = (ROOT / "main/touch_logs_ui.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing native Logs runtime contract: {description}")


def function_body(text: str, name: str, next_name: str) -> str:
    start = text.index(f"static void {name}")
    end = text.index(f"static void {next_name}", start)
    return text[start:end]


# The shared shell keeps the eight-destination rail, but no longer allocates a
# second/legacy Logs widget tree during boot.
require(BSP, r"#define\s+MANAGE_SECTION_COUNT\s+8\b",
        "eight-destination Manage rail")
rail = BSP.split("static void build_manage_page", 1)[1].split(
    "static const char *s_passkey_keyboard_map", 1)[0]
for destination in (
    "Devices", "Connectivity", "Alerts", "Uploads",
    "Storage", "System", "Logs", "Advanced",
):
    assert f'"{destination}"' in rail, f"Manage rail omits {destination}"
assert "build_logs_section" not in BSP, "legacy Logs builder remains compiled"
assert "refresh_logs_widgets" not in BSP, "legacy Logs refresh remains compiled"
assert not re.search(r"\bs_logs_", BSP), "legacy Logs widget globals remain"
require(BSP,
        r"s_manage_scrolls\[MANAGE_LOGS\]\s*=\s*destination",
        "lazy Logs destination is safe for shared scrolling checks")

# The native 7B hardware and emulator compile this UI. Both compact-board
# profiles use the original renderer and must exclude these sources.
target_match = re.search(
    r"if\(CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B OR\s*"
    r"\(CONFIG_SOMNOTRACE_BOARD_QEMU AND NOT CONFIG_SOMNOTRACE_QEMU_DISPLAY_154\)\)"
    r"(.*?)endif\(\)", CMAKE, re.DOTALL)
assert target_match, "missing native 7B source guard excluding compact QEMU"
target_block = target_match.group(1)
for source in ("touch_logs_ui.c", "touch_logs_controller.c"):
    assert f'"{source}"' in target_block, f"{source} missing from 7B/QEMU build"

# Entering Logs or returning to Manage creates/shows it; leaving either the
# destination or Manage hides it. No Logs create call exists in boot building.
active = function_body(BSP, "set_active_page", "nav_cb")
manage = function_body(BSP, "set_manage_section", "manage_section_cb")
require(active,
        r"if\s*\(page\s*==\s*2\)\s*ensure_manage_destination\(\);\s*"
        r"else\s+touch_logs_controller_hide\(\);",
        "bottom-navigation show/hide lifecycle")
require(manage,
        r"teardown_rendered_manage_destination\(\).*?"
        r"s_active_manage_section\s*=\s*section.*?"
        r"ensure_manage_destination\(\)",
        "Manage-destination teardown/build lifecycle")
require(BSP,
        r"build_manage_destination.*?case\s+MANAGE_LOGS:.*?"
        r"touch_logs_controller_show\(destination\)",
        "lazy destination creates and shows Logs")
require(BSP,
        r"teardown_rendered_manage_destination.*?"
        r"section\s*==\s*MANAGE_LOGS.*?touch_logs_controller_hide\(\)",
        "destination teardown hides Logs before release")
assert "touch_logs_controller_show" not in rail, (
    "Logs surface must not be constructed by build_manage_page"
)
require(BSP,
        r"if\s*\(section\s*==\s*MANAGE_LOGS\)\s*\{\s*"
        r"touch_logs_controller_refresh\(state->sd_ready\);\s*return;",
        "refresh is routed only for the visible Logs destination")
require(CONTROLLER,
        r"if\s*\(!controller\s*\|\|\s*!touch_logs_controller_is_visible\(\)\)"
        r"\s*return;",
        "controller refuses hidden refresh work")

# The sizeable retained-page/controller objects are PSRAM-only and every slow
# operation is a PSRAM-stack worker. The LVGL refresh path only obtains cheap
# metadata and dispatches work; it never scans 2,048 retained slots itself.
require(CONTROLLER,
        r"sizeof\(\*controller\),\s*MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT",
        "PSRAM-only controller allocation")
for job in ("snapshot_job_t", "save_job_t"):
    require(CONTROLLER,
            rf"{job}\s*\*job\s*=\s*heap_caps_calloc\(.*?"
            r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT",
            f"PSRAM-only {job} allocation")
snapshot_task = CONTROLLER.split("static void snapshot_task", 1)[1].split(
    "static bool start_snapshot_worker", 1)[0]
require(snapshot_task, r"log_stream_retained_snapshot_page\(",
        "exact paged snapshot on worker")
require(CONTROLLER,
        r"psram_task_create\(snapshot_task,\s*\"ui_log_page\"",
        "page scan uses PSRAM-stack worker")
refresh = CONTROLLER.split("void touch_logs_controller_refresh", 1)[1].split(
    "esp_err_t touch_logs_controller_destroy", 1)[0]
assert "log_stream_retained_snapshot_page" not in refresh, (
    "LVGL refresh must not scan the retained ring"
)
require(refresh,
        r"generation_changed.*?!controller->paused.*?mark_view_dirty_locked.*?"
        r"completed_revision\s*!=\s*controller->desired_revision",
        "full scan only on live generation or requested-view revision")

# Pause/search/paging use one stable strict sequence ceiling. The exact new
# count remains total_count minus the total captured at that anchor.
require(CONTROLLER,
        r"log_stream_retained_snapshot\(\s*&newest,\s*1.*?"
        r"newest.sequence\s*\+\s*1.*?before_sequence\s*=\s*before.*?"
        r"pause_total_count\s*=\s*info.total_count",
        "nonzero pause/search sequence anchor plus exact total anchor")
require(CONTROLLER,
        r"cb_search_query.*?preserve paused, before_sequence, and pause total",
        "search dismissal preserves paused anchor")
require(CONTROLLER,
        r"cb_jump_newest.*?capture_pause_anchor",
        "Jump newest reanchors but remains paused")
require(CONTROLLER,
        r"last_offset\s*=\s*matching\s*>\s*0.*?"
        r"controller->match_offset\s*<\s*last_offset.*?"
        r"TOUCH_LOGS_UI_VISIBLE_ROWS",
        "older-page offset is bounded to the last matching page")
require(CONTROLLER,
        r"page.returned\s*==\s*0.*?job->match_offset\s*>\s*0.*?"
        r"page.matching_count\s*>\s*0.*?:\s*0;",
        "ring/filter changes clamp stale offsets, including zero matches")
require(UI,
        r"info.total_count\s*-\s*s_ui->pause_anchor_total_count",
        "UI computes exact new-line count from anchor total")

# User intents dispatch immediately when idle instead of waiting for the next
# shell refresh. A pending older scan can be superseded by a newer revision.
for callback in (
    "cb_set_paused", "cb_begin_search", "cb_search_query",
    "cb_toggle_level", "cb_page_older", "cb_page_newer", "cb_jump_newest",
):
    body = CONTROLLER.split(f"static esp_err_t {callback}", 1)[1].split(
        "static esp_err_t", 1)[0]
    assert "start_snapshot_worker" in body, (
        f"{callback} delays its page response until periodic refresh"
    )
require(CONTROLLER,
        r"job->revision\s*==\s*controller->desired_revision",
        "superseded worker pages are never published")
publish = CONTROLLER.split("static void publish_model", 1)[1].split(
    "esp_err_t touch_logs_controller_show", 1)[0]
assert "snapshot_busy" not in publish, (
    "save/card/new-line status must keep painting during a page scan"
)

# Keep formatted-string work out of the cross-core spinlock; fixed-size state
# copies are sufficient once text has been normalized before entry.
for critical in re.findall(
    r"portENTER_CRITICAL\(&s_logs_lock\);(.*?)"
    r"portEXIT_CRITICAL\(&s_logs_lock\);",
    CONTROLLER,
    re.DOTALL,
):
    assert "copy_text(" not in critical and "snprintf(" not in critical, (
        "formatted text work remains inside the Logs spinlock"
    )

# Clear only touches the retained RAM service. Save and retry are determinate,
# bounded background operations, and teardown refuses while a worker owns the
# controller.
clear_body = CONTROLLER.split("static esp_err_t cb_clear_ram", 1)[1].split(
    "static esp_err_t cb_save_card", 1)[0]
assert "log_stream_retained_clear" in clear_body
for forbidden in ("remove(", "unlink(", "sd_storage"):
    assert forbidden not in clear_body, f"Clear RAM unexpectedly calls {forbidden}"
require(CONTROLLER,
        r"save_task.*?log_stream_retained_save_to_sd\(.*?save_progress.*?"
        r"psram_task_delete",
        "card save plus determinate progress stays off LVGL")
require(CONTROLLER,
        r"job->filter.before_sequence\s*=\s*controller->paused\s*\?\s*"
        r"controller->before_sequence\s*:\s*0",
        "paused save excludes lines newer than its visible anchor")
require(CONTROLLER, r"psram_task_create\(retry_task,\s*\"ui_log_retry\"",
        "connection retry stays off LVGL")
require(CONTROLLER,
        r"touch_logs_controller_destroy.*?controller->workers\s*!=\s*0.*?"
        r"return ESP_ERR_INVALID_STATE.*?touch_logs_ui_destroy.*?"
        r"heap_caps_free\(controller\)",
        "worker-safe retained teardown")

print("native Logs runtime integration contract passed")
