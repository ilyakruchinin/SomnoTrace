#!/usr/bin/env python3
"""Exercise the production flush path against an independently timed scanout."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "main/bsp_display_7b.c").read_text()


def function(name):
    match = re.search(rf"static void {name}\([^;]*?\)\s*\{{", source)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define CONFIG_SOMNOTRACE_BOARD_QEMU 0
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define CONTROLLER_PANEL_SUBMIT 0
#define CONTROLLER_PANEL_HANDOFF 1
#define WAVESHARE_7B_H_RES 1024
#define WAVESHARE_7B_V_RES 600
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef void *esp_lcd_panel_handle_t;
typedef uint16_t lv_color_t;
typedef struct {void *user_data;} lv_disp_drv_t;
typedef struct {int unused;} lv_area_t;
static uint32_t s_flush_count, s_flush_timeouts;
static unsigned notifications, timeouts_left, restarts, retries, ready;
static unsigned selected, scanned, submit_calls, handoff_errors;
static bool inject_before_selection, inject_after_selection, last_area;
static void boundary(void) {scanned = selected; ++notifications;}
static void controller_diagnostics_record(int operation, int result) {
    if (operation == CONTROLLER_PANEL_HANDOFF && result != ESP_OK) ++handoff_errors;
}
static int esp_lcd_panel_draw_bitmap(void *panel, int x, int y, int w, int h, const void *pixels) {
    (void)panel; (void)x; (void)y; (void)pixels;
    assert(w == 1024 && h == 600); ++submit_calls;
    if (retries) {--retries; return 2;}
    if (inject_before_selection) boundary();
    selected = 1;
    if (inject_after_selection) boundary();
    return ESP_OK;
}
static unsigned ulTaskNotifyTake(int clear, unsigned wait) {
    assert(clear == pdTRUE);
    if (notifications) {unsigned n = notifications; notifications = 0; return n;}
    if (!wait) return 0;
    assert(!ready);
    if (timeouts_left) {--timeouts_left; return 0;}
    boundary(); notifications = 0; return 1;
}
static int esp_lcd_rgb_panel_restart(void *panel) {
    (void)panel; assert(!ready); ++restarts; return ESP_OK;
}
static void vTaskDelay(unsigned ticks) {assert(ticks == 100 && !ready);}
static bool lv_disp_flush_is_last(lv_disp_drv_t *drv) {(void)drv; return last_area;}
static void lv_disp_flush_ready(lv_disp_drv_t *drv) {
    (void)drv;
    if (last_area) assert(selected == 1 && scanned == 1);
    ++ready;
}
'''
fixture += function("submit_rgb_frame") + "\n" + function("flush_cb")
fixture += r'''
int main(void) {
    lv_disp_drv_t drv = {0}; lv_area_t area = {0}; lv_color_t pixels = 0;
    for (unsigned before = 0; before < 2; ++before)
    for (unsigned after = 0; after < 2; ++after)
    for (unsigned stale = 0; stale < 2; ++stale)
    for (unsigned missed = 0; missed < 4; ++missed)
    for (unsigned fail = 0; fail < 2; ++fail) {
        selected = scanned = ready = restarts = submit_calls = handoff_errors = 0;
        s_flush_count = s_flush_timeouts = 0;
        inject_before_selection = before; inject_after_selection = after;
        notifications = stale; timeouts_left = missed; retries = fail; last_area = true;
        flush_cb(&drv, &area, &pixels);
        assert(ready == 1 && submit_calls == 1 + fail && s_flush_count == 1);
        assert(restarts == missed && handoff_errors == missed);
        assert(s_flush_timeouts == missed + fail);
    }
    last_area = false; ready = submit_calls = 0; selected = scanned = 0;
    flush_cb(&drv, &area, &pixels);
    assert(ready == 1 && submit_calls == 0);
    puts("RGB handoff: stale IRQs, submit races, missed frames and retries passed");
}
'''
with tempfile.TemporaryDirectory(prefix="somno-rgb-handoff-") as temporary:
    path = Path(temporary)
    (path / "test.c").write_text(fixture)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
    # The same independent scanout model must reject the previous ordering.
    raced = fixture.replace("    ulTaskNotifyTake(pdTRUE, 0);", "", 1)
    raced = raced.replace("    for (;;) {", "    ulTaskNotifyTake(pdTRUE, 0);\n    for (;;) {", 1)
    (path / "race.c").write_text(raced)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "race.c"), "-o", str(path / "race")], check=True)
    previous = subprocess.run([str(path / "race")], cwd=path, capture_output=True)
    assert previous.returncode != 0, "test failed to detect the old handoff race"
    print("Previous clear-before-submit ordering correctly rejected")
