/*
 * SomnoTrace - ST7789 LCD driver
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */


#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "sdkconfig.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "font_roboto.h"
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
#include "board_qemu_154.h"
#else
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_wifi.h"
#include "driver/ledc.h"
#endif
#include "device_settings.h"
#include "psram_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define LCD_H_RES           240
#define LCD_V_RES           240

#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
#define LCD_PIN_SCLK        38
#define LCD_PIN_MOSI        39
#define LCD_PIN_DC          45
#define LCD_PIN_CS          21
#define LCD_PIN_RST         40
#define LCD_PIN_BL          46

#define LCD_PIXEL_CLOCK_HZ  (15000000)  /* 15 MHz target (80 MHz APB / 6 = 13.33 MHz, 75 ns cycle; ST7789 spec: >=66 ns) */
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8
#define LCD_INVERT_COLOR    true

/* LEDC PWM for backlight dimming */
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ     5000
#define BL_LEDC_RESOLUTION  LEDC_TIMER_10_BIT  /* 0-1023 duty */
#define BL_DUTY_MAX         ((1 << 10) - 1)    /* 1023 */

#endif

static uint8_t s_brightness = 100;  /* current brightness (tenth-percent: 1=0.1%, 200=20%) */
static bool s_backlight_on = true;  /* backlight hardware state */
static bool s_backlight_force_on = false;  /* SoftAP/portal: keep backlight on */
static esp_timer_handle_t s_wake_timer = NULL;
static bool s_temporarily_awake = false;

static const char *TAG = "bsp_display";
static void (*s_setup_callback)(void) = NULL;

/* Forward declarations */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
static void fb_clear(uint16_t color);
static int fb_draw_string_aa(int x, int y, const font_info_t *font, const char *str, uint16_t color);
static void fb_draw_wifi_indicator(int x, int y, bool connected);
static void fb_draw_ble_indicator(int x, int y);
static void render_graph(void);
static void render_info(void);
static void render_status(void);
static void display_task(void *arg);
static void apply_panel_rotation(uint16_t degrees);

static esp_lcd_panel_handle_t s_panel = NULL;
#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
static esp_lcd_panel_io_handle_t s_io = NULL;
#endif
static uint16_t *s_fb = NULL;
static bool s_wifi_connected = false;
static bool s_as11_paired = false;
static uint16_t s_rotation = 0;  /* current LCD rotation in degrees */
static volatile bool s_rotation_pending = false;  /* set_rotation requested, deferred to render task */
static uint16_t s_pending_rotation_deg = 0;       /* requested rotation (valid when s_rotation_pending) */

/* Strip blit: the framebuffer lives in PSRAM (not DMA-capable), so it is
 * pushed to the panel in chunks via small internal DMA-capable buffers.
 * Two buffers are used so the DMA of one strip overlaps the CPU copy of the
 * next (pipelining). A counting semaphore tracks completed transfers so a
 * buffer is never reused while its DMA is still in flight. */
#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
#define LCD_STRIP_ROWS 40
#define LCD_STRIP_BUFS 2
static uint16_t *s_strip[LCD_STRIP_BUFS] = { NULL, NULL };
static SemaphoreHandle_t s_flush_done = NULL;
#endif
static volatile bool s_flush_stuck = false;
static TaskHandle_t s_display_task = NULL;

/* ── Display state (single-owner render task model) ──────────────────
 *
 * Only the render task (display_task) ever touches the framebuffer s_fb
 * or calls esp_lcd_panel_draw_bitmap(). Every other function merely
 * mutates shared state under s_state_mutex and never draws. This makes
 * the display impossible to corrupt via concurrent access and guarantees
 * a clean full-frame redraw on every mode transition (no leftover
 * artifacts, no partial frames, no stuck modes). */

typedef enum {
    DISP_MODE_STATUS = 0,
    DISP_MODE_GRAPH,
    DISP_MODE_INFO,
} disp_mode_t;

#define FLOW_BUF_SIZE      240   /* one flow sample per pixel column */
#define MAX_STATUS_LINES   4
#define STATUS_TITLE_LEN   32
#define STATUS_LINE_LEN    48

#define DISPLAY_TASK_STACK 4096
#define GRAPH_FRAME_MS     80    /* 12.5 Hz live graph cadence: 2 flow samples (2 px) per frame */
#define STATUS_FRAME_MS    1000  /* status refresh cadence (live RSSI) */

/* ── Flow graph layout (static, non-adaptive) ───────────────────────────
 * GRAPH_FULL_SCALE is the fixed full-scale deflection (L/min) from the zero
 * line to the top/bottom of the plot. It is intentionally static so the view
 * does not jitter; values beyond it clip at the plot edge (the "hard cut").
 * Adjust this single constant to match the device's flow range. */
#define GRAPH_FULL_SCALE   150.0f
#define GRAPH_AXIS_W       34    /* left scale-axis gutter width (px) */
#define GRAPH_PLOT_X0      GRAPH_AXIS_W
#define GRAPH_PLOT_TOP     16
#define GRAPH_PLOT_BOT     224
#define GRAPH_PLOT_W       (LCD_H_RES - GRAPH_AXIS_W)

static SemaphoreHandle_t s_state_mutex = NULL;  /* protects all shared state below */
static disp_mode_t s_mode = DISP_MODE_STATUS;
static bool s_status_dirty = true;              /* status content changed, force redraw */

/* Two-phase restart gate, protected by s_state_mutex together with s_mode.
 * Start waiters make the final commit fail; the restart owner releases its SD
 * lease before cancelling the reservation and waking them. */
static bool s_therapy_safe_restart_reserving;
static bool s_therapy_safe_restart_committed;
static unsigned s_therapy_start_waiters;
static unsigned s_therapy_start_claims;
static unsigned s_as11_notifications_pending;
static bool s_therapy_safe_maintenance;

#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
/* Scene changes use the public state APIs. Suppress intermediate renders and
 * acknowledge only a final scene snapshot after its virtual-panel flush. */
static bool s_qemu_scene_seeding;
static uint32_t s_qemu_scene_generation;
static uint8_t s_qemu_scene;
static const char *const s_qemu_scene_names[] = {
    "status", "flow", "info", "notice",
};
#define QEMU_DEMO_WALL_TIME 1788698040  /* 2026-09-06 12:34 UTC */
#define QEMU_DEMO_NOW_US (42 * 60 * 1000000LL + 1)
#endif
/* Status-screen content (copied from callers) */
static char s_status_title[STATUS_TITLE_LEN];
static char s_status_lines[MAX_STATUS_LINES][STATUS_LINE_LEN];
static int  s_status_nlines = 0;

/* Persistent notice banner rendered at the bottom of the status screen in
 * warning colours.  Independent of the status lines above so transient
 * messages (Wi-Fi reconnecting, SD errors) can never clobber it. */
static char s_notice[STATUS_LINE_LEN] = "";

/* Battery indicator state */
static int  s_batt_percent = -1;   /* -1 = unknown/not set */
static bool s_batt_charging = false;
static bool s_batt_valid = false;

/* Live flow ring buffer for the therapy graph (PSRAM). */
static float *s_flow_buf;
static float *s_flow_local;   /* render-task snapshot */
static float *s_flow_yf;      /* render-task y coordinates */
static int    s_flow_head = 0;
static int    s_flow_count = 0;

static volatile int64_t s_last_render_us = 0;
static esp_timer_handle_t s_display_supervisor_timer = NULL;

/* Info panel state */
static float    s_leak_lpm = 0.0f;       /* latest leak rate (L/min) */
static double   s_leak_sum = 0.0;        /* accumulated leak sum for session average */
static uint32_t s_leak_count = 0;        /* count of accumulated leak samples */
static int64_t  s_therapy_start_us = 0;  /* monotonic time of TherapyStart */

/* ── Public state-mutating API (never draws; render task handles drawing) ── */

