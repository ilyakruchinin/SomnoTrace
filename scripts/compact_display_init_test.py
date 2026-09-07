#!/usr/bin/env python3
"""Fault-inject the real compact init/cleanup and required-strip flush paths."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/bsp_display.c").read_text()


def function(signature):
    start = SOURCE.index(signature + "\n{")
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOG_WARN 2
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
#define LCD_H_RES 240
#define LCD_V_RES 240
#define FLOW_BUF_SIZE 240
#define LCD_STRIP_BUFS 2
#define LCD_STRIP_ROWS 40
#define LCD_PIN_SCLK 38
#define LCD_PIN_MOSI 39
#define LCD_PIN_DC 45
#define LCD_PIN_CS 21
#define LCD_PIN_RST 40
#define LCD_PIN_BL 46
#define LCD_PIXEL_CLOCK_HZ 26666667
#define LCD_SPI_HOST 2
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_INVERT_COLOR true
#define BL_LEDC_RESOLUTION 10
#define BL_LEDC_TIMER 0
#define BL_LEDC_FREQ_HZ 5000
#define BL_LEDC_CHANNEL 0
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_AUTO_CLK 0
#define LEDC_INTR_DISABLE 0
#define SPI_DMA_CH_AUTO 0
#define LCD_RGB_ELEMENT_ORDER_RGB 0
#define DISPLAY_TASK_STACK 4096
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_DMA 2
#define tskNO_AFFINITY -1
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
typedef int esp_err_t;
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef intptr_t esp_lcd_spi_bus_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef struct { int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num, speed_mode, channel, intr_type, timer_sel, duty, hpoint; } ledc_channel_config_t;
typedef struct { int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int dc_gpio_num, cs_gpio_num, pclk_hz, lcd_cmd_bits, lcd_param_bits, spi_mode, trans_queue_depth;
    void (*on_color_trans_done)(void); void *user_ctx; } esp_lcd_panel_io_spi_config_t;
typedef struct { int reset_gpio_num, rgb_ele_order, bits_per_pixel; } esp_lcd_panel_dev_config_t;
static uint16_t *s_fb, *s_strip[2], s_rotation;
static float *s_flow_buf, *s_flow_local, *s_flow_yf;
static SemaphoreHandle_t s_state_mutex, s_flush_done;
static TaskHandle_t s_display_task;
static esp_timer_handle_t s_display_supervisor_timer;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool s_backlight_on = true, s_flush_stuck;
static uint8_t s_brightness;
static int allocation, fail_at, live, hardware_calls, panel_live, io_live, bus_live;
static int bitmap_calls, task_calls;
static bool fail_framebuffer, fail_task;

static void *allocate(size_t bytes) {
    ++allocation;
    if (allocation == fail_at || (fail_framebuffer && bytes == 240 * 240 * 2)) return NULL;
    void *result = calloc(1, bytes);
    assert(result);
    ++live;
    return result;
}
static void *heap_caps_malloc(size_t bytes, int caps) { (void)caps; return allocate(bytes); }
static void *heap_caps_calloc(size_t n, size_t bytes, int caps) { return heap_caps_malloc(n * bytes, caps); }
static void heap_caps_free(void *p) { if (p) { --live; free(p); } }
static void *xSemaphoreCreateCounting(int count, int initial) { (void)count; (void)initial; return allocate(1); }
static void *xSemaphoreCreateMutex(void) { return allocate(1); }
static void vSemaphoreDelete(void *p) { heap_caps_free(p); }
static int xSemaphoreTake(void *p, int timeout) { (void)timeout; assert(p); return pdTRUE; }
static int xSemaphoreGive(void *p) { assert(p); return pdTRUE; }
static int hardware(void) { ++hardware_calls; return ESP_OK; }
#define esp_log_level_set(...) ((void)0)
#define ledc_timer_config(...) hardware()
#define ledc_channel_config(...) hardware()
#define ledc_stop(...) hardware()
#define esp_lcd_panel_reset(...) hardware()
#define esp_lcd_panel_init(...) hardware()
#define esp_lcd_panel_invert_color(...) hardware()
#define esp_lcd_panel_set_gap(...) hardware()
#define esp_lcd_panel_disp_on_off(...) hardware()
static int spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma) {
    (void)host; (void)cfg; (void)dma; ++bus_live; return hardware();
}
static int spi_bus_free(int host) { (void)host; --bus_live; return hardware(); }
static int esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus, const esp_lcd_panel_io_spi_config_t *cfg, void **io) {
    (void)bus; (void)cfg; ++io_live; *io = (void *)2; return hardware();
}
static int esp_lcd_panel_io_del(void *io) { assert(io); --io_live; return hardware(); }
static int esp_lcd_new_panel_st7789(void *io, const esp_lcd_panel_dev_config_t *cfg, void **panel) {
    (void)io; (void)cfg; ++panel_live; *panel = (void *)3; return hardware();
}
static int esp_lcd_panel_del(void *panel) { assert(panel); --panel_live; return ESP_OK; }
static int board_qemu_154_init(void **panel) { ++panel_live; *panel = (void *)3; return ESP_OK; }
static int board_qemu_154_flush(void *panel, const uint16_t *fb, uint16_t rotation, bool backlight) {
    (void)rotation; (void)backlight; assert(panel && fb); ++bitmap_calls; return ESP_OK;
}
static int esp_lcd_panel_draw_bitmap(void *panel, int x0, int y0, int x1, int y1, void *fb) {
    assert(panel && fb && fb != s_fb);
    assert(x0 == 0 && x1 == 240 && y1 - y0 <= 40);
    ++bitmap_calls; return ESP_OK;
}
static void lcd_color_done_cb(void) {}
static void display_task(void *arg) { (void)arg; }
static void display_supervisor_cb(void *arg) { (void)arg; }
static int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer) {
    assert(args && args->callback && timer); *timer = (void *)5; return ESP_OK;
}
static int esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period) {
    assert(timer && period == 3000000); return ESP_OK;
}
static void *psram_task_create(void (*fn)(void *), const char *name, int stack, void *arg,
                               int priority, int core, void *out_stack, void *out_tcb) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)core; (void)out_stack; (void)out_tcb;
    ++task_calls;
    assert(s_fb && s_flow_buf && s_flow_local && s_flow_yf && s_state_mutex && s_panel);
#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    assert(s_strip[0] && s_strip[1] && s_flush_done && s_io);
#endif
    return fail_task ? NULL : (void *)4;
}
'''

TEST = r'''
static void clean_success(void) {
    esp_lcd_panel_del(s_panel); s_panel = NULL;
#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    esp_lcd_panel_io_del(s_io); s_io = NULL;
    spi_bus_free(LCD_SPI_HOST);
#endif
    s_display_task = NULL;
    display_buffers_free();
}
static void assert_clean(void) {
    assert(!s_fb && !s_flow_buf && !s_flow_local && !s_flow_yf && !s_state_mutex);
    assert(!s_panel && !s_io && !s_display_task && !s_flush_done && !s_strip[0] && !s_strip[1]);
    assert(live == 0 && panel_live == 0 && io_live == 0 && bus_live == 0);
}
int main(void) {
    assert(bsp_display_init() == ESP_OK);
    int allocations = allocation;
    lcd_flush();
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    assert(bitmap_calls == 1 && hardware_calls == 0);
#else
    assert(bitmap_calls == 6);
    void *second = s_strip[1]; s_strip[1] = NULL;
    lcd_flush(); assert(bitmap_calls == 6); s_strip[1] = second;
#endif
    clean_success(); assert_clean();

    /* Every required allocation fails in turn, including the second strip.
     * The first framebuffer attempt has one intentional allocation fallback. */
    for (int failure = 1; failure <= allocations; ++failure) {
        allocation = 0; fail_at = failure; task_calls = 0; hardware_calls = 0;
        int result = bsp_display_init();
        if (failure == 1) {
            assert(result == ESP_OK && task_calls == 1);
            clean_success();
        } else {
            assert(result == ESP_ERR_NO_MEM && task_calls == 0 && hardware_calls == 0);
        }
        assert_clean();
    }
    allocation = 0; fail_at = 0; fail_framebuffer = true; task_calls = 0; hardware_calls = 0;
    assert(bsp_display_init() == ESP_ERR_NO_MEM);
    assert(task_calls == 0 && hardware_calls == 0); assert_clean();
    fail_framebuffer = false; fail_task = true; allocation = 0; task_calls = 0;
    assert(bsp_display_init() == ESP_ERR_NO_MEM && task_calls == 1); assert_clean();
    fail_task = false; allocation = 0;
    assert(bsp_display_init() == ESP_OK); clean_success(); assert_clean();
    printf("PASS compact init allocation failures, task failure, retry, required strips (qemu=%d)\n",
           CONFIG_SOMNOTRACE_QEMU_DISPLAY_154);
}
'''

with tempfile.TemporaryDirectory(prefix="somno-compact-init-") as directory:
    path = Path(directory)
    production = "\n".join(function(signature) for signature in [
        "static void display_buffers_free(void)",
        "static esp_err_t display_buffers_init(void)",
        "esp_err_t bsp_display_init(void)",
        "static void lcd_flush(void)",
    ])
    fixture = path / "test.c"
    fixture.write_text(PRELUDE + production + TEST)
    for compact in (0,):
        binary = path / f"test-{compact}"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        f"-DCONFIG_SOMNOTRACE_QEMU_DISPLAY_154={compact}",
                        str(fixture), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
