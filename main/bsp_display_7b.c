/*
 * SomnoTrace native 1024x600 touch UI for Waveshare ESP32-S3-Touch-LCD-7B.
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bsp_display.h"
#include "board_waveshare_7b.h"
#include "device_settings.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "esp_lcd_touch.h"
#include "controller_diagnostics.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "live_flow_plot.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#if CONFIG_SOMNOTRACE_BOARD_QEMU
#include "board_qemu.h"
#include "esp_lcd_qemu_rgb.h"
#else
#include "esp_lcd_panel_rgb.h"
#endif
#define FLOW_POINTS 300
#define UI_UPDATE_MS 50
#define TOUCH_FAILURE_THRESHOLD 3
#define BACKLIGHT_RETRY_US 250000
typedef struct {
    bool wifi;
    bool paired;
    bool sd_ready;
    bool storage_near_full;
    bool therapy;
    bool charging;
    int battery;
    float leak;
    float pressure;
    float respiratory_rate;
    float flow_limitation;
    int64_t leak_sample_us;
    int64_t pressure_sample_us;
    int64_t respiratory_rate_sample_us;
    int64_t flow_limitation_sample_us;
    int64_t therapy_start_us;
    int16_t flow[FLOW_POINTS];
    unsigned flow_head;
    unsigned flow_count;
    unsigned flow_version;
    int64_t flow_sample_us;
    char title[48];
    char status[192];
    char attention[256];
    char notice[64];
    int64_t notice_expires_us;
    bool notice_critical;
} ui_state_t;

static const char *TAG = "display_7b";

#if CONFIG_SOMNOTRACE_BOARD_QEMU
#define UI_BOARD_NAME "ESP32-S3 QEMU UI preview"
#define UI_TOUCH_STATUS "QEMU pointer ready"
#else
#define UI_BOARD_NAME "Waveshare ESP32-S3 Touch LCD 7B"
#define UI_TOUCH_STATUS (s_touch ? "GT911 ready" : "not detected")
#endif
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static ui_state_t s_state;
/* Protected by s_state_lock together with s_state.therapy. Start waiters force
 * a pending restart reservation to release its SD lease before they publish. */
static bool s_therapy_safe_restart_reserving;
static bool s_therapy_safe_restart_committed;
static unsigned s_therapy_start_waiters;
static unsigned s_therapy_start_claims;
static unsigned s_as11_notifications_pending;
static bool s_therapy_safe_maintenance;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static SemaphoreHandle_t s_lvgl_lock;
static TaskHandle_t s_lvgl_task;
static uint8_t s_brightness = 100;
static bool s_backlight = true;
static bool s_backlight_requested = true;
static bool s_backlight_known = true;
static bool s_wake_gesture_pending;
static uint32_t s_touch_seen_visibility;
static uint32_t s_touch_seen_continuity;
static uint32_t s_backlight_revision;
static bool s_backlight_force_on;
static bool s_temporarily_awake;
static esp_timer_handle_t s_wake_timer;
static bool s_touch_was_pressed;
static int64_t s_last_touch_activity_us;
static uint32_t s_flush_count;
static uint32_t s_flush_timeouts;
static uint32_t s_touch_read_errors;
static uint8_t s_touch_consecutive_errors;
static uint32_t s_backlight_write_errors;
static int64_t s_backlight_retry_after_us;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
static bool s_qemu_first_frame_published;
#endif
static lv_coord_t s_last_touch_x;
static lv_coord_t s_last_touch_y;
static lv_obj_t *s_title_label, *s_status_label, *s_metrics_label, *s_notice_label;
static lv_obj_t *s_wake_overlay;
static void apply_pending_backlight_locked(void);
void bsp_display_restart_idle_timeout(void);
static void wake_timer_cb(void *arg);
static bool screen_wake_input_available(void)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    return true;
#else
    touch_observation_t touch;
    waveshare_7b_touch_snapshot(&touch);
    return touch.preventive_recovery ||
           touch_observation_healthy(&touch, esp_timer_get_time());
