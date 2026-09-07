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
require(board,
        r"waveshare_7b_set_panel_pclk\s*\(uint32_t\s+hz\).*?"
        r"hz\s*!=\s*18000000U\s*&&\s*hz\s*!=\s*30850000U.*?"
        r"esp_lcd_rgb_panel_set_pclk\(s_panel,\s*hz\)",
        "runtime PCLK diagnostic restricted to the two A/B clocks")
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

assert "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=6144" in source("sdkconfig.defaults")
print("7B physical pins, transport, allocator and original NimBLE contracts passed")
