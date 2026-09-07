#!/usr/bin/env python3
"""Static contracts for the ESP32-S3 QEMU UI preview."""

from pathlib import Path
import re
from qemu_targets import target


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing QEMU UI contract: {description}")


cmake = source("main/CMakeLists.txt")
root_cmake = source("CMakeLists.txt")
lvgl_allocator = source("main/somnotrace_lvgl_psram.h")
fonts = source("main/somnotrace_fonts.h")
qemu_defaults = source("sdkconfig.qemu.defaults")
kconfig = source("main/Kconfig.projbuild")
component = source("main/idf_component.yml")
board = source("main/board_qemu.c")
display = source("main/bsp_display_7b.c")
logs_ui = source("main/touch_logs_ui.c")
maintenance_ui = source("main/touch_maintenance_ui.c")
config_ui = source("main/touch_manage_config_ui.c")
history_ui = source("main/touch_history_ui.c")
history_controller = source("main/touch_history_controller.c")
firmware_target = source("main/firmware_target.c")
demo = source("main/main_qemu.c")
defaults = source("sdkconfig.qemu.defaults")
setup = source("scripts/setup-qemu-macos.sh")
build_qemu = source("scripts/build-qemu.sh")
run = source("scripts/run-qemu-ui.sh")
smoke = source("scripts/test-qemu-ui.sh")
boot_smoke = source("scripts/qemu_boot_smoke.py")
touch_smoke = source("scripts/test-qemu-touch.py")
capture_smoke = source("scripts/capture-qemu-ui.py")
qemu_patch = source("scripts/qemu-rgb-1024.patch")
touch_patch = source("scripts/qemu-touch.patch")
stability_patch = source("scripts/qemu-emulator-stability.patch")

require(kconfig, r"config SOMNOTRACE_BOARD_QEMU.*?bool \"ESP32-S3 QEMU UI emulator\"",
        "selectable QEMU board profile")
require(kconfig, r"config SOMNOTRACE_QEMU_DISPLAY_154.*?depends on SOMNOTRACE_BOARD_QEMU.*?default n",
        "compact display is subordinate to QEMU and disabled by default")
require(cmake, r"elseif\(CONFIG_SOMNOTRACE_BOARD_QEMU\).*?if\(CONFIG_SOMNOTRACE_QEMU_DISPLAY_154\).*?"
               r'"main_qemu_154.c".*?"board_qemu_154.c".*?"bsp_display.c".*?else\(\).*?'
               r'"main_qemu.c".*?"board_qemu.c".*?"bsp_display_7b.c"',
        "QEMU selects the original compact renderer or native 7B renderer")
require(cmake, r"CONFIG_SOMNOTRACE_BOARD_QEMU AND NOT CONFIG_SOMNOTRACE_QEMU_DISPLAY_154",
        "compact renderer excludes the native 7B touch services")
assert "-u somnotrace_firmware_target" in cmake
assert "CONFIG_SOMNOTRACE_QEMU_DISPLAY_154=y" in source("sdkconfig.qemu-154.defaults")
assert "CONFIG_SOMNOTRACE_QEMU_DISPLAY_154=y" not in qemu_defaults
assert target() == target("7b")
assert (target("154")["width"], target("154")["height"]) == (240, 240)
assert (target("7b")["width"], target("7b")["height"]) == (1024, 600)
assert target("154")["ready_log"] == "240x240 original-board UI preview ready"
for unit in ("main_qemu.c", "board_qemu.c", "bsp_display_7b.c"):
    assert f'"{unit}"' in cmake, f"QEMU build omits {unit}"
assert "espressif/esp_lcd_qemu_rgb" in component
require(board, r"\.width\s*=\s*WAVESHARE_7B_H_RES", "native panel width")
require(board, r"\.height\s*=\s*WAVESHARE_7B_V_RES", "native panel height")
require(board, r"QEMU_RGB_TOUCH_POSITION", "QEMU touch MMIO bridge")
require(board, r"was_pressed\s*&&\s*!\*pressed.*?QEMU touch release sampled",
        "observable guest release edge for deterministic scripted taps")