bool bsp_display_set_therapy_active(bool active)
{
    if (!s_state_mutex) {
        ESP_LOGW(TAG, "set_therapy_active(%s) called before init — ignored",
                 active ? "true" : "false");
        /* Display failure must not suppress therapy recording. OTA restart
         * reservation remains unavailable while the mutex is absent. */
        return true;
    }

    /* Check device settings */
    const device_settings_t *dev = device_settings_get();

    bool waiting_for_restart = false;
    for (;;) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!active || !s_therapy_safe_restart_reserving) break;
        if (!waiting_for_restart) {
            s_therapy_start_waiters++;
            waiting_for_restart = true;
        }
        xSemaphoreGive(s_state_mutex);
        vTaskDelay(1);
    }
    if (waiting_for_restart) s_therapy_start_waiters--;
    if (active && s_therapy_safe_restart_committed) {
        xSemaphoreGive(s_state_mutex);
        ESP_LOGW(TAG, "therapy start refused: restart already committed");
        return false;
    }
    disp_mode_t new_mode;
    if (active) {
        if (dev->therapy_screen == THERAPY_SCREEN_INFO)
            new_mode = DISP_MODE_INFO;
        else if (dev->therapy_screen == THERAPY_SCREEN_STATUS)
            new_mode = DISP_MODE_STATUS;
        else
            new_mode = DISP_MODE_GRAPH;
    } else {
        new_mode = DISP_MODE_STATUS;
    }
    if (s_mode != new_mode) {
        s_mode = new_mode;
        if (active) {
            s_flow_head = 0;
            s_flow_count = 0;
            s_leak_sum = 0.0;
            s_leak_count = 0;
            ESP_LOGI(TAG, "therapy mode enabled: %s (display_task=%s)",
                     new_mode == DISP_MODE_INFO ? "info" :
                     new_mode == DISP_MODE_STATUS ? "status" : "graph",
                     s_display_task ? "alive" : "NULL");
        } else {
            s_status_dirty = true;  /* force immediate status redraw */
            ESP_LOGI(TAG, "therapy mode disabled");
        }
    } else {
        ESP_LOGD(TAG, "set_therapy_active(%s) — mode already %s, no-op",
                 active ? "true" : "false",
                 s_mode == DISP_MODE_GRAPH ? "GRAPH" :
                 s_mode == DISP_MODE_INFO ? "INFO" : "STATUS");
    }
    xSemaphoreGive(s_state_mutex);

    /* Backlight policy during therapy */
    if (s_temporarily_awake) {
        /* Keep temporary wake active */
    } else if (dev->backlight_mode == BACKLIGHT_MODE_OFF_THRP) {
        bsp_display_set_backlight(!active);
    } else if (dev->backlight_mode == BACKLIGHT_MODE_ALWAYS_OFF) {
        bsp_display_set_backlight(false);
    } else {
        bsp_display_set_backlight(true);
    }

    /* Wake the render task so the mode change is reflected immediately. */
    if (s_display_task) xTaskNotifyGive(s_display_task);
    return true;
}

bool bsp_display_try_reserve_therapy_safe_restart(void)
{
    if (!s_state_mutex) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool therapy_active = (s_mode == DISP_MODE_GRAPH || s_mode == DISP_MODE_INFO);
    bool reserved = !therapy_active && s_therapy_start_claims == 0 &&
                    s_therapy_start_waiters == 0 &&
                    s_as11_notifications_pending == 0 &&
                    !s_therapy_safe_maintenance &&
                    !s_therapy_safe_restart_reserving &&
                    !s_therapy_safe_restart_committed;
    if (reserved) s_therapy_safe_restart_reserving = true;
    xSemaphoreGive(s_state_mutex);
    return reserved;
}

bool bsp_display_try_commit_therapy_safe_restart(void)
{
    if (!s_state_mutex) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool therapy_active = (s_mode == DISP_MODE_GRAPH || s_mode == DISP_MODE_INFO);
    bool committed = s_therapy_safe_restart_reserving && !therapy_active &&
                     s_therapy_start_waiters == 0 &&
                     s_as11_notifications_pending == 0;
    if (committed) {
        s_therapy_safe_restart_reserving = false;
        s_therapy_safe_restart_committed = true;
    }
    xSemaphoreGive(s_state_mutex);
    return committed;
}

void bsp_display_cancel_therapy_safe_restart(void)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_therapy_safe_restart_committed) {
        s_therapy_safe_restart_reserving = false;
    }
    xSemaphoreGive(s_state_mutex);
}

bool bsp_display_reserve_therapy_start(void)
{
    if (!s_state_mutex) return true;
    bool waiting_for_restart = false;
    for (;;) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!s_therapy_safe_restart_reserving) break;
        if (!waiting_for_restart) {
            s_therapy_start_waiters++;
            waiting_for_restart = true;
        }
        xSemaphoreGive(s_state_mutex);
        vTaskDelay(1);
    }
    if (waiting_for_restart) s_therapy_start_waiters--;
    bool reserved = !s_therapy_safe_restart_committed;
    if (reserved) s_therapy_start_claims++;
    xSemaphoreGive(s_state_mutex);
    return reserved;
}

void bsp_display_release_therapy_start(void)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_therapy_start_claims > 0) s_therapy_start_claims--;
    xSemaphoreGive(s_state_mutex);
}

void bsp_display_note_as11_notification_queued(void)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_as11_notifications_pending++;
    xSemaphoreGive(s_state_mutex);
}

void bsp_display_note_as11_notification_processed(void)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_as11_notifications_pending > 0) s_as11_notifications_pending--;
    xSemaphoreGive(s_state_mutex);
}

bool bsp_display_try_begin_therapy_safe_maintenance(void)
{
    if (!s_state_mutex) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool therapy_active = (s_mode == DISP_MODE_GRAPH || s_mode == DISP_MODE_INFO);
    bool begun = !therapy_active && s_therapy_start_claims == 0 &&
                 s_therapy_start_waiters == 0 &&
                 s_as11_notifications_pending == 0 &&
                 !s_therapy_safe_maintenance &&
                 !s_therapy_safe_restart_reserving &&
                 !s_therapy_safe_restart_committed;
    if (begun) s_therapy_safe_maintenance = true;
    xSemaphoreGive(s_state_mutex);
    return begun;
}

/* Convert an active OTA maintenance gate into a short commit reservation.
 * This closes the check/boot-selection race under the therapy publication lock.
 * The updater releases this reservation immediately after SDK finish/set-boot,
 * allowing queued starts to publish before the ordinary deferred reboot loop. */
bool bsp_display_try_reserve_maintenance_commit(void)
{
    if (!s_state_mutex) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool reserved = s_therapy_safe_maintenance &&
        !(s_mode == DISP_MODE_GRAPH || s_mode == DISP_MODE_INFO) &&
        s_therapy_start_claims == 0 && s_therapy_start_waiters == 0 &&
        s_as11_notifications_pending == 0 && !s_therapy_safe_restart_reserving &&
        !s_therapy_safe_restart_committed;
    if (reserved) {
        s_therapy_safe_maintenance = false;
        s_therapy_safe_restart_reserving = true;
    }
    xSemaphoreGive(s_state_mutex);
    return reserved;
}

bool bsp_display_therapy_safe_maintenance_should_abort(void)
{
    if (!s_state_mutex) return true;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool therapy_active = (s_mode == DISP_MODE_GRAPH || s_mode == DISP_MODE_INFO);
    bool abort = !s_therapy_safe_maintenance || therapy_active ||
                 s_therapy_start_claims > 0 || s_therapy_start_waiters > 0;
    xSemaphoreGive(s_state_mutex);
    return abort;
}

void bsp_display_end_therapy_safe_maintenance(void)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_therapy_safe_maintenance = false;
    xSemaphoreGive(s_state_mutex);
}



bool bsp_display_is_therapy_active(void)
{
    if (!s_state_mutex) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool active = (s_mode == DISP_MODE_GRAPH || s_mode == DISP_MODE_INFO);
    xSemaphoreGive(s_state_mutex);
    return active;
}

void bsp_display_push_flow(float flow_lpm)
{
    if (!s_state_mutex) return;
    bool notify = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_mode == DISP_MODE_GRAPH && s_flow_buf) {
        s_flow_buf[s_flow_head] = flow_lpm;
        s_flow_head = (s_flow_head + 1) % FLOW_BUF_SIZE;
        if (s_flow_count < FLOW_BUF_SIZE) s_flow_count++;
        notify = true;
    }
    xSemaphoreGive(s_state_mutex);
    /* In Graph Mode, wake display_task on incoming flow data */
    if (notify && s_display_task) xTaskNotifyGive(s_display_task);
}


void bsp_display_push_flow_gap(uint32_t samples)
{
    if (!s_state_mutex) return;
    if (samples > FLOW_BUF_SIZE) samples = FLOW_BUF_SIZE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_mode == DISP_MODE_GRAPH && s_flow_buf) {
        for (uint32_t i = 0; i < samples; ++i) {
            s_flow_buf[s_flow_head] = NAN;
            s_flow_head = (s_flow_head + 1) % FLOW_BUF_SIZE;
            if (s_flow_count < FLOW_BUF_SIZE) ++s_flow_count;
        }
    }
    xSemaphoreGive(s_state_mutex);
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

void bsp_display_push_leak(float leak_lpm)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_leak_lpm = leak_lpm;
    s_leak_sum += leak_lpm;
    s_leak_count++;
    xSemaphoreGive(s_state_mutex);
    if (s_mode == DISP_MODE_INFO && s_display_task)
        xTaskNotifyGive(s_display_task);
}

void bsp_display_set_therapy_start_time(int64_t start_us)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_therapy_start_us = start_us;
    s_leak_sum = 0.0;
    s_leak_count = 0;
    xSemaphoreGive(s_state_mutex);
}

/* ── Framebuffer → panel blit ───────────────────────────────────────── */

#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
static bool IRAM_ATTR lcd_color_done_cb(esp_lcd_panel_io_handle_t io,
                                        esp_lcd_panel_io_event_data_t *edata,
                                        void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    BaseType_t hp = pdFALSE;
    if (s_flush_done) xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}
#endif

/* Push the entire PSRAM framebuffer to the LCD in horizontal strips.
 *
 * The framebuffer is in PSRAM, which is not DMA-capable for the SPI master,
 * so a direct full-frame draw_bitmap() forces the driver to allocate a
 * ~115 KB internal DMA bounce buffer every frame. That allocation fails once
 * Wi-Fi and SDMMC have claimed internal RAM, silently dropping frames. By
 * copying each strip into a small, permanently-allocated internal DMA buffer
 * we guarantee the transfer always succeeds regardless of heap state. */

