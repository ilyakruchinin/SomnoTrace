"""Static contracts for the persisted display inactivity timeout."""

from pathlib import Path

import re

ROOT = Path(__file__).resolve().parents[1]

HEADER = (ROOT / "main/device_settings.h").read_text(encoding="utf-8")

SOURCE = (ROOT / "main/device_settings.c").read_text(encoding="utf-8")

PORTAL = (ROOT / "main/portal.html").read_text(encoding="utf-8")

DISPLAY_HEADER = (ROOT / "main/bsp_display.h").read_text(encoding="utf-8")

TOUCH_BSP = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")

TOUCH_BOARD = (ROOT / "main/board_waveshare_7b.c").read_text(encoding="utf-8")

COMPACT_BSP = (ROOT / "main/bsp_display.c").read_text(encoding="utf-8")

HOST_TEST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")

QEMU_TOUCH_TEST = (ROOT / "scripts/test-qemu-touch.py").read_text(
    encoding="utf-8"
)

def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing screen-timeout contract: {description}")

require(HEADER, r"uint16_t\s+screen_timeout_s\s*;",
        "timeout is stored as explicit seconds")

require(HEADER,
        r"device_settings_set_screen_timeout_s\s*\(\s*uint16_t\s+seconds\s*\)",
        "validated public setter")

require(SOURCE, r'#define\s+NVS_KEY_SCREEN_TIMEOUT\s+"scr_tmo"',
        "stable NVS key")

require(SOURCE,
        r'NVS_KEY_LEGACY_LCD_THERAPY\s+"lcd_thr".*?'
        r'therapy_screen_err\s*==\s*ESP_ERR_NVS_NOT_FOUND.*?'
        r'backlight_mode_err\s*==\s*ESP_ERR_NVS_NOT_FOUND.*?'
        r'legacy_mode\s*==\s*3\s*\?.*?THERAPY_SCREEN_INFO.*?'
        r'legacy_mode\s*==\s*1\s*\?.*?BACKLIGHT_MODE_OFF_THRP.*?'
        r'legacy_mode\s*==\s*2\s*\?.*?BACKLIGHT_MODE_ALWAYS_OFF',
        "legacy combined display setting migrates only into absent split fields")

require(SOURCE,
        r"CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B\s*\|\|\s*\\\s*"
        r"\(CONFIG_SOMNOTRACE_BOARD_QEMU\s*&&\s*!CONFIG_SOMNOTRACE_QEMU_DISPLAY_154\)"
        r".*?DEFAULT_SCREEN_TIMEOUT_S\s+300"
        r".*?#else.*?DEFAULT_SCREEN_TIMEOUT_S\s+0",
        "five-minute 7B/QEMU default and disabled compact-board default")

validator = re.search(
    r"static bool screen_timeout_is_valid\s*\([^)]*\)\s*\{(.*?)\n\}",
    SOURCE, re.MULTILINE | re.DOTALL)

if not validator:
    raise AssertionError("screen-timeout validator is missing")

allowed = {int(value) for value in re.findall(r"case\s+(\d+)\s*:",
                                               validator.group(1))}

assert allowed == {0, 60, 120, 300, 900, 1800}, (
    f"screen-timeout values changed: {sorted(allowed)}")

require(SOURCE,
        r"cfg->screen_timeout_s\s*=\s*DEFAULT_SCREEN_TIMEOUT_S.*?"
        r"nvs_get_u16\s*\(\s*h\s*,\s*NVS_KEY_SCREEN_TIMEOUT",
        "default-first backward-compatible NVS load")

require(SOURCE,
        r"nvs_set_u16\s*\(\s*h\s*,\s*NVS_KEY_SCREEN_TIMEOUT\s*,\s*"
        r"cfg\.screen_timeout_s\s*\)",
        "NVS persistence")

require(SOURCE,
        r"device_settings_set_screen_timeout_s.*?screen_timeout_is_valid\(seconds\)"
        r".*?s_settings\.screen_timeout_s\s*=\s*seconds"
        r".*?bsp_display_restart_idle_timeout\(\)"
        r".*?bsp_display_apply_backlight_policy\(false\)",
        "setter validation, fresh idle window, and immediate policy application")

require(SOURCE,
        r'cJSON_AddNumberToObject\(root,\s*"screen_timeout_s",\s*'
        r'settings\.screen_timeout_s\)',
        "JSON GET uses a coherent settings snapshot")

require(SOURCE,
        r'device_settings_t\s+cfg\s*=\s*s_settings.*?'
        r'cJSON_GetObjectItem\(root,\s*'
        r'"screen_timeout_s"\).*?screen_timeout_is_valid\(val\).*?'
        r'cfg\.screen_timeout_s\s*=\s*\(uint16_t\)val',
        "validated partial JSON POST")