require(display, r"display_driver\.hor_res\s*=\s*WAVESHARE_7B_H_RES", "LVGL width")
require(display, r"display_driver\.ver_res\s*=\s*WAVESHARE_7B_V_RES", "LVGL height")
assert "LV_OPA_COVER" in display
require(display, r"board_qemu_touch_read", "LVGL QEMU touch reader")
require(display, r"s_qemu_requested_tab", "display-task-owned page switching")
require(display, r"#define\s+UI_UPDATE_MS\s+50\b", "responsive 20 Hz UI update cadence")
require(display,
        r"state\.flow_version\s*-\s*seen_flow_version.*?"
        r"live_flow_presentation_due\(&flow_presented_us,.*?now_us, pending\).*?"
        r"append_flow_visual.*?"
        r"seen_flow_version\s*\+=\s*consume",
        "elapsed-time presentation of burst-delivered 25 Hz flow positions")
require(display,
        r"if\s*\(!backlight\).*?home_was_active\s*=\s*false.*?return",
        "wake-time flow resynchronisation")
require(display,
        r"glow\.round_start\s*=\s*0;.*?glow\.round_end\s*=\s*0;.*?"
        r"trace\.round_start\s*=\s*0;.*?trace\.round_end\s*=\s*0;",
        "bead-free live graph strokes")
require(display, r"esp_lcd_rgb_qemu_get_frame_buffer\(s_panel,\s*&qemu_vram\).*?fb1\s*=\s*\(lv_color_t\s*\*\)qemu_vram\s*\+.*?WAVESHARE_7B_H_RES\s*\*\s*WAVESHARE_7B_V_RES",
        "second-half QEMU VRAM draw buffer")
assert "display_driver.full_refresh" not in display
require(display, r"display_driver\.direct_mode\s*=\s*1;",
        "persistent dirty-region composition")
require(display, r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?if\s*\(!lv_disp_flush_is_last\(drv\)\).*?lv_disp_flush_ready\(drv\).*?return;.*?0,\s*0,\s*WAVESHARE_7B_H_RES,\s*WAVESHARE_7B_V_RES,\s*pixels",
        "single completed QEMU composition handoff")
require(display, r"QEMU UI first frame published",
        "initial-frame input synchronization")
require(display,
        r"if\s*\(start\s*&&\s*!bsp_display_set_therapy_active\(true\)\).*?"
        r"result\s*=\s*ESP_ERR_INVALID_STATE.*?"
        r"if\s*\(!start\)\s*\(void\)bsp_display_set_therapy_active\(false\)",
        "QEMU therapy action respects a committed restart")
require(root_cmake, r"LV_MEM_CUSTOM_ALLOC=somnotrace_lvgl_alloc",
        "QEMU exercises the 7-inch PSRAM-first LVGL allocator")
require(lvgl_allocator, r"heap_caps_malloc_prefer.*?MALLOC_CAP_SPIRAM.*?MALLOC_CAP_INTERNAL",
        "PSRAM-first allocation with internal fallback")
require(qemu_defaults, r"CONFIG_LV_USE_FONT_COMPRESSED=y",
        "compressed custom-font renderer")
require(display, r'#include\s+"somnotrace_fonts\.h"', "custom bedside fonts")
for family in ("space_grotesk", "ibm_plex_mono"):
    assert family in fonts, f"missing {family} font declarations"
