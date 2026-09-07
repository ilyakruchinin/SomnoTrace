/*
 * SomnoTrace native 1024x600 touch UI for Waveshare ESP32-S3-Touch-LCD-7B.
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bsp_display.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "as11_ble.h"
#include "board_waveshare_7b.h"
#include "device_settings.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#if CONFIG_SOMNOTRACE_BOARD_QEMU
#include "board_qemu.h"
#include "esp_lcd_qemu_rgb.h"
#else
#include "esp_lcd_panel_rgb.h"
#endif
#include "esp_lcd_touch.h"
#include "controller_diagnostics.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "first_run_setup.h"
#include "first_run_setup_controller.h"
#include "first_run_setup_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "log_stream.h"
#include "live_flow_plot.h"
#include "net_provision.h"
#include "oximeter.h"
#include "psram_task.h"
#include "sd_storage.h"
#include "somnotrace_fonts.h"
#include "therapy_alert.h"
#include "touch_history_controller.h"
#include "touch_history_ui.h"
#include "touch_logs_controller.h"
#include "uploader.h"

#define FLOW_POINTS 300
#define FLOW_READY_POINTS 12
#define FLOW_CATCHUP_THRESHOLD 10
#define FLOW_RESYNC_THRESHOLD 25
#define UI_UPDATE_MS 50
#define DEVICE_RESULT_MAX 8
#define SCREEN_TIMEOUT_OPTION_COUNT 6
#define POLICY_PEEK_TIMEOUT_S 60
#define TOUCH_FAILURE_THRESHOLD 3
#define BACKLIGHT_RETRY_US 250000
#define MANAGE_SECTION_COUNT 8
#define UI_ACTION_SCREEN_OFF 4

typedef enum {
    MANAGE_DEVICES = 0,
    MANAGE_CONNECTIVITY,
    MANAGE_ALERTS,
    MANAGE_UPLOADS,
    MANAGE_STORAGE,
    MANAGE_SYSTEM,
    MANAGE_LOGS,
    MANAGE_ADVANCED,
} manage_section_t;

#define UI_HEADER_H 64
#define UI_CONTENT_Y 64
#define UI_CONTENT_H 462
#define UI_NAV_H 74
#define UI_PANEL_X 16
#define UI_PANEL_Y 4
#define UI_PANEL_H 450
#define UI_NAV_PILL_X 254
#define UI_NAV_PILL_STEP 175
#define UI_NAV_PILL_Y 8
#define UI_NAV_PILL_W 166
#define UI_NAV_PILL_H 54
#define UI_HEADER_SCREEN_OFF_X 500
#define UI_HEADER_SCREEN_OFF_W 132
#define UI_MANAGE_RAIL_W 212
#define UI_MANAGE_DETAIL_X 240
#define UI_MANAGE_DETAIL_W 768
#define UI_MANAGE_SCROLL_W 740
#define UI_MANAGE_SCROLL_H 360
#define UI_MANAGE_ROW_W 726
#define UI_MANAGE_ROW_FULL_W 740

#define STATUS_CAPSULE_RIGHT 1006
#define STATUS_CAPSULE_H 56
#define STATUS_CAPSULE_DOT_SIZE 9
#define STATUS_CAPSULE_LEFT_PAD 18
#define STATUS_CAPSULE_DOT_LABEL_GAP 9
#define STATUS_CAPSULE_ITEM_GAP 18
#define STATUS_CAPSULE_DIVIDER_GAP 14
#define STATUS_CAPSULE_CHEVRON_GAP 14
#define STATUS_CAPSULE_RIGHT_PAD 18

#define COLOR_BASE       0x05070e
#define COLOR_PANEL      0x181c29
#define COLOR_CARD       0x101421
#define COLOR_ROW        0x101421
#define COLOR_CAPSULE    0x1a1f2b
#define COLOR_CONTROL    0x2d333f
#define COLOR_INVERSE    0xe0ebe8
#define COLOR_TEXT       0xf0f2f6
#define COLOR_SECONDARY  0xa0a5af
#define COLOR_TERTIARY   0x818691
#define COLOR_DISABLED   0x5e636e
#define COLOR_LIVE       0x00e1e2
#define COLOR_AMBER      0xf8bd40
#define COLOR_FAULT      0xf45249

/* Live scalar channels arrive independently.  Never keep painting an old
 * value as current just because the therapy flag is still latched after a BLE
 * interruption.  The slowest of these channels normally updates at 0.5 Hz;
 * eight seconds allows several packets without masking a real disconnect. */
#define METRIC_STALE_US 8000000LL

/* Capacity copy is intentionally expressed in something useful at bedside.
 * This conservative blended allowance includes native streams, generated EDF
 * files, metadata, and normal filesystem overhead.  Oximetry is called out as
 * reducing the estimate instead of pretending every night has one fixed size. */
#define AIRSENSE_NIGHT_ESTIMATE_BYTES (80ULL * 1024ULL * 1024ULL)

#if CONFIG_SOMNOTRACE_BOARD_QEMU
#define UI_DECORATIVE_SHADOW_WIDTH(pixels) (pixels)
#define UI_DECORATIVE_SHADOW_OPA(opacity)  (opacity)
#define FLOW_RENDER_POINTS                 FLOW_POINTS
#define FLOW_RENDER_FILL                   1
#define FLOW_RENDER_GLOW                   1
#define UI_STATUS_SCRIM_COLOR              0x000000
#define UI_STATUS_SCRIM_OPA                LV_OPA_60
#else
/* Large software-blurred shadows and the filled/glowing 300-point trace are
 * disproportionately expensive on the physical ESP32-S3. Solid surfaces,
 * borders, and the foreground trace preserve hierarchy and state without
 * consuming the render budget needed for responsive touch. */
#define UI_DECORATIVE_SHADOW_WIDTH(pixels) ((void)(pixels), 0)
#define UI_DECORATIVE_SHADOW_OPA(opacity)  ((void)(opacity), LV_OPA_TRANSP)
#define FLOW_RENDER_POINTS                 150
#define FLOW_RENDER_FILL                   0
#define FLOW_RENDER_GLOW                   0
/* Keep the underlying screen legible so this reads as a temporary tray, not a
 * replacement page. Opening already pauses live-chart work and never changes
 * z-order, which removes the avoidable redraw cost around this blend. */
#define UI_STATUS_SCRIM_COLOR              0x000000
#define UI_STATUS_SCRIM_OPA                LV_OPA_60
#endif

/* Typography roles from the 7-inch design handoff.  Keeping the role names
 * here makes it hard for a later screen to drift back to LVGL's Montserrat
 * defaults, and lets data use tabular, fixed-width numerals. */
#define FONT_CLOCK          (&somnotrace_space_grotesk_medium_34)
#define FONT_STATE          (&somnotrace_space_grotesk_semibold_32)
#define FONT_SCREEN_TITLE   (&somnotrace_space_grotesk_semibold_23)
#define FONT_ROW_TITLE      (&somnotrace_space_grotesk_semibold_17)
#define FONT_BODY_LARGE     (&somnotrace_space_grotesk_medium_17)
#define FONT_BODY           (&somnotrace_space_grotesk_medium_15)
#define FONT_BODY_SMALL     (&somnotrace_space_grotesk_medium_13)
#define FONT_BUTTON         (&somnotrace_space_grotesk_semibold_17)
#define FONT_BUTTON_COMPACT (&somnotrace_space_grotesk_semibold_15)
#define FONT_BUTTON_SMALL   (&somnotrace_space_grotesk_semibold_13)
#define FONT_BUTTON_PRIMARY (&somnotrace_space_grotesk_semibold_23)
#define FONT_DATA_HERO      (&somnotrace_ibm_plex_mono_semibold_34)
#define FONT_DATA_VALUE     (&somnotrace_ibm_plex_mono_semibold_29)
#define FONT_DATA_COMPACT   (&somnotrace_ibm_plex_mono_semibold_26)
#define FONT_DATA_BODY      (&somnotrace_ibm_plex_mono_medium_15)
#define FONT_METRIC_LABEL   (&somnotrace_ibm_plex_mono_medium_13)
#define FONT_AXIS           (&somnotrace_ibm_plex_mono_medium_11)

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

typedef struct {
    char addr[18];
    char name[40];
    int rssi;
    ox_driver_t driver;
} ui_device_result_t;

typedef struct {
    ui_device_result_t as11[DEVICE_RESULT_MAX];
    size_t as11_count;
    unsigned as11_version;
    bool as11_busy;
    ui_device_result_t ox[DEVICE_RESULT_MAX];
    size_t ox_count;
    unsigned ox_version;
    bool ox_busy;
    uint64_t storage_free;
    uint64_t storage_total;
    int upload_pending;
    char upload_state[20];
    uploader_progress_snapshot_t upload_progress;
    esp_err_t upload_progress_result;
    esp_err_t storage_result;
    unsigned storage_version;
    bool storage_busy;
    therapy_alert_config_t alert_config;
    esp_err_t alert_config_result;
    unsigned alert_config_version;
    bool alert_config_busy;
} ui_service_state_t;


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
static ui_service_state_t s_services;
static ui_service_state_t *s_render_services;
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
/* SoftAP and first-run setup may overlap. Keep their visibility claims
 * independent so either owner can finish without hiding the other. */
static bool s_backlight_force_on;
static bool s_setup_backlight_force_on;
static bool s_temporarily_awake;
static esp_timer_handle_t s_wake_timer;
static bool s_touch_was_pressed;
static int64_t s_last_touch_activity_us;
static int64_t s_last_off_request_us;
static void (*s_setup_callback)(void);
static uint32_t s_flush_count;
static uint32_t s_flush_timeouts;
static uint32_t s_touch_read_errors;
static uint8_t s_touch_consecutive_errors;
static uint32_t s_backlight_write_errors;
static int64_t s_backlight_retry_after_us;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
static bool s_qemu_first_frame_published;
static bool s_qemu_setup_preview_requested;
#endif
static lv_coord_t s_last_touch_x;
static lv_coord_t s_last_touch_y;
static bool s_touch_services_ready;
static bool s_as11_service_ready;
static bool s_ox_service_ready;
static bool s_first_run_setup_active;
static uint32_t s_first_run_setup_seen_generation;
/* User-confirmed prerequisite for the AirSense application-layer pairing
 * flow. The machine must enter its own pairing mode before SomnoTrace scans;
 * doing these in the opposite order can display a code but fail the final
 * exchange. */
static bool s_therapy_command_busy;
static bool s_therapy_command_target;
static bool s_alert_ack_busy;

static lv_obj_t *s_clock_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_ble_label;
static lv_obj_t *s_sd_label;
static lv_obj_t *s_wifi_dot;
static lv_obj_t *s_ble_dot;
static lv_obj_t *s_sd_dot;
static lv_obj_t *s_status_capsule;
static lv_obj_t *s_status_divider;
static lv_obj_t *s_status_chevron;
static lv_obj_t *s_status_scrim;
static lv_obj_t *s_status_tray;
static lv_obj_t *s_status_tray_as11;
static lv_obj_t *s_status_tray_sd;
static lv_obj_t *s_status_tray_wifi;
static lv_obj_t *s_status_tray_upload;
static lv_obj_t *s_status_tray_ox;
static lv_obj_t *s_status_tray_dots[5];
static lv_obj_t *s_therapy_label;
static lv_obj_t *s_therapy_subtitle;
static lv_obj_t *s_therapy_hero;
static lv_obj_t *s_therapy_orb;
static lv_obj_t *s_therapy_orb_core;
static lv_obj_t *s_leak_label;
static lv_obj_t *s_pressure_label;
static lv_obj_t *s_resp_label;
static lv_obj_t *s_flow_lim_label;
static lv_obj_t *s_metric_bars[4];
static lv_obj_t *s_runtime_label;
static lv_obj_t *s_runtime_caption;
static lv_obj_t *s_chart_status_pill;
static lv_obj_t *s_chart_status_dot;
static lv_obj_t *s_chart_status;
static lv_obj_t *s_chart_message;
static lv_obj_t *s_chart_message_sub;
static lv_obj_t *s_notice_card;
static lv_obj_t *s_notice_label;
static lv_obj_t *s_notice_mark;
static lv_obj_t *s_alert_banner;
static lv_obj_t *s_alert_label;
static lv_obj_t *s_alert_subtitle;
static lv_obj_t *s_alert_mark;
static lv_obj_t *s_alert_ack_button;
static lv_obj_t *s_therapy_button_label;
static lv_obj_t *s_therapy_button;
static lv_obj_t *s_chart;
static int16_t s_flow_visual[FLOW_POINTS];
static unsigned s_flow_visual_count;
static bool s_flow_visual_live;
static lv_obj_t *s_ambient_glow;
static lv_obj_t *s_pages[3];
static lv_obj_t *s_nav_buttons[3];
static lv_obj_t *s_nav_labels[3];
static int s_active_page = -1;
static lv_obj_t *s_history_host;
static touch_history_ui_t *s_history_ui;
static touch_history_controller_t *s_history_controller;
static uint32_t s_history_rendered_revision = UINT32_MAX;
static bool s_history_apply_scheduled;
static lv_obj_t *s_manage_scrolls[MANAGE_SECTION_COUNT];
static lv_obj_t *s_manage_sections[MANAGE_SECTION_COUNT];
static lv_obj_t *s_manage_buttons[MANAGE_SECTION_COUNT];
static lv_obj_t *s_manage_labels[MANAGE_SECTION_COUNT];
static lv_obj_t *s_manage_dots[MANAGE_SECTION_COUNT];
static lv_obj_t *s_manage_badges[MANAGE_SECTION_COUNT];
static int s_active_manage_section = -1;
/* The rail is persistent, but the 768 x 450 detail pane owns only the visible
 * destination. Logs may briefly remain as one hidden retired tree while its
 * bounded worker releases the controller; no other destination is retained. */
static lv_obj_t *s_manage_detail_host;
static lv_obj_t *s_manage_retired_logs_section;
static int s_rendered_manage_section = -1;
static uint32_t s_manage_transition_generation;
static lv_obj_t *s_wake_overlay;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
static TaskHandle_t s_storage_worker_task;
#endif
#if CONFIG_SOMNOTRACE_BOARD_QEMU
static uint8_t s_qemu_requested_tab = UINT8_MAX;
#endif