#endif
}
static bool lock_lvgl(TickType_t timeout)
{
    return s_lvgl_lock && xSemaphoreTakeRecursive(s_lvgl_lock, timeout) == pdTRUE;
}
static void unlock_lvgl(void)
{
    xSemaphoreGiveRecursive(s_lvgl_lock);
}
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
static bool IRAM_ATTR on_frame_complete(esp_lcd_panel_handle_t panel,
                              const esp_lcd_rgb_panel_event_data_t *event,
                              void *ctx)
{
    (void)panel;
    (void)event;
    (void)ctx;
    BaseType_t wake = pdFALSE;
    if (s_lvgl_task) vTaskNotifyGiveFromISR(s_lvgl_task, &wake);
    return wake == pdTRUE;
}
static void submit_rgb_frame(lv_disp_drv_t *drv, lv_color_t *pixels)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    for (;;) {
        esp_err_t result = esp_lcd_panel_draw_bitmap(
            panel, 0, 0, WAVESHARE_7B_H_RES, WAVESHARE_7B_V_RES, pixels);
        controller_diagnostics_record(CONTROLLER_PANEL_SUBMIT, result);
        if (result == ESP_OK) break;
        s_flush_timeouts++;
        ESP_LOGE(TAG, "RGB frame submission failed: %s", esp_err_to_name(result));
        /* LVGL must not swap back to the buffer still owned by scanout. Other
         * tasks, including the web recovery interface, remain runnable. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* IDF 5.5.1 selects cur_fb_index in draw_bitmap, then latches it into
     * bb_fb_index before on_frame_buf_complete. Drain AFTER selection: an EOF
     * between a pre-submit drain and draw_bitmap still belongs to the old
     * frame and cannot release it for LVGL's next render/sync copy. A boundary
     * just before this drain costs one extra scan, but never tears a frame. */
    ulTaskNotifyTake(pdTRUE, 0);
    while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0) {
        controller_diagnostics_record(CONTROLLER_PANEL_HANDOFF, ESP_ERR_TIMEOUT);
        s_flush_timeouts++;
        if (s_flush_timeouts == 1 || (s_flush_timeouts % 100) == 0)
            ESP_LOGW(TAG, "RGB handoff delayed (%lu); retaining framebuffer",
                     (unsigned long)s_flush_timeouts);
        /* Restart is deferred to VSYNC by IDF. A timeout itself is never
         * evidence that the old buffer is free; await the actual callback. */
        (void)esp_lcd_rgb_panel_restart(panel);
    }
    controller_diagnostics_record(CONTROLLER_PANEL_HANDOFF, ESP_OK);
}
#endif
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *pixels)
{
    (void)area;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    /* In double-buffered direct mode LVGL renders only dirty areas, but the
     * RGB peripheral still needs the address of the complete finished frame.
     * Submit that framebuffer once, after LVGL has drawn every dirty region. */
    if (!lv_disp_flush_is_last(drv)) {
        lv_disp_flush_ready(drv);
        return;
    }
    submit_rgb_frame(drv, pixels);
#else
    /* LVGL composes dirty regions into one persistent direct-mode buffer.
     * Espressif's virtual panel blocks once per submitted rectangle, so skip
     * intermediate dirty-area callbacks and publish the completed buffer in
     * one host-side copy. This keeps redraw work partial without multiplying
     * QEMU's display wait by the number of invalidated objects. */
    if (!lv_disp_flush_is_last(drv)) {
        lv_disp_flush_ready(drv);
        return;
    }
    esp_err_t submitted = esp_lcd_panel_draw_bitmap(
        (esp_lcd_panel_handle_t)drv->user_data,
        0, 0, WAVESHARE_7B_H_RES, WAVESHARE_7B_V_RES, pixels);
    controller_diagnostics_record(CONTROLLER_PANEL_SUBMIT, submitted);
    if (submitted != ESP_OK) {
        s_flush_timeouts++;
        ESP_LOGE(TAG, "RGB frame submission failed: %s",
                 esp_err_to_name(submitted));
        lv_disp_flush_ready(drv);
        return;
    }
#endif
    /* Hardware has positively retired the previous framebuffer at this point. */
    if (lv_disp_flush_is_last(drv)) s_flush_count++;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (lv_disp_flush_is_last(drv) && !s_qemu_first_frame_published) {
        s_qemu_first_frame_published = true;
        ESP_LOGI(TAG, "QEMU UI first frame published");
    }
#endif
    lv_disp_flush_ready(drv);
}
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    uint16_t x = 0;
    uint16_t y = 0;
    bool pressed = false;
    board_qemu_touch_read(&x, &y, &pressed);
    controller_diagnostics_record(CONTROLLER_TOUCH_READ, ESP_OK);
    s_last_touch_x = x < WAVESHARE_7B_H_RES ? x : WAVESHARE_7B_H_RES - 1;
    s_last_touch_y = y < WAVESHARE_7B_V_RES ? y : WAVESHARE_7B_V_RES - 1;
    data->point.x = s_last_touch_x;
    data->point.y = s_last_touch_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed && !s_touch_was_pressed) {
        ESP_LOGI(TAG, "emulated touch at %u,%u", (unsigned)x, (unsigned)y);
    }
    (void)drv;
#else
    (void)drv;
    touch_observation_t touch;
    waveshare_7b_touch_snapshot(&touch);
    int64_t now = esp_timer_get_time();
    bool healthy = touch_observation_healthy(&touch, now);
    bool continuity_changed = touch.continuity != s_touch_seen_continuity;
    bool touch_became_unavailable = false;
    portENTER_CRITICAL(&s_state_lock);
    touch_became_unavailable = s_touch_consecutive_errors < TOUCH_FAILURE_THRESHOLD &&
                               touch.consecutive_errors >= TOUCH_FAILURE_THRESHOLD;
    s_touch_read_errors = touch.errors;
    s_touch_consecutive_errors = touch.consecutive_errors;
    if (continuity_changed) {
        s_touch_seen_continuity = touch.continuity;
        /* A fresh down frame cannot bridge a missed release. Disarm the
         * whole gesture even when the next frame arrived before LVGL ran. */
        s_wake_gesture_pending = true;
    }
    if (touch.visibility_requests != s_touch_seen_visibility) {
        s_touch_seen_visibility = touch.visibility_requests;
        s_backlight_requested = true;
        s_backlight_known = false;
        s_wake_gesture_pending = true;
        s_last_touch_activity_us = now;
        ++s_backlight_revision;
    }
    bool input_lost = continuity_changed || !healthy || touch.last_error ||
                      !touch.valid ||
                      (touch.pressed && !touch_observation_pressed(&touch, now));
    bool cancel_gesture = input_lost || s_wake_gesture_pending ||
                          !s_backlight_requested;
    if (cancel_gesture) s_wake_gesture_pending = true;
    if (healthy && touch.valid && !touch.pressed)
        s_wake_gesture_pending = false;
    /* Physical wake is admitted by the board's ordered visibility demand. */
    bool allow_press = s_backlight_requested && !s_wake_gesture_pending;
    portEXIT_CRITICAL(&s_state_lock);
    /* An ordinary RELEASED sample generates CLICKED in LVGL. Cancel the
     * active gesture first; never call LVGL inside the state critical section. */
    if (cancel_gesture && s_touch_was_pressed)
        lv_indev_wait_release(lv_indev_get_act());
    if (touch_observation_pressed(&touch, now) && allow_press) {
        s_last_touch_x = touch.x < WAVESHARE_7B_H_RES ? touch.x
                                                       : WAVESHARE_7B_H_RES - 1;
        s_last_touch_y = touch.y < WAVESHARE_7B_V_RES ? touch.y
                                                       : WAVESHARE_7B_V_RES - 1;
        data->point.x = s_last_touch_x;
        data->point.y = s_last_touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->point.x = s_last_touch_x;
        data->point.y = s_last_touch_y;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    if (touch_became_unavailable)
        bsp_display_set_notice("Touch unavailable - recovering controls");
#endif
    bool pressed_now = data->state == LV_INDEV_STATE_PRESSED;
    if (pressed_now) {
        int64_t now_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_state_lock);
        s_last_touch_activity_us = now_us;
        portEXIT_CRITICAL(&s_state_lock);
    }
    s_touch_was_pressed = pressed_now;
}
static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}
static void wake_overlay_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        /* This object is above every control while dark, so the wake gesture
         * cannot also activate the button that happens to be underneath it. */
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) lv_indev_wait_release(indev);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
        bsp_display_restart_idle_timeout();
        bsp_display_set_backlight(true);