assert "somnotrace_space_grotesk_semibold_32" in fonts
for pattern, description in (
    (r"#define\s+UI_HEADER_H\s+64\b", "64px shared header"),
    (r"#define\s+UI_CONTENT_Y\s+64\b", "content y origin"),
    (r"#define\s+UI_CONTENT_H\s+462\b", "462px shared content region"),
    (r"#define\s+UI_NAV_H\s+74\b", "74px shared navigation region"),
    (r"#define\s+UI_PANEL_X\s+16\b", "16px shared panel inset"),
    (r"#define\s+UI_PANEL_Y\s+4\b", "shared panels begin at absolute y=68"),
    (r"#define\s+UI_PANEL_H\s+450\b", "shared panels end at absolute y=518"),
    (r"#define\s+UI_NAV_PILL_X\s+254\b", "first bottom-nav pill x"),
    (r"#define\s+UI_NAV_PILL_STEP\s+175\b", "bottom-nav pill stride"),
    (r"#define\s+UI_NAV_PILL_Y\s+8\b", "bottom-nav pill y"),
    (r"#define\s+UI_NAV_PILL_W\s+166\b", "bottom-nav pill width"),
    (r"#define\s+UI_NAV_PILL_H\s+54\b", "bottom-nav pill height"),
    (r"#define\s+UI_MANAGE_RAIL_W\s+212\b", "212px Manage rail"),
    (r"#define\s+UI_MANAGE_DETAIL_X\s+240\b", "12px Manage panel gap"),
    (r"#define\s+UI_MANAGE_DETAIL_W\s+768\b", "768px Manage detail"),
    (r"#define\s+UI_MANAGE_SCROLL_W\s+740\b", "14px-inset Manage viewport"),
    (r"#define\s+UI_MANAGE_SCROLL_H\s+360\b", "Manage viewport bottom inset"),
    (r"#define\s+UI_MANAGE_ROW_W\s+726\b", "Manage scrollbar gutter"),
    (r"#define\s+UI_MANAGE_ROW_FULL_W\s+740\b", "full-width Manage row"),
    (r"s_pages\s*\[\s*3\s*\]", "three primary QEMU pages"),
    (r"s_nav_buttons\s*\[\s*3\s*\]", "three custom QEMU navigation buttons"),
    (r"set_active_page\s*\(", "custom page selection"),
):
    require(display, pattern, description)
for pattern, description in (
    (r"make_card\(home,\s*UI_PANEL_X,\s*UI_PANEL_Y,\s*680,\s*132\)",
     "Home grid uses the shared top-left frame"),
    (r"make_card\(home,\s*UI_PANEL_X,\s*150,\s*680,\s*304\)",
     "Home graph reaches the shared panel bottom"),
    (r"make_touch_button\(home,\s*710,\s*338,\s*298,\s*116",
     "Home action reaches x=1008 and y=518"),
    (r"make_plain_container\(\s*history,\s*UI_PANEL_X,\s*UI_PANEL_Y,\s*"
     r"TOUCH_HISTORY_UI_WIDTH,\s*TOUCH_HISTORY_UI_HEIGHT\s*\)",
     "rich History surface uses the shared 992x450 frame"),
    (r"make_card\(manage,\s*UI_PANEL_X,\s*UI_PANEL_Y,\s*"
     r"UI_MANAGE_RAIL_W,\s*UI_PANEL_H\)",
     "Manage rail is (16,68) 212x450"),
    (r"make_card\(manage,\s*UI_MANAGE_DETAIL_X,\s*UI_PANEL_Y,\s*"
     r"UI_MANAGE_DETAIL_W,\s*UI_PANEL_H\)",
     "Manage detail is (240,68) 768x450"),
    (r"rail,\s*0,\s*i\s*\*\s*52,\s*196,\s*46",
     "all eight Manage destinations use 46px visual rows"),
    (r"nav,\s*UI_NAV_PILL_X\s*\+\s*i\s*\*\s*UI_NAV_PILL_STEP,\s*"
     r"UI_NAV_PILL_Y,\s*UI_NAV_PILL_W,\s*UI_NAV_PILL_H",
     "exact 166x54 bottom-nav pill geometry"),
):
    require(display, pattern, description)