/* Hardware-reset the panel and re-send the full init sequence.  This is the
 * only reliable way to recover a wedged ST7789 that has stopped applying
 * RAMWR (frozen screen) while the CPU side keeps running normally.
 * Called from display_task only. */
static void lcd_panel_hw_recover(void)
{
#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    if (!s_panel) return;

    /* Wait briefly for any in-flight SPI DMA transactions to finish before
     * toggling the hardware reset pin, then drain the completion semaphore. */
    vTaskDelay(pdMS_TO_TICKS(10));
    if (s_flush_done) {
        while (xSemaphoreTake(s_flush_done, 0) == pdTRUE) { }
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLOR);
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    /* Re-apply rotation — panel reset wipes MADCTL back to 0° */
    if (s_rotation != 0) {
        apply_panel_rotation(s_rotation);
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "panel hardware reset + re-init (rot=%u)", (unsigned)s_rotation);
#endif
}

/* Apply the hardware portion of the requested rotation.
 *
 * The panel's 0° and 90° MADCTL paths are reliable, but mirror_y (needed for
 * a direct 180°/270° hardware rotation) leaves a split frame on this module.
 * Keep mirror_y disabled and implement the missing half-turn while packing
 * the DMA strips in lcd_flush():
 *
 *   0°   = panel 0°
 *   90°  = panel 90°
 *   180° = panel 0°  + software 180°
 *   270° = panel 90° + software 180°
 */
static void apply_panel_rotation(uint16_t degrees)
{
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    /* The virtual panel does not implement mirror/swap. The transport rotates
     * the original framebuffer in software when it converts RGB565. */
    (void)degrees;
#else
    if (!s_panel) return;
    bool quarter_turn = false;

    switch (degrees) {
        case 0:
        case 180:
            break;
        case 90:   /* clockwise 90° */
        case 270:  /* clockwise 270° */
            quarter_turn = true;
            break;
        default:
            return;
    }

    esp_lcd_panel_swap_xy(s_panel, quarter_turn);
    esp_lcd_panel_mirror(s_panel, quarter_turn, false);
#endif
}

void bsp_display_set_rotation(uint16_t degrees)
{
    switch (degrees) {
        case 0: case 90: case 180: case 270:
            break;
        default:
            ESP_LOGW(TAG, "set_rotation: invalid %u", (unsigned)degrees);
            return;
    }

    /* Defer the actual SPI panel write to the display task.  Calling
     * apply_panel_rotation() from here (e.g. an HTTP handler thread) would
     * race with lcd_flush() on the same esp_lcd_panel_io handle, corrupting
     * the SPI transaction queue and permanently wedging the display. */
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_pending_rotation_deg = degrees;
        s_rotation_pending = true;
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
        ++s_qemu_scene_generation;
#endif
        xSemaphoreGive(s_state_mutex);
    } else {
        /* Init-time path (no task yet): apply directly. */
        s_rotation = degrees;
        apply_panel_rotation(degrees);
    }

    ESP_LOGI(TAG, "rotation set to %u°", (unsigned)degrees);
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

static void lcd_flush(void)
{
    if (!s_panel || !s_fb) return;

#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool backlight_on = s_backlight_on;
    xSemaphoreGive(s_state_mutex);
    ESP_ERROR_CHECK(board_qemu_154_flush(s_panel, s_fb, s_rotation, backlight_on));
#else
    if (!s_strip[0] || !s_strip[1] || !s_flush_done) {
        ESP_LOGE(TAG, "LCD strip transport unavailable; frame skipped");
        return;
    }

    int inflight = 0;
    int bi = 0;
    for (int y0 = 0; y0 < LCD_V_RES; y0 += LCD_STRIP_ROWS) {
        int rows = LCD_V_RES - y0;
        if (rows > LCD_STRIP_ROWS) rows = LCD_STRIP_ROWS;

        /* Wait for previous strip DMA to complete BEFORE queueing this one.
         * This guarantees num_trans_inflight is 0 when esp_lcd_panel_draw_bitmap()
         * sends CASET/RASET commands, preventing ESP-IDF from calling
         * spi_device_get_trans_result(..., portMAX_DELAY).
         *
         * A 100 ms timeout (~9x normal 11.5 ms strip transfer time)
         * catches any DMA stall cleanly without hanging display_task. */
        if (inflight > 0) {
            if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "lcd_flush: DMA timeout on row %d, aborting frame", y0);
                s_flush_stuck = true;
                return;
            }
            inflight = 0;
        }

        uint16_t *buf = s_strip[bi];
        bi = (bi + 1) % LCD_STRIP_BUFS;
        if (s_rotation == 180 || s_rotation == 270) {
            /* Rotate the framebuffer by 180° while copying it out of PSRAM.
             * For 270°, the panel applies the remaining known-good 90° turn.
             * Destination pixels stay contiguous so the DMA transfer format
             * and strip address windows are unchanged. */
            for (int dy = 0; dy < rows; dy++) {
                int src_y = LCD_V_RES - 1 - (y0 + dy);
                const uint16_t *src = &s_fb[src_y * LCD_H_RES + LCD_H_RES - 1];
                uint16_t *dst = &buf[dy * LCD_H_RES];
                for (int x = 0; x < LCD_H_RES; x++) {
                    dst[x] = src[-x];
                }
            }
        } else {
            memcpy(buf, &s_fb[y0 * LCD_H_RES],
                   (size_t)rows * LCD_H_RES * sizeof(uint16_t));
        }

        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_H_RES, y0 + rows, buf);
        inflight = 1;
    }
    /* Drain final in-flight transfer. */
    if (inflight > 0) {
        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "lcd_flush: DMA drain timeout, aborting");
            s_flush_stuck = true;
            return;
        }
        inflight = 0;
    }
#endif
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);
}

static inline uint32_t utf8_decode(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    if (!p || !*p) return 0;

    uint32_t c = *p;
    if (c < 0x80) {
        (*s)++;
        return c;
    }

    if ((c & 0xE0) == 0xC0) {
        if (!p[1]) {
            *s += 1;
            return '?';
        }
        c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        *s += 2;
        return c;
    }

    if ((c & 0xF0) == 0xE0) {
        if (!p[1]) {
            *s += 1;
            return '?';
        }
        if (!p[2]) {
            *s += 2;
            return '?';
        }
        c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *s += 3;
        return c;
    }

    if ((c & 0xF8) == 0xF0) {
        if (!p[1]) {
            *s += 1;
            return '?';
        }
        if (!p[2]) {
            *s += 2;
            return '?';
        }
        if (!p[3]) {
            *s += 3;
            return '?';
        }
        c = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *s += 4;
        return c;
    }

    (*s)++;
    return '?';
}

static const font_glyph_t *find_glyph(const font_info_t *font, uint32_t codepoint)
{
    int low = 0;
    int high = font->glyph_count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        uint32_t cp = font->glyphs[mid].codepoint;
        if (cp == codepoint) {
            return &font->glyphs[mid];
        } else if (cp < codepoint) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (codepoint != '?') {
        return find_glyph(font, '?');
    }
    return NULL;
}

static inline void unpack_rgb565(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint16_t c = (color >> 8) | (color << 8);
    *r = (c >> 8) & 0xF8;
    *r |= (*r >> 5);
    *g = (c >> 3) & 0xFC;
    *g |= (*g >> 6);
    *b = (c << 3) & 0xF8;
    *b |= (*b >> 5);
}

static inline uint16_t blend_pixels(uint16_t bg_color, uint16_t fg_color, uint8_t alpha)
{
    if (alpha == 0) return bg_color;
    if (alpha == 15) return fg_color;

    uint8_t bg_r, bg_g, bg_b;
    uint8_t fg_r, fg_g, fg_b;

    unpack_rgb565(bg_color, &bg_r, &bg_g, &bg_b);
    unpack_rgb565(fg_color, &fg_r, &fg_g, &fg_b);

    uint8_t blended_r = (fg_r * alpha + bg_r * (15 - alpha)) / 15;
    uint8_t blended_g = (fg_g * alpha + bg_g * (15 - alpha)) / 15;
    uint8_t blended_b = (fg_b * alpha + bg_b * (15 - alpha)) / 15;

    return rgb565(blended_r, blended_g, blended_b);
}

static void fb_draw_char_aa(int x, int y, const font_info_t *font, const font_glyph_t *glyph, uint16_t color)
{
    if (glyph->width == 0 || glyph->height == 0) return;

    uint32_t offset = glyph->bitmap_offset;

    for (int row = 0; row < glyph->height; row++) {
        int target_y = y + glyph->bearing_y + row;
        if (target_y < 0 || target_y >= LCD_V_RES) continue;

        for (int col = 0; col < glyph->width; col++) {
            int target_x = x + glyph->bearing_x + col;
            if (target_x < 0 || target_x >= LCD_H_RES) continue;

            uint32_t pixel_idx = row * glyph->width + col;
            uint32_t byte_idx = offset + (pixel_idx / 2);
            uint8_t byte_val = font->bitmaps[byte_idx];
            uint8_t alpha;
            if (pixel_idx % 2 == 0) {
                alpha = byte_val >> 4;
            } else {
                alpha = byte_val & 0x0F;
            }

            if (alpha > 0) {
                uint32_t fb_idx = target_y * LCD_H_RES + target_x;
                s_fb[fb_idx] = blend_pixels(s_fb[fb_idx], color, alpha);
            }
        }
    }
}

