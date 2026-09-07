#!/usr/bin/env python3
"""Static hardware-contract checks for the Waveshare 7B board profile.

The expected values come from Waveshare's ESP32-S3-Touch-LCD-7B ESP-IDF
reference at commit c652c902db607f7ffb376257393cfd7657aa6428. These checks do not
replace a physical bring-up, but they make accidental pin, timing, framebuffer,
or target-config regressions fail loudly during ordinary host testing.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing 7B contract: {description}")


board = source("main/board_waveshare_7b.c")
display = source("main/bsp_display_7b.c")
storage = source("main/sd_storage.c")
defaults = source("sdkconfig.7b.defaults")
cmake = source("main/CMakeLists.txt")
root_cmake = source("CMakeLists.txt")
lvgl_allocator = source("main/somnotrace_lvgl_psram.h")
fonts = source("main/somnotrace_fonts.h")
board_defaults = source("sdkconfig.7b.defaults")
history = source("main/touch_history.c")
history_header = source("main/touch_history.h")
history_ui = source("main/touch_history_ui.c")
history_ui_header = source("main/touch_history_ui.h")
history_controller = source("main/touch_history_controller.c")
history_controller_header = source("main/touch_history_controller.h")
edf = source("main/edf_gen.c")
main = source("main/main.c")
as11 = source("main/as11_ble.c")
device_settings = source("main/device_settings.c")
net_provision = source("main/net_provision.c")
native_maintenance = source("main/touch_maintenance_ui.c")
native_config = source("main/touch_manage_config_ui.c")

expected_scalars = {
    r"#define\s+I2C_SDA\s+GPIO_NUM_8\b": "I2C SDA GPIO8",
    r"#define\s+I2C_SCL\s+GPIO_NUM_9\b": "I2C SCL GPIO9",
    r"#define\s+IOX_ADDR\s+0x24\b": "CH32V003 controller address 0x24",
    r"\.pclk_hz\s*=\s*30850000\b": "accepted 30.85 MHz pixel clock",
    r"\.hsync_pulse_width\s*=\s*162\b": "HSYNC pulse",
    r"\.hsync_back_porch\s*=\s*152\b": "HSYNC back porch",
    r"\.hsync_front_porch\s*=\s*48\b": "HSYNC front porch",
    r"\.vsync_pulse_width\s*=\s*45\b": "VSYNC pulse",
    r"\.vsync_back_porch\s*=\s*13\b": "VSYNC back porch",
    r"\.vsync_front_porch\s*=\s*3\b": "VSYNC front porch",
    r"\.hsync_gpio_num\s*=\s*GPIO_NUM_46\b": "HSYNC GPIO46",
    r"\.vsync_gpio_num\s*=\s*GPIO_NUM_3\b": "VSYNC GPIO3",
    r"\.de_gpio_num\s*=\s*GPIO_NUM_5\b": "DE GPIO5",
    r"\.pclk_gpio_num\s*=\s*GPIO_NUM_7\b": "PCLK GPIO7",
    r"\.num_fbs\s*=\s*2\b": "double framebuffer",
    r"\.bounce_buffer_size_px\s*=\s*WAVESHARE_7B_H_RES\s*\*\s*10\b":
        "cache-sized ten-line bounce buffer",
    r"\.flags\.fb_in_psram\s*=\s*true\b": "PSRAM framebuffers",
    r"\.flags\.pclk_active_neg\s*=\s*true\b": "negative PCLK edge",
}
for pattern, description in expected_scalars.items():
    require(board, pattern, description)

rgb_match = re.search(r"\.data_gpio_nums\s*=\s*\{(.*?)\}", board, re.DOTALL)
if not rgb_match:
    raise AssertionError("missing 7B RGB data pin array")
rgb_pins = [int(value) for value in re.findall(r"GPIO_NUM_(\d+)", rgb_match.group(1))]
expected_rgb = [14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40]
assert rgb_pins == expected_rgb, f"RGB pin order changed: {rgb_pins}"
assert len(set(rgb_pins)) == 16, "RGB data pins must be unique"

rgb_bus = set(rgb_pins) | {3, 5, 7, 46}
control_bus = {4, 8, 9, 11, 12, 13}
assert not rgb_bus & control_bus, f"RGB/control GPIO collision: {rgb_bus & control_bus}"

require(board, r"\.x_max\s*=\s*WAVESHARE_7B_H_RES", "GT911 X range")
require(board, r"\.y_max\s*=\s*WAVESHARE_7B_V_RES", "GT911 Y range")
require(board, r"\.int_gpio_num\s*=\s*GPIO_NUM_4", "GT911 interrupt GPIO4")
require(board, r"IOX_TOUCH_RST,\s*false.*pdMS_TO_TICKS\(100\).*GPIO_NUM_4,\s*0.*pdMS_TO_TICKS\(100\).*IOX_TOUCH_RST,\s*true.*pdMS_TO_TICKS\(200\)",
        "Waveshare GT911 reset/address-selection timing")

require(storage, r"s\.clk\s*=\s*GPIO_NUM_12", "TF CLK GPIO12")
require(storage, r"s\.cmd\s*=\s*GPIO_NUM_11", "TF CMD GPIO11")
require(storage, r"s\.d0\s*=\s*GPIO_NUM_13", "TF D0 GPIO13")
require(storage, r"s\.width\s*=\s*1", "TF one-bit SDMMC mode")
require(board, r"iox_output\(IOX_SD_CS,\s*true\)", "TF DAT3/CS held high")
require(board,
        r"attenuation\s*=\s*\(uint8_t\)\(100U\s*-\s*percent\).*?"
        r"attenuation\s*>\s*97.*?IOX_REG_PWM,\s*pwm",
        "active-low backlight PWM mapping with vendor attenuation limit")
require(display,
        r"physical_brightness.*?tenth_percent\s*\+\s*1U\)\s*/\s*2U",
        "7B legacy brightness range mapped to 1-100 percent")
require(display, r'waveshare_7b_set_brightness\(100\)',
        "steady full-brightness display initialization")
require(native_maintenance,
        r'"%d%%"\s*,\s*\(cfg\.brightness\s*\+\s*1\)\s*/\s*2',
        "native Display maps the legacy brightness endpoint to 100 percent")
require(device_settings,
        r"CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B\s*\|\|\s*\\\s*"
        r"\(CONFIG_SOMNOTRACE_BOARD_QEMU\s*&&\s*!CONFIG_SOMNOTRACE_QEMU_DISPLAY_154\)"
        r".*?DEFAULT_BRIGHTNESS\s+200",
        "7-inch default is 100 percent steady backlight")
require(board,
        r"waveshare_7b_set_panel_pclk\s*\(uint32_t\s+hz\).*?"
        r"hz\s*!=\s*18000000U\s*&&\s*hz\s*!=\s*30850000U.*?"
        r"esp_lcd_rgb_panel_set_pclk\(s_panel,\s*hz\)",
        "runtime PCLK diagnostic restricted to the two A/B clocks")
require(net_provision,
        r'strcmp\(action,\s*"display-pclk"\)\s*==\s*0.*?'
        r'hz_item->valuedouble\s*!=\s*18000000\.0.*?'
        r'hz_item->valuedouble\s*!=\s*30850000\.0.*?'
        r'waveshare_7b_set_panel_pclk\(hz\)',
        "non-persistent display PCLK action with a strict two-clock allowlist")
require(net_provision, r'"boot_default_hz\\":30850000',
        "PCLK diagnostic reports the accepted boot clock")
assert '"/api/diagnostics/display-pclk"' not in net_provision, \
       "PCLK diagnostic must reuse /api/actions without consuming a URI slot"

require(display, r"\.on_frame_buf_complete\s*=\s*on_frame_complete", "frame-buffer handoff")
require(display, r"display_driver\.hor_res\s*=\s*WAVESHARE_7B_H_RES", "LVGL width")
require(display, r"display_driver\.ver_res\s*=\s*WAVESHARE_7B_V_RES", "LVGL height")
require(display, r"esp_lcd_rgb_panel_get_frame_buffer\(s_panel,\s*2,\s*&fb1,\s*&fb2\)",
        "two panel-owned framebuffers")
require(display, r"display_driver\.direct_mode\s*=\s*1", "dirty-region direct rendering")
require(display, r"if\s*\(!lv_disp_flush_is_last\(drv\)\).*?lv_disp_flush_ready\(drv\).*?return",
        "one panel handoff after the final dirty area")
require(display,
        r"esp_lcd_panel_draw_bitmap.*?ulTaskNotifyTake\(pdTRUE,\s*0\).*?"
        r"ulTaskNotifyTake\(pdTRUE,\s*pdMS_TO_TICKS\(100\)\)",
        "discard stale completions after framebuffer selection")
require(display, r"boot_scanout\s*=\s*fb1;\s*fb1\s*=\s*fb2;\s*fb2\s*=\s*boot_scanout;",
        "first LVGL render starts outside the boot scanout buffer")
require(root_cmake, r"LV_MEM_CUSTOM_ALLOC=somnotrace_lvgl_alloc",
        "7-inch LVGL allocator override")
require(lvgl_allocator, r"heap_caps_malloc_prefer.*?MALLOC_CAP_SPIRAM.*?MALLOC_CAP_INTERNAL",
        "LVGL PSRAM-first allocation with internal fallback")
require(lvgl_allocator, r"heap_caps_realloc_prefer", "matched LVGL reallocator")
require(lvgl_allocator, r"heap_caps_free", "matched LVGL free")
require(board_defaults, r"CONFIG_LV_USE_FONT_COMPRESSED=y",
        "compressed custom-font renderer")
require(display, r'#include\s+"somnotrace_fonts\.h"', "custom bedside fonts")
for family in ("space_grotesk", "ibm_plex_mono"):
    assert family in fonts, f"missing {family} font declarations"

for setting in (
    "CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B=y",
    "CONFIG_ESP32S3_DATA_CACHE_64KB=y",
    "CONFIG_ESP32S3_DATA_CACHE_SIZE=0x10000",
    "CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y",
    "CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y",
    "CONFIG_SPIRAM_RODATA=y",
    "CONFIG_LV_COLOR_DEPTH_16=y",
    "CONFIG_LV_INDEV_DEF_READ_PERIOD=10",
    "CONFIG_LV_DISP_DEF_REFR_PERIOD=16",
    "CONFIG_LV_SPRINTF_USE_FLOAT=y",
    "CONFIG_LV_USE_CHART=y",
    "CONFIG_LV_USE_MSGBOX=y",
):
    assert setting in defaults, f"missing 7B sdkconfig default: {setting}"

for unit in ("board_waveshare_7b.c", "bsp_display_7b.c", "bsp_power_7b.c", "bsp_audio_7b.c"):
    assert f'"{unit}"' in cmake, f"7B build omits {unit}"

# The 7-inch build follows the fixed three-screen bedside handoff.  Navigation
# is custom rather than an LVGL tabview so the centred 166x54 pills and the
# separate eight-section Manage rail can be represented without tab chrome.
for pattern, description in (
    (r"#define\s+UI_HEADER_H\s+64\b", "64px shared header"),
    (r"#define\s+UI_CONTENT_Y\s+64\b", "content begins below the 64px header"),
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
    (r"s_pages\s*\[\s*3\s*\]", "three primary screen containers"),
    (r"s_nav_buttons\s*\[\s*3\s*\]", "three custom navigation buttons"),
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
     "rich History surface fills the shared 992x450 frame"),
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
assert "lv_tabview_create" not in display, "bedside shell must use custom navigation"
assert "lv_tabview_add_tab" not in display, "legacy five-tab navigation remains"
for page in ("Home", "History", "Manage"):
    require(display, rf'"{page}"', f"{page} primary navigation label")
for section_label in (
    "Devices", "Connectivity", "Alerts", "Uploads",
    "Storage", "System", "Logs", "Advanced",
):
    require(display, rf'"{section_label}"', f"{section_label} Manage rail label")

# LVGL v8 paints dropdown and textarea copy from pad_top.  Keep the shared
# single-line geometry explicit and independent of pre-layout object coords.
field_style = display.split(
    "static void style_manage_field(lv_obj_t *field)\n{", 1)[1].split("\n}\n", 1)[0]
for pattern, description in (
    (r"lv_obj_get_style_height\(field,\s*LV_PART_MAIN\)",
     "configured field height used before first layout"),
    (r"lv_font_get_line_height\(font\)", "custom font line-height centering"),
    (r"lv_obj_set_style_pad_top\(field,\s*free_height\s*/\s*2,\s*LV_PART_MAIN\)",
     "single-line top padding"),
    (r"lv_obj_set_style_pad_bottom\(field,\s*free_height\s*-\s*free_height\s*/\s*2,\s*LV_PART_MAIN\)",
     "single-line bottom padding"),
):
    require(field_style, pattern, description)
surface_style = display.split(
    "static void style_manage_surface(lv_obj_t *field)\n{", 1)[1].split("\n}\n", 1)[0]
require(surface_style,
        r"border_width\(field,\s*1,\s*0\).*?"
        r"border_width\(field,\s*1,\s*LV_STATE_FOCUSED\)",
        "focus ring keeps the content origin stable")
textarea_style = display.split(
    "static void style_manage_textarea(lv_obj_t *field)\n{", 1)[1].split("\n}\n", 1)[0]
for pattern, description in (
    (r"pad_bottom\(field,\s*0,\s*LV_PART_MAIN\)",
     "one-line textarea cursor slack"),
    (r"set_scroll_dir\(field,\s*LV_DIR_HOR\)",
     "one-line textarea horizontal scrolling"),
    (r"set_scrollbar_mode\(field,\s*LV_SCROLLBAR_MODE_OFF\)",
     "one-line textarea hidden scrollbar"),
):
    require(textarea_style, pattern, description)

dropdown_fields = {"s_as11_dropdown", "s_ox_dropdown"}
ready_bindings = re.findall(
    r"lv_obj_add_event_cb\(\s*"
    r"(s_(?:as11_dropdown|ox_dropdown))"
    r"\s*,\s*([A-Za-z_]\w*)\s*,\s*LV_EVENT_READY\s*,\s*NULL\s*\)",
    display, re.MULTILINE | re.DOTALL)
assert {field for field, _ in ready_bindings} == dropdown_fields, \
    f"dropdown READY styling incomplete: {ready_bindings}"
assert len({callback for _, callback in ready_bindings}) == 1, \
    "all Manage dropdowns must reuse one popup styling callback"
require(display,
        r"manage_dropdown_list_ready_cb.*?max_height\(list,\s*250.*?"
        r"pad_top\(list,\s*12.*?pad_bottom\(list,\s*12.*?"
        r"text_line_space\(list,\s*28",
        "scrollable bedside-sized dropdown options")
for field in ("timeout", "mode"):
    require(native_maintenance,
            rf"lv_obj_add_event_cb\({field},\s*dropdown_ready,\s*LV_EVENT_READY,\s*NULL\)",
            f"native Display {field} dropdown READY styling")
require(native_maintenance,
        r"dropdown_ready.*?text_line_space\(list,\s*28.*?"
        r"max_height\(list,\s*250.*?"
        r"pad_top\(list,\s*12.*?pad_bottom\(list,\s*12.*?"
        ,
        "native Display dropdowns use bedside-sized popup options")
assert display.count("make_manage_field_chevron(") == 4, \
    "three shell Manage fields plus the helper must use aligned chevrons"

# Rev C Display is composed by the lazy maintenance module. Validate the active
# 16px slider and 56px touch area, not the retired shell layout.
require(native_maintenance,
        r"lv_obj_set_size\(slider,\s*670,\s*16\).*?"
        r"lv_obj_set_ext_click_area\(slider,\s*20\).*?"
        r"lv_obj_set_style_pad_all\(slider,\s*12,\s*LV_PART_KNOB\)",
        "native Display slider has a generous touch area and padded LVGL v8 knob")
assert "LCD_THERAPY_INFO" not in native_maintenance
assert "day/night theme and larger text are deferred" in native_maintenance
require(display, r"build_system_section.*?build_maintenance_section\(section, MAINT_UI_SYSTEM\)",
        "System routes to the actual native maintenance pane")

# The shared header status capsule sizes itself from the rendered status text.
# Keep every state on one line, vertically centre dots against that line, and
# preserve the fixed screen-right edge plus an explicit chevron inset.
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

# Pure destination navigation responds at touch-down; operational controls keep
# the generic completed-click behavior so a drag/cancel cannot trigger them.
require(display,
        r"make_destination_button.*?lv_obj_remove_event_cb_with_user_data.*?"
        r"lv_obj_add_event_cb\(button,\s*callback,\s*LV_EVENT_PRESSED",
        "pressed-event destination button factory")
require(display,
        r"lv_obj_add_event_cb\(button,\s*callback,\s*LV_EVENT_CLICKED",
        "completed-click generic action button factory")
require(display, r"s_nav_buttons\[i\]\s*=\s*make_destination_button",
        "immediate bottom navigation")
require(display, r"s_manage_buttons\[i\]\s*=\s*make_destination_button",
        "immediate Manage section rail")
require(display, r"s_status_capsule\s*=\s*make_destination_button",
        "immediate non-destructive status tray")
require(display,
        r"s_status_tray_actions\[i\]\s*=\s*make_destination_button",
        "immediate status-tray destination actions")
require(history_ui,
        r"ui->channels\[i\]\.button.*?history_ui_channel_pressed,\s*"
        r"LV_EVENT_PRESSED",
        "immediate rich History channel selection")
require(display,
        r"static void set_destination_surface.*?"
        r"LV_STYLE_TRANSLATE_Y,\s*0,\s*LV_STATE_PRESSED.*?"
        r"LV_STYLE_BG_OPA,\s*resting_opa,\s*LV_STATE_PRESSED",
        "destination controls do not replay generic press feedback on release")
require(display,
        r"lv_obj_add_event_cb\(s_status_scrim,\s*status_tray_close_cb,\s*"
        r"LV_EVENT_PRESSED",
        "immediate non-destructive status tray dismissal")
for control, description in (
    ("s_therapy_button", "therapy command"),
    ("s_alert_ack_button", "alert acknowledgement"),
):
    require(display, rf"{control}\s*=\s*make_touch_button",
            f"{description} remains completed-click")
require(native_maintenance,
        r"lv_obj_add_event_cb\(o,\s*event,\s*LV_EVENT_CLICKED",
        "native maintenance commands use completed clicks")
require(native_maintenance,
        r"case BUTTON_RESTART:.*?!blocked\(\).*?s->hooks.restart\(\)",
        "native restart revalidates current therapy state and invokes the shared confirmation")
require(display, r"maintenance_restart\(void\).*?reboot_prompt_cb\(NULL\)",
        "native restart retains the established confirmation and restart fencing")
require(display, r"static\s+int\s+s_active_page\s*=\s*-1",
        "unselected page sentinel for first render")
require(display, r"static\s+int\s+s_active_manage_section\s*=\s*-1",
        "unselected Manage-section sentinel for first render")
require(display,
        r"if\s*\(section\s*==\s*s_active_manage_section\)\s*return",
        "current Manage-section navigation no-op")
active_page_start = display.index("static void set_active_page(int page)\n{")
active_page_source = display[
    active_page_start:display.index("static void nav_cb", active_page_start)
]
manage_section_start = display.index("static void set_manage_section(int section)\n{")
manage_section_source = display[
    manage_section_start:
    display.index("static void manage_section_cb", manage_section_start)
]
require(active_page_source,
        r"set_destination_surface\(s_nav_buttons\[i\]",
        "bottom navigation uses stable destination surfaces")
require(active_page_source,
        r"portENTER_CRITICAL\(&s_state_lock\);\s*"
        r"bool\s+already_active\s*=\s*page\s*==\s*s_active_page;\s*"
        r"if\s*\(!already_active\)\s*s_active_page\s*=\s*page;\s*"
        r"portEXIT_CRITICAL\(&s_state_lock\);\s*"
        r"if\s*\(already_active\)\s*return;",
        "page navigation publishes and checks the active page under lock")
require(active_page_source,
        r"previous_page\s*==\s*1.*?touch_history_controller_set_active\("
        r".*?false\)",
        "leaving History cancels active controller work")
require(active_page_source,
        r"page\s*==\s*1.*?touch_history_controller_set_active\(.*?true\)",
        "entering History activates newest-first loading")
for selection_source, description in (
    (active_page_source, "page navigation"),
    (manage_section_source, "Manage section navigation"),
):
    assert "lv_obj_set_style_" not in selection_source, \
           f"{description} bypasses changed-only style helpers"
    assert "lv_obj_add_flag(" not in selection_source and \
           "lv_obj_clear_flag(" not in selection_source, \
           f"{description} bypasses changed-only visibility helper"
require(native_maintenance,
        r'button\(s->root,\s*"Refresh".*?BUTTON_SCAN',
        "native storage refresh remains a distinct completed-click action")
require(display,
        r'xTaskCreatePinnedToCore\(lvgl_task,\s*"display_7b",\s*12288,\s*'
        r"NULL,\s*5,\s*&s_lvgl_task,\s*1\)",
        "responsive priority-5 display task with measured stack headroom")

# Scroll only where content can actually overflow, and avoid elastic overscroll
# on the RGB panel because it redraws the whole viewport without revealing any
# additional content. The five status rows already fit their tray.
require(display,
        r"can_overflow\s*=\s*index\s*==\s*MANAGE_DEVICES\s*\|\|\s*"
        r"index\s*==\s*MANAGE_CONNECTIVITY\s*\|\|\s*"
        r"index\s*==\s*MANAGE_UPLOADS\s*\|\|\s*"
        r"index\s*==\s*MANAGE_SYSTEM;.*?if\s*\(can_overflow\)\s*\{.*?"
        r"LV_OBJ_FLAG_SCROLLABLE.*?LV_OBJ_FLAG_SCROLL_ELASTIC",
        "only overflowing Manage panes scroll without elasticity")
require(history_ui,
        r"list_viewport.*?LV_OBJ_FLAG_SCROLLABLE.*?LV_DIR_VER.*?"
        r"LV_EVENT_SCROLL_END",
        "History list uses bounded scroll-driven page recycling")
tray_scroll_start = display.index(
    "lv_obj_t *tray_scroll = make_plain_container(s_status_tray"
)
tray_scroll_source = display[
    tray_scroll_start:display.index("static const char *tray_titles", tray_scroll_start)
]
assert "LV_OBJ_FLAG_SCROLLABLE" not in tray_scroll_source, \
       "fixed-height status tray must not scroll"
secondary_start = display.index(
    "static void refresh_secondary_pages(const ui_state_t *state, int active_tab)\n{"
)
secondary_source = display[
    secondary_start:display.index("static void resync_flow_visual", secondary_start)
]
require(secondary_source,
        r"if\s*\(ble_started.*?end_ble_operation\(\);.*?"
        r"if\s*\(active_tab\s*!=\s*2\)\s*return;",
        "BLE completion survives hidden Manage render gating")
require(secondary_source,
        r"lv_obj_t\s*\*active_scroll\s*=\s*s_manage_scrolls\[section\];.*?"
        r"if\s*\(active_scroll\s*&&\s*lv_obj_is_scrolling\(active_scroll\)\).*?"
        r"return;",
        "visible Manage repaint defers while its active scroll root is moving")
require(secondary_source,
        r"if\s*\(section\s*==\s*MANAGE_CONNECTIVITY\s*\|\|\s*"
        r"section\s*==\s*MANAGE_ALERTS\s*\|\|\s*section\s*==\s*MANAGE_UPLOADS\)\s*\{\s*"
        r"touch_manage_config_refresh\(\);\s*return;",
        "native configuration refresh only in its visible destination")

# QEMU keeps the full-fidelity handoff, while the physical build compiles out
# large software-blurred shadows and expensive flow fill/glow layers.
require(display,
        r"#if\s+CONFIG_SOMNOTRACE_BOARD_QEMU.*?"
        r"UI_STATUS_SCRIM_OPA\s+LV_OPA_60.*?"
        r"#else.*?UI_STATUS_SCRIM_OPA\s+LV_OPA_60",
        "status tray preserves a translucent view of its source screen")
status_open_start = display.index("static void status_tray_open_cb(lv_event_t *event)\n{")
status_open_source = display[
    status_open_start:display.index("static void status_tray_route_cb", status_open_start)
]
assert "lv_obj_move_foreground" not in status_open_source, \
       "opening status tray must not invalidate the screen through reordering"
require(display,
        r"lv_obj_move_foreground\(s_status_scrim\);\s*"
        r"lv_obj_move_foreground\(s_status_tray\);.*?"
        r"set_manage_section\(MANAGE_DEVICES\);",
        "status overlay z-order established before first frame")
require(display,
        r"active_tab\s*==\s*0\s*&&\s*!status_tray_open.*?"
        r"status_tray_just_closed",
        "live chart redraw pauses behind status tray and resyncs on close")
require(display,
        r"#if\s+CONFIG_SOMNOTRACE_BOARD_QEMU.*?"
        r"UI_DECORATIVE_SHADOW_WIDTH\(pixels\)\s+\(pixels\).*?"
        r"FLOW_RENDER_POINTS\s+FLOW_POINTS.*?#else.*?"
        r"UI_DECORATIVE_SHADOW_WIDTH\(pixels\).*?,\s*0\).*?"
        r"UI_DECORATIVE_SHADOW_OPA\(opacity\).*?LV_OPA_TRANSP\).*?"
        r"FLOW_RENDER_POINTS\s+150.*?FLOW_RENDER_FILL\s+0.*?"
        r"FLOW_RENDER_GLOW\s+0",
        "physical low-cost rendering profile")
for target, description in (
    ("card", "generic cards"),
    ("s_ambient_glow", "ambient glow"),
    ("s_alert_banner", "alert banner"),
    ("s_keyboard_sheet", "keyboard sheet"),
):
    require(display,
            rf"lv_obj_set_style_shadow_width\(\s*{target}.*?"
            r"UI_DECORATIVE_SHADOW_WIDTH",
            f"physical shadow suppression for {description}")
require(history_ui,
        r"static lv_obj_t \*history_ui_button.*?"
        r"lv_obj_set_style_shadow_width\(button,\s*0,\s*0\)",
        "rich History controls avoid software-blurred shadows")
for target, description in (
    ("s_nav_buttons\\[i\\]", "selected navigation"),
    ("s_manage_buttons\\[i\\]", "selected Manage rail"),
    ("s_therapy_hero", "therapy hero"),
    ("s_therapy_orb", "therapy orb"),
    ("s_therapy_button", "therapy action"),
):
    require(display,
            rf"{target},\s*LV_STYLE_SHADOW_WIDTH,.*?"
            r"UI_DECORATIVE_SHADOW_WIDTH",
            f"physical dynamic shadow suppression for {description}")
require(display, r"lv_point_t\s+points\[FLOW_RENDER_POINTS\]",
        "bounded physical flow point array")
require(display,
        r"live_flow_plot_indices\(.*?source_indices,\s*FLOW_RENDER_POINTS\).*?"
        r"source_index\s*=\s*first_source_point\s*\+\s*source_indices\[i\]",
        "extrema-preserving physical flow reduction at original time positions")

# Concrete state/component coverage from the bedside handoff. Keeping this in
# the hardware contract ensures the native build cannot silently regress to a
# three-tab mock with none of the real loading, empty, fault, or pairing states.
for literal, description in (
    ("Therapy active", "active therapy state"),
    ("Therapy stopped", "stopped therapy state"),
    ("Starting...", "therapy start busy state"),
    ("Stopping...", "therapy stop busy state"),
    ("Pair a device", "unpaired primary state"),
    ("Therapy stopped unexpectedly", "interruption alert state"),
    ("Acknowledge", "interruption acknowledgement control"),
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
    ("Recorded files", "native storage summary"),
    ("Controller error history", "native diagnostic disclosure"),
):
    assert literal in display + native_maintenance + native_config, \
        f"missing bedside state: {description}"
require(display, r'"Waiting for (?:therapy|breathing) data(?:\.\.\.|…)?"',
        "first-sample loading state")
require(display, r"flow_count\s*>=\s*FLOW_READY_POINTS",
        "valid sample threshold before chart becomes live")
require(display, r'"Therapy status unknown"', "stale AirSense state")
for history_copy in (
    "Loading recorded night",
    "No recorded nights yet",
    "Could not read the card",
    "Retry",
    "Card status",
    "Trend review only. Not a diagnosis or a prescription.",
):
    assert history_copy in history_ui, \
           f"missing rich History state: {history_copy}"
for metric in ("Usage", "ST AHI", "Device AHI", "Recorded", "O₂ coverage"):
    assert f'"{metric}"' in history_ui, f"missing rich History metric: {metric}"

# Home has one contextual therapy action.  Administration and acknowledgement
# live in Manage or transient banners, never in a permanent Home utility row.
home_match = re.search(
    r"static\s+void\s+build_home_page\s*\([^)]*\)\s*\{(.*?)\n\}",
    display,
    re.MULTILINE | re.DOTALL,
)
assert home_match, "missing build_home_page() for static Home acceptance checks"
home_source = home_match.group(1)
for forbidden in ('"Wi-Fi setup"', '"Acknowledge"', '"Screen off"'):
    assert forbidden not in home_source, f"permanent Home action remains: {forbidden}"
require(display, r"xTaskCreate\(device_scan_task", "non-blocking BLE scan worker")
require(display, r"as11_ble_start_pair\(job->addr\)", "native AirSense pairing action")
require(display, r"as11_ble_confirm_pair\(job->passkey\)", "native AirSense passkey action")
require(display,
        r"action\s*==\s*DEVICE_PAIR_AS11\s*&&\s*!pairing_mode_confirmed",
        "AirSense connect is gated on machine pairing-mode acknowledgement")
require(display, r"oximeter_pair\(job->addr,\s*OX_DRIVER_AUTO\)",
        "auto-detected native O2 pairing action")
require(native_maintenance, r"device_settings_set_brightness", "native brightness control")
require(native_maintenance, r"device_settings_set_backlight_mode", "native therapy backlight policy")
assert "device_settings_set_lcd_therapy_mode" not in native_maintenance
require(display, r"netprov_save_config\(&cfg\)", "native Wi-Fi credential save")
require(display, r"LV_KEYBOARD_MODE_TEXT_LOWER", "on-screen Wi-Fi keyboard")
for api in (
    "touch_history_load_page_ex", "touch_history_load_night_ex",
    "touch_history_load_view_ex", "touch_history_load_stats_ex",
):
    require(history_controller, rf"\b{api}\b",
            f"controller-backed asynchronous rich History read {api}")
require(history, r"touch_history_decode_summary_record",
        "allocation-conscious native History summary decoder")
for unit in ("touch_history.c", "touch_history_ui.c",
             "touch_history_controller.c"):
    assert f'"{unit}"' in cmake, f"7B build omits {unit}"
require(main, r"oximeter_init\(\).*?bsp_display_enable_touch_services\(as11_ready,\s*oximeter_ready\)",
        "touch BLE controls enabled only after service initialization")

# Review-derived safety contracts that can be checked without physical hardware.
require(as11, r"as11_ble_get_values.*?xSemaphoreTake\(s_cmd_mtx.*?clear_response\(\)",
        "serialized AS11 Get RPC")
require(as11, r"therapy_command.*?xSemaphoreTake\(s_cmd_mtx.*?clear_response\(\)",
        "serialized therapy RPC")
require(display, r"s_therapy_command_busy", "single-flight therapy control")
require(display,
        r"start_therapy_with_lifecycle_gate.*?"
        r"bsp_display_reserve_therapy_start\(\).*?"
        r"as11_ble_start_therapy_tracked\(&may_have_started\).*?"
        r"result\s*==\s*ESP_OK\s*\|\|\s*may_have_started.*?"
        r"bsp_display_set_therapy_active\(true\).*?"
        r"bsp_display_release_therapy_start\(\)",
        "therapy start command excludes a concurrent restart commit")
assert '"Therapy command sent"' not in display, \
       "successful start/stop must not raise a redundant confirmation notice"
for toast in ('"Starting therapy..."', '"Stopping therapy..."'):
    assert toast not in display, \
           "therapy progress belongs in the hero card, not a toast"
require(display,
        r"if\s*\(result\s*!=\s*ESP_OK\)\s*"
        r"bsp_display_set_notice\(\"Therapy command failed\"\)",
        "therapy command failures remain visible")
require(display, r"s_wake_overlay.*?LV_EVENT_PRESSED", "wake-only touch interception")
require(display, r"s_backlight_requested", "sticky backlight desired state")
require(display, r"sd_storage_lease_acquire\(SD_LEASE_UPLOAD,\s*250\)",
        "leased UI free-space query")
require(display, r"flow_sample_us\s*=\s*esp_timer_get_time\(\)",
        "flow freshness uses source arrival time")
for timestamp in (
    "leak_sample_us",
    "pressure_sample_us",
    "respiratory_rate_sample_us",
    "flow_limitation_sample_us",
):
    require(display, rf"{timestamp}\s*=\s*(?:sample_us|esp_timer_get_time\(\))",
            f"{timestamp} tracks independent metric freshness")
require(display,
        r"!paired.*?s_therapy_command_busy\s*=\s*false;.*?"
        r"set_active_page\(2\);.*?set_manage_section\(MANAGE_DEVICES\);",
        "unpaired Home action routes to AirSense setup")
require(display, r"sd_storage_recording_active\(\)",
        "Home recording copy follows writer state")
require(display, r"sd_storage_deinit\(\);\s*esp_restart\(\)",
        "clean SD unmount before UI restart")

# The physical RGB panel is sensitive to large redundant redraws: unchanged
# Home presentation state must not invalidate labels, visibility, or styles on
# every 500 ms pass. Keep these checks close to the hardware contracts because
# this is also a panel-stability requirement, not just a rendering optimization.
require(display,
        r"set_label_text_if_changed.*?lv_label_get_text.*?strcmp.*?"
        r"lv_label_set_text",
        "state-aware label updates")
require(display,
        r"set_hidden.*?lv_obj_has_flag\(obj,\s*LV_OBJ_FLAG_HIDDEN\)\s*==\s*hidden",
        "state-aware visibility updates")
require(display, r"lv_obj_get_local_style_prop",
        "exact local-style comparison before Home style mutation")
update_start = display.index("static void update_ui(void)")
update_end = display.index("static void lvgl_task(void", update_start)
update_source = display[update_start:update_end]
assert "lv_label_set_text(" not in update_source, \
       "update_ui must use state-aware label setters"
assert "lv_label_set_text_fmt(" not in update_source, \
       "update_ui must use state-aware formatted label setters"
assert "lv_obj_set_style_" not in update_source, \
       "update_ui must compare Home style values before mutation"
assert "lv_obj_add_flag(" not in update_source and \
       "lv_obj_clear_flag(" not in update_source, \
       "update_ui must compare visibility before mutation"
require(update_source,
        r"lv_bar_get_value\(s_metric_bars\[i\]\)\s*!=\s*bar_values\[i\]",
        "state-aware Home metric bars")
require(update_source,
        r"alert_visibility_changed\s*=\s*set_hidden\(s_alert_banner",
        "state-aware alert banner visibility")

# Rich History is one retained 992x450 surface backed by one serialized
# controller. The display task alone applies coherent revisions to LVGL; the
# worker callback only schedules that apply.
for pattern, description in (
    (r"TOUCH_HISTORY_UI_WIDTH\s+992\b", "992-pixel History width"),
    (r"TOUCH_HISTORY_UI_HEIGHT\s+450\b", "450-pixel History height"),
    (r"TOUCH_HISTORY_UI_LIST_ROWS\s+7\b", "seven recycled night rows"),
    (r"TOUCH_HISTORY_UI_CHANNEL_CONTROLS\s+TOUCH_HISTORY_SIGNAL_COUNT",
     "all eight signal controls"),
):
    require(history_ui_header, pattern, description)
for label in (
    "Breathing / Flow", "Pressure", "Leak", "Flow limit",
    "Snore", "SpO₂", "Pulse", "Motion",
):
    assert f'"{label}"' in history_ui, f"missing rich History channel {label}"
assert "lv_chart_create" not in history_ui, \
       "rich History must use one bounded draw surface, not an LVGL chart"
assert "lv_canvas_create" not in history_ui, \
       "rich History must not allocate a full-frame canvas"
require(history_ui,
        r"heap_caps_calloc\(\s*1,\s*sizeof\(\*ui\),\s*"
        r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT\)",
        "retained History snapshot lives in PSRAM")

build_history_start = display.index("static void build_history_page(lv_obj_t *history)\n{")
build_history_source = display[
    build_history_start:
    display.index("static lv_obj_t *make_manage_section", build_history_start)
]
require(build_history_source,
        r"touch_history_ui_create\(\s*s_history_host,\s*&config,\s*"
        r"&s_history_ui\s*\)",
        "one retained rich History object tree")
require(display,
        r"touch_history_controller_create\(\s*&history_config,\s*"
        r"&s_history_controller\s*\).*?build_ui\(\);",
        "History controller exists before its retained UI is built")
require(display,
        r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?\.deterministic_preview\s*=\s*true",
        "QEMU exercises deterministic rich History data")

history_changed_start = display.index(
    "static void history_controller_changed(void *context)\n{"
)
history_changed_source = display[
    history_changed_start:
    display.index("static void history_route_card", history_changed_start)
]
assert "__atomic_store_n" in history_changed_source
assert "lv_" not in history_changed_source and \
       "xTaskNotify" not in history_changed_source, \
       "History worker callback must only schedule an LVGL-task apply"
require(display,
        r"apply_history_controller_if_needed.*?"
        r"touch_history_controller_revision.*?touch_history_controller_apply",
        "History revisions are coherently applied by LVGL")
require(update_source,
        r"active_tab\s*==\s*1.*?apply_history_controller_if_needed",
        "visible History revisions are applied from the display task")
require(display,
        r"history_route_card.*?set_manage_section\(MANAGE_STORAGE\).*?"
        r"set_active_page\(2\)",
        "card failures route to Manage Storage")
require(display,
        r"therapy_finished.*?touch_history_controller_refresh",
        "therapy completion invalidates the History index")

for api in (
    "touch_history_controller_create", "touch_history_controller_destroy",
    "touch_history_controller_set_active", "touch_history_controller_refresh",
    "touch_history_controller_handle_intent", "touch_history_controller_apply",
):
    assert api in history_controller_header and api in history_controller, \
           f"missing rich History controller API {api}"
require(history_controller, r"xSemaphoreCreateMutex\(\).*?xQueueCreate\(",
        "internal-RAM History control objects")
require(history_controller,
        r"heap_caps_calloc\(.*?sizeof\(\*controller\).*?MALLOC_CAP_SPIRAM.*?"
        r"psram_task_create\(.*?history_controller_worker",
        "bounded History model and worker stack live in PSRAM")
require(history_controller,
        r"vQueueDelete\(controller->queue\).*?"
        r"vSemaphoreDelete\(controller->mutex\).*?heap_caps_free\(controller\)",
        "History controller releases kernel objects before its PSRAM model")
require(history_controller,
        r"HISTORY_JOB_INITIAL.*?result->days\[0\]\.day.*?global_index\s*=\s*0",
        "newest night is selected and loaded automatically")
for intent in (
    "PAGE_RELATIVE", "OPEN_CALENDAR", "SELECT_CHANNEL", "FIT_NIGHT",
    "ZOOM_RELATIVE", "PAN_RELATIVE", "SET_CURSOR",
):
    assert f"TOUCH_HISTORY_UI_INTENT_{intent}" in history_controller, \
           f"controller omits History intent {intent}"
require(history_controller,
        r"touch_history_load_view_ex\(.*?window_start_ms.*?window_end_ms",
        "History graph follows the visible window")
require(history_controller,
        r"touch_history_load_stats_ex\(.*?window_start_ms.*?window_end_ms",
        "History source statistics follow the visible window")
require(history_controller,
        r"touch_history_load_window_events_ex.*?model->window_start_ms.*?"
        r"model->window_end_ms",
        "History event markers are filtered to the visible window")
assert "TOUCH_HISTORY_MAX_DAYS" not in history_ui + history_controller, \
       "rich History UI must page the full index instead of imposing a 30-night cap"
for legacy in (
    "s_history_worker_task", "s_history_trace_worker_task",
    "history_trace_task", "queue_history_trace_load", "start_history_load",
    "refresh_history_widgets", "s_services.history",
):
    assert legacy not in display, f"duplicate legacy History path remains: {legacy}"

require(display, r"s_keyboard_sheet.*?keyboard_sheet_action_cb",
        "explicit touch keyboard sheet with completion actions")
require(display, r"s_text_keyboard_lower_map.*?\"q\".*?\"p\".*?LV_SYMBOL_BACKSPACE.*?LV_SYMBOL_UP.*?\"123\".*?\"@\".*?\"space\".*?\"-\".*?\"_\"",
        "five-row handoff text keyboard")
require(display, r"lv_obj_set_align\(s_keyboard,\s*LV_ALIGN_TOP_LEFT\)",
        "visible top-aligned keyboard geometry")
require(display, r"lv_textarea_set_text\(s_wifi_password,\s*\"\"\)",
        "stored Wi-Fi password excluded from LVGL")
assert "CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y" in defaults
assert "CONFIG_ESP_MAIN_TASK_STACK_SIZE=14336" in defaults

print("Waveshare 7B hardware contract passed")