assert "lv_tabview_create" not in display
assert "lv_tabview_add_tab" not in display
for token, value in (
    ("COLOR_BASE", "0x05070e"),
    ("COLOR_PANEL", "0x181c29"),
    ("COLOR_CARD", "0x101421"),
    ("COLOR_CONTROL", "0x2d333f"),
):
    require(display, rf"#define\s+{token}\s+{value}\b",
            f"handoff {token.lower()} slate token")
for obsolete_gradient_token in (
    "COLOR_BASE_TOP", "COLOR_BASE_END", "COLOR_PANEL_TOP", "COLOR_PANEL_END"
):
    assert obsolete_gradient_token not in display
require(display,
        r"make_card.*?bg_color\(card,\s*lv_color_hex\(COLOR_PANEL\).*?"
        r"bg_grad_dir\(card,\s*LV_GRAD_DIR_NONE.*?"
        r"border_side\(card,\s*LV_BORDER_SIDE_TOP",
        "RGB565-safe panel surface with top highlight")
for page in ("Home", "History", "Manage"):
    require(display, rf'"{page}"', f"{page} QEMU navigation label")
for section_label in (
    "Devices", "Connectivity", "Alerts", "Uploads",
    "Storage", "System", "Logs", "Advanced",
):
    require(display, rf'"{section_label}"', f"{section_label} Manage rail label")

# The QEMU build exercises the same content-sized header status capsule as the
# panel. Protect its one-line geometry, centre alignment, and right-side inset.
for pattern, description in (
    (r"status_label_width.*?lv_txt_get_size\(&size,\s*lv_label_get_text\(label\),\s*"
     r"FONT_BODY,\s*0,\s*0,\s*LV_COORD_MAX,\s*LV_TEXT_FLAG_NONE\).*?"
     r"return\s+LV_MAX\(size\.x,\s*1\)",
     "content-measured status labels"),
    (r"layout_status_capsule.*?font_h\s*=\s*lv_font_get_line_height\(FONT_BODY\).*?"
     r"label_y\s*=\s*\(STATUS_CAPSULE_H\s*-\s*font_h\)\s*/\s*2.*?"
     r"dot_y\s*=\s*label_y\s*\+\s*"
     r"\(font_h\s*-\s*STATUS_CAPSULE_DOT_SIZE\)\s*/\s*2",
     "vertically centred status dots and labels"),
    (r"layout_status_capsule.*?text_w\s*=\s*status_label_width\(labels\[i\]\).*?"
     r"lv_label_set_long_mode\(labels\[i\],\s*"
     r"LV_LABEL_LONG_CLIP\).*?lv_obj_set_size\(labels\[i\],\s*text_w,\s*font_h\)",
     "single-line non-wrapping status labels"),
    (r"#define\s+STATUS_CAPSULE_RIGHT\s+1006\b.*?"
     r"#define\s+STATUS_CAPSULE_RIGHT_PAD\s+18\b.*?"
     r"lv_obj_set_pos\(s_status_chevron,\s*cursor,.*?"
     r"cursor\s*\+=\s*14\s*\+\s*STATUS_CAPSULE_RIGHT_PAD.*?"
     r"lv_obj_set_pos\(s_status_capsule,\s*STATUS_CAPSULE_RIGHT\s*-\s*cursor,\s*7\).*?"
     r"lv_obj_set_size\(s_status_capsule,\s*cursor,\s*STATUS_CAPSULE_H\)",
     "right-anchored status capsule with chevron padding"),
):
    require(display, pattern, description)

require(display,
        r"static\s+bool\s+set_label_text_if_changed.*?"
        r"strcmp\(current,\s*text\)\s*==\s*0\)\s*return\s+false;.*?"
        r"lv_label_set_text\(label,\s*text\);\s*return\s+true;",
        "status relayout change signal")