static int fb_draw_string_aa(int x, int y, const font_info_t *font, const char *str, uint16_t color)
{
    int cx = x;
    const char *p = str;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) break;
        const font_glyph_t *glyph = find_glyph(font, cp);
        if (glyph) {
            fb_draw_char_aa(cx, y, font, glyph, color);
            cx += glyph->advance;
        }
    }
    return cx - x;
}

static int str_width_aa(const font_info_t *font, const char *str)
{
    int width = 0;
    const char *p = str;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) break;
        const font_glyph_t *glyph = find_glyph(font, cp);
        if (glyph)
            width += glyph->advance;
    }
    return width;
}

/* ── 2× scaled text rendering for the info panel ────────────────────── */

static void fb_draw_char_aa_2x(int x, int y, const font_info_t *font,
                               const font_glyph_t *glyph, uint16_t color)
{
    if (glyph->width == 0 || glyph->height == 0) return;

    uint32_t offset = glyph->bitmap_offset;

    for (int row = 0; row < glyph->height; row++) {
        int base_y = y + (glyph->bearing_y + row) * 2;
        for (int col = 0; col < glyph->width; col++) {
            int base_x = x + (glyph->bearing_x + col) * 2;

            uint32_t pixel_idx = row * glyph->width + col;
            uint32_t byte_idx = offset + (pixel_idx / 2);
            uint8_t byte_val = font->bitmaps[byte_idx];
            uint8_t alpha;
            if (pixel_idx % 2 == 0)
                alpha = byte_val >> 4;
            else
                alpha = byte_val & 0x0F;

            if (alpha > 0) {
                for (int dy = 0; dy < 2; dy++) {
                    int ty = base_y + dy;
                    if (ty < 0 || ty >= LCD_V_RES) continue;
                    for (int dx = 0; dx < 2; dx++) {
                        int tx = base_x + dx;
                        if (tx < 0 || tx >= LCD_H_RES) continue;
                        s_fb[ty * LCD_H_RES + tx] =
                            blend_pixels(s_fb[ty * LCD_H_RES + tx], color, alpha);
                    }
                }
            }
        }
    }
}

static int fb_draw_string_aa_2x(int x, int y, const font_info_t *font,
                                const char *str, uint16_t color)
{
    int cx = x;
    const char *p = str;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) break;
        const font_glyph_t *glyph = find_glyph(font, cp);
        if (glyph) {
            fb_draw_char_aa_2x(cx, y, font, glyph, color);
            cx += glyph->advance * 2;
        }
    }
    return cx - x;
}

static int str_width_aa_2x(const font_info_t *font, const char *str)
{
    return str_width_aa(font, str) * 2;
}

static void display_supervisor_cb(void *arg)
{
    (void)arg;
    if (!s_display_task) return;
    int64_t last = s_last_render_us;
    if (last == 0) return;
    int64_t elapsed = esp_timer_get_time() - last;
    if (elapsed > 3500000) { /* > 3.5 seconds with no render */
        ESP_LOGW(TAG, "display supervisor: no frame rendered in %lld ms — waking display_task",
                 (long long)(elapsed / 1000));
        s_flush_stuck = true;
        xTaskNotifyGive(s_display_task);
    }
}

static void display_buffers_free(void)
{
    heap_caps_free(s_fb);
    s_fb = NULL;
    heap_caps_free(s_flow_buf);
    s_flow_buf = NULL;
    heap_caps_free(s_flow_local);
    s_flow_local = NULL;
    heap_caps_free(s_flow_yf);
    s_flow_yf = NULL;
    if (s_state_mutex) vSemaphoreDelete(s_state_mutex);
    s_state_mutex = NULL;
#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    for (int i = 0; i < LCD_STRIP_BUFS; ++i) {
        heap_caps_free(s_strip[i]);
        s_strip[i] = NULL;
    }
    if (s_flush_done) vSemaphoreDelete(s_flush_done);
    s_flush_done = NULL;
#endif
}
static esp_err_t display_buffers_init(void)
{
    s_fb = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_fb)
        s_fb = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_fb) goto no_mem;

    s_flow_buf = heap_caps_calloc(FLOW_BUF_SIZE, sizeof(float), MALLOC_CAP_SPIRAM);
    s_flow_local = heap_caps_malloc(FLOW_BUF_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    s_flow_yf = heap_caps_malloc(LCD_H_RES * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_flow_buf || !s_flow_local || !s_flow_yf) goto no_mem;

#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    for (int i = 0; i < LCD_STRIP_BUFS; ++i) {
        s_strip[i] = heap_caps_malloc(LCD_H_RES * LCD_STRIP_ROWS * sizeof(uint16_t),
                                      MALLOC_CAP_DMA);
        if (!s_strip[i]) goto no_mem;
    }
    s_flush_done = xSemaphoreCreateCounting(LCD_STRIP_BUFS, 0);
    if (!s_flush_done) goto no_mem;
#endif
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) goto no_mem;
    return ESP_OK;

no_mem:
    display_buffers_free();
    ESP_LOGE(TAG, "display buffer/semaphore allocation failed");
    return ESP_ERR_NO_MEM;
}

esp_err_t bsp_display_init(void)
{
    esp_err_t err = display_buffers_init();
    if (err != ESP_OK) return err;
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    ESP_ERROR_CHECK(board_qemu_154_init(&s_panel));
#else
    /* The project's default log level is DEBUG; spi_master emits several DEBUG
     * lines per DMA transaction. At the LCD's transfer rate that is a real CPU
     * and I/O drain, so quiet it down to WARN regardless of the global level. */
    esp_log_level_set("spi_master", ESP_LOG_WARN);

    /* Backlight: LEDC PWM for brightness control */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_RESOLUTION,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));

    ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));
    s_backlight_on = true;
    s_brightness = 100;

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_color_done_cb,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));
    s_io = io;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
#endif

    /* Start the single-owner render task after every resource exists. Only this
     * task ever touches the framebuffer or the LCD panel. */
    s_display_task = psram_task_create(display_task, "display", DISPLAY_TASK_STACK, NULL, 4, tskNO_AFFINITY, NULL, NULL);
    if (!s_display_task) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        spi_bus_free(LCD_SPI_HOST);
        ledc_stop(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, 0);
#endif
        display_buffers_free();
        ESP_LOGE(TAG, "display task allocation failed");
        return ESP_ERR_NO_MEM;
    }

    /* Start non-intrusive display supervisor timer to monitor display responsiveness */
    esp_timer_create_args_t sup_args = {
        .callback = display_supervisor_cb,
        .name = "disp_sup",
    };
    if (esp_timer_create(&sup_args, &s_display_supervisor_timer) == ESP_OK) {
        esp_timer_start_periodic(s_display_supervisor_timer, 3000000); /* 3s */
    }

#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    ESP_LOGI(TAG, "original 240x240 renderer initialised on QEMU RGB panel");
#else
    ESP_LOGI(TAG, "ST7789 display initialised");
#endif
    return ESP_OK;
}

void bsp_display_set_wifi_connected(bool connected)
{
    if (!s_state_mutex) {
        s_wifi_connected = connected;
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_wifi_connected = connected;
    s_status_dirty = true;
    xSemaphoreGive(s_state_mutex);
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

void bsp_display_set_as11_paired(bool paired)
{
    if (!s_state_mutex) {
        s_as11_paired = paired;
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_as11_paired = paired;
    s_status_dirty = true;
    xSemaphoreGive(s_state_mutex);
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

void bsp_display_set_battery(int percent, bool charging, bool valid)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_batt_percent == percent && s_batt_charging == charging && s_batt_valid == valid) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    s_batt_percent = percent;
    s_batt_charging = charging;
    s_batt_valid = valid;
    s_status_dirty = true;
    xSemaphoreGive(s_state_mutex);
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

/* ── Backlight control ─────────────────────────────────────────────── */

void bsp_display_set_brightness(uint8_t percent)
{
    if (percent < 1) percent = 1;
    if (percent > 200) percent = 200;
    s_brightness = percent;
#if !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    if (s_backlight_on) {
        /* percent is in tenth-percent units (1=0.1%), so divide by 1000 */
        uint32_t duty = (uint32_t)(percent) * BL_DUTY_MAX / 1000;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
    }
#endif
}

void bsp_display_set_backlight(bool on)
{
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (on != s_backlight_on) {
        s_backlight_on = on;
        ++s_qemu_scene_generation;
        s_status_dirty = true;
    }
    if (s_state_mutex) xSemaphoreGive(s_state_mutex);
    if (s_display_task) xTaskNotifyGive(s_display_task);
#else
    if (on == s_backlight_on) return;
    s_backlight_on = on;
    if (on) {
        uint32_t duty = (uint32_t)(s_brightness) * BL_DUTY_MAX / 1000;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
    }
#endif
}

bool bsp_display_toggle_backlight(void)
{
    /* SoftAP setup deliberately keeps its connection details visible. */
    if (s_backlight_force_on) {
        bsp_display_set_backlight(true);
        return true;
    }

    bool on = !s_backlight_on;
    bsp_display_set_backlight(on);
    return on;
}

uint8_t bsp_display_get_brightness(void)
{
    return s_brightness;
}

void bsp_display_apply_backlight_policy(bool force_on)
{
    if (force_on) {
        s_backlight_force_on = true;
        bsp_display_set_backlight(true);
        return;
    }

    /* If force-on is active (SoftAP), keep backlight on regardless of mode */
    if (s_backlight_force_on) {
        bsp_display_set_backlight(true);
        return;
    }

    /* If temporarily awake (touch or motion), keep backlight on until timer expires */
    if (s_temporarily_awake) {
        bsp_display_set_backlight(true);
        return;
    }

    const device_settings_t *dev = device_settings_get();
    bool therapy_active = bsp_display_is_therapy_active();

    switch (dev->backlight_mode) {
    case BACKLIGHT_MODE_ON:
        bsp_display_set_backlight(true);
        break;
    case BACKLIGHT_MODE_OFF_THRP:
        bsp_display_set_backlight(!therapy_active);
        break;
    case BACKLIGHT_MODE_ALWAYS_OFF:
        bsp_display_set_backlight(false);
        break;
    default:
        bsp_display_set_backlight(true);
        break;
    }
}

static void wake_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "temporary wake timeout expired");
    s_temporarily_awake = false;
    bsp_display_apply_backlight_policy(false);
}