static void set_active_page(int page);
static void set_manage_section(int section);
static void ensure_manage_destination(void);
static void teardown_rendered_manage_destination(void);
static void reap_retired_logs_destination(void);
static void update_manage_rail_selection(int section);
static void start_storage_refresh(void);
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
        /* The worker may publish after a later OFF while waiting for I2C. */
        if (touch.visibility_requested_us > s_last_off_request_us) {
            s_backlight_requested = true;
            s_backlight_known = false;
            s_wake_gesture_pending = true;
            s_last_touch_activity_us = now;
            ++s_backlight_revision;
        }
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

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    /* The handoff permits a solid surface plus a one-pixel top highlight.
     * That form preserves its four-step slate hierarchy in RGB565 without
     * turning low-light gradients into visible horizontal bands. */
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_INVERSE), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x010207), 0);
    lv_obj_set_style_shadow_width(card, UI_DECORATIVE_SHADOW_WIDTH(22), 0);
    lv_obj_set_style_shadow_ofs_y(card, 9, 0);
    lv_obj_set_style_shadow_opa(card, UI_DECORATIVE_SHADOW_OPA(LV_OPA_40), 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_text_color(card, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_inner_card(lv_obj_t *parent, int x, int y, int w, int h,
                                 int radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_text_color(card, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_status_dot(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_DISABLED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

static bool local_style_color_matches(lv_obj_t *obj, lv_style_prop_t property,
                                      lv_color_t color,
                                      lv_style_selector_t selector)
{
    lv_style_value_t current;
    return lv_obj_get_local_style_prop(obj, property, &current, selector) ==
               LV_STYLE_RES_FOUND &&
           lv_color_to32(current.color) == lv_color_to32(color);
}

static void set_style_color_if_changed(lv_obj_t *obj,
                                       lv_style_prop_t property,
                                       uint32_t color,
                                       lv_style_selector_t selector)
{
    lv_color_t next = lv_color_hex(color);
    if (local_style_color_matches(obj, property, next, selector)) return;

    lv_style_value_t value = { .color = next };
    lv_obj_set_local_style_prop(obj, property, value, selector);
}

static void set_style_num_if_changed(lv_obj_t *obj, lv_style_prop_t property,
                                     int32_t number,
                                     lv_style_selector_t selector)
{
    lv_style_value_t current;
    if (lv_obj_get_local_style_prop(obj, property, &current, selector) ==
            LV_STYLE_RES_FOUND &&
        current.num == number) {
        return;
    }

    lv_style_value_t value = { .num = number };
    lv_obj_set_local_style_prop(obj, property, value, selector);
}

static void set_style_ptr_if_changed(lv_obj_t *obj, lv_style_prop_t property,
                                     const void *pointer,
                                     lv_style_selector_t selector)
{
    lv_style_value_t current;
    if (lv_obj_get_local_style_prop(obj, property, &current, selector) ==
            LV_STYLE_RES_FOUND &&
        current.ptr == pointer) {
        return;
    }

    lv_style_value_t value = { .ptr = pointer };
    lv_obj_set_local_style_prop(obj, property, value, selector);
}

static bool set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label) return false;
    if (!text) text = "";
    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0) return false;
    lv_label_set_text(label, text);
    return true;
}

static bool set_label_text_fmt_if_changed(lv_obj_t *label, const char *format,
                                          ...)
{
    /* The largest call is title + attention (48 + 256 bytes plus framing). */
    char text[384];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (written < 0) return false;
    return set_label_text_if_changed(label, text);
}

static void set_dot_tone(lv_obj_t *dot, uint32_t color, bool glow)
{
    set_style_color_if_changed(dot, LV_STYLE_BG_COLOR, color, 0);
    set_style_color_if_changed(dot, LV_STYLE_SHADOW_COLOR, color, 0);
    set_style_num_if_changed(dot, LV_STYLE_SHADOW_WIDTH, glow ? 9 : 0, 0);
    set_style_num_if_changed(dot, LV_STYLE_SHADOW_OPA,
                             glow ? LV_OPA_70 : LV_OPA_TRANSP, 0);
}

static void chevron_draw_cb(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    if (!obj || !draw_ctx) return;
    lv_area_t area;
    lv_obj_get_content_coords(obj, &area);
    lv_coord_t center_x = area.x1 + lv_area_get_width(&area) / 2;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(COLOR_SECONDARY);
    line.width = 2;
    line.round_start = 1;
    line.round_end = 1;
    lv_point_t left[] = {
        { area.x1 + 1, area.y1 + 2 }, { center_x, area.y2 - 1 },
    };
    lv_point_t right[] = {
        { center_x, area.y2 - 1 }, { area.x2 - 1, area.y1 + 2 },
    };
    lv_draw_line(draw_ctx, &line, &left[0], &left[1]);
    lv_draw_line(draw_ctx, &line, &right[0], &right[1]);
}

static lv_obj_t *make_down_chevron(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *chevron = lv_obj_create(parent);
    lv_obj_set_pos(chevron, x, y);
    lv_obj_set_size(chevron, 14, 10);
    lv_obj_set_style_bg_opa(chevron, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chevron, 0, 0);
    lv_obj_set_style_pad_all(chevron, 0, 0);
    lv_obj_clear_flag(chevron,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chevron, chevron_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return chevron;
}


static lv_coord_t status_label_width(lv_obj_t *label)
{
    lv_point_t size = {0};
    lv_txt_get_size(&size, lv_label_get_text(label), FONT_BODY, 0, 0,
                    LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return LV_MAX(size.x, 1);
}

static void layout_status_capsule(void)
{
    if (!s_status_capsule || !s_status_divider || !s_status_chevron) return;

    lv_obj_t *dots[] = {s_ble_dot, s_sd_dot, s_wifi_dot};
    lv_obj_t *labels[] = {s_ble_label, s_sd_label, s_wifi_label};
    const lv_coord_t font_h = lv_font_get_line_height(FONT_BODY);
    const lv_coord_t label_y = (STATUS_CAPSULE_H - font_h) / 2;
    const lv_coord_t dot_y = label_y + (font_h - STATUS_CAPSULE_DOT_SIZE) / 2;
    lv_coord_t cursor = STATUS_CAPSULE_LEFT_PAD;

    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        lv_coord_t text_w = status_label_width(labels[i]);
        lv_obj_set_pos(dots[i], cursor, dot_y);
        cursor += STATUS_CAPSULE_DOT_SIZE + STATUS_CAPSULE_DOT_LABEL_GAP;
        lv_label_set_long_mode(labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(labels[i], cursor, label_y);
        lv_obj_set_size(labels[i], text_w, font_h);
        cursor += text_w;
        if (i + 1 < sizeof(labels) / sizeof(labels[0]))
            cursor += STATUS_CAPSULE_ITEM_GAP;
    }

    cursor += STATUS_CAPSULE_DIVIDER_GAP;
    lv_obj_set_pos(s_status_divider, cursor, (STATUS_CAPSULE_H - 20) / 2);
    cursor += 1 + STATUS_CAPSULE_CHEVRON_GAP;
    lv_obj_set_pos(s_status_chevron, cursor, (STATUS_CAPSULE_H - 10) / 2);
    cursor += 14 + STATUS_CAPSULE_RIGHT_PAD;

    lv_obj_set_pos(s_status_capsule, STATUS_CAPSULE_RIGHT - cursor, 7);
    lv_obj_set_size(s_status_capsule, cursor, STATUS_CAPSULE_H);
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w,
                             const char *text, uint32_t color,
                             lv_event_cb_t callback, intptr_t action)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, 104);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(button, 2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x20252f), LV_STATE_DISABLED);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 28, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)action);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FONT_BUTTON, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_DISABLED), LV_STATE_DISABLED);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *make_touch_button(lv_obj_t *parent, int x, int y, int w, int h,
                                   const char *text, uint32_t color,
                                   lv_event_cb_t callback, intptr_t action)
{
    lv_obj_t *button = make_button(parent, x, y, w, text, color, callback, action);
    lv_obj_set_height(button, h);
    return button;
}

static lv_obj_t *make_destination_button(lv_obj_t *parent, int x, int y,
                                         int w, int h, const char *text,
                                         uint32_t color,
                                         lv_event_cb_t callback,
                                         intptr_t destination)
{
    lv_obj_t *button = make_touch_button(parent, x, y, w, h, text, color,
                                         callback, destination);
    /* Destination changes can safely happen on touch-down. Keep the generic
     * button factory on CLICKED so commands and destructive actions still
     * require a complete press/release gesture. */
    lv_obj_remove_event_cb_with_user_data(button, callback,
                                          (void *)destination);
    lv_obj_add_event_cb(button, callback, LV_EVENT_PRESSED,
                        (void *)destination);
    return button;
}

static void set_button_surface(lv_obj_t *button, uint32_t color,
                               lv_opa_t resting_opa)
{
    set_style_color_if_changed(button, LV_STYLE_BG_COLOR, color,
                               LV_STATE_DEFAULT);
    set_style_num_if_changed(button, LV_STYLE_BG_OPA, resting_opa,
                             LV_STATE_DEFAULT);
    /* Pointer clicks leave LVGL buttons focused. Define that state explicitly
     * so a selected light pill cannot fall back to the dark theme colour. */
    set_style_color_if_changed(button, LV_STYLE_BG_COLOR, color,
                               LV_STATE_FOCUSED);
    set_style_num_if_changed(button, LV_STYLE_BG_OPA, resting_opa,
                             LV_STATE_FOCUSED);
    set_style_color_if_changed(button, LV_STYLE_BG_COLOR, color,
                               LV_STATE_PRESSED);
    set_style_num_if_changed(button, LV_STYLE_BG_OPA, LV_OPA_80,
                             LV_STATE_PRESSED);
    set_style_color_if_changed(button, LV_STYLE_BG_COLOR, color,
                               LV_STATE_FOCUSED | LV_STATE_PRESSED);
    set_style_num_if_changed(button, LV_STYLE_BG_OPA, LV_OPA_80,
                             LV_STATE_FOCUSED | LV_STATE_PRESSED);
}

static void set_destination_surface(lv_obj_t *button, uint32_t color,
                                    lv_opa_t resting_opa)
{
    set_button_surface(button, color, resting_opa);
    /* A destination changes immediately on touch-down. Reusing the generic
     * action-button press animation makes its old surface move/fade one frame
     * later on release, which reads as delayed or stuck feedback. */
    set_style_num_if_changed(button, LV_STYLE_TRANSLATE_Y, 0,
                             LV_STATE_PRESSED);
    set_style_num_if_changed(button, LV_STYLE_BG_OPA, resting_opa,
                             LV_STATE_PRESSED);
    set_style_num_if_changed(button, LV_STYLE_BG_OPA, resting_opa,
                             LV_STATE_FOCUSED | LV_STATE_PRESSED);
}