#endif
        /* Hardware wake comes from the board worker. A delayed overlay event
         * must not create another ON demand after a newer OFF request. */
    }
}
static void screen_off_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && screen_wake_input_available())
        bsp_display_set_backlight(false);
}

static void build_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    s_title_label = lv_label_create(screen);
    lv_obj_set_pos(s_title_label, 40, 40);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_28, 0);
    s_status_label = lv_label_create(screen);
    lv_obj_set_pos(s_status_label, 40, 110);
    lv_obj_set_width(s_status_label, 930);
    s_metrics_label = lv_label_create(screen);
    lv_obj_set_pos(s_metrics_label, 40, 260);
    lv_obj_set_width(s_metrics_label, 930);
    s_notice_label = lv_label_create(screen);
    lv_obj_set_pos(s_notice_label, 40, 440);
    lv_obj_set_width(s_notice_label, 930);
    lv_obj_t *off = lv_btn_create(screen);
    lv_obj_set_pos(off, 800, 520);
    lv_obj_set_size(off, 180, 50);
    lv_obj_t *label = lv_label_create(off);
    lv_label_set_text(label, "Screen off");
    lv_obj_center(label);
    lv_obj_add_event_cb(off, screen_off_cb, LV_EVENT_CLICKED, NULL);
    s_wake_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_wake_overlay, 0, 0);
    lv_obj_set_size(s_wake_overlay, WAVESHARE_7B_H_RES, WAVESHARE_7B_V_RES);
    lv_obj_set_style_bg_color(s_wake_overlay, lv_color_black(), 0);
    /* Hardware sleep electrically disables the backlight, so painting black
     * there would only force two needless full-screen redraws. QEMU has no
     * physical lamp to disable; make the same state visibly black so emulator
     * acceptance can observe the command as well as its wake-only shield. */
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    lv_obj_set_style_bg_opa(s_wake_overlay, LV_OPA_COVER, 0);
#else
    lv_obj_set_style_bg_opa(s_wake_overlay, LV_OPA_TRANSP, 0);
#endif
    lv_obj_set_style_border_width(s_wake_overlay, 0, 0);
    lv_obj_set_style_radius(s_wake_overlay, 0, 0);
    lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_wake_overlay, wake_overlay_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);

}

static void update_ui(void)
{
    ui_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    int64_t now = esp_timer_get_time();
    if (state.notice_critical || !screen_wake_input_available()) {
        portENTER_CRITICAL(&s_state_lock);
        bool wake_needed = !s_backlight_requested;
        portEXIT_CRITICAL(&s_state_lock);
        if (wake_needed) bsp_display_set_backlight(true);
    }
    char battery[24] = "unavailable";
    if (state.battery >= 0) snprintf(battery, sizeof(battery), "%d%%", state.battery);
    lv_label_set_text(s_title_label, state.title);
    lv_label_set_text_fmt(s_status_label, "%s\n\nWi-Fi: %s    AirSense: %s    SD: %s    Battery: %s\n%s",
        state.attention, state.wifi ? "connected" : "disconnected",
        state.paired ? "paired" : "unpaired", state.sd_ready ? "ready" : "unavailable",
        battery, state.therapy ? "Therapy active" : "Waiting for therapy");
    char flow[24] = "--", pressure[24] = "--", leak[24] = "--";
    if (state.flow_count && now - state.flow_sample_us < 8000000) {
        int16_t value = state.flow[(state.flow_head + FLOW_POINTS - 1) % FLOW_POINTS];
        if (value != LIVE_FLOW_MISSING) snprintf(flow, sizeof(flow), "%.1f", value / 10.0f);
    }
    if (isfinite(state.pressure) && now - state.pressure_sample_us < 8000000)
        snprintf(pressure, sizeof(pressure), "%.1f", state.pressure);
    if (isfinite(state.leak) && now - state.leak_sample_us < 8000000)
        snprintf(leak, sizeof(leak), "%.1f", state.leak);
    lv_label_set_text_fmt(s_metrics_label, "Flow: %s L/min    Pressure: %s cmH2O    Leak: %s L/min\n\n%s",
                          flow, pressure, leak, UI_TOUCH_STATUS);
    lv_label_set_text(s_notice_label, state.notice_critical || now < state.notice_expires_us ? state.notice : "");
    lv_obj_set_style_text_color(s_notice_label, lv_color_hex(state.notice_critical ? 0xff7070 : 0xffd080), 0);
}
static void lvgl_task(void *arg)
{
    (void)arg;
    s_lvgl_task = xTaskGetCurrentTaskHandle();
    TickType_t last_update = 0;
    while (true) {
        if (lock_lvgl(portMAX_DELAY)) {
            apply_pending_backlight_locked();
            TickType_t now = xTaskGetTickCount();
            if (now - last_update >= pdMS_TO_TICKS(UI_UPDATE_MS)) {
                update_ui();
                last_update = now;
            }
            uint32_t delay = lv_timer_handler();
            unlock_lvgl();
            if (delay < 5) delay = 5;
            if (delay > 50) delay = 50;
            vTaskDelay(pdMS_TO_TICKS(delay));
        }
    }
}
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;
} panel_init_context_t;