void bsp_display_wake_temporary(uint32_t duration_sec)
{
    if (duration_sec == 0) return;

    if (!s_wake_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = wake_timer_cb,
            .arg = NULL,
            .name = "lcd_wake_tmr"
        };
        esp_err_t err = esp_timer_create(&timer_args, &s_wake_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to create wake timer: %s", esp_err_to_name(err));
            return;
        }
    }

    /* Stop any active timer to re-trigger / extend duration */
    esp_timer_stop(s_wake_timer);

    s_temporarily_awake = true;
    bsp_display_set_backlight(true);
    esp_timer_start_once(s_wake_timer, (uint64_t)duration_sec * 1000000ULL);
}

bool bsp_display_is_temporarily_awake(void)
{
    return s_temporarily_awake;
}

void bsp_display_cancel_temporary_wake(void)
{
    if (s_wake_timer) {
        esp_timer_stop(s_wake_timer);
    }
    s_temporarily_awake = false;
    bsp_display_apply_backlight_policy(false);
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= LCD_V_RES) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= LCD_H_RES) continue;
            s_fb[row * LCD_H_RES + col] = color;
        }
    }
}

static void fb_clear(uint16_t color)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        s_fb[i] = color;
    }
}

/* Alpha-blend src over dst. Both are byte-swapped RGB565 (the on-wire format
 * produced by rgb565()); a is 0..255 coverage. */
static inline uint16_t blend565(uint16_t dst_sw, uint16_t src_sw, uint8_t a)
{
    uint16_t d = (uint16_t)((dst_sw >> 8) | (dst_sw << 8));
    uint16_t s = (uint16_t)((src_sw >> 8) | (src_sw << 8));
    uint16_t ia = (uint16_t)(255 - a);
    uint16_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    uint16_t sr = (s >> 11) & 0x1F, sg = (s >> 5) & 0x3F, sb = s & 0x1F;
    uint16_t rr = (uint16_t)((sr * a + dr * ia) / 255);
    uint16_t rg = (uint16_t)((sg * a + dg * ia) / 255);
    uint16_t rb = (uint16_t)((sb * a + db * ia) / 255);
    uint16_t r = (uint16_t)((rr << 11) | (rg << 5) | rb);
    return (uint16_t)((r >> 8) | (r << 8));
}

static inline void fb_blend(int x, int y, uint16_t color_sw, uint8_t a)
{
    if (a == 0 || x < 0 || x >= LCD_H_RES || y < 0 || y >= LCD_V_RES) return;
    uint16_t *p = &s_fb[y * LCD_H_RES + x];
    *p = blend565(*p, color_sw, a);
}

/* Antialiased line of given thickness, drawn via per-pixel distance coverage
 * and alpha-blended at up to max_alpha. Round end caps. */
static void fb_draw_line_aa(float x0, float y0, float x1, float y1,
                            float thick, uint16_t color, uint8_t max_alpha)
{
    float half = thick * 0.5f;
    int ix0 = (int)floorf(fminf(x0, x1) - half - 1.0f);
    int ix1 = (int)ceilf(fmaxf(x0, x1) + half + 1.0f);
    int iy0 = (int)floorf(fminf(y0, y1) - half - 1.0f);
    int iy1 = (int)ceilf(fmaxf(y0, y1) + half + 1.0f);

    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;

    for (int y = iy0; y <= iy1; y++) {
        if (y < 0 || y >= LCD_V_RES) continue;
        for (int x = ix0; x <= ix1; x++) {
            if (x < 0 || x >= LCD_H_RES) continue;
            float t = len2 > 0.0f ? ((x - x0) * dx + (y - y0) * dy) / len2 : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float cx = x0 + t * dx, cy = y0 + t * dy;
            float ex = x - cx, ey = y - cy;
            float dist = sqrtf(ex * ex + ey * ey);
            float cov = half + 0.5f - dist;   /* coverage in px */
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            fb_blend(x, y, color, (uint8_t)(cov * max_alpha));
        }
    }
}

void bsp_display_show_number(uint32_t value)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);
    const char *lines[] = { buf };
    bsp_display_show_lines(NULL, lines, 1);
}

static uint16_t get_wifi_rssi_color(int rssi)
{
    if (rssi >= -60) {
        return rgb565(0, 255, 120);   // Excellent: green
    } else if (rssi >= -70) {
        return rgb565(100, 255, 100);  // Good: vibrant light green
    } else if (rssi >= -80) {
        return rgb565(255, 220, 0);   // Fair: yellow
    } else {
        return rgb565(255, 50, 50);   // Poor: red
    }
}

static void fb_draw_wifi_indicator(int x, int y, bool connected)
{
    if (!connected) {
        return; // Not connected, don't draw indicator
    }

#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    int rssi = -55;  /* deterministic fixture; never starts or queries Wi-Fi */
#else
    int rssi = -128;
    if (esp_wifi_sta_get_rssi(&rssi) != ESP_OK) {
        return; // Not connected, don't draw indicator
    }
#endif

    int active_bars = 0;
    if (rssi >= -60) active_bars = 4;
    else if (rssi >= -70) active_bars = 3;
    else if (rssi >= -80) active_bars = 2;
    else if (rssi >= -90) active_bars = 1;

    uint16_t inactive_col = rgb565(60, 60, 60);
    uint16_t active_col = get_wifi_rssi_color(rssi);

    // Draw 4 bars of increasing height
    for (int i = 0; i < 4; i++) {
        uint16_t col = (i < active_bars) ? active_col : inactive_col;
        int bar_h = (i + 1) * 4;
        fb_fill_rect(x + i * 5, y + 16 - bar_h, 3, bar_h, col);
    }
}

void bsp_display_show_lines(const char *title, const char *const *lines, int n_lines)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (title) {
        strncpy(s_status_title, title, STATUS_TITLE_LEN - 1);
        s_status_title[STATUS_TITLE_LEN - 1] = '\0';
    } else {
        s_status_title[0] = '\0';
    }

    if (n_lines < 0) n_lines = 0;
    if (n_lines > MAX_STATUS_LINES) n_lines = MAX_STATUS_LINES;
    s_status_nlines = n_lines;
    for (int i = 0; i < n_lines; i++) {
        if (lines[i]) {
            strncpy(s_status_lines[i], lines[i], STATUS_LINE_LEN - 1);
            s_status_lines[i][STATUS_LINE_LEN - 1] = '\0';
        } else {
            s_status_lines[i][0] = '\0';
        }
    }

    s_status_dirty = true;
    xSemaphoreGive(s_state_mutex);
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

/* ── Render helpers — called ONLY by display_task ───────────────────── */

/* Render the scrolling flow waveform with a fixed (static) vertical scale and
 * a left scale axis. Snapshots the flow ring buffer under the state mutex,
 * then draws without holding it so high-rate push_flow() callers are never
 * blocked for long. */