require(display,
        r"bool\s+status_capsule_layout_dirty\s*=\s*false;.*?"
        r"status_capsule_layout_dirty\s*\|=.*?s_sd_label.*?"
        r"status_capsule_layout_dirty\s*\|=.*?s_wifi_label.*?"
        r"status_capsule_layout_dirty\s*\|=.*?s_ble_label.*?"
        r"if\s*\(status_capsule_layout_dirty\)\s*layout_status_capsule\(\);",
        "status capsule relayout only after visible text changes")

# Protect the concrete bedside state vocabulary, not just the shell. These
# literals correspond to visible controls or explicit loading/degraded states
# in the handoff and must remain present in the firmware exercised by QEMU.
bedside_ui = "\n".join((display, logs_ui, history_ui, history_controller,
                         maintenance_ui, config_ui))
for literal, description in (
    ("Therapy active", "active therapy state"),
    ("Therapy stopped", "stopped therapy state"),
    ("Starting...", "therapy start busy state"),
    ("Stopping...", "therapy stop busy state"),
    ("Pair a device", "unpaired primary state"),
    ("Therapy stopped unexpectedly", "interruption alert state"),
    ("Acknowledge", "interruption acknowledgement control"),
    ("Loading History…", "History initial auto-load state"),
    ("Updating…", "History zoom state"),
    ("ALL %u RECORDED\\nNIGHTS", "complete History index count"),
    ("No recorded nights yet", "History empty state"),
    ("Could not read the card", "History error state"),
    ("The card is busy. Try again in a moment.", "History card-busy state"),
    ("Select a recorded night", "History unselected state"),
    ("Retry", "History error recovery action"),
    ("Trend review only. Not a diagnosis or a prescription.", "History safety copy"),
    ("Searching for nearby machines", "AirSense scanning state"),
    ("Connecting securely", "AirSense connecting state"),
    ("Enter the 4-digit code shown on your AirSense", "AirSense passcode state"),
    ("Confirming the code", "AirSense confirmation state"),
    ("Pairing failed · enable pairing mode first", "actionable AirSense pairing error state"),
    ("First on AirSense: More › MyAir App › OK, downloaded › Connect",
     "machine-first AirSense pairing prerequisite"),
    ("AirSense is ready", "explicit AirSense pairing-mode acknowledgement"),
    ("Saved settings apply after restart.", "configuration restart state"),
    ("Wi-Fi saved; restart deferred while recording", "saved Wi-Fi deferred notice"),
    ("Send test push", "alert test control"),
    ("Capacity and file counts unavailable", "unknown storage census"),
    ("Destination status and retry progress", "uploads summary"),
    ("Save to card", "native retained-log export"),
    ("Touch and display controllers", "measured controller diagnostics route"),
    ("Export report", "diagnostic export control"),
    ("Firmware has not been checked.", "truthful firmware status value"),
):
    assert literal in bedside_ui, f"missing bedside state: {description}"

# These routes must build the real maintenance surface, and its backend must
# remain responsible for work after a detail pane is destroyed. Behavior and
# failure injection are covered by maintenance_model_test / OTA race tests.
require(display, r"touch_maintenance_ui_show\(section,\s*destination,\s*&hooks\)",
        "native maintenance detail routing")
require(display, r"touch_maintenance_ui_destroy\(\)",
        "maintenance detail teardown on navigation")
for action in ("BUTTON_FIRMWARE", "BUTTON_DISPLAY", "BUTTON_EXPORT",
               "BUTTON_DELETE", "BUTTON_RESET", "BUTTON_FORMAT"):
    require(maintenance_ui, rf"case\s+{action}\s*:", f"{action} native event route")
require(display, r"if\s*\(active_tab\s*!=\s*2\)\s*return;\s*"
                 r"refresh_manage_rail\(state\);",
        "persistent rail refresh before lazy detail returns")
require(display, r'"Waiting for (?:therapy|breathing) data(?:\.\.\.|…)?"',
        "first-sample loading state")
require(display, r"flow_count\s*>=\s*FLOW_READY_POINTS",
        "valid sample threshold before chart becomes live")