require(HEADER, r"device_settings_save_current\s*\(void\)",
        "interactive settings can persist the current state")

require(HEADER,
        r"device_settings_snapshot\s*\(\s*device_settings_t\s*\*out\s*\)",
        "readers can obtain a coherent settings snapshot")

require(SOURCE,
        r"copy_current_settings.*?\*revision\s*=\s*s_settings_revision",
        "settings snapshots carry their revision")

require(SOURCE,
        r"persist_current_locked.*?copy_current_settings\(&s_save_work,\s*&revision\).*?"
        r"nvs_writer_run\(do_device_settings_save,\s*&s_save_work\).*?"
        r"revision\s*==\s*s_settings_revision.*?if\s*\(stable\)\s*return ESP_OK",
        "a concurrent change causes the durable snapshot to be retried")

require(PORTAL,
        r'id="screen-timeout".*?onchange="onScreenTimeoutChange\(this\)"',
        "web timeout select")

timeout_select = re.search(
    r'<select id="screen-timeout".*?</select>', PORTAL,
    re.MULTILINE | re.DOTALL)

if not timeout_select:
    raise AssertionError("screen-timeout web select is missing")

options = dict(re.findall(
    r'<option value="(0|60|120|300|900|1800)"(?: selected)?>([^<]+)</option>',
    timeout_select.group(0)))

assert options == {
    "0": "Never",
    "60": "1 minute",
    "120": "2 minutes",
    "300": "5 minutes",
    "900": "15 minutes",
    "1800": "30 minutes",
}, f"screen-timeout web choices changed: {options}"

require(PORTAL,
        r"function onScreenTimeoutChange.*?JSON\.stringify\(\{\s*"
        r"screen_timeout_s:\s*seconds\s*\}\)",
        "immediate web save")

require(PORTAL,
        r"renderDeviceSettings.*?d\.screen_timeout_s.*?timeoutCtl\.value",
        "web setting hydration")

require(PORTAL,
        r"function renderDeviceSettings\(d\).*?d\.screen_timeout_s.*?"
        r"function loadDeviceSettings\(\).*?renderDeviceSettings\(d\)",
        "single device-settings renderer supports direct and aggregate hydration")

require(PORTAL,
        r"fetch\('/api/settings/all'\).*?"
        r"if \(d\.device\) renderDeviceSettings\(d\.device\)",
        "aggregate settings load hydrates the display controls without fallback")

require(PORTAL,
        r"Only while therapy is stopped\..*?first touch wakes",
        "web UI explains therapy and wake-only behavior")

require(DISPLAY_HEADER, r"void\s+bsp_display_restart_idle_timeout\s*\(void\)",
        "cross-board idle-window hook")

require(TOUCH_BSP,
        r"void\s+bsp_display_restart_idle_timeout\s*\(void\).*?"
        r"s_last_touch_activity_us\s*=\s*now_us",
        "touch BSP restarts the monotonic activity window")

require(COMPACT_BSP,
        r"void\s+bsp_display_restart_idle_timeout\s*\(void\)",
        "compact BSP keeps the shared settings API linkable")

require(TOUCH_BSP, r"#define\s+UI_ACTION_SCREEN_OFF\s+4\b",
        "one shared manual screen-off action")

require(TOUCH_BSP, r"#define\s+UI_HEADER_SCREEN_OFF_X\s+500\b",
        "screen-off control leaves the date clear")

require(TOUCH_BSP, r"#define\s+UI_HEADER_SCREEN_OFF_W\s+132\b",
        "screen-off control has a generous pure-touch target")

shared_header = TOUCH_BSP.split(
    "lv_obj_t *header = make_plain_container", 1)[1].split(
        "for (int i = 0; i < 3; ++i)", 1)[0]

require(shared_header,
        r"header_screen_off\s*=\s*make_touch_button\(\s*"
        r"header,\s*UI_HEADER_SCREEN_OFF_X,\s*7,\s*"
        r"UI_HEADER_SCREEN_OFF_W,\s*STATUS_CAPSULE_H,\s*"
        r'"Screen off",\s*COLOR_CONTROL,\s*action_cb,\s*'
        r"UI_ACTION_SCREEN_OFF\s*\)",
        "screen-off action belongs to the shared header")

require(shared_header,
        r"lv_obj_set_style_radius\(header_screen_off,\s*28.*?"
        r"FONT_BUTTON_COMPACT.*?"
        r"!screen_wake_input_available\(\).*?"
        r"lv_obj_add_state\(header_screen_off,\s*LV_STATE_DISABLED\)",
        "header control follows the capsule design and fails open without touch")