static void panel_init_task(void *arg)
{
    panel_init_context_t *ctx = arg;
    ESP_LOGI(TAG, "allocating RGB panel on core %d beside LVGL",
             xPortGetCoreID());
    ctx->result = waveshare_7b_init(&ctx->panel, &ctx->touch);

    /* The caller owns ctx and this task. Do not access ctx after signalling;
     * suspending lets the caller delete us and reclaim the temporary internal
     * stack synchronously before the rest of boot consumes that heap. */
    xSemaphoreGive(ctx->done);
    vTaskSuspend(NULL);
}

static esp_err_t init_panel_on_render_core(esp_lcd_panel_handle_t *panel,
                                           esp_lcd_touch_handle_t *touch)
{
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&done_storage);
    ESP_RETURN_ON_FALSE(done, ESP_ERR_NO_MEM, TAG,
                        "create panel-init completion signal");

    panel_init_context_t ctx = {
        .done = done,
        .result = ESP_FAIL,
    };
    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        panel_init_task, "panel_init", 8192, &ctx, 5, &task, 1);
    if (created != pdPASS || !task) {
        /* IRQ/render colocation is part of framebuffer ownership, not an
         * optional optimization. Never fall back to a different IRQ core. */
        ESP_LOGE(TAG, "panel-init task unavailable on render core");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "panel initialization timed out");
        vTaskDelete(task);
        return ESP_ERR_TIMEOUT;
    }

    /* panel_init_task no longer touches ctx after giving the semaphore. */
    vTaskDelete(task);
    if (ctx.result == ESP_OK) {
        *panel = ctx.panel;
        *touch = ctx.touch;
    }
    return ctx.result;
}
#endif

esp_err_t bsp_display_init(void)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    controller_diagnostics_init(true);
#else
    controller_diagnostics_init(false);
#endif
    memset(&s_state, 0, sizeof(s_state));
    strcpy(s_state.title, "SomnoTrace");
    strcpy(s_state.status, "Initializing display...");
    s_state.battery = -1;
    s_state.leak = NAN;
    s_state.pressure = NAN;
    s_state.respiratory_rate = NAN;
    s_state.flow_limitation = NAN;
    portENTER_CRITICAL(&s_state_lock);
    s_touch_was_pressed = false;
    s_backlight_force_on = false;
    s_temporarily_awake = false;
    s_backlight_known = true;
    s_wake_gesture_pending = false;
    s_touch_seen_visibility = 0;
    s_touch_seen_continuity = 0;
    s_backlight_revision = 0;
    s_last_touch_activity_us = esp_timer_get_time();
    s_touch_consecutive_errors = 0;
    s_backlight_write_errors = 0;
    s_backlight_retry_after_us = 0;
    portEXIT_CRITICAL(&s_state_lock);

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    esp_err_t display_init_result = waveshare_7b_init(&s_panel, &s_touch);
    if (display_init_result == ESP_OK)
        controller_diagnostics_record(CONTROLLER_TOUCH_INIT, ESP_OK);
#else
    /* ESP-IDF installs the RGB DMA EOF interrupt on the core which allocates
     * the panel. Keep that PSRAM-to-bounce-buffer copy on core 1 beside LVGL,
     * so it preempts framebuffer rendering instead of racing it from core 0. */
    esp_err_t display_init_result = init_panel_on_render_core(&s_panel, &s_touch);
#endif
    controller_diagnostics_record(CONTROLLER_PANEL_INIT, display_init_result);
    ESP_RETURN_ON_ERROR(display_init_result, TAG, "initialize display on render core");
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    esp_err_t touch_start = waveshare_7b_start_touch();
    if (touch_start != ESP_OK)
        ESP_LOGE(TAG, "touch observation worker unavailable: %s", esp_err_to_name(touch_start));
#endif

    /* With an RGB bounce buffer, frame-buffer handoff completion is reported
     * by on_frame_buf_complete. Waiting for it prevents LVGL from drawing into
     * a buffer that the panel is still scanning out. */
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_frame_buf_complete = on_frame_complete,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel,
                                                                  &callbacks, NULL),
                        TAG, "register display VSYNC");