require(display, r'"Therapy status unknown"', "stale AirSense state")
for channel in ("Breathing / Flow", "Pressure", "Leak", "Flow limit",
                "Snore", "SpO₂", "Pulse", "Motion"):
    assert channel in history_ui, f"missing rich History channel: {channel}"
require(history_ui, r"history_ui_graph_draw.*?aggregation\s*==\s*"
                    r"TOUCH_HISTORY_AGGREGATION_ENVELOPE.*?cursor_x",
        "one custom-draw rich graph with envelope and cursor")
require(history_ui, r"history_ui_draw_event_lane.*?"
                    r"history_ui_event_color\(marker->type\).*?"
                    r"history_ui_draw_rect",
        "source-colored square event lane")
require(demo, r"QEMU preview.*simulated data", "honest simulated-data labeling")
require(display, r"simulated preview", "honest simulated device status")
require(demo, r"bsp_display_qemu_seed_demo\(\)", "deterministic histories and devices")
require(demo, r"click to emulate touch", "interactive touch preview")
require(demo, r"log_stream_init\(\).*?bsp_display_init\(\)",
        "live retained Logs feed starts before the QEMU shell")
assert "bsp_display_qemu_set_tab(tab)" not in demo

for setting in (
    "CONFIG_SOMNOTRACE_BOARD_QEMU=y",
    "CONFIG_SPIRAM_MODE_QUAD=y",
    "# CONFIG_SPIRAM_MODE_OCT is not set",
    "# CONFIG_SPI_FLASH_AUTO_SUSPEND is not set",
    "CONFIG_ESP_CONSOLE_UART_DEFAULT=y",
    "CONFIG_LV_COLOR_DEPTH_16=y",
    "CONFIG_LV_SPRINTF_USE_FLOAT=y",
):
    assert setting in defaults, f"missing QEMU sdkconfig default: {setting}"

assert "40edccac415693c5130f91c01d84176ae6008566" in setup
assert "ESP_RGB_MAX_WIDTH   (1024)" in qemu_patch
assert "esp-rgb-touch" in touch_patch
assert "A_RGB_TOUCH_STATUS" in touch_patch
assert "s->width * s->height * s->bpp" in touch_patch
assert "qemu-touch.patch" in setup
assert "qemu-emulator-stability.patch" in setup
assert 'PATCH_REVISION="4"' in setup
assert "dpy_gfx_update(s->con, 0, 0, s->width, s->height)" in stability_patch
assert "touch_press_latched" in stability_patch
assert "SomnoTrace: retain a complete short click" in stability_patch
require(stability_patch, r"s->touch_pressed\s*=\s*false;.*?s->touch_press_latched\s*=\s*false;",
        "touch latch reset state")
assert "--disable-dbus-display" in setup
assert "SomnoTrace QEMU is already running" in run
assert "input-send-event" in touch_smoke
assert "emulated touch at" in touch_smoke
assert "emulated touch selected page 1" in touch_smoke
assert "QEMU UI first frame published" in touch_smoke
require(touch_smoke, r"click_latency\s*>\s*1\.5",
        "bounded post-render navigation latency")
require(touch_smoke, r"x\s*=\s*round\(512\s*\*\s*32767\s*/\s*1023\)",
        "synthetic click uses History pill horizontal centre")
require(touch_smoke, r"y\s*=\s*round\(563\s*\*\s*32767\s*/\s*599\)",
        "synthetic click uses History pill vertical centre")
require(build_qemu,
        r"SDKCONFIG_DEFAULTS=.*?reconfigure.*?SDKCONFIG_DEFAULTS=.*?build",
        "QEMU firmware refreshes its git-derived version before building")
for driver in (touch_smoke, capture_smoke):
    require(driver, r'wait_for_log\(process, log_path, "QEMU touch release sampled", 3',
            "next scripted press waits for the guest to sample release")
    require(driver,
            r'"button":\s*"left",\s*"down":\s*False.*?time\.sleep\(0\.08\)',
            "scripted touch release is sampled before the next press")