static void render_graph(void)
{
    if (!s_panel || !s_fb || !s_flow_buf || !s_flow_local || !s_flow_yf) return;

    const uint16_t bg        = rgb565(9, 11, 18);
    const uint16_t grid_col  = rgb565(28, 32, 46);
    const uint16_t zero_col  = rgb565(70, 78, 102);
    const uint16_t axis_col  = rgb565(40, 46, 64);
    const uint16_t flow_col  = rgb565(54, 247, 160);   /* sharp mint line */
    const uint16_t glow_col  = rgb565(20, 205, 132);   /* soft glow */
    const uint16_t fill_col  = rgb565(28, 150, 104);   /* area under curve */
    const uint16_t label_col = rgb565(122, 134, 158);
    const uint16_t unit_col  = rgb565(86, 96, 120);

    int n, head;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    n = s_flow_count;
    head = s_flow_head;
    memcpy(s_flow_local, s_flow_buf, FLOW_BUF_SIZE * sizeof(float));
    xSemaphoreGive(s_state_mutex);

    fb_clear(bg);

    const int mid_y  = (GRAPH_PLOT_TOP + GRAPH_PLOT_BOT) / 2;
    const int half_h = (GRAPH_PLOT_BOT - GRAPH_PLOT_TOP) / 2;
    const float scale = (float)half_h / GRAPH_FULL_SCALE;  /* px per L/min */

    /* ── Grid + left scale axis ──────────────────────────────────────── */
    const int tick_step = 50;  /* L/min between labelled ticks */
    int nticks = (int)(GRAPH_FULL_SCALE / tick_step);
    if (nticks < 1) nticks = 1;
    for (int t = -nticks; t <= nticks; t++) {
        int gy = mid_y + (int)lroundf(t * tick_step * scale);
        if (gy < GRAPH_PLOT_TOP || gy > GRAPH_PLOT_BOT) continue;
        if (t == 0) {
            for (int x = GRAPH_PLOT_X0; x < LCD_H_RES; x++)
                s_fb[gy * LCD_H_RES + x] = zero_col;
        } else {
            for (int x = GRAPH_PLOT_X0; x < LCD_H_RES; x += 5)
                s_fb[gy * LCD_H_RES + x] = grid_col;
        }
        char tb[8];
        snprintf(tb, sizeof(tb), "%d", t < 0 ? -t * tick_step : t * tick_step);
        int tw = str_width_aa(&roboto_body, tb);
        int tx = GRAPH_AXIS_W - 4 - tw;
        if (tx < 1) tx = 1;
        fb_draw_string_aa(tx, gy - roboto_body.height / 2, &roboto_body, tb, label_col);
    }

    /* Vertical time gridlines (anchored to the right/newest edge). */
    for (int x = LCD_H_RES - 1; x >= GRAPH_PLOT_X0; x -= 42)
        for (int y = GRAPH_PLOT_TOP; y <= GRAPH_PLOT_BOT; y += 5)
            s_fb[y * LCD_H_RES + x] = grid_col;

    /* Axis separator + unit label. */
    for (int y = GRAPH_PLOT_TOP; y <= GRAPH_PLOT_BOT; y++)
        s_fb[y * LCD_H_RES + (GRAPH_AXIS_W - 1)] = axis_col;
    fb_draw_string_aa(GRAPH_AXIS_W + 3, 1, &roboto_body, "L/m", unit_col);

    /* ── Waveform (static scale, right-aligned newest sample) ────────── */
    int m = n;
    if (m > GRAPH_PLOT_W) m = GRAPH_PLOT_W;
    int start = (head - m + FLOW_BUF_SIZE) % FLOW_BUF_SIZE;
    int xbase = LCD_H_RES - m;

    float *yf = s_flow_yf;
    for (int j = 0; j < m; j++) {
        float val = s_flow_local[(start + j) % FLOW_BUF_SIZE];
        float y = mid_y - val * scale;  /* positive flow (inhale) → up */
        if (y < GRAPH_PLOT_TOP) y = GRAPH_PLOT_TOP;   /* static hard cut */
        if (y > GRAPH_PLOT_BOT) y = GRAPH_PLOT_BOT;
        yf[xbase + j] = y;
    }

    /* Translucent area fill between the curve and the zero line. */
    for (int j = 0; j < m; j++) {
        int x = xbase + j;
        if (!isfinite(yf[x])) continue;
        int y0 = (int)(yf[x] < mid_y ? yf[x] : mid_y);
        int y1 = (int)(yf[x] < mid_y ? mid_y : yf[x]);
        for (int y = y0; y <= y1; y++)
            fb_blend(x, y, fill_col, 30);
    }

    /* Soft glow pass, then the sharp antialiased line on top. */
    for (int j = 1; j < m; j++) {
        int x = xbase + j;
        if (!isfinite(yf[x - 1]) || !isfinite(yf[x])) continue;
        fb_draw_line_aa(x - 1, yf[x - 1], x, yf[x], 4.5f, glow_col, 45);
    }
    for (int j = 1; j < m; j++) {
        int x = xbase + j;
        if (!isfinite(yf[x - 1]) || !isfinite(yf[x])) continue;
        fb_draw_line_aa(x - 1, yf[x - 1], x, yf[x], 2.0f, flow_col, 255);
    }

    lcd_flush();
}

/* Helper to colour-code leak values by severity */
static inline uint16_t leak_val_color(float leak)
{
    if (leak < 12.0f)
        return rgb565(50, 245, 90);    /* green - low / good seal */
    else if (leak < 24.0f)
        return rgb565(255, 225, 0);    /* yellow - moderate */
    else if (leak < 36.0f)
        return rgb565(255, 135, 0);    /* orange - high */
    else
        return rgb565(255, 60, 60);     /* red - excessive / large leak */
}

/* ── Info panel: leak rates (CUR | AVG) + session runtime ─────────────
 *
 * Top half:   Single header line: "CUR" (left accent), "Leak (L/min)" (center gray), "AVG" (right accent)
 *             Left column: Current leak in roboto_title 2× scale (~66px tall)
 *             Right column: Session average leak in roboto_title 2× scale (~66px tall)
 *             Vertical divider line at x = 120.
 * Bottom half: "Session runtime" header in roboto_body (slate gray),
 *              then elapsed time "H:MM" in roboto_title at 2× scale (~66px tall).
 */
static void render_info(void)
{
    if (!s_panel || !s_fb) return;

    float leak_lpm;
    double leak_sum;
    uint32_t leak_count;
    int64_t start_us;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    leak_lpm = s_leak_lpm;
    leak_sum = s_leak_sum;
    leak_count = s_leak_count;
    start_us = s_therapy_start_us;
    xSemaphoreGive(s_state_mutex);

    const uint16_t bg        = rgb565(9, 11, 18);
    const uint16_t hdr_col   = rgb565(122, 134, 158);
    const uint16_t label_col = rgb565(100, 190, 240);  /* cyan accent for CUR & AVG */
    const uint16_t div_col   = rgb565(28, 32, 46);

    fb_clear(bg);

    /* ── Top half: Header line ───────────────────────────────────────── */
    /* "CUR" label at left margin */
    int cur_lbl_x = 12;
    fb_draw_string_aa(cur_lbl_x, 10, &roboto_body, "CUR", label_col);

    /* "Leak (L/min)" title in center */
    const char *leak_hdr = "Leak (L/min)";
    int hdr_w = str_width_aa(&roboto_body, leak_hdr);
    int hdr_x = (LCD_H_RES - hdr_w) / 2;
    if (hdr_x < 4) hdr_x = 4;
    fb_draw_string_aa(hdr_x, 10, &roboto_body, leak_hdr, hdr_col);

    /* "AVG" label at right margin */
    int avg_lbl_w = str_width_aa(&roboto_body, "AVG");
    int avg_lbl_x = LCD_H_RES - 12 - avg_lbl_w;
    fb_draw_string_aa(avg_lbl_x, 10, &roboto_body, "AVG", label_col);

    /* ── Top half: Values (roboto_title 2× scaled, ~66px tall) ───────── */
    /* Current leak */
    char cur_str[16];
    if (leak_lpm < 100.0f)
        snprintf(cur_str, sizeof(cur_str), "%.1f", leak_lpm);
    else
        snprintf(cur_str, sizeof(cur_str), "%.0f", leak_lpm);

    int cur_w = str_width_aa_2x(&roboto_title, cur_str);
    int cur_x = 58 - cur_w / 2;
    if (cur_x < 4) cur_x = 4;
    fb_draw_string_aa_2x(cur_x, 42, &roboto_title, cur_str, leak_val_color(leak_lpm));

    /* Average leak */
    float avg_lpm = (leak_count > 0) ? (float)(leak_sum / leak_count) : leak_lpm;
    char avg_str[16];
    if (avg_lpm < 100.0f)
        snprintf(avg_str, sizeof(avg_str), "%.1f", avg_lpm);
    else
        snprintf(avg_str, sizeof(avg_str), "%.0f", avg_lpm);

    int avg_w = str_width_aa_2x(&roboto_title, avg_str);
    int avg_x = 182 - avg_w / 2;
    if (avg_x < 124) avg_x = 124;
    fb_draw_string_aa_2x(avg_x, 42, &roboto_title, avg_str, leak_val_color(avg_lpm));

    /* Vertical divider tick between CUR and AVG columns */
    for (int y = 36; y < 104; y++)
        s_fb[y * LCD_H_RES + 120] = div_col;

    /* ── Divider line ────────────────────────────────────────────────── */
    int mid_y = LCD_V_RES / 2;
    for (int x = 16; x < LCD_H_RES - 16; x++)
        s_fb[mid_y * LCD_H_RES + x] = div_col;

    /* ── Bottom half: Session runtime ────────────────────────────────── */
    const char *rt_hdr = "Session runtime";
    int rt_hdr_w = str_width_aa(&roboto_body, rt_hdr);
    int rt_hdr_x = (LCD_H_RES - rt_hdr_w) / 2;
    if (rt_hdr_x < 4) rt_hdr_x = 4;
    fb_draw_string_aa(rt_hdr_x, mid_y + 10, &roboto_body, rt_hdr, hdr_col);

    /* Calculate elapsed time from TherapyStart (monotonic) */
    int hours = 0, mins = 0;
    if (start_us > 0) {
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
        int64_t elapsed_us = QEMU_DEMO_NOW_US - start_us;
#else
        int64_t elapsed_us = esp_timer_get_time() - start_us;
#endif
        if (elapsed_us < 0) elapsed_us = 0;
        int64_t elapsed_sec = elapsed_us / 1000000;
        hours = (int)(elapsed_sec / 3600);
        mins = (int)((elapsed_sec % 3600) / 60);
    }

    char rt_str[16];
    snprintf(rt_str, sizeof(rt_str), "%d:%02d", hours, mins);

    int rt_w = str_width_aa_2x(&roboto_title, rt_str);
    int rt_x = (LCD_H_RES - rt_w) / 2;
    if (rt_x < 4) rt_x = 4;
    fb_draw_string_aa_2x(rt_x, mid_y + 42, &roboto_title, rt_str,
                         rgb565(200, 210, 225));

    lcd_flush();
}