#endif

    lv_init();
    void *fb1 = NULL, *fb2 = NULL;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    /* The virtual device reserves four bytes per pixel even in RGB565 mode.
     * Keep its first half free as the conventional panel framebuffer and use
     * the second half as LVGL's persistent full-frame composition buffer. */
    void *qemu_vram = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_qemu_get_frame_buffer(s_panel, &qemu_vram),
                        TAG, "get QEMU framebuffer");
    fb1 = (lv_color_t *)qemu_vram +
          WAVESHARE_7B_H_RES * WAVESHARE_7B_V_RES;
#else
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb1, &fb2),
                        TAG, "get RGB framebuffers");
    /* IDF starts scanout from its first buffer. LVGL's first render must use
     * the second one, before the normal submit/retire alternation begins. */
    void *boot_scanout = fb1;
    fb1 = fb2;
    fb2 = boot_scanout;
#endif
    static lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer, fb1, fb2,
                          WAVESHARE_7B_H_RES * WAVESHARE_7B_V_RES);
    static lv_disp_drv_t display_driver;
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = WAVESHARE_7B_H_RES;
    display_driver.ver_res = WAVESHARE_7B_V_RES;
    display_driver.flush_cb = flush_cb;
    display_driver.draw_buf = &draw_buffer;
    display_driver.user_data = s_panel;
    /* Both targets retain a complete composition buffer while LVGL redraws
     * only invalidated regions. Hardware swaps two RGB buffers at its
     * frame-complete boundary; QEMU publishes its single buffer on the final
     * dirty-area callback. */
    display_driver.direct_mode = 1;
    lv_disp_drv_register(&display_driver);

    /* Register even after a failed boot probe: the retained worker can repair
     * the controller later, while invalid snapshots keep input released. */
    const bool input_available = true;
    if (input_available) {
        static lv_indev_drv_t touch_driver;
        lv_indev_drv_init(&touch_driver);
        touch_driver.type = LV_INDEV_TYPE_POINTER;
        touch_driver.read_cb = touch_read_cb;
        touch_driver.user_data = s_touch;
        lv_indev_drv_register(&touch_driver);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
        ESP_LOGI(TAG, "QEMU pointer registered as LVGL touch input");
#endif
    }

    s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_lock, ESP_ERR_NO_MEM, TAG, "create LVGL mutex");
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    UBaseType_t build_stack_before = uxTaskGetStackHighWaterMark(NULL);
    if (lock_lvgl(portMAX_DELAY)) {
        build_ui();
        unlock_lvgl();
    }
    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    UBaseType_t build_stack_after = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "UI tree: internal %d bytes, PSRAM %d bytes; init stack %u -> %u free",
             (int)(internal_before - internal_after),
             (int)(psram_before - psram_after),
             (unsigned)build_stack_before, (unsigned)build_stack_after);

    esp_timer_handle_t tick_timer;
    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG,
                        "create LVGL tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 5000), TAG,
                        "start LVGL tick timer");
    const esp_timer_create_args_t wake_timer_args = {
        .callback = wake_timer_cb,
        .name = "lcd_wake_tmr",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&wake_timer_args, &s_wake_timer), TAG,
                        "create temporary-wake timer");
    /* Service snapshots live in PSRAM and are refreshed before LVGL reads them,
     * keeping the display-task stack independent of History depth. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(lvgl_task, "display_7b", 12288,
                                               NULL, 5, &s_lvgl_task, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "create display task");


    /* Start at the exact steady/full-on endpoint. Persisted hardware PWM
     * dimming is applied by main once NVS is available. */
    waveshare_7b_set_brightness(100);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    ESP_LOGI(TAG, "native 1024x600 QEMU dashboard initialized");
#else
    ESP_LOGI(TAG, "native 1024x600 touch dashboard initialized");
#endif
    return ESP_OK;
}
void bsp_display_show_number(uint32_t value)
{
    char line[16];
    snprintf(line, sizeof(line), "%lu", (unsigned long)value);
    const char *lines[] = { line };
    bsp_display_show_lines(NULL, lines, 1);
}