for contract, description in (
    ('"-display", "sdl,show-cursor=off"', "cursor-free capture input backend"),
    ('"screendump"', "QMP framebuffer capture"),
    ('dimensions != (1024, 600)', "native capture dimension validation"),
    ('len(sampled_colours) < 8', "blank-frame rejection"),
    ('"Guru Meditation Error"', "runtime panic rejection"),
    ('"home": (0, (330, 563))', "deterministic Home selection"),
    ('"history": (1, (512, 563))', "deterministic History selection"),
    ('"manage": (2, (694, 563))', "deterministic Manage selection"),
    ('"--screen"', "selective screen capture option"),
    ('"--representative"', "literal handoff-state capture option"),
    ('"--interaction-states"', "additional interaction-state capture option"),
    ('"--interaction-state"', "selective interaction-state capture option"),
    ('"devices"', "therapy-stopped Devices capture"),
    ('"system-display-controls"', "System display-controls capture"),
    ('"system-display-timeout-open"', "open timeout dropdown capture"),
    ('"logs"', "native retained-log capture"),
    ('"history-calendar"', "History left-rail Calendar capture"),
    ('"history-calendar-selection"',
     "Calendar selection persistence capture"),
    ('"connectivity-password-keyboard"', "open password-keyboard capture"),
    ('"connectivity-password-revealed"', "revealed password capture"),
    ('"connectivity-password-remasked"', "remasked password capture"),
    ('((110, 150), 0.5, "__wifi_ready__")', "Connectivity rail and saved-list readiness"),
    ('((130, 420), 0.6, "QEMU native Logs pane ready")',
     "Logs rail interaction and lazy-render synchronization"),
    ('"__drag__"', "System display-controls scroll gesture"),
    ('((550, 175), 0.5, "__view_changed__")', "saved-network editor readiness"),
    ('((690, 223), 0.5, "__password_ready__")', "native password-editor readiness"),
    ('((38, 135), 0.75, None)', "previous calendar-month coordinate"),
    ('((36, 433), 1.0, "day=20260831")',
     "August 31 calendar selection synchronization"),
    ('((575, 204), 1.0, "signal=1")',
     "Calendar persistence through right-detail reload"),
    ('light_surface_samples(', "calendar-specific light-surface validation"),
    ('(164, 72, 300, 110)', "selected Calendar segment signature"),
    ('(20, 72, 156, 110)', "deselected List segment signature"),
    ('(18, 410, 58, 452)', "selected August 31 cell signature"),
    ('start_offset=log_offset', "new touch/page log synchronization"),
    ('"QEMU UI first frame published"', "initial-frame capture synchronization"),
    ('validate_persistent_shell(', "persistent shell capture validation"),
    ('"Home navigation":', "persistent editor Home navigation validation"),
    ('"History navigation":', "persistent editor History navigation validation"),
    ('"Manage navigation":', "persistent editor Manage navigation validation"),
    ('validate_interaction_frame(name, payload)', "keyboard clipping rejection"),
):
    assert contract in capture_smoke, f"capture smoke omits {description}"
require(display, r"s_keyboard_sheet.*?keyboard_sheet_action_cb",
        "explicit touch keyboard sheet with completion actions")
require(display, r"s_text_keyboard_lower_map.*?\"q\".*?\"p\".*?LV_SYMBOL_BACKSPACE.*?LV_SYMBOL_UP.*?\"123\".*?\"@\".*?\"space\".*?\"-\".*?\"_\"",
        "five-row handoff text keyboard")
require(display, r"lv_obj_remove_event_cb\(s_keyboard,\s*lv_keyboard_def_event_cb\).*?keyboard_cb",
        "custom functional keyboard legends")
require(config_ui, r"case\s+MC_WIFI_PASSWORD:.*?editor\(action,\s*\"Network password\"",
        "native password-editing destination")