touch_reader = TOUCH_BSP.split(
    "static void touch_read_cb", 1)[1].split(
        "static void tick_cb", 1)[0]

update_ui = TOUCH_BSP.split(
    "static void update_ui", 1)[1].split(
        "static void ui_task", 1)[0]

backlight_apply = TOUCH_BSP.split(
    "static void apply_pending_backlight_locked", 2)[2].split(
        "uint8_t bsp_display_get_brightness", 1)[0]

require(TOUCH_BSP, r"#define\s+TOUCH_FAILURE_THRESHOLD\s+3",
        "touch health changes only after a consecutive failure threshold")

require(TOUCH_BSP,
        r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?return true;.*?"
        r"waveshare_7b_touch_snapshot.*?touch_observation_healthy",
        "wake input requires a fresh worker observation, including recovery after boot failure")

assert "if (!s_touch)" not in TOUCH_BSP.split("static bool screen_wake_input_available", 1)[1].split("static bool lock_lvgl", 1)[0]

require(touch_reader, r"waveshare_7b_touch_snapshot.*?touch_observation_pressed",
        "LVGL consumes a timestamped point without synchronous I2C")

assert "esp_lcd_touch_read_data" not in touch_reader

require(touch_reader,
        r"touch_became_unavailable.*?"
        r'bsp_display_set_notice\("Touch unavailable - recovering controls"\)',
        "the transition to unhealthy touch is surfaced once")

require(touch_reader,
        r"visibility_requests.*?s_wake_gesture_pending\s*=\s*true.*?"
        r"healthy && touch.valid && !touch.pressed.*?s_wake_gesture_pending = false",
        "recovery consumes the wake gesture through a fresh release")

require(TOUCH_BSP,
        r"request_idle_sleep_if_due.*?!screen_wake_input_available\(\).*?"
        r"portENTER_CRITICAL\(&s_state_lock\).*?!s_backlight_force_on.*?"
        r"s_backlight_requested\s*=\s*false",
        "idle-off eligibility and request are atomic and fail open without touch")

require(TOUCH_BSP,
        r"action_cb.*?action\s*==\s*UI_ACTION_SCREEN_OFF.*?"
        r"!screen_wake_input_available\(\).*?"
        r'"Touch is unavailable - screen kept on".*?'
        r"bsp_display_set_backlight\(false\)",
        "shared manual action retains the established guarded Off now behavior")

require(TOUCH_BSP,
        r"request_idle_sleep_if_due\(&display_settings,\s*visual_alarm,\s*now_us\)"
        r".*?apply_pending_backlight_locked\(\)",
        "standby timeout applies its hardware-off request")

require(TOUCH_BSP,
        r"bsp_display_apply_backlight_policy.*?!screen_wake_input_available\(\)"
        r".*?bsp_display_set_backlight\(true\)",
        "therapy display policy also fails open without touch")

require(TOUCH_BSP,
        r"bsp_display_apply_backlight_policy.*?bool\s+off\s*=\s*"
        r"settings\.backlight_mode\s*==\s*BACKLIGHT_MODE_ALWAYS_OFF.*?"
        r"therapy\s*&&\s*settings\.backlight_mode\s*==\s*BACKLIGHT_MODE_OFF_THRP.*?"
        r"bsp_display_set_backlight\(!off\)",
        "therapy screen-off modes request a physical backlight change")

require(update_ui,
        r"if\s*\(\s*!screen_wake_input_available\(\)\s*&&\s*!backlight\s*\)"
        r".*?bsp_display_set_backlight\(true\).*?"
        r"apply_pending_backlight_locked\(\).*?"
        r"portENTER_CRITICAL\(&s_state_lock\).*?"
        r"backlight\s*=\s*s_backlight",
        "runtime touch failure actively wakes a dark touch-only device")

require(TOUCH_BSP, r"#define\s+POLICY_PEEK_TIMEOUT_S\s+60",
        "therapy-off policies get a bounded control-window wake")

require(TOUCH_BSP,
        r"policy_prefers_off.*?BACKLIGHT_MODE_ALWAYS_OFF.*?BACKLIGHT_MODE_OFF_THRP.*?"
        r"POLICY_PEEK_TIMEOUT_S",
        "Screen off and Off except alerts re-sleep after their wake window")

require(TOUCH_BSP,
        r"request_idle_sleep_if_due.*?!s_temporarily_awake.*?elapsed",
        "ordinary idle sleep cannot cut short an explicit temporary wake")