void bsp_display_show_lines(const char *title, const char *const *lines, int n_lines)
{
    portENTER_CRITICAL(&s_state_lock);
    if (title && title[0]) snprintf(s_state.title, sizeof(s_state.title), "%s", title);
    s_state.status[0] = '\0';
    s_state.attention[0] = '\0';
    for (int i = 0; lines && i < n_lines; ++i) {
        size_t used = strlen(s_state.status);
        if (used && used + 3 < sizeof(s_state.status)) strcat(s_state.status, "  |  ");
        used = strlen(s_state.status);
        if (used < sizeof(s_state.status) - 1) {
            snprintf(s_state.status + used, sizeof(s_state.status) - used, "%s", lines[i]);
        }
        used = strlen(s_state.attention);
        if (used && used + 1 < sizeof(s_state.attention)) strcat(s_state.attention, "\n");
        used = strlen(s_state.attention);
        if (used < sizeof(s_state.attention) - 1) {
            snprintf(s_state.attention + used,
                     sizeof(s_state.attention) - used, "%s", lines[i]);
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_notice(const char *text)
{
    portENTER_CRITICAL(&s_state_lock);
    if (text && strstr(text, "microSD nearly full"))
        s_state.storage_near_full = true;
    if (!text || !text[0]) {
        s_state.notice[0] = '\0';
        s_state.notice_expires_us = 0;
        s_state.notice_critical = false;
    } else if (!s_state.notice_critical) {
        snprintf(s_state.notice, sizeof(s_state.notice), "%s", text);
        s_state.notice_expires_us = esp_timer_get_time() + 3000000;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_critical_notice(const char *text)
{
    portENTER_CRITICAL(&s_state_lock);
    snprintf(s_state.notice, sizeof(s_state.notice), "%s", text ? text : "");
    s_state.notice_expires_us = 0;
    s_state.notice_critical = text && text[0];
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_wifi_connected(bool connected)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.wifi = connected;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_as11_paired(bool paired)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.paired = paired;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_sd_ready(bool ready)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.sd_ready = ready;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_battery(int percent, bool charging, bool valid)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.battery = valid ? percent : -1;
    s_state.charging = charging;
    portEXIT_CRITICAL(&s_state_lock);
}

bool bsp_display_set_therapy_active(bool active)
{
    bool waiting_for_restart = false;
    for (;;) {
        portENTER_CRITICAL(&s_state_lock);
        if (!active || !s_therapy_safe_restart_reserving) break;
        if (!waiting_for_restart) {
            s_therapy_start_waiters++;
            waiting_for_restart = true;
        }
        portEXIT_CRITICAL(&s_state_lock);
        vTaskDelay(1);
    }
    if (waiting_for_restart) s_therapy_start_waiters--;
    if (active && s_therapy_safe_restart_committed) {
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG, "therapy start refused: restart already committed");
        return false;
    }
    bool changed = s_state.therapy != active;
    s_state.therapy = active;
    if (changed) {
        s_state.leak = NAN;
        s_state.pressure = NAN;
        s_state.respiratory_rate = NAN;
        s_state.flow_limitation = NAN;
        s_state.leak_sample_us = 0;
        s_state.pressure_sample_us = 0;
        s_state.respiratory_rate_sample_us = 0;
        s_state.flow_limitation_sample_us = 0;
        s_state.flow_head = 0;
        s_state.flow_count = 0;
        s_state.flow_version++;
        s_state.flow_sample_us = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (changed) bsp_display_restart_idle_timeout();
    bsp_display_apply_backlight_policy(false);
    return true;
}

bool bsp_display_try_reserve_therapy_safe_restart(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool reserved = !s_state.therapy && s_therapy_start_claims == 0 &&
                    s_therapy_start_waiters == 0 &&
                    s_as11_notifications_pending == 0 &&
                    !s_therapy_safe_maintenance &&
                    !s_therapy_safe_restart_reserving &&
                    !s_therapy_safe_restart_committed;
    if (reserved) s_therapy_safe_restart_reserving = true;
    portEXIT_CRITICAL(&s_state_lock);
    return reserved;
}

bool bsp_display_try_commit_therapy_safe_restart(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool committed = s_therapy_safe_restart_reserving && !s_state.therapy &&
                     s_therapy_start_waiters == 0 &&
                     s_as11_notifications_pending == 0;
    if (committed) {
        s_therapy_safe_restart_reserving = false;
        s_therapy_safe_restart_committed = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return committed;
}

void bsp_display_cancel_therapy_safe_restart(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_therapy_safe_restart_committed) {
        s_therapy_safe_restart_reserving = false;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

bool bsp_display_reserve_therapy_start(void)
{
    bool waiting_for_restart = false;
    for (;;) {
        portENTER_CRITICAL(&s_state_lock);
        if (!s_therapy_safe_restart_reserving) break;
        if (!waiting_for_restart) {
            s_therapy_start_waiters++;
            waiting_for_restart = true;
        }
        portEXIT_CRITICAL(&s_state_lock);
        vTaskDelay(1);
    }
    if (waiting_for_restart) s_therapy_start_waiters--;
    bool reserved = !s_therapy_safe_restart_committed;
    if (reserved) s_therapy_start_claims++;
    portEXIT_CRITICAL(&s_state_lock);
    return reserved;
}

void bsp_display_release_therapy_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_therapy_start_claims > 0) s_therapy_start_claims--;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_note_as11_notification_queued(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_as11_notifications_pending++;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_note_as11_notification_processed(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_as11_notifications_pending > 0) s_as11_notifications_pending--;
    portEXIT_CRITICAL(&s_state_lock);
}

bool bsp_display_try_begin_therapy_safe_maintenance(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool begun = !s_state.therapy && s_therapy_start_claims == 0 &&
                 s_therapy_start_waiters == 0 &&
                 s_as11_notifications_pending == 0 &&
                 !s_therapy_safe_maintenance &&
                 !s_therapy_safe_restart_reserving &&
                 !s_therapy_safe_restart_committed;
    if (begun) s_therapy_safe_maintenance = true;
    portEXIT_CRITICAL(&s_state_lock);
    return begun;
}

/* Convert an active OTA maintenance gate into a short commit reservation.
 * This closes the check/boot-selection race under the therapy publication lock.
 * The updater releases this reservation immediately after SDK finish/set-boot,
 * allowing queued starts to publish before the ordinary deferred reboot loop. */
bool bsp_display_try_reserve_maintenance_commit(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool reserved = s_therapy_safe_maintenance && !(s_state.therapy) &&
        s_therapy_start_claims == 0 && s_therapy_start_waiters == 0 &&
        s_as11_notifications_pending == 0 && !s_therapy_safe_restart_reserving &&
        !s_therapy_safe_restart_committed;
    if (reserved) {
        s_therapy_safe_maintenance = false;
        s_therapy_safe_restart_reserving = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return reserved;
}

bool bsp_display_therapy_safe_maintenance_should_abort(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool abort = !s_therapy_safe_maintenance || s_state.therapy ||
                 s_therapy_start_claims > 0 || s_therapy_start_waiters > 0;
    portEXIT_CRITICAL(&s_state_lock);
    return abort;
}

void bsp_display_end_therapy_safe_maintenance(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_therapy_safe_maintenance = false;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_push_flow(float flow_lpm)
{
    if (!isfinite(flow_lpm)) {
        bsp_display_push_flow_gap(1);
        return;
    }
    int value = (int)lrintf(flow_lpm * 10.0f);
    if (value > 1000) value = 1000;
    if (value < -1000) value = -1000;
    portENTER_CRITICAL(&s_state_lock);
    s_state.flow[s_state.flow_head] = (int16_t)value;
    s_state.flow_head = (s_state.flow_head + 1) % FLOW_POINTS;
    if (s_state.flow_count < FLOW_POINTS) s_state.flow_count++;
    s_state.flow_version++;
    s_state.flow_sample_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_push_flow_gap(uint32_t samples)
{
    if (samples > FLOW_POINTS) samples = FLOW_POINTS;
    portENTER_CRITICAL(&s_state_lock);
    for (uint32_t i = 0; i < samples; ++i) {
        s_state.flow[s_state.flow_head] = LIVE_FLOW_MISSING;
        s_state.flow_head = (s_state.flow_head + 1) % FLOW_POINTS;
        if (s_state.flow_count < FLOW_POINTS) ++s_state.flow_count;
        ++s_state.flow_version;
    }
    /* A gap is not a new valid Flow measurement; keep the freshness clock. */
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_push_leak(float leak_lpm)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.leak = leak_lpm;
    s_state.leak_sample_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_push_metrics(float pressure_cmh2o, float respiratory_rate,
                              float flow_limitation)
{
    int64_t sample_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_state.pressure = pressure_cmh2o;
    s_state.respiratory_rate = respiratory_rate;
    s_state.flow_limitation = flow_limitation;
    s_state.pressure_sample_us = sample_us;
    s_state.respiratory_rate_sample_us = sample_us;
    s_state.flow_limitation_sample_us = sample_us;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_therapy_start_time(int64_t start_us)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.therapy_start_us = start_us;
    portEXIT_CRITICAL(&s_state_lock);
}

bool bsp_display_is_therapy_active(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool active = s_state.therapy;
    portEXIT_CRITICAL(&s_state_lock);
    return active;
}

static uint8_t physical_brightness(uint8_t tenth_percent)
{
    uint8_t physical_percent = (uint8_t)(((uint16_t)tenth_percent + 1U) / 2U);
    return physical_percent < 1 ? 1 : physical_percent;
}

void bsp_display_set_brightness(uint8_t tenth_percent)
{
    if (tenth_percent < 1) tenth_percent = 1;
    if (tenth_percent > 200) tenth_percent = 200;
    portENTER_CRITICAL(&s_state_lock);
    s_brightness = tenth_percent;
    bool backlight = s_backlight;
    portEXIT_CRITICAL(&s_state_lock);
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    waveshare_7b_recovery_brightness(physical_brightness(tenth_percent));
#endif
    if (backlight) {
        /* The original 1.54-inch target stores 1..200 as tenths of a percent.
         * On the 7B that same byte spans 1..100% hardware brightness. The
         * board driver inverts this logical value for the active-low PWM;
         * exactly 100% is a steady level with no PWM interruption. */
        esp_err_t result = waveshare_7b_set_brightness(physical_brightness(tenth_percent));
        if (result != ESP_OK) {
            portENTER_CRITICAL(&s_state_lock);
            s_backlight_known = false;
            ++s_backlight_revision;
            portEXIT_CRITICAL(&s_state_lock);
        }
    }
}

void bsp_display_set_backlight(bool on)
{
    portENTER_CRITICAL(&s_state_lock);
    s_backlight_requested = on;
    ++s_backlight_revision;
    /* An explicit ON request is a physical reassertion, even after peripheral
     * state loss left the application's last successful value unchanged. */
    if (on) s_backlight_known = false;
    portEXIT_CRITICAL(&s_state_lock);
}

bool bsp_display_toggle_backlight(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool on = s_backlight_force_on || !s_backlight_requested;
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_set_backlight(on);
    return on;
}

/* Called only while the LVGL mutex is held. Keeping this as a sticky desired
 * state means a one-shot safety policy change cannot be lost to lock pressure. */
static void apply_pending_backlight_locked(void)
{
    int64_t now_us = esp_timer_get_time();
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    touch_observation_t touch;
    waveshare_7b_touch_snapshot(&touch);
#endif
    portENTER_CRITICAL(&s_state_lock);
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    if (touch.visibility_requests != s_touch_seen_visibility) {
        s_touch_seen_visibility = touch.visibility_requests;
        s_backlight_requested = true;
        s_backlight_known = false;
        s_wake_gesture_pending = true;
        s_last_touch_activity_us = now_us;
        ++s_backlight_revision;
    }
#endif
    bool requested = s_backlight_requested;
    bool current = s_backlight;
    bool known = s_backlight_known;
    uint32_t revision = s_backlight_revision;
    uint8_t brightness = s_brightness;
    int64_t retry_after_us = s_backlight_retry_after_us;
    portEXIT_CRITICAL(&s_state_lock);

    /* Keep the wake surface synchronized even after a failed or superseded
     * hardware request. */
    if (requested == current && known) {
        if (s_wake_overlay) {
            if (current) lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
            else {
                lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_wake_overlay);
            }
        }
        return;
    }
    if (now_us < retry_after_us) return;

    /* Install the input shield before switching off. On wake, retain it until
     * the I/O controller confirms that the backlight is physically enabled. */
    if (!requested && s_wake_overlay) {
        lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wake_overlay);
    }

    esp_err_t brightness_result = ESP_OK;
    if (requested) {
        brightness_result =
            waveshare_7b_set_brightness(physical_brightness(brightness));
    }
    esp_err_t backlight_result;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    backlight_result = waveshare_7b_set_backlight(requested);
#else
    backlight_result = requested ? waveshare_7b_reassert_visible()
                                 : waveshare_7b_set_backlight(false);
#endif
    if (backlight_result == ESP_OK && brightness_result != ESP_OK)
        backlight_result = brightness_result;
    if (backlight_result != ESP_OK) {
        uint32_t failure_count;
        portENTER_CRITICAL(&s_state_lock);
        s_backlight_write_errors++;
        s_backlight_known = false;
        failure_count = s_backlight_write_errors;
        if (requested) {
            s_backlight_retry_after_us = now_us + BACKLIGHT_RETRY_US;
        } else {
            /* Sleep is optional; abandon a failed off request and restart its
             * idle window. A failed wake is safety-critical and keeps retrying. */
            if (!s_backlight_requested) s_backlight_requested = true;
            s_last_touch_activity_us = now_us;
            s_backlight_retry_after_us = 0;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (!requested && s_wake_overlay)
            lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
        if (failure_count == 1 || (failure_count % 10U) == 0) {
            ESP_LOGE(TAG, "backlight %s failed (%lu): %s",
                     requested ? "wake" : "sleep",
                     (unsigned long)failure_count,
                     esp_err_to_name(backlight_result));
        }
        if (!requested)
            bsp_display_set_notice("Could not turn screen off - kept on");
        return;
    }

    if (s_wake_overlay) {
        if (requested) {
            lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_wake_overlay);
        }
    }
    ESP_LOGI(TAG, "backlight %s", requested ? "on" : "off");

    portENTER_CRITICAL(&s_state_lock);
    s_backlight = requested;
    s_backlight_known = s_backlight_revision == revision;
    s_backlight_retry_after_us = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

uint8_t bsp_display_get_brightness(void)
{
    portENTER_CRITICAL(&s_state_lock);
    uint8_t brightness = s_brightness;
    portEXIT_CRITICAL(&s_state_lock);
    return brightness;
}

void bsp_display_restart_idle_timeout(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_last_touch_activity_us = now_us;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_apply_backlight_policy(bool force_on)
{
    if (force_on) {
        bsp_display_restart_idle_timeout();
        portENTER_CRITICAL(&s_state_lock);
        s_backlight_force_on = true;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_backlight(true);
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    bool forced = s_backlight_force_on;
    bool temporarily_awake = s_temporarily_awake;
    portEXIT_CRITICAL(&s_state_lock);
    if (forced || temporarily_awake) {
        bsp_display_set_backlight(true);
        return;
    }
    if (!screen_wake_input_available()) {
        /* The 7B has no alternate input control. Fail visibly if GT911 is not
         * available instead of honoring a policy the user cannot wake from. */
        bsp_display_set_backlight(true);
        return;
    }
    device_settings_t settings;
    device_settings_snapshot(&settings);
    bool therapy = bsp_display_is_therapy_active();
    bool off = settings.backlight_mode == BACKLIGHT_MODE_ALWAYS_OFF ||
               (therapy && settings.backlight_mode == BACKLIGHT_MODE_OFF_THRP);
    bsp_display_set_backlight(!off);
}

static void wake_timer_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_state_lock);
    s_temporarily_awake = false;
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_apply_backlight_policy(false);
}

void bsp_display_wake_temporary(uint32_t duration_sec)
{
    if (duration_sec == 0 || !s_wake_timer) return;

    (void)esp_timer_stop(s_wake_timer);
    portENTER_CRITICAL(&s_state_lock);
    s_temporarily_awake = true;
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_restart_idle_timeout();
    bsp_display_set_backlight(true);
    esp_err_t result = esp_timer_start_once(
        s_wake_timer, (uint64_t)duration_sec * 1000000ULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "temporary-wake timer failed: %s", esp_err_to_name(result));
        portENTER_CRITICAL(&s_state_lock);
        s_temporarily_awake = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_apply_backlight_policy(false);
    }
}

bool bsp_display_is_temporarily_awake(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool awake = s_temporarily_awake;
    portEXIT_CRITICAL(&s_state_lock);
    return awake;
}

void bsp_display_cancel_temporary_wake(void)
{
    if (s_wake_timer) (void)esp_timer_stop(s_wake_timer);
    portENTER_CRITICAL(&s_state_lock);
    s_temporarily_awake = false;
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_apply_backlight_policy(false);
}

void bsp_display_set_rotation(uint16_t degrees)
{
    if (degrees != 0) {
        ESP_LOGW(TAG, "rotation %u ignored: the 7B dashboard is landscape-native",
                 (unsigned)degrees);
    }
}

void bsp_display_qemu_seed_demo(void)
{
}

void bsp_display_qemu_set_tab(uint8_t tab)
{
    (void)tab;
}

esp_err_t bsp_display_qemu_start_setup_preview(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