require(display, r"lv_obj_set_align\(s_keyboard,\s*LV_ALIGN_TOP_LEFT\)",
        "visible top-aligned keyboard geometry")
require(display, r"lv_obj_set_y\(s_keyboard,\s*top\s*==\s*356\s*\?\s*58\s*:\s*67\).*?top\s*==\s*356\s*\?\s*168\s*:\s*203",
        "handoff keyboard and keypad bounds")
require(config_ui, r"case\s+V_EDITOR:.*?lv_obj_set_size\(u->field,\s*728,\s*56\).*?"
                   r"lv_obj_set_pos\(u->keyboard,\s*20,\s*207\).*?"
                   r"lv_obj_set_size\(u->keyboard,\s*728,\s*232\)",
        "visible field and keyboard stay inside the native detail pane")
require(config_ui, r"if\s*\(u->secret\)\s*button\(\"Show\",\s*630,\s*148,\s*118,\s*44,\s*A_SHOW\)",
        "touch-sized native password reveal control")
require(config_ui, r"case\s+A_SHOW:.*?lv_textarea_set_password_mode.*?"
                   r"lv_textarea_get_password_mode.*?\"Show\".*?\"Hide\"",
        "native password reveal and remask behavior")
require(config_ui, r"lv_textarea_set_password_mode\(u->field,\s*u->secret\)",
        "new secret editors start masked")
require(config_ui, r"touch_manage_config_hide\(void\).*?lv_timer_del.*?lv_obj_del.*?"
                   r"memset\(s_ui,\s*0,\s*sizeof\(\*s_ui\)\)",
        "editor navigation destroys widgets and clears copied form values")
require(display,
        r"has_scroll_gutter.*?s_manage_scrolls\[MANAGE_DEVICES\].*?"
        r"s_manage_scrolls\[MANAGE_CONNECTIVITY\].*?"
        r"s_manage_scrolls\[MANAGE_UPLOADS\].*?"
        r"s_manage_scrolls\[MANAGE_SYSTEM\].*?UI_MANAGE_ROW_W.*?"
        r"UI_MANAGE_ROW_FULL_W",
        "scrollbar gutter only on overflowing Manage sections")
require(display, r"bool\s+show_device_change\s*=\s*as_paired\s*\|\|\s*ox_paired;",
        "paired-state therapy-change explanation")
require(display, r"state\.paired\s*\?\s*0x636975",
        "dim stopped-therapy orb")
for launcher in (run, boot_smoke):
    assert "nvram.esp32s3.efuse" in launcher
    assert "timer.esp32s3.timg" in launcher
assert "-m 8M" in run
assert '"-m", "8M"' in boot_smoke
assert '"${SCRIPT_DIR}/qemu_boot_smoke.py" --board "${BOARD}"' in smoke
assert "ELF file SHA256:" in boot_smoke
for launcher in (build_qemu, run, smoke):
    assert "qemu_targets.py" in launcher
    assert '--board "${BOARD}"' in launcher
assert "-display sdl,show-cursor=on" in run
for failure in ("Invalid drawing area", "assert failed", "Guru Meditation Error"):
    assert failure in boot_smoke, f"smoke test does not reject {failure}"
assert "CONFIG_ESP_MAIN_TASK_STACK_SIZE=14336" in qemu_defaults
for path in ("main/main_qemu.c", "main/main_qemu_154.c"):
    preview = source(path)
    forbidden = ["touch_history", "touch_maintenance",
                 "as11_ble_init", "net_provision_init", "sd_storage_init"]
    if path.endswith("_154.c"):
        forbidden.extend(["first_run_setup", "touch_logs"])
    for feature in forbidden:
        assert feature not in preview, (path, feature)
for identity in ("waveshare-7b", "waveshare-154", "qemu-ui", "qemu-154"):
    assert f'"{identity}"' in firmware_target

print("QEMU UI contract passed")