/* Draw Bluetooth paired icon (standard 1px-stroke Bluetooth rune).
 * Layout: 9×15px icon centered to the left of WiFi bars.
 * x,y is the top-left corner of the icon bounding box. */
static void fb_draw_ble_indicator(int x, int y)
{
    static const uint16_t bitmap[15] = {
        0x030, /* ...##.... */
        0x02C, /* ...#.##.. */
        0x022, /* ...#...#. */
        0x121, /* #..#....# */
        0x0A2, /* .#.#...#. */
        0x064, /* ..##..#.. */
        0x028, /* ...#.#... */
        0x030, /* ...##.... */
        0x028, /* ...#.#... */
        0x064, /* ..##..#.. */
        0x0A2, /* .#.#...#. */
        0x121, /* #..#....# */
        0x022, /* ...#...#. */
        0x02C, /* ...#.##.. */
        0x030, /* ...##.... */
    };
    uint16_t col = rgb565(80, 180, 255);

    for (int r = 0; r < 15; r++) {
        uint16_t row = bitmap[r];
        int py = y + r;
        if (py < 0 || py >= LCD_V_RES) continue;
        for (int c = 0; c < 9; c++) {
            if (row & (1 << (8 - c))) {
                int px = x + c;
                if (px >= 0 && px < LCD_H_RES) {
                    s_fb[py * LCD_H_RES + px] = col;
                }
            }
        }
    }
}

/* Draw battery percentage with an outline icon and prominent charging bolt.
 * Layout: [22×14px battery outline + 3×6px nub] [N% text]
 * x,y is the top-left of the battery outline. */
static void fb_draw_battery_indicator(int x, int y, int percent, bool charging)
{
    uint16_t frame_col = rgb565(180, 180, 180);
    uint16_t bolt_col = rgb565(255, 204, 0);

    /* Text color based on charge level */
    uint16_t text_col;
    if (percent < 0) {
        text_col = rgb565(160, 180, 205);   /* calibrating / unknown */
    } else if (percent <= 15) {
        text_col = rgb565(255, 60, 60);      /* red */
    } else if (percent <= 30) {
        text_col = rgb565(255, 180, 0);      /* orange */
    } else {
        text_col = rgb565(80, 220, 100);     /* green */
    }

    /* Battery outline: 22px wide × 14px tall body + 3×6px terminal nub */
    fb_fill_rect(x, y, 22, 14, frame_col);             /* outer frame */
    fb_fill_rect(x + 1, y + 1, 20, 12, rgb565(0, 0, 0)); /* inner cavity */
    fb_fill_rect(x + 22, y + 4, 3, 6, frame_col);      /* terminal nub */

    /* Charging bolt inside the cavity, or proportional fill if on battery */
    if (charging) {
        /* Prominent bold lightning bolt centered in 20×12 cavity */
        int bx = x + 7;
        int by = y + 2;
        fb_fill_rect(bx + 5, by + 0, 2, 1, bolt_col);
        fb_fill_rect(bx + 4, by + 1, 2, 1, bolt_col);
        fb_fill_rect(bx + 3, by + 2, 3, 1, bolt_col);
        fb_fill_rect(bx + 2, by + 3, 3, 1, bolt_col);
        fb_fill_rect(bx + 1, by + 4, 4, 1, bolt_col);
        fb_fill_rect(bx + 0, by + 5, 8, 1, bolt_col);  /* waist bar */
        fb_fill_rect(bx + 3, by + 6, 4, 1, bolt_col);
        fb_fill_rect(bx + 2, by + 7, 4, 1, bolt_col);
        fb_fill_rect(bx + 2, by + 8, 3, 1, bolt_col);
        fb_fill_rect(bx + 1, by + 9, 3, 1, bolt_col);
        fb_fill_rect(bx + 0, by + 10, 2, 1, bolt_col);
    } else if (percent > 0) {
        int fill_w = (percent * 20 + 50) / 100;
        if (fill_w < 1) fill_w = 1;
        if (fill_w > 20) fill_w = 20;
        fb_fill_rect(x + 1, y + 1, fill_w, 12, text_col);
    }

    /* Percentage text to the right of the outline */
    char pct_str[16];
    if (percent >= 0) {
        snprintf(pct_str, sizeof(pct_str), "%d%%", percent);
    } else {
        snprintf(pct_str, sizeof(pct_str), "--%%");
    }
    fb_draw_string_aa(x + 28, y - 3, &roboto_body, pct_str, text_col);
}

/* Render the status screen. Snapshots content under the state mutex, then
 * draws without holding it. RSSI is read live each refresh. */
static void render_status(void)
{
    if (!s_panel || !s_fb) return;

    char title[STATUS_TITLE_LEN];
    char lines[MAX_STATUS_LINES][STATUS_LINE_LEN];
    int nlines;
    bool wifi;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memcpy(title, s_status_title, sizeof(title));
    memcpy(lines, s_status_lines, sizeof(lines));
    nlines = s_status_nlines;
    wifi = s_wifi_connected;
    bool as11_paired = s_as11_paired;
    int batt_pct = s_batt_percent;
    bool batt_chg = s_batt_charging;
    bool batt_valid = s_batt_valid;
    char notice[STATUS_LINE_LEN];
    memcpy(notice, s_notice, sizeof(notice));
    xSemaphoreGive(s_state_mutex);

    const uint16_t bg = rgb565(0, 0, 0);
    const uint16_t title_col = rgb565(0, 255, 120);
    const uint16_t text_col = rgb565(255, 255, 255);

    fb_clear(bg);

    /* Clock display (top-left) */
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    time_t now = QEMU_DEMO_WALL_TIME;
#else
    time_t now = time(NULL);
#endif
    if (now > 1700000000) {  /* only show if NTP-synced (after ~Nov 2023) */
        struct tm tm_info;
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
        gmtime_r(&now, &tm_info);
#else
        localtime_r(&now, &tm_info);
#endif
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
        fb_draw_string_aa(6, 9, &roboto_body, time_str, rgb565(200, 210, 225));
    }

    fb_draw_wifi_indicator(218, 10, wifi);

    /* AS11 BLE paired icon — Bluetooth rune to the left of WiFi bars */
    if (as11_paired) {
        fb_draw_ble_indicator(199, 11);
    }

    /* Battery indicator — left of BLE icon, right of clock area */
    if (batt_valid) {
        fb_draw_battery_indicator(118, 12, batt_pct, batt_chg);
    }

    int y = 48;
    if (title[0]) {
        int w = str_width_aa(&roboto_title, title);
        int x = (LCD_H_RES - w) / 2;
        if (x < 4) x = 4;
        fb_draw_string_aa(x, y, &roboto_title, title, title_col);
        y += 40;
    } else {
        y = 60;
    }

    int line_h = roboto_body.height + 6;
    if (line_h < 18) line_h = 18;
    for (int i = 0; i < nlines; i++) {
        if (!lines[i][0]) continue;
        int w = str_width_aa(&roboto_body, lines[i]);
        int x = (LCD_H_RES - w) / 2;
        if (x < 4) x = 4;
        fb_draw_string_aa(x, y, &roboto_body, lines[i], text_col);
        y += line_h;
    }

    /* ── Persistent notice banner (bottom, amber) ──────────────────────
     * Drawn last and anchored to the bottom edge so it survives whatever
     * the status lines above happen to say. */
    if (notice[0]) {
        const uint16_t notice_col = rgb565(255, 190, 30);
        const uint16_t notice_bg  = rgb565(46, 34, 0);

        int band_h = roboto_body.height + 10;
        int band_y = LCD_V_RES - band_h;
        fb_fill_rect(0, band_y, LCD_H_RES, band_h, notice_bg);

        /* Warning triangle, drawn from primitives (the font has no glyph). */
        int tri_h = 11;
        int tri_x = 8;
        int tri_y = band_y + (band_h - tri_h) / 2;
        for (int row = 0; row < tri_h; row++) {
            int half = (row * 6) / tri_h;
            fb_fill_rect(tri_x + 6 - half, tri_y + row, half * 2 + 1, 1, notice_col);
        }
        /* Exclamation mark punched out of the triangle. */
        fb_fill_rect(tri_x + 6, tri_y + 4, 1, 4, notice_bg);
        fb_fill_rect(tri_x + 6, tri_y + 9, 1, 1, notice_bg);

        int tw = str_width_aa(&roboto_body, notice);
        int tx = tri_x + 16 + ((LCD_H_RES - tri_x - 16) - tw) / 2;
        if (tx < tri_x + 16) tx = tri_x + 16;
        fb_draw_string_aa(tx, band_y + 5, &roboto_body, notice, notice_col);
    }

    lcd_flush();
}