require(TOUCH_BSP,
        r"visual_alarm.*?bsp_display_set_backlight\(true\).*?"
        r"device_settings_snapshot\(&display_settings\).*?"
        r"request_idle_sleep_if_due\(&display_settings,\s*visual_alarm,\s*now_us\)",
        "alerts wake before idle sleep is evaluated")

require(TOUCH_BSP,
        r"s_wake_overlay\s*=\s*lv_obj_create\(lv_layer_top\(\)\).*?"
        r"lv_obj_set_style_bg_opa\(s_wake_overlay,\s*LV_OPA_TRANSP.*?"
        r"LV_OBJ_FLAG_CLICKABLE.*?LV_OBJ_FLAG_PRESS_LOCK.*?"
        r"wake_overlay_cb,\s*LV_EVENT_PRESSED",
        "hardware wake surface stays transparent above dialogs and intercepts presses")

require(TOUCH_BSP,
        r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?"
        r"lv_obj_set_style_bg_opa\(s_wake_overlay,\s*LV_OPA_COVER.*?"
        r"#else.*?"
        r"lv_obj_set_style_bg_opa\(s_wake_overlay,\s*LV_OPA_TRANSP",
        "QEMU visibly emulates an electrically dark backlight")

require(TOUCH_BSP,
        r"wake_overlay_cb.*?lv_indev_wait_release\(indev\).*?"
        r"bsp_display_restart_idle_timeout\(\).*?"
        r"bsp_display_set_backlight\(true\)",
        "first dark-screen press is consumed before waking")

require(TOUCH_BSP,
        r"lv_obj_move_foreground\(s_wake_overlay\)",
        "wake surface is raised whenever the display goes dark")

require(TOUCH_BSP, r"#define\s+BACKLIGHT_RETRY_US\s+250000",
        "failed wake writes use a bounded retry interval")

require(backlight_apply,
        r"if\s*\(now_us\s*<\s*retry_after_us\)\s*return;.*?"
        r"esp_err_t\s+backlight_result;.*?"
        r"waveshare_7b_reassert_visible\(\).*?"
        r"if\s*\(backlight_result\s*!=\s*ESP_OK\).*?"
        r"s_backlight_write_errors\+\+",
        "backlight writes are checked, rate-limited, and diagnosed")

require(backlight_apply,
        r"if\s*\(requested\)\s*\{\s*"
        r"s_backlight_retry_after_us\s*=\s*now_us\s*\+\s*"
        r"BACKLIGHT_RETRY_US.*?else\s*\{.*?"
        r"s_backlight_requested\s*=\s*true.*?"
        r"s_last_touch_activity_us\s*=\s*now_us.*?"
        r'Could not turn screen off - kept on',
        "failed wakes retry while failed optional sleeps fail open")

require(backlight_apply,
        r"esp_err_t\s+brightness_result\s*=\s*ESP_OK.*?"
        r"waveshare_7b_set_brightness.*?"
        r"if\s*\(backlight_result == ESP_OK && brightness_result != ESP_OK\).*?"
        r"s_backlight_write_errors\+\+",
        "wake-time brightness restoration is checked")

require(backlight_apply,
        r"if\s*\(backlight_result\s*!=\s*ESP_OK\).*?return;\s*\}.*?"
        r"portENTER_CRITICAL\(&s_state_lock\).*?"
        r"s_backlight\s*=\s*requested\s*;.*?"
        r"s_backlight_retry_after_us\s*=\s*0",
        "authoritative backlight state changes only after hardware success")

require(TOUCH_BOARD,
        r"waveshare_7b_set_backlight\(bool\s+on\).*?"
        r"iox_output\(IOX_BACKLIGHT,\s*on\)",
        "logical screen off hard-disables the panel backlight through EXIO2")

require(HOST_TEST, r"python3 scripts/screen_timeout_contract_test\.py",
        "screen-timeout contracts run in the host suite")

require(QEMU_TOUCH_TEST,
        r"sleep_from_header_then_wake_over.*?tap\("
        r"process,\s*log_path,\s*stream,\s*566,\s*35\).*?"
        r'"backlight off".*?assert_emulated_backlight_frame\(.*?True.*?'
        r'"backlight on".*?assert_emulated_backlight_frame\(.*?False.*?'
        r"leaked_selection",
        "QEMU checks visible off/on frames and wake-only input consumption")

print("screen timeout contracts: PASS")

for array in ('allowed', 'allowedTimeouts'):
    match = re.search(r'var ' + array + r' = \[([^]]+)\]', PORTAL)
    assert match and {int(v.strip()) for v in match.group(1).split(',')} == allowed, array