static bool set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return false;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                            int width, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    if (width > 0) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *make_value_card(lv_obj_t *parent, int x, int y,
                                 const char *caption, const char *unit,
                                 lv_obj_t **value, lv_obj_t **bar)
{
    lv_obj_t *card = make_card(parent, x, y, 143, 153);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    int value_width = unit && unit[0] ? 70 : 105;
    *value = make_label(card, "—", 18, 43, value_width, FONT_DATA_VALUE,
                        COLOR_TEXT);
    make_label(card, unit, 88, 55, 47, FONT_METRIC_LABEL,
               COLOR_SECONDARY);
    make_label(card, caption, 18, 84, 112, FONT_METRIC_LABEL,
               COLOR_TERTIARY);
    *bar = lv_bar_create(card);
    lv_obj_set_pos(*bar, 18, 118);
    lv_obj_set_size(*bar, 112, 7);
    lv_bar_set_range(*bar, 0, 100);
    lv_bar_set_value(*bar, 0, LV_ANIM_OFF);
    lv_obj_set_height(*bar, 4);
    lv_obj_set_style_radius(*bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(*bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(*bar, lv_color_hex(COLOR_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(*bar, lv_color_hex(COLOR_LIVE), LV_PART_INDICATOR);
    return card;
}


#if CONFIG_SOMNOTRACE_BOARD_QEMU
static void qemu_upload_progress(uploader_progress_snapshot_t *progress)
{
    memset(progress, 0, sizeof(*progress));
    strlcpy(progress->status, "1 part pending", sizeof(progress->status));
    progress->max_days = 30;
    progress->next_scan_s = 420;
    progress->backend_count = 2;

    uploader_backend_progress_t *nas = &progress->backends[0];
    strlcpy(nas->id, "smb", sizeof(nas->id));
    strlcpy(nas->label, "Network folder (NAS)", sizeof(nas->label));
    nas->configured = true;
    nas->state = UPLOADER_BACKEND_IDLE;
    nas->days_done = 7;
    nas->days_total = 7;
    nas->last_success_valid = true;
    nas->last_success_epoch_s = 1788327660U; /* deterministic preview only */

    uploader_backend_progress_t *shq = &progress->backends[1];
    strlcpy(shq->id, "sleephq", sizeof(shq->id));
    strlcpy(shq->label, "SleepHQ", sizeof(shq->label));
    shq->configured = true;
    shq->state = UPLOADER_BACKEND_UPLOADING;
    shq->days_done = 2;
    shq->days_total = 3;
    shq->current_valid = true;
    strlcpy(shq->current_day, "20260901", sizeof(shq->current_day));
    shq->current_unit = 4;
    shq->current_units = 11;
}
#endif

static void storage_status_task(void *arg)
{
    (void)arg;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#endif
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
    esp_err_t result = ESP_ERR_TIMEOUT;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    free_bytes = 1932735283ULL; /* 1.8 GiB, matching the design preview. */
    total_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
    result = ESP_OK;
#else
    if (sd_storage_lease_acquire(SD_LEASE_UPLOAD, 250)) {
        result = sd_storage_get_free(&free_bytes, &total_bytes);
        sd_storage_lease_release(SD_LEASE_UPLOAD);
    }
#endif
    int pending = 0;
    const char *worst = "idle";
    uploader_progress_snapshot_t upload_progress = {0};
    esp_err_t upload_result;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    qemu_upload_progress(&upload_progress);
    upload_result = ESP_OK;
    pending = 1;
    worst = "uploading";
#else
    uploader_get_summary(&pending, &worst);
    upload_result = uploader_get_progress_snapshot(&upload_progress);
#endif
    portENTER_CRITICAL(&s_state_lock);
    s_services.storage_free = free_bytes;
    s_services.storage_total = total_bytes;
    s_services.upload_pending = pending;
    strlcpy(s_services.upload_state, worst ? worst : "idle",
            sizeof(s_services.upload_state));
    s_services.upload_progress = upload_progress;
    s_services.upload_progress_result = upload_result;
    s_services.storage_result = result;
    s_services.storage_busy = false;
    s_services.storage_version++;
    if (result == ESP_OK && total_bytes > 0)
        s_state.storage_near_full = free_bytes < (24ULL * 1024 * 1024);
    portEXIT_CRITICAL(&s_state_lock);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    vTaskDelete(NULL);
#else
    }
#endif
}

static void start_storage_refresh(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_services.storage_busy;
    if (!busy) s_services.storage_busy = true;
    portEXIT_CRITICAL(&s_state_lock);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (!busy && xTaskCreate(storage_status_task, "ui_storage", 4096,
                             NULL, 2, NULL) != pdPASS) {
#else
    if (!busy && s_storage_worker_task) {
        xTaskNotifyGive(s_storage_worker_task);
    } else if (!busy) {
#endif
        portENTER_CRITICAL(&s_state_lock);
        s_services.storage_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Unable to read storage status");
    }
}

static unsigned estimated_airsense_nights(uint64_t free_bytes)
{
    uint64_t raw = free_bytes / AIRSENSE_NIGHT_ESTIMATE_BYTES;
    if (raw > UINT_MAX) raw = UINT_MAX;
    if (raw >= 100) return (unsigned)(((raw + 5) / 10) * 10);
    if (raw >= 20) return (unsigned)(((raw + 2) / 5) * 5);
    return (unsigned)raw;
}







static void set_active_page(int page)
{
    if (page < 0 || page >= 3) return;
    portENTER_CRITICAL(&s_state_lock);
    int previous_page = s_active_page;
    portEXIT_CRITICAL(&s_state_lock);
    portENTER_CRITICAL(&s_state_lock);
    bool already_active = page == s_active_page;
    if (!already_active) s_active_page = page;
    portEXIT_CRITICAL(&s_state_lock);
    if (already_active) return;
    if (page == 0 && s_touch_services_ready) start_storage_refresh();

    /* Modal controls can retain pointers into the current detail tree. Close
     * them while that tree is still valid, then release the destination when
     * Manage is no longer visible. */
    if (previous_page == 2) {
        teardown_rendered_manage_destination();
    }
    if (previous_page == 1 && s_history_controller)
        (void)touch_history_controller_set_active(
            s_history_controller, false);
    for (int i = 0; i < 3; ++i) {
        bool selected = i == page;
        set_hidden(s_pages[i], !selected);
        set_destination_surface(s_nav_buttons[i],
                                selected ? COLOR_INVERSE : COLOR_CAPSULE,
                                LV_OPA_COVER);
        set_style_color_if_changed(s_nav_labels[i], LV_STYLE_TEXT_COLOR,
                                   selected ? COLOR_BASE : COLOR_SECONDARY, 0);
        set_style_ptr_if_changed(s_nav_labels[i], LV_STYLE_TEXT_FONT,
                                 selected ? FONT_BUTTON : FONT_BODY_LARGE, 0);
        set_style_color_if_changed(s_nav_buttons[i], LV_STYLE_SHADOW_COLOR,
                                   0x010207, 0);
        set_style_num_if_changed(s_nav_buttons[i], LV_STYLE_SHADOW_WIDTH,
                                 UI_DECORATIVE_SHADOW_WIDTH(selected ? 18 : 0),
                                 0);
        set_style_num_if_changed(s_nav_buttons[i], LV_STYLE_SHADOW_OFS_Y,
                                 selected ? 6 : 0, 0);
        set_style_num_if_changed(s_nav_buttons[i], LV_STYLE_SHADOW_OPA,
                                 UI_DECORATIVE_SHADOW_OPA(
                                     selected ? LV_OPA_50 : LV_OPA_TRANSP),
                                 0);
    }
    if (page == 2) ensure_manage_destination();
    else touch_logs_controller_hide();
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    ESP_LOGI(TAG, "emulated touch selected page %u", (unsigned)page);
#endif
    if (page == 1 && s_history_controller) {
        esp_err_t result = touch_history_controller_set_active(
            s_history_controller, true);
        if (result != ESP_OK)
            ESP_LOGW(TAG, "activate rich History: %s", esp_err_to_name(result));
        __atomic_store_n(&s_history_apply_scheduled, true, __ATOMIC_RELEASE);
    }
}

static void nav_cb(lv_event_t *event)
{
    set_active_page((int)(intptr_t)lv_event_get_user_data(event));
}

static void set_manage_section(int section)
{
    if (!(section == MANAGE_LOGS)) return;
    if (section == s_active_manage_section) return;

    /* Teardown must precede changing the selected index so callbacks and
     * periodic painters can no longer mistake the old tree for visible. */
    if (s_active_page == 2) {
        teardown_rendered_manage_destination();
    }
    s_active_manage_section = section;
    update_manage_rail_selection(section);
    if (s_active_page == 2) ensure_manage_destination();
    /* Rev C controller owns alert configuration refresh. */
    if (section == MANAGE_STORAGE || section == MANAGE_UPLOADS)
        start_storage_refresh();
}

static void manage_section_cb(lv_event_t *event)
{
    set_manage_section((int)(intptr_t)lv_event_get_user_data(event));
}


static void status_tray_close_cb(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(s_status_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_tray, LV_OBJ_FLAG_HIDDEN);
}

static void status_tray_open_cb(lv_event_t *event)
{
    (void)event;
    if (s_touch_services_ready) start_storage_refresh();
    lv_obj_clear_flag(s_status_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_status_tray, LV_OBJ_FLAG_HIDDEN);
}


static void history_controller_changed(void *context)
{
    (void)context;
    /* This callback also runs on the History worker. It only schedules a
     * future LVGL-thread apply; task notification index 0 is deliberately not
     * used because the RGB driver owns it for its VSYNC handshake. */
    __atomic_store_n(&s_history_apply_scheduled, true, __ATOMIC_RELEASE);
}


static void apply_history_controller_if_needed(void)
{
    if (!s_history_controller || !s_history_ui || s_active_page != 1) return;
    bool scheduled = __atomic_exchange_n(
        &s_history_apply_scheduled, false, __ATOMIC_ACQ_REL);
    uint32_t revision = touch_history_controller_revision(s_history_controller);
    if (!scheduled && revision == s_history_rendered_revision) return;
    esp_err_t result = touch_history_controller_apply(
        s_history_controller, s_history_ui);
    if (result == ESP_OK) {
        s_history_rendered_revision = revision;
    } else {
        __atomic_store_n(&s_history_apply_scheduled, true, __ATOMIC_RELEASE);
        ESP_LOGW(TAG, "apply rich History UI: %s", esp_err_to_name(result));
    }
}


#if !CONFIG_SOMNOTRACE_BOARD_QEMU
static esp_err_t start_therapy_with_lifecycle_gate(void)
{
    if (!bsp_display_reserve_therapy_start()) {
        return ESP_ERR_INVALID_STATE;
    }
    bool may_have_started = false;
    esp_err_t result = as11_ble_start_therapy_tracked(&may_have_started);
    bool published = result == ESP_OK || may_have_started;
    if (published && !bsp_display_set_therapy_active(true)) {
        /* The start claim excludes a restart commit, so this is defensive. */
        result = ESP_ERR_INVALID_STATE;
        published = false;
    }
    if (published) {
        bsp_display_set_therapy_start_time(esp_timer_get_time());
    }
    bsp_display_release_therapy_start();
    return result;
}
#endif

static void action_task(void *arg)
{
    intptr_t action = (intptr_t)arg;
    esp_err_t result = ESP_OK;
    if (action == 5 || action == 6) {
        bool start = action == 5;
        bool active = bsp_display_is_therapy_active();
        if (active != start) {
#if CONFIG_SOMNOTRACE_BOARD_QEMU
            if (start && !bsp_display_set_therapy_active(true)) {
                result = ESP_ERR_INVALID_STATE;
            } else {
                if (!start) (void)bsp_display_set_therapy_active(false);
                if (start) bsp_display_set_therapy_start_time(esp_timer_get_time());
            }
#else
            result = start ? start_therapy_with_lifecycle_gate()
                           : as11_ble_stop_therapy();
#endif
        }
    } else if (action == 2) {
        therapy_alert_acknowledge();
    }
    if (action == 5 || action == 6) {
        portENTER_CRITICAL(&s_state_lock);
        s_therapy_command_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        if (result != ESP_OK)
            bsp_display_set_notice("Therapy command failed");
    }
    vTaskDelete(NULL);
}

static void action_cb(lv_event_t *event)
{
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    if (action == 1) {
        portENTER_CRITICAL(&s_state_lock);
        bool busy = s_therapy_command_busy;
        bool paired = s_state.paired;
        bool start = !s_state.therapy;
        if (!busy) {
            s_therapy_command_busy = true;
            s_therapy_command_target = start;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (busy) return;
        if (!paired) {
            portENTER_CRITICAL(&s_state_lock);
            s_therapy_command_busy = false;
            portEXIT_CRITICAL(&s_state_lock);
            return;
        }
        if (xTaskCreate(action_task, "ui_therapy", 4096,
                        (void *)(intptr_t)(start ? 5 : 6), 4, NULL) != pdPASS) {
            portENTER_CRITICAL(&s_state_lock);
            s_therapy_command_busy = false;
            portEXIT_CRITICAL(&s_state_lock);
            bsp_display_set_notice("Unable to start therapy action");
        }
    } else if (action == 2) {
        portENTER_CRITICAL(&s_state_lock);
        bool busy = s_alert_ack_busy;
        if (!busy) s_alert_ack_busy = true;
        portEXIT_CRITICAL(&s_state_lock);
        if (busy) return;
        if (xTaskCreate(action_task, "ui_action", 4096,
                        (void *)action, 4, NULL) != pdPASS) {
            portENTER_CRITICAL(&s_state_lock);
            s_alert_ack_busy = false;
            portEXIT_CRITICAL(&s_state_lock);
            bsp_display_set_notice("Unable to start touch action");
        }
    } else if (action == 3) {
        if (bsp_display_is_therapy_active()) {
            bsp_display_set_notice("Stop therapy before starting Wi-Fi setup");
            return;
        }
        bsp_display_set_notice("Starting Wi-Fi setup hotspot...");
        if (s_setup_callback) s_setup_callback();
    } else if (action == UI_ACTION_SCREEN_OFF) {
        if (!screen_wake_input_available()) {
            bsp_display_set_notice("Touch is unavailable - screen kept on");
            return;
        }
        bsp_display_set_backlight(false);
    }
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


static lv_obj_t *make_plain_container(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

#if FLOW_RENDER_FILL
static void draw_flow_fill_span(lv_draw_ctx_t *draw_ctx,
                                const lv_draw_rect_dsc_t *above,
                                const lv_draw_rect_dsc_t *below,
                                const lv_point_t *left,
                                const lv_point_t *right,
                                lv_coord_t middle)
{
    if (left->y == middle && right->y == middle) return;
    bool left_above = left->y <= middle;
    bool right_above = right->y <= middle;
    if (left_above == right_above) {
        lv_point_t quad[] = {
            *left, *right,
            { right->x, middle }, { left->x, middle },
        };
        lv_draw_polygon(draw_ctx, left_above ? above : below, quad, 4);
        return;
    }

    /* Split a baseline crossing into two convex triangles. A single four
     * point polygon would self-intersect here and can exhaust LVGL's mask
     * allocator on a continuously updating waveform. */
    int32_t dy = (int32_t)right->y - left->y;
    lv_coord_t crossing_x = left->x;
    if (dy != 0) {
        crossing_x += (lv_coord_t)(((int32_t)(middle - left->y) *
                                    (right->x - left->x)) / dy);
    }
    lv_point_t crossing = { crossing_x, middle };
    lv_point_t left_base = { left->x, middle };
    lv_point_t right_base = { right->x, middle };
    lv_point_t left_triangle[] = { *left, crossing, left_base };
    lv_point_t right_triangle[] = { crossing, *right, right_base };
    if (left->y != middle)
        lv_draw_polygon(draw_ctx, left_above ? above : below,
                        left_triangle, 3);
    if (right->y != middle)
        lv_draw_polygon(draw_ctx, right_above ? above : below,
                        right_triangle, 3);
}
#endif

/* A small custom draw object avoids a framebuffer-sized canvas or hundreds of
 * child objects. QEMU retains the handoff's four visual layers; hardware uses
 * the baseline and foreground trace so touch remains responsive. */
static void flow_plot_draw_cb(lv_event_t *event)
{
    lv_obj_t *plot = lv_event_get_target(event);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    if (!plot || !draw_ctx)
        return;

    lv_area_t area;
    lv_obj_get_content_coords(plot, &area);
    const lv_coord_t width = lv_area_get_width(&area);
    const lv_coord_t height = lv_area_get_height(&area);
    if (width < 2 || height < 8)
        return;

    const lv_coord_t middle = area.y1 + height / 2;
    const bool live = s_flow_visual_live;
    /* The handoff's stopped state is an intentionally quiet empty chart. Its
     * centred explanation replaces every plot layer, including the baseline. */
    if (!live) return;
    const unsigned first_source_point =
        live && s_flow_visual_count < FLOW_POINTS
            ? FLOW_POINTS - s_flow_visual_count : 0;
    uint16_t source_indices[FLOW_RENDER_POINTS];
    const unsigned render_count = (unsigned)live_flow_plot_indices(
        s_flow_visual + first_source_point, FLOW_POINTS - first_source_point,
        source_indices, FLOW_RENDER_POINTS);

    lv_draw_line_dsc_t baseline;
    lv_draw_line_dsc_init(&baseline);
    baseline.color = lv_color_hex(0x373d49);
    baseline.width = 1;
    baseline.dash_width = 3;
    baseline.dash_gap = 10;
    baseline.opa = LV_OPA_COVER;
    lv_point_t baseline_start = { area.x1, middle };
    lv_point_t baseline_end = { area.x2, middle };
    lv_draw_line(draw_ctx, &baseline, &baseline_start, &baseline_end);

    lv_point_t points[FLOW_RENDER_POINTS];
    for (unsigned i = 0; i < render_count; ++i) {
        unsigned source_index = first_source_point + source_indices[i];
        points[i].x = area.x1 + (lv_coord_t)(((int32_t)source_index * (width - 1)) /
                                             (FLOW_POINTS - 1));
        int32_t value = s_flow_visual[source_index];
        int32_t offset = value * (height - 18) / 2000;
        if (offset > height / 2 - 4) offset = height / 2 - 4;
        if (offset < -(height / 2 - 4)) offset = -(height / 2 - 4);
        points[i].y = middle - (lv_coord_t)offset;
    }

#if FLOW_RENDER_FILL
    if (live) {
        /* Draw short convex spans rather than one 300-edge polygon (unsafe in
         * LVGL 8) or separated vertical lines (visibly striped in RGB565).
         * Each span carries a vertical fade toward the baseline. */
        lv_draw_rect_dsc_t above_fill;
        lv_draw_rect_dsc_t below_fill;
        lv_draw_rect_dsc_init(&above_fill);
        lv_draw_rect_dsc_init(&below_fill);
        above_fill.bg_opa = LV_OPA_30;
        below_fill.bg_opa = LV_OPA_30;
        above_fill.bg_grad.dir = LV_GRAD_DIR_VER;
        below_fill.bg_grad.dir = LV_GRAD_DIR_VER;
        above_fill.bg_grad.stops_count = 2;
        below_fill.bg_grad.stops_count = 2;
        above_fill.bg_grad.stops[0].color = lv_color_hex(COLOR_LIVE);
        above_fill.bg_grad.stops[0].frac = 0;
        above_fill.bg_grad.stops[1].color = lv_color_hex(COLOR_PANEL);
        above_fill.bg_grad.stops[1].frac = 255;
        below_fill.bg_grad.stops[0].color = lv_color_hex(COLOR_PANEL);
        below_fill.bg_grad.stops[0].frac = 0;
        below_fill.bg_grad.stops[1].color = lv_color_hex(COLOR_LIVE);
        below_fill.bg_grad.stops[1].frac = 255;
        for (unsigned left = 0; left + 1 < render_count;) {
            unsigned right = left + 3;
            if (right >= render_count) right = render_count - 1;
            if (live_flow_plot_contiguous(s_flow_visual + first_source_point,
                                          source_indices[left], source_indices[right]))
                draw_flow_fill_span(draw_ctx, &above_fill, &below_fill,
                                    &points[left], &points[right], middle);
            left = right;
        }
    }
#endif

#if FLOW_RENDER_GLOW
    lv_draw_line_dsc_t glow;
    lv_draw_line_dsc_init(&glow);
    glow.color = lv_color_hex(COLOR_LIVE);
    glow.width = 11;
    glow.opa = live ? LV_OPA_30 : LV_OPA_TRANSP;
    /* Per-segment round caps draw a circle at every sample and make a moving
     * trace look like a string of beads. Butt-joined segments form one quiet
     * visual stroke and avoid hundreds of extra circle blends per frame. */
    glow.round_start = 0;
    glow.round_end = 0;
#endif
    lv_draw_line_dsc_t trace;
    lv_draw_line_dsc_init(&trace);
    trace.color = lv_color_hex(live ? 0x66ffff : 0x373d49);
    trace.width = 3;
    trace.opa = LV_OPA_COVER;
    trace.round_start = 0;
    trace.round_end = 0;
    for (unsigned i = 1; i < render_count; ++i) {
        if (!live_flow_plot_contiguous(s_flow_visual + first_source_point,
                                      source_indices[i - 1], source_indices[i]))
            continue;
#if FLOW_RENDER_GLOW
        if (live && ((i - 1) % 2U) == 0) {
            unsigned glow_left = i > 1 ? i - 2 : i - 1;
            if (live_flow_plot_contiguous(s_flow_visual + first_source_point,
                                          source_indices[glow_left], source_indices[i]))
                lv_draw_line(draw_ctx, &glow, &points[glow_left], &points[i]);
        }
#endif
        lv_draw_line(draw_ctx, &trace, &points[i - 1], &points[i]);
    }
}

static void build_home_page(lv_obj_t *home)
{
    s_therapy_hero = make_card(home, UI_PANEL_X, UI_PANEL_Y, 680, 132);
    lv_obj_set_style_pad_all(s_therapy_hero, 0, 0);
    s_therapy_orb = lv_obj_create(s_therapy_hero);
    lv_obj_set_pos(s_therapy_orb, 26, 30);
    lv_obj_set_size(s_therapy_orb, 74, 74);
    lv_obj_set_style_radius(s_therapy_orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_therapy_orb, 1, 0);
    lv_obj_set_style_border_color(s_therapy_orb, lv_color_hex(COLOR_TERTIARY), 0);
    lv_obj_set_style_bg_color(s_therapy_orb, lv_color_hex(COLOR_CONTROL), 0);
    lv_obj_clear_flag(s_therapy_orb, LV_OBJ_FLAG_SCROLLABLE);
    s_therapy_orb_core = lv_obj_create(s_therapy_orb);
    lv_obj_set_size(s_therapy_orb_core, 18, 18);
    lv_obj_center(s_therapy_orb_core);
    lv_obj_set_style_radius(s_therapy_orb_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_therapy_orb_core, 0, 0);
    lv_obj_set_style_bg_color(s_therapy_orb_core, lv_color_hex(COLOR_TERTIARY), 0);
    lv_obj_clear_flag(s_therapy_orb_core, LV_OBJ_FLAG_SCROLLABLE);

    s_therapy_label = make_label(s_therapy_hero, "Therapy stopped", 118, 33, 350,
                                 FONT_STATE, COLOR_TEXT);
    s_therapy_subtitle = make_label(s_therapy_hero, "Ready when you are", 118, 76, 390,
                                    FONT_BODY, COLOR_SECONDARY);
    s_runtime_caption = make_label(s_therapy_hero, "LAST SESSION", 510, 36, 142,
                                   FONT_METRIC_LABEL, COLOR_TERTIARY);
    lv_obj_set_style_text_align(s_runtime_caption, LV_TEXT_ALIGN_RIGHT, 0);
    s_runtime_label = make_label(s_therapy_hero, "—", 462, 65, 190,
                                 FONT_DATA_HERO, COLOR_TEXT);
    lv_obj_set_style_text_align(s_runtime_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *graph_card = make_card(home, UI_PANEL_X, 150, 680, 304);
    lv_obj_set_style_pad_all(graph_card, 0, 0);
    make_label(graph_card, "Breathing flow", 24, 15, 240,
               FONT_ROW_TITLE, COLOR_TEXT);
    s_chart_status_pill = make_inner_card(graph_card, 556, 10, 98, 32, 16);
    s_chart_status_dot = make_status_dot(s_chart_status_pill, 13, 12, 8);
    s_chart_status = make_label(s_chart_status_pill, "Waiting", 29, 7, 62,
                                FONT_METRIC_LABEL, COLOR_SECONDARY);
    s_chart = lv_obj_create(graph_card);
    lv_obj_set_pos(s_chart, 0, 52);
    lv_obj_set_size(s_chart, 680, 252);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_chart, flow_plot_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_chart_message = make_label(graph_card, "Graph paused", 120, 139,
                                 438, &somnotrace_space_grotesk_semibold_19,
                                 COLOR_TEXT);
    lv_obj_set_style_text_align(s_chart_message, LV_TEXT_ALIGN_CENTER, 0);
    s_chart_message_sub = make_label(graph_card,
                                     "Live flow appears while therapy is running",
                                     120, 173, 438,
                                     FONT_BODY, COLOR_SECONDARY);
    lv_obj_set_style_text_align(s_chart_message_sub, LV_TEXT_ALIGN_CENTER, 0);

    make_value_card(home, 710, UI_PANEL_Y, "PRESSURE", "cmH₂O",
                    &s_pressure_label, &s_metric_bars[0]);
    make_value_card(home, 865, UI_PANEL_Y, "LEAK", "L/min",
                    &s_leak_label, &s_metric_bars[1]);
    make_value_card(home, 710, 171, "RESP RATE", "br/min",
                    &s_resp_label, &s_metric_bars[2]);
    make_value_card(home, 865, 171, "FLOW LIMIT", "",
                    &s_flow_lim_label, &s_metric_bars[3]);

    s_therapy_button = make_touch_button(home, 710, 338, 298, 116,
                                         "Start therapy", COLOR_LIVE,
                                         action_cb, 1);
    lv_obj_set_style_radius(s_therapy_button, 28, 0);
    s_therapy_button_label = lv_obj_get_child(s_therapy_button, 0);
    lv_obj_set_style_text_font(s_therapy_button_label, FONT_BUTTON_PRIMARY, 0);
    lv_obj_set_style_text_color(s_therapy_button_label, lv_color_hex(COLOR_BASE), 0);
}

static void build_history_page(lv_obj_t *history)
{
    s_history_host = make_plain_container(
        history, UI_PANEL_X, UI_PANEL_Y,
        TOUCH_HISTORY_UI_WIDTH, TOUCH_HISTORY_UI_HEIGHT);
    if (!s_history_controller) {
        make_label(s_history_host, "History is unavailable", 24, 24, 500,
                   FONT_SCREEN_TITLE, COLOR_FAULT);
        return;
    }
    const touch_history_ui_config_t config = {
        .on_intent = touch_history_controller_handle_intent,
        .intent_context = s_history_controller,
    };
    esp_err_t result = touch_history_ui_create(
        s_history_host, &config, &s_history_ui);
    if (result != ESP_OK) {
        make_label(s_history_host, "History could not allocate its view",
                   24, 24, 620, FONT_SCREEN_TITLE, COLOR_FAULT);
        ESP_LOGE(TAG, "create rich History UI: %s", esp_err_to_name(result));
        return;
    }
    s_history_apply_scheduled = true;
}


static void clear_manage_section_pointers(int section)
{
    if (section < 0 || section >= MANAGE_SECTION_COUNT) return;
    s_manage_sections[section] = NULL;
    s_manage_scrolls[section] = NULL;
}

#if CONFIG_SOMNOTRACE_BOARD_QEMU
static unsigned manage_descendant_count(lv_obj_t *root)
{
    if (!root) return 0;
    unsigned count = lv_obj_get_child_cnt(root);
    for (unsigned i = 0; i < lv_obj_get_child_cnt(root); ++i)
        count += manage_descendant_count(lv_obj_get_child(root, i));
    return count;
}

static void log_manage_ownership(const char *action)
{
    unsigned roots = s_manage_detail_host
                         ? lv_obj_get_child_cnt(s_manage_detail_host) : 0;
    unsigned objects = s_manage_detail_host
                           ? manage_descendant_count(s_manage_detail_host) : 0;
    ESP_LOGI(TAG,
             "manage lifecycle %s gen=%lu selected=%d rendered=%d roots=%u objects=%u internal=%u psram=%u",
             action, (unsigned long)s_manage_transition_generation,
             s_active_manage_section, s_rendered_manage_section, roots, objects,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
#else
static void log_manage_ownership(const char *action)
{
    (void)action;
}
#endif

static void update_manage_rail_selection(int section)
{
    for (int i = 0; i < MANAGE_SECTION_COUNT; ++i) {
        if (!s_manage_buttons[i]) continue;
        bool selected = i == section;
        set_destination_surface(s_manage_buttons[i],
                                selected ? COLOR_INVERSE : COLOR_PANEL,
                                selected ? LV_OPA_COVER : LV_OPA_TRANSP);
        set_style_color_if_changed(s_manage_labels[i], LV_STYLE_TEXT_COLOR,
                                   selected ? COLOR_BASE : COLOR_SECONDARY, 0);
        set_style_ptr_if_changed(s_manage_labels[i], LV_STYLE_TEXT_FONT,
                                 selected ? FONT_BUTTON : FONT_BODY_LARGE, 0);
        set_style_color_if_changed(s_manage_buttons[i], LV_STYLE_SHADOW_COLOR,
                                   COLOR_BASE, 0);
        set_style_num_if_changed(s_manage_buttons[i], LV_STYLE_SHADOW_WIDTH,
                                 UI_DECORATIVE_SHADOW_WIDTH(selected ? 18 : 0),
                                 0);
        set_style_num_if_changed(s_manage_buttons[i], LV_STYLE_SHADOW_OFS_Y,
                                 selected ? 6 : 0, 0);
        set_style_num_if_changed(s_manage_buttons[i], LV_STYLE_SHADOW_OPA,
                                 UI_DECORATIVE_SHADOW_OPA(
                                     selected ? LV_OPA_50 : LV_OPA_TRANSP),
                                 0);
    }
}

static void reap_retired_logs_destination(void)
{
    lv_obj_t *retired = s_manage_retired_logs_section;
    if (!retired || s_rendered_manage_section == MANAGE_LOGS) return;
    if (touch_logs_controller_destroy() != ESP_OK) return;
    s_manage_retired_logs_section = NULL;
    lv_obj_del(retired);
    s_manage_transition_generation++;
    log_manage_ownership("logs-reaped");
}

static void teardown_rendered_manage_destination(void)
{
    int section = s_rendered_manage_section;
    if (section < 0 || section >= MANAGE_SECTION_COUNT) return;

    /* Any editor or confirmation can own a pointer into this destination.
     * Tear those down before publishing NULL widget pointers. */
    lv_obj_t *root = s_manage_sections[section];
    s_rendered_manage_section = -1;
    if (section == MANAGE_LOGS) touch_logs_controller_hide();

    clear_manage_section_pointers(section); /* invalidate before lv_obj_del */
    if (section == MANAGE_LOGS &&
        touch_logs_controller_destroy() == ESP_ERR_INVALID_STATE) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        s_manage_retired_logs_section = root;
    } else if (root) {
        lv_obj_del(root);
    }
    clear_manage_section_pointers(section); /* remain invalid after teardown */
    s_manage_transition_generation++;
    log_manage_ownership(section == MANAGE_LOGS &&
                         s_manage_retired_logs_section
                             ? "logs-retired" : "destroy");
}

static void build_manage_destination(int section)
{
    if (!s_manage_detail_host || section < 0 ||
        section >= MANAGE_SECTION_COUNT) return;

    reap_retired_logs_destination();
    lv_obj_t *destination = NULL;
    if (section == MANAGE_LOGS && s_manage_retired_logs_section) {
        destination = s_manage_retired_logs_section;
        s_manage_retired_logs_section = NULL;
        lv_obj_clear_flag(destination, LV_OBJ_FLAG_HIDDEN);
    } else {
        destination = make_plain_container(
            s_manage_detail_host, 0, 0, UI_MANAGE_DETAIL_W, UI_PANEL_H);
    }

    s_manage_sections[section] = destination;
    s_rendered_manage_section = section;
    switch ((manage_section_t)section) {
    default: break;
    case MANAGE_LOGS: {
        s_manage_scrolls[MANAGE_LOGS] = destination;
        esp_err_t logs_result = touch_logs_controller_show(destination);
        if (logs_result != ESP_OK) {
            make_label(destination, "Logs are unavailable", 24, 24, 620,
                       FONT_SCREEN_TITLE, COLOR_TEXT);
            make_label(destination, "The retained log viewer could not be allocated.",
                       24, 62, 620, FONT_BODY, COLOR_SECONDARY);
            bsp_display_set_notice("Unable to open retained logs");
        }
#if CONFIG_SOMNOTRACE_BOARD_QEMU
        else
            ESP_LOGI(TAG, "QEMU native Logs pane ready");
#endif
        break;
    }
    }
    s_manage_transition_generation++;
    log_manage_ownership("build");
}

static void ensure_manage_destination(void)
{
    if (s_active_page != 2 || s_active_manage_section < 0 ||
        s_active_manage_section >= MANAGE_SECTION_COUNT) return;
    if (s_rendered_manage_section == s_active_manage_section &&
        s_manage_sections[s_active_manage_section]) return;
    if (s_rendered_manage_section >= 0)
        teardown_rendered_manage_destination();
    build_manage_destination(s_active_manage_section);
}

static void build_manage_page(lv_obj_t *manage)
{
    static const int section_ids[] = { MANAGE_LOGS };
    static const char *section_names[] = { "Logs" };
    lv_obj_t *rail = make_card(manage, UI_PANEL_X, UI_PANEL_Y,
                               UI_MANAGE_RAIL_W, UI_PANEL_H);
    lv_obj_set_style_radius(rail, 28, 0);
    lv_obj_set_style_pad_all(rail, 8, 0);
    for (unsigned row = 0; row < sizeof(section_ids) / sizeof(section_ids[0]); ++row) {
        int i = section_ids[row];
        s_manage_buttons[i] = make_destination_button(
            rail, 0, i * 52, 196, 46, section_names[row], COLOR_PANEL,
            manage_section_cb, i);
        lv_obj_set_style_radius(s_manage_buttons[i], 20, 0);
        s_manage_labels[i] = lv_obj_get_child(s_manage_buttons[i], 0);
        lv_obj_align(s_manage_labels[i], LV_ALIGN_LEFT_MID, 36, 0);
        lv_obj_set_style_text_font(s_manage_labels[i], FONT_BODY, 0);
        s_manage_dots[i] = lv_obj_create(s_manage_buttons[i]);
        lv_obj_set_pos(s_manage_dots[i], 16, 19);
        lv_obj_set_size(s_manage_dots[i], 8, 8);
        lv_obj_set_style_radius(s_manage_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_manage_dots[i], 0, 0);
        lv_obj_set_style_bg_color(s_manage_dots[i], lv_color_hex(COLOR_TERTIARY), 0);
        lv_obj_clear_flag(s_manage_dots[i],
                          LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_manage_badges[i] = lv_label_create(s_manage_buttons[i]);
        lv_obj_set_size(s_manage_badges[i], 24, 24);
        lv_obj_align(s_manage_badges[i], LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_style_text_font(s_manage_badges[i], FONT_BUTTON_SMALL, 0);
        lv_obj_set_style_text_align(s_manage_badges[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(s_manage_badges[i], 3, 0);
        lv_obj_set_style_radius(s_manage_badges[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_manage_badges[i], lv_color_hex(COLOR_FAULT), 0);
        lv_obj_set_style_bg_opa(s_manage_badges[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_manage_badges[i], lv_color_hex(COLOR_TEXT), 0);
        lv_obj_clear_flag(s_manage_badges[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_manage_badges[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_manage_detail_host = make_card(manage, UI_MANAGE_DETAIL_X, UI_PANEL_Y,
                                     UI_MANAGE_DETAIL_W, UI_PANEL_H);
    lv_obj_set_style_radius(s_manage_detail_host, 28, 0);
    lv_obj_set_style_pad_all(s_manage_detail_host, 0, 0);
    /* Selection is established during build_ui(), but detail allocation waits
     * until Manage actually becomes the active top-level page. */
}

#if CONFIG_SOMNOTRACE_BOARD_QEMU
static void qemu_setup_preview_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) return;
    s_qemu_setup_preview_requested = true;
    ESP_LOGI(TAG, "QEMU first-run setup preview requested");
}
#endif

static void build_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BASE), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* A single low-cost native shadow supplies the handoff's ambient state
     * glow beneath the content and navigation. It is decorative only and can
     * never consume a touch. */
    s_ambient_glow = lv_obj_create(screen);
    lv_obj_set_pos(s_ambient_glow, 232, 576);
    lv_obj_set_size(s_ambient_glow, 560, 1);
    lv_obj_set_style_radius(s_ambient_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ambient_glow, 0, 0);
    lv_obj_set_style_pad_all(s_ambient_glow, 0, 0);
    lv_obj_set_style_bg_color(s_ambient_glow, lv_color_hex(COLOR_LIVE), 0);
    lv_obj_set_style_bg_opa(s_ambient_glow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_color(s_ambient_glow, lv_color_hex(COLOR_LIVE), 0);
    lv_obj_set_style_shadow_width(s_ambient_glow,
                                  UI_DECORATIVE_SHADOW_WIDTH(120), 0);
    lv_obj_set_style_shadow_opa(s_ambient_glow,
                                UI_DECORATIVE_SHADOW_OPA(LV_OPA_10), 0);
    lv_obj_clear_flag(s_ambient_glow,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *header = make_plain_container(screen, 0, 0, 1024, UI_HEADER_H);
    s_clock_label = make_label(header,
#if CONFIG_SOMNOTRACE_BOARD_QEMU
                               "02:14",
#else
                               "--:--",
#endif
                               26, 10, 116, FONT_CLOCK, COLOR_TEXT);
    s_date_label = make_label(header,
#if CONFIG_SOMNOTRACE_BOARD_QEMU
                              "Tue 2 Sep",
#else
                              "",
#endif
                              130, 23, 330,
                              FONT_BODY, COLOR_SECONDARY);
    /* This is the same command as System > Display > Off now, hosted by the
     * one header shared by Home, History, and Manage. Keep it left of even the
     * expanded degraded-state status capsule so neither target is crowded. */
    lv_obj_t *header_screen_off = make_touch_button(
        header, UI_HEADER_SCREEN_OFF_X, 7,
        UI_HEADER_SCREEN_OFF_W, STATUS_CAPSULE_H,
        "Screen off", COLOR_CONTROL, action_cb, UI_ACTION_SCREEN_OFF);
    lv_obj_set_style_radius(header_screen_off, 28, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(header_screen_off, 0),
                               FONT_BUTTON_COMPACT, 0);
    if (!screen_wake_input_available())
        lv_obj_add_state(header_screen_off, LV_STATE_DISABLED);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    /* A transparent emulator-only hit target over the deterministic clock
     * opens a fresh setup acceptance run.  A plain object is used instead of
     * relying on label hit-testing, which differs between LVGL/QEMU builds.
     * There is no production affordance and the default finished-shell
     * preview remains unchanged until this target is deliberately tapped. */
    lv_obj_t *setup_preview_hotspot = lv_obj_create(header);
    lv_obj_set_pos(setup_preview_hotspot, 8, 0);
    lv_obj_set_size(setup_preview_hotspot, 112, UI_HEADER_H);
    lv_obj_set_style_bg_opa(setup_preview_hotspot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(setup_preview_hotspot, 0, 0);
    lv_obj_set_style_pad_all(setup_preview_hotspot, 0, 0);
    lv_obj_clear_flag(setup_preview_hotspot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(setup_preview_hotspot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(setup_preview_hotspot, qemu_setup_preview_cb,
                        LV_EVENT_PRESSED, NULL);
#endif
    /* This only reveals a navigation overlay, so respond on touch-down like
     * the bottom navigation instead of waiting for a complete tap/release. */
    s_status_capsule = make_destination_button(
        header, 725, 7, 281, STATUS_CAPSULE_H, "", COLOR_CAPSULE,
        status_tray_open_cb, 0);
    set_destination_surface(s_status_capsule, COLOR_CAPSULE, LV_OPA_COVER);
    lv_obj_set_style_radius(s_status_capsule, 28, 0);
    lv_obj_set_style_bg_color(s_status_capsule, lv_color_hex(COLOR_CAPSULE), 0);
    lv_obj_set_style_shadow_width(s_status_capsule, 0, 0);
    s_ble_dot = make_status_dot(s_status_capsule, 18, 24, 9);
    s_ble_label = make_label(s_status_capsule, "AirSense", 35, 18, 70,
                             FONT_BODY, COLOR_SECONDARY);
    s_sd_dot = make_status_dot(s_status_capsule, 111, 24, 9);
    s_sd_label = make_label(s_status_capsule, "Card", 128, 18, 58,
                            FONT_BODY, COLOR_SECONDARY);
    s_wifi_dot = make_status_dot(s_status_capsule, 193, 24, 9);
    s_wifi_label = make_label(s_status_capsule, "Wi-Fi", 210, 18, 44,
                              FONT_BODY, COLOR_SECONDARY);
    s_status_divider = make_inner_card(s_status_capsule, 259, 18, 1, 20, 0);
    lv_obj_set_style_bg_color(s_status_divider, lv_color_hex(0x373d49), 0);
    s_status_chevron = make_down_chevron(s_status_capsule, 268, 23);
    layout_status_capsule();

    for (int i = 0; i < 3; ++i) {
        s_pages[i] = make_plain_container(screen, 0, UI_CONTENT_Y,
                                           1024, UI_CONTENT_H);
        lv_obj_set_style_bg_color(s_pages[i], lv_color_hex(COLOR_BASE), 0);
        lv_obj_set_style_bg_opa(s_pages[i], LV_OPA_TRANSP, 0);
    }
    build_home_page(s_pages[0]);
    build_history_page(s_pages[1]);
    build_manage_page(s_pages[2]);

    lv_obj_t *nav = make_plain_container(screen, 0, UI_CONTENT_Y + UI_CONTENT_H,
                                          1024, UI_NAV_H);
    lv_obj_set_style_bg_opa(nav, LV_OPA_TRANSP, 0);
    static const char *nav_names[] = { "Home", "History", "Manage" };
    for (int i = 0; i < 3; ++i) {
        s_nav_buttons[i] = make_destination_button(
            nav, UI_NAV_PILL_X + i * UI_NAV_PILL_STEP, UI_NAV_PILL_Y,
            UI_NAV_PILL_W, UI_NAV_PILL_H, nav_names[i], COLOR_CAPSULE,
            nav_cb, i);
        s_nav_labels[i] = lv_obj_get_child(s_nav_buttons[i], 0);
        lv_obj_set_style_text_font(s_nav_labels[i], FONT_BODY_LARGE, 0);
        lv_obj_set_style_radius(s_nav_buttons[i], 27, 0);
    }

    s_status_scrim = lv_obj_create(screen);
    lv_obj_set_pos(s_status_scrim, 0, 0);
    lv_obj_set_size(s_status_scrim, 1024, 600);
    lv_obj_set_style_bg_color(s_status_scrim,
                              lv_color_hex(UI_STATUS_SCRIM_COLOR), 0);
    lv_obj_set_style_bg_opa(s_status_scrim, UI_STATUS_SCRIM_OPA, 0);
    lv_obj_set_style_border_width(s_status_scrim, 0, 0);
    lv_obj_clear_flag(s_status_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_status_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_status_scrim, status_tray_close_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_status_scrim, LV_OBJ_FLAG_HIDDEN);

    s_status_tray = make_card(screen, 510, 74, 496, 462);
    lv_obj_set_style_radius(s_status_tray, 30, 0);
    lv_obj_set_style_pad_all(s_status_tray, 0, 0);
    make_label(s_status_tray, "System status", 24, 16, 220,
               FONT_ROW_TITLE, COLOR_TEXT);
    lv_obj_t *updated = make_label(s_status_tray, "Updated just now", 320, 18, 150,
                                   FONT_BODY_SMALL, COLOR_SECONDARY);
    lv_obj_set_style_text_align(updated, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *tray_scroll = make_plain_container(s_status_tray, 8, 56, 480, 398);
    /* Five 66 px rows plus their gaps fit in this viewport. Keeping the tray
     * fixed avoids elastic scrolling and a full tray redraw on stray drags. */
    static const char *tray_titles[] = {
        "AirSense 11", "microSD card", "Wi-Fi", "Uploads", "O2 Ring"
    };
    lv_obj_t **details[] = {
        &s_status_tray_as11, &s_status_tray_sd, &s_status_tray_wifi,
        &s_status_tray_upload, &s_status_tray_ox
    };
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *row = make_inner_card(tray_scroll, 0, i * 70, 464, 66, 22);
        s_status_tray_dots[i] = make_status_dot(row, 14, 27, 12);
        make_label(row, tray_titles[i], 42, 9, 300,
                   FONT_BODY_SMALL, COLOR_TEXT);
        *details[i] = make_label(row, "Checking...", 42, 34, 320,
                                 FONT_BODY_SMALL, COLOR_SECONDARY);
    }
    lv_obj_add_flag(s_status_tray, LV_OBJ_FLAG_HIDDEN);

    s_notice_card = make_card(screen, 18, 14, 988, 76);
    lv_obj_set_style_radius(s_notice_card, 30, 0);
    lv_obj_set_style_pad_all(s_notice_card, 0, 0);
    lv_obj_set_style_bg_color(s_notice_card, lv_color_hex(COLOR_CONTROL), 0);
    lv_obj_set_style_bg_grad_dir(s_notice_card, LV_GRAD_DIR_NONE, 0);
    s_notice_mark = make_status_dot(s_notice_card, 22, 19, 38);
    set_dot_tone(s_notice_mark, COLOR_LIVE, true);
    lv_obj_t *notice_symbol = make_label(s_notice_mark, "i", 0, 0, 38,
                                         FONT_SCREEN_TITLE, COLOR_BASE);
    lv_obj_set_style_text_align(notice_symbol, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(notice_symbol);
    s_notice_label = make_label(s_notice_card, "", 76, 25, 880,
                                FONT_ROW_TITLE, COLOR_TEXT);
    lv_obj_add_flag(s_notice_card, LV_OBJ_FLAG_HIDDEN);

    s_alert_banner = make_card(screen, 18, 14, 988, 88);
    lv_obj_set_style_radius(s_alert_banner, 30, 0);
    lv_obj_set_style_pad_all(s_alert_banner, 0, 0);
    lv_obj_set_style_bg_color(s_alert_banner, lv_color_hex(0xa71a1b), 0);
    lv_obj_set_style_bg_grad_color(s_alert_banner, lv_color_hex(0x77020c), 0);
    lv_obj_set_style_shadow_color(s_alert_banner, lv_color_hex(0x4b0004), 0);
    lv_obj_set_style_shadow_width(s_alert_banner,
                                  UI_DECORATIVE_SHADOW_WIDTH(32), 0);
    lv_obj_set_style_shadow_ofs_y(s_alert_banner, 12, 0);
    lv_obj_set_style_shadow_opa(s_alert_banner,
                                UI_DECORATIVE_SHADOW_OPA(LV_OPA_60), 0);
    s_alert_mark = make_status_dot(s_alert_banner, 22, 22, 44);
    set_dot_tone(s_alert_mark, 0xff7837, true);
    lv_obj_t *alert_symbol = make_label(s_alert_mark, "!", 0, 0, 44,
                                        FONT_SCREEN_TITLE, COLOR_BASE);
    lv_obj_set_style_text_align(alert_symbol, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(alert_symbol);
    s_alert_label = make_label(s_alert_banner, "", 84, 15, 640,
                               FONT_SCREEN_TITLE, COLOR_TEXT);
    s_alert_subtitle = make_label(s_alert_banner, "", 84, 49, 640,
                                  FONT_BODY_SMALL, 0xf8c4c0);
    s_alert_ack_button = make_touch_button(s_alert_banner, 736, 11, 230, 66,
                                            "Acknowledge", COLOR_INVERSE,
                                            action_cb, 2);
    lv_obj_set_style_radius(s_alert_ack_button, 33, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_alert_ack_button, 0),
                                lv_color_hex(0x68191a), 0);
    lv_obj_add_flag(s_alert_banner, LV_OBJ_FLAG_HIDDEN);

    /* Dialog backdrops also live on LVGL's top layer. Parenting the wake
     * surface there ensures it remains above a confirmation dialog while the
     * physical backlight is dark. */
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

    /* Establish the ordinary modal order once, before the first frame. Runtime
     * reordering invalidates the entire screen; visibility toggles are enough.
     * Notices, alerts, and the keyboard can still move above this pair when
     * they become active. */
    lv_obj_move_foreground(s_status_scrim);
    lv_obj_move_foreground(s_status_tray);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    /* Keep the emulator-only selector reachable even while the deterministic
     * startup notice overlaps the clock. It is a screen child, so the real
     * setup surface on lv_layer_top() still replaces it completely. */
    lv_obj_move_foreground(setup_preview_hotspot);
#endif

    set_manage_section(MANAGE_LOGS);
    set_active_page(0);
    lv_obj_invalidate(screen);
}


/* Persistent rail state must update before any lazy detail returns. Read only
 * bounded RAM observations here: no NVS, filesystem traversal or service I/O. */
static void refresh_manage_rail(const ui_state_t *state)
{
    uint32_t dots[MANAGE_SECTION_COUNT];
    unsigned badges[MANAGE_SECTION_COUNT] = {0};
    for (size_t i = 0; i < MANAGE_SECTION_COUNT; ++i) dots[i] = COLOR_TERTIARY;
    if (s_touch_services_ready) {
        dots[MANAGE_DEVICES] = state->paired ? COLOR_LIVE : COLOR_AMBER;
        dots[MANAGE_CONNECTIVITY] = state->wifi ? COLOR_LIVE : COLOR_AMBER;
        dots[MANAGE_STORAGE] = state->sd_ready ? COLOR_LIVE : COLOR_FAULT;
    }

    alert_state_t alert = therapy_alert_get_state();
    bool active_alert = therapy_alert_is_actionable(alert);
    /* Armed alone establishes the schedule, not push deliverability. */
    dots[MANAGE_ALERTS] = active_alert ? COLOR_FAULT :
                          alert == ALERT_ARMED ? COLOR_AMBER : COLOR_TERTIARY;
    badges[MANAGE_ALERTS] = active_alert ? 1U : 0U;

    uploader_progress_snapshot_t uploads;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    uploads = s_render_services->upload_progress;
    bool uploads_known = s_render_services->upload_progress_result == ESP_OK;
#else
    bool uploads_known = uploader_get_progress_snapshot(&uploads) == ESP_OK;
#endif
    unsigned configured = 0;
    bool retrying = false;
    for (size_t i = 0; uploads_known && i < uploads.backend_count &&
                       i < UPLOADER_PROGRESS_MAX_BACKENDS; ++i) {
        const uploader_backend_progress_t *backend = &uploads.backends[i];
        if (!backend->configured) continue;
        configured++;
        if (backend->error_valid) badges[MANAGE_UPLOADS]++;
        if (backend->state == UPLOADER_BACKEND_COOLDOWN) retrying = true;
    }
    dots[MANAGE_UPLOADS] = badges[MANAGE_UPLOADS] ? COLOR_FAULT :
                           retrying ? COLOR_AMBER :
                           configured ? COLOR_LIVE : COLOR_TERTIARY;

    controller_diagnostics_snapshot_t controllers;
    controller_diagnostics_get_snapshot(&controllers);
    bool controller_failed = false;
    for (size_t i = 0; i < CONTROLLER_OPERATION_COUNT; ++i)
        if (controllers.operations[i].observed &&
            controllers.operations[i].last_result != ESP_OK)
            controller_failed = true;
    bool controllers_known = controllers.initialized &&
        controllers.operations[CONTROLLER_PANEL_INIT].observed &&
        controllers.operations[CONTROLLER_TOUCH_INIT].observed;
    dots[MANAGE_SYSTEM] = controller_failed ? COLOR_FAULT :
        !controllers_known || !s_touch_services_ready ? COLOR_TERTIARY :
        !state->wifi || !state->sd_ready || !state->paired ||
        badges[MANAGE_UPLOADS] || active_alert ? COLOR_AMBER : COLOR_LIVE;

    log_stream_retained_info_t logs;
    esp_err_t log_result = log_stream_retained_get_info(&logs);
    dots[MANAGE_LOGS] = log_result == ESP_OK && logs.available ?
        (touch_logs_controller_is_paused() ? COLOR_AMBER : COLOR_LIVE) :
        s_touch_services_ready ? COLOR_FAULT : COLOR_TERTIARY;
    for (size_t i = 0; i < MANAGE_SECTION_COUNT; ++i) {
        if (!s_manage_buttons[i]) continue;
        set_dot_tone(s_manage_dots[i], dots[i], dots[i] != COLOR_TERTIARY);
        set_hidden(s_manage_badges[i], badges[i] == 0);
        if (badges[i]) set_label_text_fmt_if_changed(s_manage_badges[i], "%u", badges[i]);
    }
}

static void refresh_secondary_pages(const ui_state_t *state, int active_tab)
{
    reap_retired_logs_destination();
    if (active_tab != 2) return;
    refresh_manage_rail(state);
    int section = s_rendered_manage_section;
    if (section == MANAGE_LOGS) {
        touch_logs_controller_refresh(state->sd_ready);
        return;
    }
}

static void resync_flow_visual(const ui_state_t *state)
{
    memset(s_flow_visual, 0, sizeof(s_flow_visual));
    unsigned count = state->flow_count < FLOW_POINTS ? state->flow_count
                                                      : FLOW_POINTS;
    s_flow_visual_count = count;
    unsigned destination = FLOW_POINTS - count;
    unsigned source = (state->flow_head + FLOW_POINTS - count) % FLOW_POINTS;
    for (unsigned i = 0; i < count; ++i) {
        s_flow_visual[destination + i] = state->flow[source];
        source = (source + 1) % FLOW_POINTS;
    }
}

static void append_flow_visual(int16_t sample)
{
    memmove(s_flow_visual, s_flow_visual + 1,
            (FLOW_POINTS - 1) * sizeof(s_flow_visual[0]));
    s_flow_visual[FLOW_POINTS - 1] = sample;
    if (s_flow_visual_count < FLOW_POINTS) s_flow_visual_count++;
}

/* Check and request idle sleep under the same state lock used by therapy and
 * SoftAP transitions. This prevents a stale UI snapshot from overwriting a
 * simultaneous request that must keep the display awake. */
static bool request_idle_sleep_if_due(const device_settings_t *settings,
                                      bool visual_alarm, int64_t now_us)
{
    if (!settings || visual_alarm || !screen_wake_input_available()) return false;

    bool requested = false;
    portENTER_CRITICAL(&s_state_lock);
    bool policy_prefers_off =
        settings->backlight_mode == BACKLIGHT_MODE_ALWAYS_OFF ||
        (s_state.therapy && settings->backlight_mode == BACKLIGHT_MODE_OFF_THRP);
    uint16_t effective_timeout_s = policy_prefers_off
                                       ? POLICY_PEEK_TIMEOUT_S
                                       : (!s_state.therapy
                                              ? settings->screen_timeout_s : 0);
    bool elapsed = s_last_touch_activity_us > 0 &&
                   effective_timeout_s > 0 &&
                   now_us >= s_last_touch_activity_us &&
                   now_us - s_last_touch_activity_us >=
                       (int64_t)effective_timeout_s * 1000000LL;
    if (s_backlight && s_backlight_requested && !s_state.notice_critical &&
        !s_backlight_force_on && !s_setup_backlight_force_on &&
        !s_temporarily_awake && elapsed) {
        s_backlight_requested = false;
        s_last_off_request_us = esp_timer_get_time();
        ++s_backlight_revision;
        requested = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}

static void update_ui(void)
{
    static TickType_t last_text_update;
    static TickType_t last_service_snapshot;
    static unsigned seen_flow_version;
    static bool plotted_flow_live;
    static bool home_was_active;
    static bool status_tray_was_open;
    static int64_t flow_presented_us;
    static bool alarm_forced_awake;
    ui_state_t state;
    bool therapy_command_busy;
    bool therapy_command_target;
    bool alert_ack_busy;
    bool backlight;
    TickType_t now_ticks = xTaskGetTickCount();
    int64_t now_us = esp_timer_get_time();

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (s_qemu_setup_preview_requested &&
        !bsp_display_first_run_setup_active()) {
        s_qemu_setup_preview_requested = false;
        esp_err_t preview = bsp_display_qemu_start_setup_preview();
        if (preview != ESP_OK) {
            ESP_LOGE(TAG, "could not open QEMU setup preview: %s",
                     esp_err_to_name(preview));
        } else {
            return;
        }
    }
#endif

    /* The setup surface is an opaque, navigation-free top layer.  Only the
     * display task copies controller state into LVGL, and only when its
     * generation changes, so a slow Wi-Fi/BLE worker can never stall touch or
     * trigger periodic full-pane rebuilds. */
    if (bsp_display_first_run_setup_active()) {
        first_run_setup_ui_live_t live;
        uint32_t generation = 0;
        if (first_run_setup_controller_snapshot(&live, &generation) &&
            generation != s_first_run_setup_seen_generation) {
            esp_err_t update = first_run_setup_ui_update(&live);
            if (update == ESP_OK)
                s_first_run_setup_seen_generation = generation;
            else
                ESP_LOGE(TAG, "setup UI update failed: %s",
                         esp_err_to_name(update));
        }
        if (first_run_setup_controller_take_finished()) {
            first_run_setup_ui_destroy();
            first_run_setup_controller_stop();
            s_first_run_setup_seen_generation = 0;
            portENTER_CRITICAL(&s_state_lock);
            s_first_run_setup_active = false;
            s_setup_backlight_force_on = false;
            portEXIT_CRITICAL(&s_state_lock);
            bsp_display_apply_backlight_policy(false);
            lv_obj_invalidate(lv_scr_act());
            ESP_LOGI(TAG, "first-run setup finished; normal shell revealed");
        } else {
            return;
        }
    }

    bool refresh_services = last_service_snapshot == 0 ||
                            now_ticks - last_service_snapshot >= pdMS_TO_TICKS(250);
    portENTER_CRITICAL(&s_state_lock);
    if (s_state.notice_expires_us > 0 &&
        esp_timer_get_time() >= s_state.notice_expires_us) {
        s_state.notice[0] = '\0';
        s_state.notice_expires_us = 0;
        s_state.notice_critical = false;
    }
    state = s_state;
    if (refresh_services) *s_render_services = s_services;
    therapy_command_busy = s_therapy_command_busy;
    therapy_command_target = s_therapy_command_target;
    alert_ack_busy = s_alert_ack_busy;
    backlight = s_backlight;
    portEXIT_CRITICAL(&s_state_lock);
    if (refresh_services) last_service_snapshot = now_ticks;

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    uint8_t requested_tab;
    portENTER_CRITICAL(&s_state_lock);
    requested_tab = s_qemu_requested_tab;
    s_qemu_requested_tab = UINT8_MAX;
    portEXIT_CRITICAL(&s_state_lock);
    if (requested_tab < 3) set_active_page(requested_tab);
#endif

    alert_state_t alert_state = therapy_alert_get_state();
    bool alert_actionable = therapy_alert_is_actionable(alert_state);
    bool visual_alarm = alert_actionable || state.notice_critical;
    if (visual_alarm && !backlight) {
        /* The 7B has no onboard speaker, so the persistent visual alarm must
         * wake the panel. The topmost wake layer still consumes the first tap. */
        bsp_display_set_backlight(true);
        apply_pending_backlight_locked();
        portENTER_CRITICAL(&s_state_lock);
        backlight = s_backlight;
        portEXIT_CRITICAL(&s_state_lock);
    }
    if (visual_alarm) {
        alarm_forced_awake = true;
    } else if (alarm_forced_awake) {
        alarm_forced_awake = false;
        portENTER_CRITICAL(&s_state_lock);
        s_last_touch_activity_us = now_us;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_apply_backlight_policy(false);
        apply_pending_backlight_locked();
        portENTER_CRITICAL(&s_state_lock);
        backlight = s_backlight;
        portEXIT_CRITICAL(&s_state_lock);
    }

    if (!screen_wake_input_available() && !backlight) {
        /* A touch controller can fail after a successful boot. Never leave a
         * no-button device dark once the wake path becomes unhealthy. */
        bsp_display_set_backlight(true);
        apply_pending_backlight_locked();
        portENTER_CRITICAL(&s_state_lock);
        backlight = s_backlight;
        portEXIT_CRITICAL(&s_state_lock);
    }

    device_settings_t display_settings;
    device_settings_snapshot(&display_settings);
    if (request_idle_sleep_if_due(&display_settings, visual_alarm, now_us)) {
        apply_pending_backlight_locked();
        portENTER_CRITICAL(&s_state_lock);
        backlight = s_backlight;
        portEXIT_CRITICAL(&s_state_lock);
    }

    if (alert_ack_busy && !alert_actionable) {
        portENTER_CRITICAL(&s_state_lock);
        s_alert_ack_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        alert_ack_busy = false;
        bsp_display_set_notice("Alert acknowledged");
    }

    /* Keep LVGL responsive for the wake overlay but avoid chart/label churn
     * while the panel is intentionally dark. */
    if (!backlight) {
        /* Waking Home is a visual re-entry: resynchronise rather than replaying
         * samples accumulated while the panel was intentionally dark. */
        home_was_active = false;
        return;
    }

    int active_tab = s_active_page;
    bool entered_home = active_tab == 0 && !home_was_active;
    home_was_active = active_tab == 0;
    bool status_tray_open = !lv_obj_has_flag(s_status_tray, LV_OBJ_FLAG_HIDDEN);
    bool status_tray_just_closed = status_tray_was_open && !status_tray_open;
    status_tray_was_open = status_tray_open;
    bool flow_live = state.therapy && state.flow_count >= FLOW_READY_POINTS &&
                     state.flow_sample_us > 0 &&
                     now_us - state.flow_sample_us < 2500000;
    if (active_tab == 0 && !status_tray_open) {
        bool chart_dirty = false;
        if (!flow_live) {
            if (plotted_flow_live || s_flow_visual_count > 0) {
                memset(s_flow_visual, 0, sizeof(s_flow_visual));
                s_flow_visual_count = 0;
                chart_dirty = true;
            }
            seen_flow_version = state.flow_version;
            flow_presented_us = now_us;
        } else {
            unsigned pending = state.flow_version - seen_flow_version;
            /* AirSense delivers five positioned samples per 200 ms. Pace by
             * elapsed source intervals, with a bounded backlog after stalls. */
            if (!plotted_flow_live || entered_home || status_tray_just_closed ||
                pending > state.flow_count ||
                pending > FLOW_RESYNC_THRESHOLD) {
                resync_flow_visual(&state);
                seen_flow_version = state.flow_version;
                flow_presented_us = now_us;
                chart_dirty = true;
            } else if (pending > 0) {
                unsigned consume = live_flow_presentation_due(&flow_presented_us,
                                                               now_us, pending);
                unsigned source = (state.flow_head + FLOW_POINTS - pending) %
                                  FLOW_POINTS;
                for (unsigned i = 0; i < consume; ++i) {
                    append_flow_visual(state.flow[source]);
                    source = (source + 1) % FLOW_POINTS;
                }
                seen_flow_version += consume;
                chart_dirty = consume > 0;
            } else {
                flow_presented_us = now_us;
            }
        }
        if (flow_live != plotted_flow_live) chart_dirty = true;
        s_flow_visual_live = flow_live;
        plotted_flow_live = flow_live;
        if (chart_dirty) lv_obj_invalidate(s_chart);
    }
    if (active_tab == 1) apply_history_controller_if_needed();

    if (now_ticks - last_text_update < pdMS_TO_TICKS(500)) return;
    last_text_update = now_ticks;

    bool storage_fault = state.notice_critical && strstr(state.notice, "microSD");
    bool storage_degraded = !state.sd_ready || state.storage_near_full;
    bool has_flow_window = state.flow_count >= FLOW_READY_POINTS;
    bool therapy_past_startup = state.therapy_start_us > 0 &&
                                now_us - state.therapy_start_us > 10000000LL;
    bool airsense_stale = state.paired && state.therapy && !flow_live &&
                          (state.flow_sample_us > 0 || therapy_past_startup);
    uint32_t sd_tone = storage_fault ? COLOR_FAULT :
                       storage_degraded ? COLOR_AMBER : COLOR_LIVE;
    uint32_t as_tone = airsense_stale ? COLOR_FAULT :
                       state.paired ? COLOR_LIVE : COLOR_AMBER;
    uint32_t wifi_tone = state.wifi ? COLOR_LIVE : COLOR_AMBER;
    int card_used_pct = -1;
    if (s_render_services->storage_total > 0 &&
        s_render_services->storage_free <= s_render_services->storage_total) {
        card_used_pct = (int)(((s_render_services->storage_total -
                                s_render_services->storage_free) * 100ULL) /
                              s_render_services->storage_total);
    }
    bool status_capsule_layout_dirty = false;
    if (storage_fault) {
        status_capsule_layout_dirty |=
            set_label_text_if_changed(s_sd_label, "Card fault");
    } else if (!state.sd_ready) {
        status_capsule_layout_dirty |=
            set_label_text_if_changed(s_sd_label, "No card");
    } else if (state.storage_near_full && card_used_pct >= 0) {
        status_capsule_layout_dirty |= set_label_text_fmt_if_changed(
            s_sd_label, "Card %d%%", card_used_pct);
    } else if (state.storage_near_full) {
        status_capsule_layout_dirty |=
            set_label_text_if_changed(s_sd_label, "Card low");
    } else {
        status_capsule_layout_dirty |=
            set_label_text_if_changed(s_sd_label, "Card");
    }
    set_style_color_if_changed(s_sd_label, LV_STYLE_TEXT_COLOR,
                               sd_tone == COLOR_LIVE ? COLOR_SECONDARY : sd_tone,
                               0);
    status_capsule_layout_dirty |= set_label_text_if_changed(
        s_wifi_label, state.wifi ? "Wi-Fi" : "Offline");
    set_style_color_if_changed(s_wifi_label, LV_STYLE_TEXT_COLOR,
                               state.wifi ? COLOR_SECONDARY : COLOR_AMBER, 0);
    status_capsule_layout_dirty |= set_label_text_if_changed(
        s_ble_label, state.paired ? "AirSense" : "Unpaired");
    set_style_color_if_changed(s_ble_label, LV_STYLE_TEXT_COLOR,
                               as_tone == COLOR_LIVE ? COLOR_SECONDARY : as_tone,
                               0);
    set_dot_tone(s_sd_dot, sd_tone, true);
    set_dot_tone(s_wifi_dot, wifi_tone, true);
    set_dot_tone(s_ble_dot, as_tone, true);
    if (status_capsule_layout_dirty) layout_status_capsule();
    uint32_t capsule_tone = storage_fault || airsense_stale ? 0x53151a :
                                storage_degraded || !state.wifi || !state.paired
                                    ? 0x443817 : COLOR_CAPSULE;
    set_style_color_if_changed(s_status_capsule, LV_STYLE_BG_COLOR,
                               capsule_tone, 0);
    uint32_t ambient_tone = alert_actionable || storage_fault || airsense_stale
                                ? COLOR_FAULT
                            : storage_degraded || !state.wifi || !state.paired
                                ? COLOR_AMBER
                            : state.therapy ? COLOR_LIVE : 0x454b58;
    set_style_color_if_changed(s_ambient_glow, LV_STYLE_BG_COLOR,
                               ambient_tone, 0);
    set_style_color_if_changed(s_ambient_glow, LV_STYLE_SHADOW_COLOR,
                               ambient_tone, 0);
    lv_opa_t ambient_opa = state.therapy || alert_actionable ? LV_OPA_20
                                                             : LV_OPA_10;
    set_style_num_if_changed(s_ambient_glow, LV_STYLE_BG_OPA,
                             LV_OPA_TRANSP, 0);
    set_style_num_if_changed(s_ambient_glow, LV_STYLE_SHADOW_OPA,
                             UI_DECORATIVE_SHADOW_OPA(ambient_opa), 0);

    set_label_text_if_changed(s_status_tray_as11,
                              airsense_stale
                                  ? "Connection lost - reconnecting"
                              : state.paired ? "Connected and ready"
                                             : "Not paired - pair to start therapy");
    if (storage_fault)
        set_label_text_if_changed(s_status_tray_sd,
                                  "Write fault - this night may be incomplete");
    else if (!state.sd_ready)
        set_label_text_if_changed(s_status_tray_sd,
                                  "No card fitted - nothing is recorded");
    else if (state.storage_near_full) {
        if (s_render_services->storage_total > 0) {
            set_label_text_fmt_if_changed(
                s_status_tray_sd, "Nearly full · about %u nights",
                estimated_airsense_nights(
                    s_render_services->storage_free));
        } else {
            set_label_text_if_changed(s_status_tray_sd,
                                      "Nearly full · free space soon");
        }
    } else if (s_render_services->storage_total > 0) {
        set_label_text_fmt_if_changed(
            s_status_tray_sd, "About %u AirSense-only nights",
            estimated_airsense_nights(s_render_services->storage_free));
    } else {
        set_label_text_if_changed(s_status_tray_sd, "Ready for recording");
    }
    set_label_text_if_changed(s_status_tray_wifi,
                              state.wifi
                                  ? "Connected - local dashboard available"
                                  : "Offline - uploads paused");
    set_label_text_fmt_if_changed(
        s_status_tray_upload, "%d pending · %s",
        s_render_services->upload_pending,
        s_render_services->upload_state[0]
            ? s_render_services->upload_state : "idle");
    bool ring_paired = false;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    ring_paired = true;
#else
    if (s_ox_service_ready) {
        const char *ring_status = oximeter_get_status();
        ring_paired = !strcmp(ring_status, OX_STATUS_PAIRED) ||
                      !strcmp(ring_status, OX_STATUS_MONITORING) ||
                      !strcmp(ring_status, OX_STATUS_PULLING);
    }
#endif
    set_label_text_if_changed(s_status_tray_ox,
                              ring_paired ? "Paired - monitoring available"
                                          : "Optional - not paired");
    uint32_t tray_tones[] = {
        as_tone, sd_tone, wifi_tone,
        s_render_services->upload_pending > 0 && !state.wifi ? COLOR_AMBER
                                                             : COLOR_LIVE,
        ring_paired ? COLOR_LIVE : COLOR_DISABLED,
    };
    for (int i = 0; i < 5; ++i)
        set_dot_tone(s_status_tray_dots[i], tray_tones[i],
                     tray_tones[i] != COLOR_DISABLED);

    bool recording;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    recording = state.therapy && state.sd_ready;
#else
    recording = sd_storage_recording_active();
#endif
    /* The rich History model owns card-derived night summaries. Home does not
     * mirror that large model into its periodic service snapshot. */
    char stopped_runtime[20] = "—";
    const char *therapy_label = therapy_command_busy
                                    ? (therapy_command_target
                                           ? "Starting therapy"
                                           : "Stopping therapy")
                                    : airsense_stale
                                          ? "Therapy status unknown"
                                    : state.therapy ? "Therapy active"
                                                    : state.paired
                                                          ? "Therapy stopped"
                                                          : "No machine paired";
    set_label_text_if_changed(s_therapy_label, therapy_label);
    set_style_color_if_changed(s_therapy_label, LV_STYLE_TEXT_COLOR,
                               state.therapy ? 0xc7fbfb : COLOR_TEXT, 0);
    set_label_text_if_changed(
        s_therapy_subtitle,
        therapy_command_busy
            ? (therapy_command_target ? "Sending start command..."
                                      : "Sending stop command...")
        : !state.paired ? "AirSense is not paired"
        : airsense_stale ? "AirSense data is stale - reconnecting"
        : state.therapy && recording ? "Recording to card"
        : state.therapy && (!state.sd_ready || storage_fault)
              ? "microSD unavailable - therapy continues"
        : state.therapy ? "Preparing recording"
                        : "Ready when you are");
    set_style_color_if_changed(s_therapy_hero, LV_STYLE_BG_COLOR,
                               state.therapy ? 0x0b2d32 : COLOR_PANEL, 0);
    set_style_num_if_changed(s_therapy_hero, LV_STYLE_BG_GRAD_DIR,
                             LV_GRAD_DIR_NONE, 0);
    set_style_color_if_changed(s_therapy_hero, LV_STYLE_SHADOW_COLOR,
                               state.therapy ? 0x008f96 : 0x010207, 0);
    set_style_num_if_changed(s_therapy_hero, LV_STYLE_SHADOW_WIDTH,
                             UI_DECORATIVE_SHADOW_WIDTH(
                                 state.therapy ? 30 : 22),
                             0);
    set_style_num_if_changed(s_therapy_hero, LV_STYLE_SHADOW_OPA,
                             UI_DECORATIVE_SHADOW_OPA(
                                 state.therapy ? LV_OPA_20 : LV_OPA_40),
                             0);
    lv_coord_t orb_core_size = state.therapy ? 26 : 18;
    if (lv_obj_get_width(s_therapy_orb_core) != orb_core_size ||
        lv_obj_get_height(s_therapy_orb_core) != orb_core_size) {
        lv_obj_set_size(s_therapy_orb_core, orb_core_size, orb_core_size);
    }
    set_style_color_if_changed(
        s_therapy_orb_core, LV_STYLE_BG_COLOR,
        state.therapy ? COLOR_LIVE : state.paired ? 0x636975 : COLOR_AMBER, 0);
    set_style_color_if_changed(
        s_therapy_orb, LV_STYLE_BORDER_COLOR,
        state.therapy ? COLOR_LIVE
                      : state.paired ? COLOR_TERTIARY : COLOR_AMBER,
        0);
    set_style_color_if_changed(s_therapy_orb, LV_STYLE_BG_COLOR,
                               state.therapy ? 0x005057 : COLOR_CONTROL, 0);
    set_style_color_if_changed(s_therapy_orb, LV_STYLE_BG_GRAD_COLOR,
                               state.therapy ? 0x172632 : COLOR_CONTROL, 0);
    set_style_num_if_changed(s_therapy_orb, LV_STYLE_BG_GRAD_DIR,
                             state.therapy ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE,
                             0);
    set_style_color_if_changed(s_therapy_orb, LV_STYLE_SHADOW_COLOR,
                               state.therapy ? COLOR_LIVE : COLOR_DISABLED, 0);
    set_style_num_if_changed(s_therapy_orb, LV_STYLE_SHADOW_WIDTH,
                             UI_DECORATIVE_SHADOW_WIDTH(
                                 state.therapy ? 24 : 0),
                             0);
    set_style_num_if_changed(s_therapy_orb, LV_STYLE_SHADOW_OPA,
                             UI_DECORATIVE_SHADOW_OPA(
                                 state.therapy ? LV_OPA_40 : LV_OPA_TRANSP),
                             0);
    set_label_text_if_changed(
        s_therapy_button_label,
        therapy_command_busy
            ? (therapy_command_target ? "Starting..." : "Stopping...")
        : !state.paired ? "AirSense not paired"
                        : (state.therapy ? "Stop therapy" : "Start therapy"));
    bool therapy_button_disabled = !state.paired || therapy_command_busy;
    if (therapy_button_disabled)
        lv_obj_add_state(s_therapy_button, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(s_therapy_button, LV_STATE_DISABLED);
    if (!therapy_button_disabled && !state.therapy) {
        set_style_color_if_changed(s_therapy_button, LV_STYLE_BG_COLOR,
                                   0x3bf4f4, 0);
        set_style_color_if_changed(s_therapy_button, LV_STYLE_BG_GRAD_COLOR,
                                   0x00c8ce, 0);
        set_style_num_if_changed(s_therapy_button, LV_STYLE_BG_GRAD_DIR,
                                 LV_GRAD_DIR_VER, 0);
        set_style_num_if_changed(s_therapy_button, LV_STYLE_BORDER_WIDTH, 0, 0);
        set_style_color_if_changed(s_therapy_button, LV_STYLE_SHADOW_COLOR,
                                   COLOR_LIVE, 0);
        set_style_num_if_changed(
            s_therapy_button, LV_STYLE_SHADOW_WIDTH,
            UI_DECORATIVE_SHADOW_WIDTH(28), 0);
        set_style_num_if_changed(s_therapy_button, LV_STYLE_SHADOW_OPA,
                                 UI_DECORATIVE_SHADOW_OPA(LV_OPA_30), 0);
        set_style_color_if_changed(s_therapy_button_label, LV_STYLE_TEXT_COLOR,
                                   0x062a2c, 0);
    } else {
        set_style_color_if_changed(s_therapy_button, LV_STYLE_BG_COLOR,
                                   0x2c323e, 0);
        set_style_num_if_changed(s_therapy_button, LV_STYLE_BG_GRAD_DIR,
                                 LV_GRAD_DIR_NONE, 0);
        set_style_num_if_changed(
            s_therapy_button, LV_STYLE_BORDER_WIDTH,
            state.therapy && !therapy_command_busy ? 2 : 1, 0);
        set_style_color_if_changed(s_therapy_button, LV_STYLE_BORDER_COLOR,
                                   0x6d7584, 0);
        set_style_num_if_changed(s_therapy_button, LV_STYLE_SHADOW_WIDTH, 0, 0);
        set_style_color_if_changed(
            s_therapy_button_label, LV_STYLE_TEXT_COLOR,
            therapy_button_disabled ? COLOR_DISABLED : COLOR_TEXT, 0);
    }
    bool leak_live = state.therapy && state.leak_sample_us > 0 &&
                     now_us - state.leak_sample_us < METRIC_STALE_US &&
                     isfinite(state.leak);
    bool pressure_live = state.therapy && state.pressure_sample_us > 0 &&
                         now_us - state.pressure_sample_us < METRIC_STALE_US &&
                         isfinite(state.pressure);
    bool respiratory_rate_live =
        state.therapy && state.respiratory_rate_sample_us > 0 &&
        now_us - state.respiratory_rate_sample_us < METRIC_STALE_US &&
        isfinite(state.respiratory_rate);
    bool flow_limitation_live =
        state.therapy && state.flow_limitation_sample_us > 0 &&
        now_us - state.flow_limitation_sample_us < METRIC_STALE_US &&
        isfinite(state.flow_limitation);
    if (leak_live)
        set_label_text_fmt_if_changed(s_leak_label, "%.1f", state.leak);
    else
        set_label_text_if_changed(s_leak_label, "—");
    if (pressure_live)
        set_label_text_fmt_if_changed(s_pressure_label, "%.1f", state.pressure);
    else
        set_label_text_if_changed(s_pressure_label, "—");
    if (respiratory_rate_live)
        set_label_text_fmt_if_changed(s_resp_label, "%.0f",
                                      state.respiratory_rate);
    else
        set_label_text_if_changed(s_resp_label, "—");
    if (flow_limitation_live)
        set_label_text_fmt_if_changed(s_flow_lim_label, "%.2f",
                                      state.flow_limitation);
    else
        set_label_text_if_changed(s_flow_lim_label, "—");

    int bar_values[4] = {
        pressure_live
            ? (int)((state.pressure - 4.0f) * 100.0f / 16.0f) : 0,
        leak_live
            ? (int)(state.leak * 100.0f / 24.0f) : 0,
        respiratory_rate_live
            ? (int)((state.respiratory_rate - 8.0f) * 100.0f / 16.0f) : 0,
        flow_limitation_live ? (int)(state.flow_limitation * 100.0f) : 0,
    };
    const bool metric_live[] = {
        pressure_live, leak_live, respiratory_rate_live, flow_limitation_live
    };
    for (int i = 0; i < 4; ++i) {
        if (bar_values[i] < 0) bar_values[i] = 0;
        if (bar_values[i] > 100) bar_values[i] = 100;
        if (lv_bar_get_value(s_metric_bars[i]) != bar_values[i])
            lv_bar_set_value(s_metric_bars[i], bar_values[i], LV_ANIM_OFF);
        set_style_color_if_changed(s_metric_bars[i], LV_STYLE_BG_COLOR,
                                   metric_live[i] ? COLOR_LIVE : COLOR_DISABLED,
                                   LV_PART_INDICATOR);
    }
    set_style_color_if_changed(
        s_metric_bars[1], LV_STYLE_BG_COLOR,
        leak_live && state.leak > 24.0f
            ? COLOR_AMBER
            : leak_live ? COLOR_LIVE : COLOR_DISABLED,
        LV_PART_INDICATOR);
    lv_obj_t *metric_labels[] = {
        s_pressure_label, s_leak_label, s_resp_label, s_flow_lim_label
    };
    for (int i = 0; i < 4; ++i)
        set_style_color_if_changed(metric_labels[i], LV_STYLE_TEXT_COLOR,
                                   metric_live[i] ? COLOR_TEXT : COLOR_DISABLED,
                                   0);

    int64_t elapsed = 0;
    if (state.therapy && state.therapy_start_us != 0) {
        elapsed = (esp_timer_get_time() - state.therapy_start_us) / 1000000;
        if (elapsed < 0) elapsed = 0;
    }
    set_label_text_if_changed(s_runtime_caption,
                              state.therapy ? "RUNTIME" : "LAST SESSION");
    if (state.therapy) {
        set_label_text_fmt_if_changed(s_runtime_label, "%02lld:%02lld:%02lld",
                                      (long long)(elapsed / 3600),
                                      (long long)((elapsed / 60) % 60),
                                      (long long)(elapsed % 60));
    } else {
        set_label_text_if_changed(s_runtime_label, stopped_runtime);
    }

    if (flow_live) {
        set_label_text_if_changed(s_chart_status, "Live");
        set_style_color_if_changed(s_chart_status, LV_STYLE_TEXT_COLOR,
                                   COLOR_LIVE, 0);
        set_style_color_if_changed(s_chart_status_pill, LV_STYLE_BG_COLOR,
                                   0x003639, 0);
        set_dot_tone(s_chart_status_dot, COLOR_LIVE, true);
        set_hidden(s_chart_message, true);
        set_hidden(s_chart_message_sub, true);
    } else {
        set_label_text_if_changed(
            s_chart_status,
            !state.therapy ? "Paused"
                           : has_flow_window ? "No signal" : "Waiting");
        set_style_color_if_changed(
            s_chart_status, LV_STYLE_TEXT_COLOR,
            state.therapy && has_flow_window ? COLOR_FAULT : COLOR_SECONDARY,
            0);
        set_style_color_if_changed(
            s_chart_status_pill, LV_STYLE_BG_COLOR,
            state.therapy && has_flow_window ? 0x53151a : COLOR_CONTROL, 0);
        set_dot_tone(s_chart_status_dot,
                     state.therapy && has_flow_window ? COLOR_FAULT
                                                          : COLOR_DISABLED,
                     state.therapy && has_flow_window);
        set_label_text_if_changed(
            s_chart_message,
            !state.paired ? "No AirSense paired"
            : !state.therapy ? "Graph paused"
            : airsense_stale ? "Therapy status unknown"
                              : "Waiting for breathing data…");
        set_label_text_if_changed(
            s_chart_message_sub,
            !state.paired ? "Pair a machine to see live breathing flow"
            : !state.therapy ? "Live flow appears while therapy is running"
            : has_flow_window
                ? "The AirSense connection was lost — reconnecting"
                : "First samples usually arrive within 10 seconds");
        set_hidden(s_chart_message, false);
        set_hidden(s_chart_message_sub, false);
    }

    refresh_secondary_pages(&state, active_tab);

    bool ordinary_notice = state.notice[0] && !state.notice_critical;
    bool attention = !state.therapy && state.attention[0] &&
                     strcmp(state.title, "SomnoTrace") != 0;
    if (ordinary_notice || attention) {
        if (ordinary_notice)
            set_label_text_if_changed(s_notice_label, state.notice);
        else
            set_label_text_fmt_if_changed(s_notice_label, "%s  -  %s",
                                          state.title, state.attention);
        bool notice_failed = strstr(s_notice_label ? lv_label_get_text(s_notice_label) : "",
                                    "Unable") ||
                             strstr(s_notice_label ? lv_label_get_text(s_notice_label) : "",
                                    "failed") ||
                             strstr(s_notice_label ? lv_label_get_text(s_notice_label) : "",
                                    "Could not");
        bool notice_warn = strstr(s_notice_label ? lv_label_get_text(s_notice_label) : "",
                                  "deferred") || state.storage_near_full;
        set_dot_tone(s_notice_mark,
                     notice_failed ? COLOR_FAULT : notice_warn ? COLOR_AMBER
                                                               : COLOR_LIVE,
                     true);
    }
    bool notice_visibility_changed =
        set_hidden(s_notice_card, !(ordinary_notice || attention));
    if ((ordinary_notice || attention) && notice_visibility_changed)
        lv_obj_move_foreground(s_notice_card);

    if (alert_actionable || state.notice_critical) {
        if (alert_actionable) {
            set_label_text_if_changed(s_alert_label,
                                      "Therapy stopped unexpectedly");
            set_label_text_if_changed(
                s_alert_subtitle,
                alert_state == ALERT_BUZZING
                    ? "Escalated screen alert - review the mask and machine"
                : alert_state == ALERT_PUSH_SENT
                    ? "Push service accepted; phone delivery is unverified"
                : alert_state == ALERT_PUSH_FAILED
                    ? "Push failed; the on-screen alert remains active"
                : alert_state == ALERT_SCREEN_ONLY
                    ? "Screen-only alert; this board has no speaker"
                    : "Review the mask and machine, then acknowledge");
            set_hidden(s_alert_ack_button, false);
            set_label_text_if_changed(
                lv_obj_get_child(s_alert_ack_button, 0),
                alert_ack_busy ? "Acknowledging..." : "Acknowledge");
            if (alert_ack_busy)
                lv_obj_add_state(s_alert_ack_button, LV_STATE_DISABLED);
            else
                lv_obj_clear_state(s_alert_ack_button, LV_STATE_DISABLED);
        } else {
            bool card_full = strstr(state.notice, "full") != NULL;
            set_label_text_if_changed(s_alert_label,
                                      card_full ? "microSD card is full"
                                                : "microSD write error");
            set_label_text_if_changed(
                s_alert_subtitle,
                card_full
                    ? "Therapy continues; a new recording cannot begin until card space is freed."
                    : "Therapy can continue. Tonight's recording may be incomplete.");
            set_hidden(s_alert_ack_button, true);
        }
    }
    bool alert_visible = alert_actionable || state.notice_critical;
    bool alert_visibility_changed = set_hidden(s_alert_banner, !alert_visible);
    /* If a lower-priority notice appeared this pass, restore the alert above it
     * once. Avoid reordering both overlays on every 500 ms presentation pass. */
    if (alert_visible &&
        (alert_visibility_changed || notice_visibility_changed))
        lv_obj_move_foreground(s_alert_banner);

    time_t now = time(NULL);
    struct tm local;
    if (now > 100000 && localtime_r(&now, &local)) {
        char clock[16], date[32];
        static const char *weekday[] = {
            "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
        };
        static const char *month[] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        strftime(clock, sizeof(clock), "%H:%M", &local);
        snprintf(date, sizeof(date), "%s %d %s", weekday[local.tm_wday],
                 local.tm_mday, month[local.tm_mon]);
        set_label_text_if_changed(s_clock_label, clock);
        set_label_text_if_changed(s_date_label, date);
    }
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
    strcpy(s_state.status, "Initializing 7-inch dashboard...");
    s_state.leak = NAN;
    s_state.pressure = NAN;
    s_state.respiratory_rate = NAN;
    s_state.flow_limitation = NAN;
    portENTER_CRITICAL(&s_state_lock);
    s_touch_was_pressed = false;
    s_backlight_force_on = false;
    s_setup_backlight_force_on = false;
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
    s_last_off_request_us = 0;
    portEXIT_CRITICAL(&s_state_lock);
    s_render_services = heap_caps_calloc(1, sizeof(*s_render_services),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_render_services) s_render_services = calloc(1, sizeof(*s_render_services));
    ESP_RETURN_ON_FALSE(s_render_services, ESP_ERR_NO_MEM, TAG,
                        "allocate UI service snapshot");

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
    const touch_history_controller_config_t history_config = {
        .changed = history_controller_changed,
        .route_card = NULL,
        .context = NULL,
        .usage_target_minutes = 240,
#if CONFIG_SOMNOTRACE_BOARD_QEMU
        .deterministic_preview = true,
#endif
    };
    ESP_RETURN_ON_ERROR(
        touch_history_controller_create(&history_config, &s_history_controller),
        TAG, "create rich History controller");
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

#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    /* Storage details retain one dormant PSRAM-backed worker. Rich History
     * owns its own single serialized PSRAM worker and queue. */
    s_storage_worker_task = psram_task_create(
        storage_status_task, "ui_storage", 8192, NULL, 2,
        tskNO_AFFINITY, NULL, NULL);
    if (!s_storage_worker_task)
        ESP_LOGE(TAG, "storage status worker unavailable");
#endif

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

void bsp_display_set_setup_callback(void (*callback)(void))
{
    s_setup_callback = callback;
}

void bsp_display_enable_touch_services(bool as11_ready, bool oximeter_ready)
{
    portENTER_CRITICAL(&s_state_lock);
    s_touch_services_ready = true;
    s_as11_service_ready = as11_ready;
    s_ox_service_ready = oximeter_ready;
    portEXIT_CRITICAL(&s_state_lock);
    start_storage_refresh();
}

esp_err_t bsp_display_start_first_run_setup(esp_err_t initial_card_result)
{
    first_run_setup_snapshot_t durable;
    first_run_setup_snapshot(&durable);
    if (!durable.schema_compatible) return durable.last_storage_result;
    if (first_run_setup_is_finished(&durable.state)) return ESP_OK;
    if (bsp_display_first_run_setup_active()) return ESP_OK;

    esp_err_t result = first_run_setup_controller_start(initial_card_result);
    if (result != ESP_OK) return result;
    const first_run_setup_ui_controller_t *callbacks =
        first_run_setup_controller_callbacks();
    if (!callbacks) {
        first_run_setup_controller_stop();
        return ESP_ERR_INVALID_STATE;
    }

    if (!lock_lvgl(pdMS_TO_TICKS(2000))) {
        first_run_setup_controller_stop();
        return ESP_ERR_TIMEOUT;
    }
    result = first_run_setup_ui_create(lv_layer_top(), callbacks);
    if (result == ESP_OK) {
        first_run_setup_ui_live_t live;
        uint32_t generation = 0;
        if (first_run_setup_controller_snapshot(&live, &generation)) {
            result = first_run_setup_ui_update(&live);
            if (result == ESP_OK)
                s_first_run_setup_seen_generation = generation;
        }
    }
    if (result == ESP_OK) result = first_run_setup_ui_show();
    if (result == ESP_OK) {
        /* Setup is a bedside interaction surface and must not disappear under
         * the ordinary standby timeout while the owner is completing it. */
        portENTER_CRITICAL(&s_state_lock);
        s_first_run_setup_active = true;
        s_setup_backlight_force_on = true;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_backlight(true);
    } else {
        first_run_setup_ui_destroy();
        first_run_setup_controller_stop();
    }
    unlock_lvgl();
    return result;
}

bool bsp_display_first_run_setup_active(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool active = s_first_run_setup_active;
    portEXIT_CRITICAL(&s_state_lock);
    return active;
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
    bool therapy_finished = changed && !active;
    portEXIT_CRITICAL(&s_state_lock);
    if (changed) bsp_display_restart_idle_timeout();
    bsp_display_apply_backlight_policy(false);
    /* Finalisation changes the all-days index. Inactive History is marked
     * stale for its next entry; visible History reloads asynchronously now. */
    if (therapy_finished && s_history_controller) {
        esp_err_t result = touch_history_controller_refresh(
            s_history_controller);
        if (result != ESP_OK)
            ESP_LOGW(TAG, "refresh History after therapy: %s",
                     esp_err_to_name(result));
    }
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
    if (!on) s_last_off_request_us = esp_timer_get_time();
    ++s_backlight_revision;
    /* An explicit ON request is a physical reassertion, even after peripheral
     * state loss left the application's last successful value unchanged. */
    if (on) s_backlight_known = false;
    portEXIT_CRITICAL(&s_state_lock);
}

bool bsp_display_toggle_backlight(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool on = s_backlight_force_on || s_setup_backlight_force_on ||
              !s_backlight_requested;
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
        /* The worker may publish after a later OFF while waiting for I2C. */
        if (touch.visibility_requested_us > s_last_off_request_us) {
            s_backlight_requested = true;
            s_backlight_known = false;
            s_wake_gesture_pending = true;
            s_last_touch_activity_us = now_us;
            ++s_backlight_revision;
        }
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
    bool forced = s_backlight_force_on || s_setup_backlight_force_on;
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

void bsp_display_qemu_seed_demo(void)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    uploader_progress_snapshot_t upload_progress;
    qemu_upload_progress(&upload_progress);
    portENTER_CRITICAL(&s_state_lock);
    s_services.storage_free = 1932735283ULL;
    s_services.storage_total = 32ULL * 1024ULL * 1024ULL * 1024ULL;
    s_services.storage_result = ESP_OK;
    s_services.storage_version++;
    s_services.upload_pending = 1;
    strlcpy(s_services.upload_state, "uploading",
            sizeof(s_services.upload_state));
    s_services.upload_progress = upload_progress;
    s_services.upload_progress_result = ESP_OK;
    /* Start the visual preview with a complete, explicitly simulated window.
     * Otherwise unwritten ring-buffer slots look like measured zero flow for
     * the first thirty seconds and make the chart appear broken. */
    for (unsigned i = 0; i < FLOW_POINTS; ++i) {
        float phase = (float)i * 0.06f;
        s_state.flow[i] = (int16_t)lrintf(
            (36.0f * sinf(phase) + 7.0f * sinf(phase * 2.3f)) * 10.0f);
    }
    s_state.flow_head = 0;
    s_state.flow_count = FLOW_POINTS;
    s_state.flow_version++;
    s_state.flow_sample_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
    if (s_history_controller)
        (void)touch_history_controller_refresh(s_history_controller);
#endif
}

void bsp_display_qemu_set_tab(uint8_t tab)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (tab >= 3) return;
    /* The display task owns LVGL. Queue navigation into that task so preview
     * input never mutates the object tree from a service callback. */
    portENTER_CRITICAL(&s_state_lock);
    s_qemu_requested_tab = tab;
    portEXIT_CRITICAL(&s_state_lock);
#else
    (void)tab;
#endif
}

esp_err_t bsp_display_qemu_start_setup_preview(void)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (bsp_display_first_run_setup_active()) return ESP_OK;
    esp_err_t result = first_run_setup_reset();
    if (result != ESP_OK) return result;
    result = bsp_display_start_first_run_setup(ESP_OK);
    if (result == ESP_OK)
        ESP_LOGI(TAG, "QEMU setup preview ready; simulated services only");
    return result;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void bsp_display_set_rotation(uint16_t degrees)
{
    if (degrees != 0) {
        ESP_LOGW(TAG, "rotation %u ignored: the 7B dashboard is landscape-native",
                 (unsigned)degrees);
    }
}