void bsp_display_set_notice(const char *text)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (text && text[0]) {
        strncpy(s_notice, text, STATUS_LINE_LEN - 1);
        s_notice[STATUS_LINE_LEN - 1] = '\0';
    } else {
        s_notice[0] = '\0';
    }
    s_status_dirty = true;
    xSemaphoreGive(s_state_mutex);
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

/* The single owner of the framebuffer and LCD panel. Renders the current
 * mode at a fixed cadence and on every mode/content change. */

void bsp_display_set_critical_notice(const char *text)
{
    bsp_display_set_notice(text);
}

static void display_task(void *arg)
{
    (void)arg;
    TickType_t last_render = 0;
    disp_mode_t last_mode = (disp_mode_t)-1;

    for (;;) {
        /* Block until woken by new data / state change (push_flow,
         * set_therapy_active, show_lines, set_wifi_connected) or until the
         * status refresh interval elapses. Coalesced notifications mean we
         * redraw immediately when a packet arrives with zero latency and zero
         * busy-polling. */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STATUS_FRAME_MS));

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
        if (s_qemu_scene_seeding) {
            xSemaphoreGive(s_state_mutex);
            continue;
        }
#endif
        disp_mode_t mode = s_mode;
        bool dirty = s_status_dirty;
        s_status_dirty = false;
        bool rot_pending = s_rotation_pending;
        uint16_t rot_deg = s_pending_rotation_deg;
        s_rotation_pending = false;
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
        uint32_t scene_generation = s_qemu_scene_generation;
        uint8_t scene = s_qemu_scene;
        bool backlight_on = s_backlight_on;
#endif
        xSemaphoreGive(s_state_mutex);

        /* Apply deferred rotation before any render/flush so the SPI panel
         * IO handle is only ever touched from this task. */
        if (rot_pending) {
            s_rotation = rot_deg;
            apply_panel_rotation(rot_deg);
            /* Force a full re-render after rotation change. */
            last_mode = (disp_mode_t)-1;
        }

        bool mode_changed = (mode != last_mode);
        TickType_t now = xTaskGetTickCount();

        /* On any mode transition, hardware-reset the panel first so a wedged
         * ST7789 (frozen screen, ignoring SPI commands) is recovered before
         * the new frame is drawn. */
        if (mode_changed) {
            lcd_panel_hw_recover();
        }

        if (mode == DISP_MODE_GRAPH) {
            /* Instantaneous zero-lag redraw on fresh packet data, mode change,
             * or every second for status/keepalive. */
            if (notified || mode_changed || dirty ||
                (now - last_render) >= pdMS_TO_TICKS(1000)) {
                render_graph();
                last_render = now;
                s_last_render_us = esp_timer_get_time();
            }
        } else if (mode == DISP_MODE_INFO) {
            /* Redraw on new leak data, mode change, or every second for
             * the session runtime clock. */
            if (notified || mode_changed ||
                (now - last_render) >= pdMS_TO_TICKS(1000)) {
                render_info();
                last_render = now;
                s_last_render_us = esp_timer_get_time();
            }
        } else {
            if (mode_changed || dirty ||
                (now - last_render) >= pdMS_TO_TICKS(STATUS_FRAME_MS)) {
                render_status();
                last_render = now;
                s_last_render_us = esp_timer_get_time();
            }
        }

        /* If lcd_flush() timed out, the DMA/SPI bus is wedged.  Reset the
         * panel hardware and force a full re-render on the next iteration. */
        if (s_flush_stuck) {
            s_flush_stuck = false;
            lcd_panel_hw_recover();
            last_mode = (disp_mode_t)-1;
            continue;
        }

        last_mode = mode;

#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
        static uint32_t rendered_generation;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        bool scene_complete = scene_generation != 0 &&
            scene_generation == s_qemu_scene_generation && !s_qemu_scene_seeding;
        xSemaphoreGive(s_state_mutex);
        if (scene_complete && rendered_generation != scene_generation) {
            ESP_LOGI(TAG, "QEMU 1.54 scene ready: %u (%s)",
                     (unsigned)scene, s_qemu_scene_names[scene]);
            ESP_LOGI(TAG, "QEMU 1.54 frame ready: scene=%u rotation=%u backlight=%s",
                     (unsigned)scene, (unsigned)s_rotation, backlight_on ? "on" : "off");
            if (rendered_generation == 0)
                ESP_LOGI(TAG, "240x240 original-board UI preview ready");
            rendered_generation = scene_generation;
        }
#endif

        /* One-shot high-water mark after the first render to verify the
         * 4 KB PSRAM stack is sufficient for the rendering + SPI blit path. */
        static bool hwm_logged = false;
        if (!hwm_logged) {
            hwm_logged = true;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "display: stack high-water = %u bytes",
                     (unsigned)(hwm * sizeof(StackType_t)));
        }
    }
}

void bsp_display_set_sd_ready(bool ready)
{
    (void)ready;
}

void bsp_display_qemu_seed_demo(void)
{
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    bsp_display_qemu_set_tab(0);
#endif
}
void bsp_display_qemu_set_tab(uint8_t tab)
{
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
    /* These are capture scenes, not touchscreen tabs: the compact firmware
     * has no local navigation surface. UART selects the same existing views. */
    if (!s_state_mutex || tab >= sizeof(s_qemu_scene_names) / sizeof(s_qemu_scene_names[0]))
        return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_qemu_scene_seeding = true;
    xSemaphoreGive(s_state_mutex);

    bsp_display_set_therapy_active(false);
    bsp_display_set_notice(NULL);
    bsp_display_set_wifi_connected(tab != 3);
    bsp_display_set_as11_paired(tab != 3);
    bsp_display_set_battery(tab == 3 ? 18 : 82, false, true);
    bsp_display_set_rotation(0);
    bsp_display_set_brightness(100);
    bsp_display_set_backlight(true);

    if (tab == 1 || tab == 2) {
        device_settings_set_therapy_screen(tab == 1 ? THERAPY_SCREEN_GRAPH : THERAPY_SCREEN_INFO);
        bsp_display_set_therapy_active(true);
        /* Positive origin lets the real runtime renderer display 0:42 even
         * though this fixture starts immediately after guest boot. */
        bsp_display_set_therapy_start_time(1);
        if (tab == 1) {
            for (int i = 0; i < FLOW_BUF_SIZE; ++i) {
                if ((i >= 78 && i < 91) || (i >= 168 && i < 177)) {
                    bsp_display_push_flow_gap(1);
                } else {
                    float phase = i * 0.10f;
                    bsp_display_push_flow(48.0f * sinf(phase) + 9.0f * sinf(phase * 2.0f));
                }
            }
        } else {
            bsp_display_push_leak(2.4f);
            bsp_display_push_leak(3.6f);
            bsp_display_push_leak(4.8f);
        }
    } else {
        const char *const ready[] = {"AirSense paired", "Waiting for therapy", "Simulated data"};
        const char *const disconnected[] = {"Wi-Fi disconnected", "AirSense not paired", "Simulated data"};
        bsp_display_show_lines("SomnoTrace", tab == 3 ? disconnected : ready, 3);
        if (tab == 3) bsp_display_set_notice("Check connection");
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_qemu_scene = tab;
    ++s_qemu_scene_generation;
    s_qemu_scene_seeding = false;
    s_status_dirty = true;
    xSemaphoreGive(s_state_mutex);
    if (s_display_task) xTaskNotifyGive(s_display_task);
#else
    (void)tab;
#endif
}
esp_err_t bsp_display_qemu_start_setup_preview(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void bsp_display_enable_touch_services(bool as11_ready, bool oximeter_ready)
{
    /* The compact non-touch target has no local service controls. */
    (void)as11_ready;
    (void)oximeter_ready;
}

void bsp_display_restart_idle_timeout(void)
{
    /* The compact profile has no local touch surface, so automatic idle sleep
     * remains disabled there. Keep the cross-board settings API harmless. */
}

void bsp_display_push_metrics(float pressure_cmh2o, float respiratory_rate,
                              float flow_limitation)
{
    (void)pressure_cmh2o;
    (void)respiratory_rate;
    (void)flow_limitation;
}

esp_err_t bsp_display_start_first_run_setup(esp_err_t initial_card_result)
{
    (void)initial_card_result;
    return ESP_ERR_NOT_SUPPORTED;
}

bool bsp_display_first_run_setup_active(void)
{
    return false;
}

void bsp_display_set_setup_callback(void (*callback)(void))
{
    /* The 1.54-inch target enters setup with its physical BOOT button. Keep
     * the hook for a uniform BSP contract and future touch revisions. */
    s_setup_callback = callback;
    (void)s_setup_callback;
}

bool bsp_display_get_wake_snapshot(bsp_display_wake_snapshot_t *out)
{
    (void)out;
    return false;
}
