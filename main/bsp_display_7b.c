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
#include "touch_manage_config.h"
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
#include "touch_maintenance_ui.h"
#include "touch_maintenance.h"
#include "uploader.h"

#define FLOW_POINTS 300
#define FLOW_READY_POINTS 12
#define FLOW_CATCHUP_THRESHOLD 10
#define FLOW_RESYNC_THRESHOLD 25
#define UI_UPDATE_MS 50
#define DEVICE_RESULT_MAX 8
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

typedef enum {
    BLE_UI_IDLE,
    BLE_UI_SCAN_AS11,
    BLE_UI_SCAN_OX,
    BLE_UI_PAIR_AS11,
    BLE_UI_PAIR_OX,
    BLE_UI_FORGET,
} ble_ui_operation_t;

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
static int64_t s_last_display_service_us;
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
static bool s_as11_pairing_mode_confirmed;
static bool s_therapy_command_busy;
static bool s_therapy_command_target;
static bool s_alert_ack_busy;
static bool s_alert_test_busy;
static bool s_reboot_busy;
static bool s_wifi_save_busy;
static bool s_wifi_restart_pending;
static ble_ui_operation_t s_ble_operation;
static int64_t s_ble_operation_started_us;

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
static lv_obj_t *s_status_tray_actions[5];
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
static lv_obj_t *s_alert_test_button;
static lv_obj_t *s_reboot_button;
static lv_obj_t *s_wifi_save_button;
static lv_obj_t *s_wifi_hotspot_button;
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
static lv_obj_t *s_as11_row;
static lv_obj_t *s_ox_row;
static lv_obj_t *s_as11_title;
static lv_obj_t *s_ox_title;
static lv_obj_t *s_as11_dot;
static lv_obj_t *s_ox_dot;
static lv_obj_t *s_as11_status;
static lv_obj_t *s_as11_badge;
static lv_obj_t *s_as11_dropdown;
static lv_obj_t *s_as11_pair_button;
static lv_obj_t *s_ble_buttons[6];
static lv_obj_t *s_pair_steps[5];
static lv_obj_t *s_pair_step_labels[5];
static lv_obj_t *s_passkey_confirm_button;
static lv_obj_t *s_passkey;
static lv_obj_t *s_ox_status;
static lv_obj_t *s_ox_badge;
static lv_obj_t *s_ox_dropdown;
static lv_obj_t *s_device_change_row;
static lv_obj_t *s_device_change_title;
static lv_obj_t *s_device_change_detail;
static lv_obj_t *s_network_status;
static lv_obj_t *s_wifi_ssid;
static lv_obj_t *s_wifi_password;
static lv_obj_t *s_connectivity_rows[5];
static lv_obj_t *s_wifi_scan_button;
static lv_obj_t *s_wifi_scan_button_label;
static lv_obj_t *s_wifi_scan_row;
static lv_obj_t *s_wifi_scan_status;
static lv_obj_t *s_wifi_scan_dropdown;
static lv_obj_t *s_wifi_scan_use_button;
static bool s_wifi_scan_requested;
static bool s_wifi_scan_open_selected;
static bool s_wifi_force_clear_password;
static uint32_t s_wifi_scan_seen_generation = UINT32_MAX;
static netprov_scan_state_t s_wifi_scan_seen_state = NETPROV_SCAN_IDLE;
static lv_obj_t *s_wifi_password_helper;
static lv_obj_t *s_wifi_password_reveal;
static lv_obj_t *s_wifi_password_reveal_label;
static bool s_wifi_password_revealed;
static lv_obj_t *s_wifi_restart_detail;
static lv_obj_t *s_alert_status;
static lv_obj_t *s_storage_status;
static lv_obj_t *s_storage_estimate;
static lv_obj_t *s_storage_meter;
static lv_obj_t *s_storage_refresh_button;
static lv_obj_t *s_upload_rows[UPLOADER_PROGRESS_MAX_BACKENDS];
static lv_obj_t *s_upload_dots[UPLOADER_PROGRESS_MAX_BACKENDS];
static lv_obj_t *s_upload_titles[UPLOADER_PROGRESS_MAX_BACKENDS];
static lv_obj_t *s_upload_details[UPLOADER_PROGRESS_MAX_BACKENDS];
static lv_obj_t *s_upload_states[UPLOADER_PROGRESS_MAX_BACKENDS];
static lv_obj_t *s_upload_meters[UPLOADER_PROGRESS_MAX_BACKENDS];
static lv_obj_t *s_upload_browser_row;
static lv_obj_t *s_storage_browser_row;
static lv_obj_t *s_system_health_title;
static lv_obj_t *s_system_health_dot;
static lv_obj_t *s_system_details;
static lv_obj_t *s_system_firmware;
static lv_obj_t *s_system_restart_detail;
static lv_obj_t *s_device_section_subtitle;
static lv_obj_t *s_connectivity_section_subtitle;
static lv_obj_t *s_system_section_subtitle;
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
static lv_obj_t *s_manage_dialog;
static lv_obj_t *s_keyboard_sheet;
static lv_obj_t *s_keyboard_title;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_keyboard_target;
static char s_keyboard_initial[NETPROV_PASS_MAXLEN + 1];
static lv_obj_t *s_wake_overlay;
static unsigned s_seen_as11_version;
static unsigned s_seen_ox_version;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
static TaskHandle_t s_storage_worker_task;
#endif
static bool s_connectivity_settings_synced;
static char s_saved_wifi_ssid[NETPROV_SSID_MAXLEN + 1];
#if CONFIG_SOMNOTRACE_BOARD_QEMU
static int64_t s_qemu_wifi_scan_started_us;
static uint8_t s_qemu_requested_tab = UINT8_MAX;
#endif

static void set_active_page(int page);
static void set_manage_section(int section);
static void ensure_manage_destination(void);
static void teardown_rendered_manage_destination(void);
static void reap_retired_logs_destination(void);
static void update_manage_rail_selection(int section);
static void close_manage_dialog(void);
static void start_storage_refresh(void);
static void apply_pending_backlight_locked(void);
void bsp_display_restart_idle_timeout(void);
static void wake_timer_cb(void *arg);
static void style_manage_surface(lv_obj_t *obj);
static void style_manage_field(lv_obj_t *field);
static void style_manage_textarea(lv_obj_t *field);
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

static bool begin_ble_operation(ble_ui_operation_t operation)
{
    bool started = false;
    portENTER_CRITICAL(&s_state_lock);
    if (s_ble_operation == BLE_UI_IDLE) {
        s_ble_operation = operation;
        s_ble_operation_started_us = esp_timer_get_time();
        started = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return started;
}

static void end_ble_operation(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ble_operation = BLE_UI_IDLE;
    s_ble_operation_started_us = 0;
    portEXIT_CRITICAL(&s_state_lock);
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
    touch_maintenance_ui_frame_published();
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
    /* Physical wake is admitted by the board's ordered visibility demand.
     * An old down frame must not bypass a newer OFF through the overlay. */
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

static lv_obj_t *make_manage_field_chevron(lv_obj_t *field)
{
    lv_obj_t *chevron = make_down_chevron(field, 0, 0);
    /* Field children are positioned in the padded content box. Aligning here
     * keeps the glyph centred when a field's font, height, or padding changes. */
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
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

static void manage_dropdown_list_ready_cb(lv_event_t *event)
{
    lv_obj_t *list = lv_dropdown_get_list(lv_event_get_target(event));
    if (!list) return;

    /* A 45 px option pitch is large enough for reliable bedside selection;
     * longer device and Wi-Fi result sets remain vertically scrollable. */
    style_manage_surface(list);
    lv_obj_set_style_max_height(list, 250, LV_PART_MAIN);
    lv_obj_set_style_pad_top(list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(list, 12, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(list, 28, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(list, 28, LV_PART_SELECTED);
    lv_obj_t *label = lv_obj_get_child(list, 0);
    if (label) {
        lv_obj_set_style_text_font(label, FONT_BODY, LV_PART_MAIN);
        lv_obj_set_style_text_line_space(label, 28, LV_PART_MAIN);
    }
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

static void set_storage_night_estimate(lv_obj_t *label, uint64_t free_bytes)
{
    if (!label) return;
    unsigned nights = estimated_airsense_nights(free_bytes);
    if (nights == 0) {
        lv_label_set_text(label,
                          "Less than one AirSense-only night · O2 uses more");
    } else {
        lv_label_set_text_fmt(label,
                              "About %u AirSense-only nights · fewer with O2",
                              nights);
    }
}

static void alert_config_task(void *arg)
{
    (void)arg;
    therapy_alert_config_t config = ALERT_DEFAULTS;
    esp_err_t result = therapy_alert_load_config(&config);
    portENTER_CRITICAL(&s_state_lock);
    s_services.alert_config = config;
    s_services.alert_config_result = result;
    s_services.alert_config_busy = false;
    s_services.alert_config_version++;
    portEXIT_CRITICAL(&s_state_lock);
    vTaskDelete(NULL);
}

static void __attribute__((unused)) start_alert_config_refresh(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_services.alert_config_busy;
    if (!busy) s_services.alert_config_busy = true;
    portEXIT_CRITICAL(&s_state_lock);
    if (!busy && xTaskCreate(alert_config_task, "ui_alert_cfg", 4096,
                             NULL, 2, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_services.alert_config_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Unable to read alert settings");
    }
}

static void device_scan_task(void *arg)
{
    bool oxygen = (intptr_t)arg == 1;
    cJSON *results = NULL;
    esp_err_t result = bsp_display_is_therapy_active()
                       ? ESP_ERR_INVALID_STATE
                       : (oxygen ? oximeter_scan(7) : as11_ble_scan(7));
    if (result == ESP_OK)
        results = oxygen ? oximeter_get_scan_results() : as11_ble_get_scan_results();

    ui_device_result_t local[DEVICE_RESULT_MAX] = {0};
    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (count >= DEVICE_RESULT_MAX) break;
        cJSON *addr = cJSON_GetObjectItemCaseSensitive(item, "addr");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *rssi = cJSON_GetObjectItemCaseSensitive(item, "rssi");
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(addr)) continue;
        strlcpy(local[count].addr, addr->valuestring, sizeof(local[count].addr));
        strlcpy(local[count].name,
                cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : "Unnamed device",
                sizeof(local[count].name));
        local[count].rssi = cJSON_IsNumber(rssi) ? rssi->valueint : 0;
        local[count].driver = cJSON_IsString(type) && !strcmp(type->valuestring, "legacy")
                                ? OX_DRIVER_LEGACY : OX_DRIVER_OXYII;
        count++;
    }
    cJSON_Delete(results);

    portENTER_CRITICAL(&s_state_lock);
    ui_device_result_t *target = oxygen ? s_services.ox : s_services.as11;
    memcpy(target, local, sizeof(local));
    if (oxygen) {
        s_services.ox_count = count;
        s_services.ox_busy = false;
        s_services.ox_version++;
    } else {
        s_services.as11_count = count;
        s_services.as11_busy = false;
        s_services.as11_version++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    end_ble_operation();
    vTaskDelete(NULL);
}

static void scan_cb(lv_event_t *event)
{
    bool oxygen = (intptr_t)lv_event_get_user_data(event) == 1;
    if (!s_touch_services_ready || (oxygen ? !s_ox_service_ready
                                          : !s_as11_service_ready)) {
        bsp_display_set_notice("Pairing services are still starting");
        return;
    }
    if (bsp_display_is_therapy_active()) {
        bsp_display_set_notice("Stop therapy before scanning for devices");
        return;
    }
    if (!begin_ble_operation(oxygen ? BLE_UI_SCAN_OX : BLE_UI_SCAN_AS11)) {
        bsp_display_set_notice("Another Bluetooth action is already running");
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (oxygen) {
        s_services.ox_busy = true;
    } else {
        /* Tapping “AirSense is ready” is the explicit acknowledgement that
         * More > MyAir App > OK, downloaded > Connect was completed first. */
        s_as11_pairing_mode_confirmed = true;
        s_services.as11_busy = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_set_notice(oxygen ? "Scanning for O2 rings..." : "Scanning for AirSense 11...");
    if (xTaskCreate(device_scan_task, oxygen ? "ui_scan_ox" : "ui_scan_as11",
                    8192, (void *)(intptr_t)(oxygen ? 1 : 0), 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        if (oxygen) s_services.ox_busy = false;
        else s_services.as11_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        end_ble_operation();
    }
}

typedef enum { DEVICE_PAIR_AS11, DEVICE_PAIR_OX, DEVICE_CONFIRM_AS11,
               DEVICE_FORGET_AS11, DEVICE_FORGET_OX } device_action_t;

typedef struct {
    device_action_t action;
    char addr[18];
    char passkey[5];
    ox_driver_t driver;
} device_job_t;

static void device_action_task(void *arg)
{
    device_job_t *job = arg;
    esp_err_t result = ESP_FAIL;
    if (bsp_display_is_therapy_active()) {
        result = ESP_ERR_INVALID_STATE;
    } else if (job->action == DEVICE_PAIR_AS11) {
        result = as11_ble_start_pair(job->addr);
    } else if (job->action == DEVICE_PAIR_OX) {
        result = oximeter_pair(job->addr, OX_DRIVER_AUTO);
    } else if (job->action == DEVICE_CONFIRM_AS11) {
        result = as11_ble_confirm_pair(job->passkey);
    } else if (job->action == DEVICE_FORGET_AS11) {
        result = as11_ble_forget();
    } else if (job->action == DEVICE_FORGET_OX) {
        result = oximeter_forget();
    }
    if (job->action == DEVICE_FORGET_AS11 ||
        (job->action == DEVICE_PAIR_AS11 && result != ESP_OK)) {
        portENTER_CRITICAL(&s_state_lock);
        s_as11_pairing_mode_confirmed = false;
        portEXIT_CRITICAL(&s_state_lock);
    }
    bsp_display_set_notice(
        result == ESP_OK ? "Device action started" :
        job->action == DEVICE_PAIR_AS11
            ? "Pairing could not start · enable AirSense pairing mode first"
            : "Device action failed");
    if (result != ESP_OK || job->action == DEVICE_FORGET_AS11 ||
        job->action == DEVICE_FORGET_OX) end_ble_operation();
    free(job);
    vTaskDelete(NULL);
}

static void device_action_cb(lv_event_t *event)
{
    device_action_t action = (device_action_t)(intptr_t)lv_event_get_user_data(event);
    bool oxygen = action == DEVICE_PAIR_OX || action == DEVICE_FORGET_OX;
    if (!s_touch_services_ready || (oxygen ? !s_ox_service_ready
                                          : !s_as11_service_ready)) {
        bsp_display_set_notice("Pairing services are still starting");
        return;
    }
    if (bsp_display_is_therapy_active()) {
        bsp_display_set_notice("Stop therapy before changing paired devices");
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    bool pairing_mode_confirmed = s_as11_pairing_mode_confirmed;
    portEXIT_CRITICAL(&s_state_lock);
    if (action == DEVICE_PAIR_AS11 && !pairing_mode_confirmed) {
        bsp_display_set_notice("First enable AirSense pairing mode, then scan");
        return;
    }
    if (action == DEVICE_CONFIRM_AS11 &&
        strcmp(as11_ble_get_status(), AS11_STATUS_WAIT_PASSKEY) != 0) {
        bsp_display_set_notice("AirSense is not waiting for a passkey");
        return;
    }
    ble_ui_operation_t operation =
        action == DEVICE_PAIR_AS11 || action == DEVICE_CONFIRM_AS11 ? BLE_UI_PAIR_AS11 :
        action == DEVICE_PAIR_OX ? BLE_UI_PAIR_OX : BLE_UI_FORGET;
    portENTER_CRITICAL(&s_state_lock);
    bool continuing_passkey = action == DEVICE_CONFIRM_AS11 &&
                              s_ble_operation == BLE_UI_PAIR_AS11;
    portEXIT_CRITICAL(&s_state_lock);
    if (!continuing_passkey && !begin_ble_operation(operation)) {
        bsp_display_set_notice("Another Bluetooth action is already running");
        return;
    }
    device_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        if (!continuing_passkey) end_ble_operation();
        bsp_display_set_notice("Unable to allocate pairing job");
        return;
    }
    job->action = action;
    if (action == DEVICE_PAIR_AS11) {
        uint16_t selected = lv_dropdown_get_selected(s_as11_dropdown);
        portENTER_CRITICAL(&s_state_lock);
        if (selected < s_services.as11_count)
            *job = (device_job_t){ .action = action, .driver = OX_DRIVER_AUTO };
        if (selected < s_services.as11_count)
            strlcpy(job->addr, s_services.as11[selected].addr, sizeof(job->addr));
        portEXIT_CRITICAL(&s_state_lock);
    } else if (action == DEVICE_PAIR_OX) {
        uint16_t selected = lv_dropdown_get_selected(s_ox_dropdown);
        portENTER_CRITICAL(&s_state_lock);
        if (selected < s_services.ox_count) {
            strlcpy(job->addr, s_services.ox[selected].addr, sizeof(job->addr));
            job->driver = OX_DRIVER_AUTO;
        }
        portEXIT_CRITICAL(&s_state_lock);
    } else if (action == DEVICE_CONFIRM_AS11) {
        strlcpy(job->passkey, lv_textarea_get_text(s_passkey), sizeof(job->passkey));
    }
    if ((action == DEVICE_PAIR_AS11 || action == DEVICE_PAIR_OX) && !job->addr[0]) {
        free(job);
        if (!continuing_passkey) end_ble_operation();
        bsp_display_set_notice("Scan and select a device first");
        return;
    }
    if (action == DEVICE_CONFIRM_AS11 && strlen(job->passkey) != 4) {
        free(job);
        if (!continuing_passkey) end_ble_operation();
        bsp_display_set_notice("Enter the 4-digit AirSense code");
        return;
    }
    if (xTaskCreate(device_action_task, "ui_pair", 6144,
                    job, 4, NULL) != pdPASS) {
        free(job);
        if (!continuing_passkey) end_ble_operation();
        bsp_display_set_notice("Unable to start pairing action");
    }
}

static void manage_dialog_deleted_cb(lv_event_t *event)
{
    if (s_manage_dialog == lv_event_get_target(event)) s_manage_dialog = NULL;
}

static void track_manage_dialog(lv_obj_t *dialog)
{
    if (!dialog) return;
    close_manage_dialog();
    s_manage_dialog = dialog;
    lv_obj_add_event_cb(dialog, manage_dialog_deleted_cb, LV_EVENT_DELETE, NULL);
}

static void close_manage_dialog(void)
{
    lv_obj_t *dialog = s_manage_dialog;
    if (!dialog) return;
    /* Clear ownership before deletion because LV_EVENT_DELETE is synchronous
     * and may run callbacks which attempt another destination transition. */
    s_manage_dialog = NULL;
    lv_msgbox_close(dialog);
}

static void forget_dialog_cb(lv_event_t *event)
{
    lv_obj_t *dialog = lv_event_get_current_target(event);
    const char *button = lv_msgbox_get_active_btn_text(dialog);
    if (!button) return;
    if (!strcmp(button, "Forget")) {
        if (!begin_ble_operation(BLE_UI_FORGET)) {
            bsp_display_set_notice("Another Bluetooth action is already running");
            lv_msgbox_close(dialog);
            return;
        }
        device_job_t *job = calloc(1, sizeof(*job));
        if (job) {
            job->action = (device_action_t)(intptr_t)lv_event_get_user_data(event);
            if (xTaskCreate(device_action_task, "ui_forget", 4096,
                            job, 4, NULL) != pdPASS) {
                free(job);
                end_ble_operation();
            }
        } else {
            end_ble_operation();
        }
    }
    lv_msgbox_close(dialog);
}

static void forget_prompt_cb(lv_event_t *event)
{
    device_action_t action = (device_action_t)(intptr_t)lv_event_get_user_data(event);
    bool oxygen = action == DEVICE_FORGET_OX;
    if (!s_touch_services_ready || (oxygen ? !s_ox_service_ready
                                          : !s_as11_service_ready)) {
        bsp_display_set_notice("Pairing service is unavailable");
        return;
    }
    if (bsp_display_is_therapy_active()) {
        bsp_display_set_notice("Stop therapy before forgetting a device");
        return;
    }
    static const char *buttons[] = { "Cancel", "Forget", "" };
    const char *device = action == DEVICE_FORGET_AS11 ? "AirSense 11" : "O2 ring";
    char question[224];
    snprintf(question, sizeof(question),
             "SomnoTrace will stop collecting data from %s until it is paired again. Recorded nights already on the card are kept.",
             device);
    lv_obj_t *dialog = lv_msgbox_create(NULL,
                                         action == DEVICE_FORGET_AS11
                                             ? "Forget AirSense 11?"
                                             : "Forget O2 ring?",
                                         question, buttons, true);
    lv_obj_set_width(dialog, 620);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x172640), 0);
    lv_obj_set_style_text_color(dialog, lv_color_hex(0xe7edf7), 0);
    lv_obj_add_event_cb(dialog, forget_dialog_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)action);
    track_manage_dialog(dialog);
    lv_obj_center(dialog);
}

static void layout_connectivity_rows(void)
{
    if (!s_connectivity_rows[0]) return;
    set_hidden(s_wifi_scan_row,
               !s_wifi_scan_requested || s_keyboard_target != NULL);
    /* The keyboard owns the active field's temporary y=0 geometry.  The
     * periodic scan refresh must not move it back into the scrolled layout. */
    if (s_keyboard_target == s_wifi_ssid ||
        s_keyboard_target == s_wifi_password)
        return;
    int offset = s_wifi_scan_requested ? 120 : 0;
    if (s_wifi_scan_row) lv_obj_set_pos(s_wifi_scan_row, 0, 94);
    lv_obj_set_pos(s_connectivity_rows[1], 0, 94 + offset);
    lv_obj_set_pos(s_connectivity_rows[2], 0, 214 + offset);
    lv_obj_set_pos(s_connectivity_rows[3], 0, 334 + offset);
    lv_obj_set_pos(s_connectivity_rows[4], 0, 424 + offset);
}

static void set_connectivity_editing(lv_obj_t *target, bool editing)
{
    bool network_field = target == s_wifi_ssid || target == s_wifi_password;
    if (!network_field || !s_connectivity_rows[0]) return;

    lv_obj_t *active_row = target == s_wifi_ssid
                               ? s_connectivity_rows[1]
                               : s_connectivity_rows[2];
    for (int i = 0; i < 5; ++i) {
        if (editing && s_connectivity_rows[i] != active_row)
            lv_obj_add_flag(s_connectivity_rows[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(s_connectivity_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_scan_row)
        set_hidden(s_wifi_scan_row, editing || !s_wifi_scan_requested);

    if (editing) {
        lv_obj_set_pos(active_row, 0, 0);
        lv_obj_set_size(active_row, UI_MANAGE_ROW_FULL_W, 124);
        lv_obj_set_pos(target, 0, 42);
        lv_obj_set_size(target, 708, 60);
        if (target == s_wifi_password) {
            lv_obj_add_flag(s_wifi_password_helper, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_wifi_password_reveal, 580, 46);
            lv_obj_move_foreground(s_wifi_password_reveal);
        }
        lv_obj_scroll_to_y(s_manage_scrolls[MANAGE_CONNECTIVITY], 0,
                           LV_ANIM_OFF);
        return;
    }

    layout_connectivity_rows();
    lv_obj_set_size(s_connectivity_rows[1], UI_MANAGE_ROW_W, 112);
    lv_obj_set_pos(s_wifi_ssid, 190, 0);
    lv_obj_set_size(s_wifi_ssid, 504, 60);
    lv_obj_set_size(s_connectivity_rows[2], UI_MANAGE_ROW_W, 112);
    lv_obj_set_pos(s_wifi_password, 190, 0);
    lv_obj_set_size(s_wifi_password, 504, 60);
    lv_obj_set_pos(s_wifi_password_reveal, 574, 4);
    lv_obj_clear_flag(s_wifi_password_helper, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(s_manage_scrolls[MANAGE_CONNECTIVITY], 0, LV_ANIM_OFF);
}

static void close_keyboard_sheet(bool restore)
{
    if (!s_keyboard_sheet) return;
    lv_obj_t *target = s_keyboard_target;
    if (restore && target) lv_textarea_set_text(target, s_keyboard_initial);
    /* Release keyboard ownership before restoring the scrolling layout so
     * layout_connectivity_rows() can put the optional scan row and fields
     * back in their steady-state positions in this same frame. */
    s_keyboard_target = NULL;
    set_connectivity_editing(target, false);
    if (target && target == s_wifi_password) {
        s_wifi_password_revealed = false;
        lv_textarea_set_password_mode(s_wifi_password, true);
        lv_label_set_text(s_wifi_password_reveal_label, "Reveal");
    }
    if (target) lv_obj_clear_state(target, LV_STATE_FOCUSED);
    lv_obj_add_flag(s_keyboard_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, NULL);
}

static void open_keyboard_sheet(lv_obj_t *target, lv_keyboard_mode_t mode,
                                const char *title, int top)
{
    if (!target || !s_keyboard_sheet) return;
    s_keyboard_target = target;
    strlcpy(s_keyboard_initial, lv_textarea_get_text(target),
            sizeof(s_keyboard_initial));
    lv_label_set_text(s_keyboard_title, title);
    if (target == s_wifi_ssid)
        lv_label_set_text(s_connectivity_section_subtitle,
                          "Editing network name");
    else if (target == s_wifi_password)
        lv_label_set_text(s_connectivity_section_subtitle,
                          "Editing network password");
    set_connectivity_editing(target, true);
    lv_keyboard_set_mode(s_keyboard, mode);
    lv_keyboard_set_textarea(s_keyboard, target);
    lv_obj_set_y(s_keyboard_sheet, top);
    lv_obj_set_height(s_keyboard_sheet, 320);
    lv_obj_set_y(s_keyboard, top == 356 ? 58 : 67);
    lv_obj_set_height(s_keyboard, top == 356 ? 168 : 203);
    lv_obj_clear_flag(s_keyboard_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard_sheet);
}

static void passkey_focus_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED)
        open_keyboard_sheet(lv_event_get_target(event),
                            LV_KEYBOARD_MODE_NUMBER, "Pairing code", 356);
}

static void text_focus_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_FOCUSED) return;
    lv_obj_t *target = lv_event_get_target(event);
    open_keyboard_sheet(target, LV_KEYBOARD_MODE_TEXT_LOWER,
                        target == s_wifi_ssid ? "Network name"
                                              : "Network password",
                        314);
}

static void wifi_field_changed_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    s_wifi_force_clear_password = false;
    s_wifi_scan_open_selected = false;
    portENTER_CRITICAL(&s_state_lock);
    s_wifi_restart_pending = false;
    portEXIT_CRITICAL(&s_state_lock);
}

static void wifi_password_reveal_cb(lv_event_t *event)
{
    (void)event;
    s_wifi_password_revealed = !s_wifi_password_revealed;
    lv_textarea_set_password_mode(s_wifi_password, !s_wifi_password_revealed);
    lv_obj_scroll_to_y(s_wifi_password, 0, LV_ANIM_OFF);
    lv_label_set_text(s_wifi_password_reveal_label,
                      s_wifi_password_revealed ? "Mask" : "Reveal");
}

static void wifi_scan_snapshot(netprov_scan_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (!s_wifi_scan_requested) {
        snapshot->state = NETPROV_SCAN_IDLE;
        snapshot->result = ESP_OK;
        return;
    }
    snapshot->generation = 1;
    if (s_qemu_wifi_scan_started_us > 0 &&
        esp_timer_get_time() - s_qemu_wifi_scan_started_us < 650000) {
        snapshot->state = NETPROV_SCAN_RUNNING;
        snapshot->result = ESP_OK;
        return;
    }
    static const netprov_scan_ap_t fixture[] = {
        { .ssid = "Hearthstone", .rssi = -42, .secure = true },
        { .ssid = "Bedroom mesh", .rssi = -61, .secure = true },
        { .ssid = "Guest open", .rssi = -74, .secure = false },
    };
    snapshot->state = NETPROV_SCAN_READY;
    snapshot->result = ESP_OK;
    snapshot->count = sizeof(fixture) / sizeof(fixture[0]);
    memcpy(snapshot->aps, fixture, sizeof(fixture));
#else
    netprov_scan_get_snapshot(snapshot);
#endif
}

static const char *wifi_scan_interaction_reason(bool scan_running)
{
    if (!s_touch_services_ready) return "Network service is still starting";
    if (bsp_display_is_therapy_active() || sd_storage_recording_active())
        return "Stop therapy to scan for Wi-Fi";
    if (s_keyboard_target) return "Finish editing before scanning";
    portENTER_CRITICAL(&s_state_lock);
    bool save_busy = s_wifi_save_busy;
    bool reboot_busy = s_reboot_busy;
    bool restart_pending = s_wifi_restart_pending;
    portEXIT_CRITICAL(&s_state_lock);
    if (save_busy) return "Wait for Wi-Fi settings to finish saving";
    if (reboot_busy) return "Restart is already in progress";
    if (restart_pending) return "Restart saved Wi-Fi changes before scanning";
    if (scan_running) return "Wi-Fi scan is already running";
    return NULL;
}

static void wifi_scan_request_cb(lv_event_t *event)
{
    (void)event;
    netprov_scan_snapshot_t snapshot;
    wifi_scan_snapshot(&snapshot);
    const char *blocked = wifi_scan_interaction_reason(
        snapshot.state == NETPROV_SCAN_RUNNING);
    if (blocked) {
        bsp_display_set_notice(blocked);
        return;
    }

    s_wifi_scan_requested = true;
    s_wifi_scan_open_selected = false;
    s_wifi_scan_seen_generation = UINT32_MAX;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    s_qemu_wifi_scan_started_us = esp_timer_get_time();
    bsp_display_set_notice("Scanning nearby Wi-Fi · simulated preview");
#else
    esp_err_t result = netprov_scan_request();
    if (result == ESP_OK)
        bsp_display_set_notice("Scanning nearby Wi-Fi networks...");
    else {
        wifi_scan_snapshot(&snapshot);
        if (snapshot.blocked_by == NETPROV_SCAN_BLOCK_RECORDING)
            bsp_display_set_notice("Stop therapy to scan for Wi-Fi");
        else if (snapshot.blocked_by == NETPROV_SCAN_BLOCK_RADIO_BUSY)
            bsp_display_set_notice("Wi-Fi is reconnecting · try again shortly");
        else
            bsp_display_set_notice("Unable to start Wi-Fi scan");
    }
#endif
}

static void wifi_scan_selection_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    s_wifi_scan_open_selected = false;
}

static void wifi_scan_use_cb(lv_event_t *event)
{
    (void)event;
    netprov_scan_snapshot_t snapshot;
    wifi_scan_snapshot(&snapshot);
    const char *blocked = wifi_scan_interaction_reason(
        snapshot.state == NETPROV_SCAN_RUNNING);
    if (blocked) {
        bsp_display_set_notice(blocked);
        return;
    }
    if (snapshot.state != NETPROV_SCAN_READY || snapshot.count == 0) {
        bsp_display_set_notice("Choose an available Wi-Fi network first");
        return;
    }
    uint16_t selected = lv_dropdown_get_selected(s_wifi_scan_dropdown);
    if (selected >= snapshot.count) selected = 0;
    const netprov_scan_ap_t *network = &snapshot.aps[selected];
    lv_textarea_set_text(s_wifi_ssid, network->ssid);
    lv_textarea_set_text(s_wifi_password, "");
    s_wifi_force_clear_password = !network->secure;
    s_wifi_scan_open_selected = !network->secure;
    if (network->secure) {
        lv_obj_add_state(s_wifi_password, LV_STATE_FOCUSED);
        open_keyboard_sheet(s_wifi_password, LV_KEYBOARD_MODE_TEXT_LOWER,
                            "Network password", 314);
        bsp_display_set_notice("Secure network selected · enter its password");
    } else {
        bsp_display_set_notice("Open network selected · no password required");
    }
}

static void keyboard_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED && s_keyboard_target) {
        uint16_t selected = lv_btnmatrix_get_selected_btn(s_keyboard);
        const char *text = lv_btnmatrix_get_btn_text(s_keyboard, selected);
        if (!text) return;
        if (!strcmp(text, "Clear")) {
            lv_textarea_set_text(s_keyboard_target, "");
        } else if (!strcmp(text, LV_SYMBOL_UP)) {
            lv_keyboard_mode_t mode = lv_keyboard_get_mode(s_keyboard);
            lv_keyboard_set_mode(s_keyboard,
                                 mode == LV_KEYBOARD_MODE_TEXT_UPPER
                                     ? LV_KEYBOARD_MODE_TEXT_LOWER
                                     : LV_KEYBOARD_MODE_TEXT_UPPER);
        } else if (!strcmp(text, "123")) {
            lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_SPECIAL);
        } else if (!strcmp(text, "space")) {
            lv_textarea_add_char(s_keyboard_target, ' ');
        } else {
            lv_keyboard_def_event_cb(event);
        }
        return;
    }
    if (code == LV_EVENT_READY) close_keyboard_sheet(false);
    else if (code == LV_EVENT_CANCEL) close_keyboard_sheet(true);
}

static void keyboard_sheet_action_cb(lv_event_t *event)
{
    bool cancel = (intptr_t)lv_event_get_user_data(event) == 0;
    close_keyboard_sheet(cancel);
}

static void reboot_task(void *arg)
{
    bool from_wifi_save = (intptr_t)arg == 1;
    vTaskDelay(pdMS_TO_TICKS(500));
    if (bsp_display_is_therapy_active() || sd_storage_recording_active() ||
        !bsp_display_try_reserve_therapy_safe_restart()) {
        bsp_display_set_notice("Restart cancelled: therapy recording is active");
        portENTER_CRITICAL(&s_state_lock);
        s_reboot_busy = false;
        if (from_wifi_save) s_wifi_save_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        netprov_lifecycle_release();
        vTaskDelete(NULL);
        return;
    }
    if (!sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE, 0)) {
        bsp_display_cancel_therapy_safe_restart();
        bsp_display_set_notice("Restart cancelled: microSD is busy");
        portENTER_CRITICAL(&s_state_lock);
        s_reboot_busy = false;
        if (from_wifi_save) s_wifi_save_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        netprov_lifecycle_release();
        vTaskDelete(NULL);
        return;
    }
    if (bsp_display_is_therapy_active() || sd_storage_recording_active() ||
        !bsp_display_try_commit_therapy_safe_restart()) {
        sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
        bsp_display_cancel_therapy_safe_restart();
        bsp_display_set_notice("Restart cancelled: therapy recording started");
        portENTER_CRITICAL(&s_state_lock);
        s_reboot_busy = false;
        if (from_wifi_save) s_wifi_save_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        netprov_lifecycle_release();
        vTaskDelete(NULL);
        return;
    }
    sd_storage_deinit();
    esp_restart();
}

static void reboot_cb(lv_event_t *event)
{
    (void)event;
    if (bsp_display_is_therapy_active() || sd_storage_recording_active()) {
        bsp_display_set_notice("Stop therapy before restarting");
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_reboot_busy || s_wifi_save_busy;
    if (!busy) s_reboot_busy = true;
    portEXIT_CRITICAL(&s_state_lock);
    if (busy) return;
    if (!netprov_lifecycle_try_claim("ui-reboot")) {
        portENTER_CRITICAL(&s_state_lock);
        s_reboot_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Restart deferred: update or restart in progress");
        return;
    }
    bsp_display_set_notice("Restarting SomnoTrace...");
    if (xTaskCreate(reboot_task, "ui_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        netprov_lifecycle_release();
        portENTER_CRITICAL(&s_state_lock);
        s_reboot_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Unable to start restart task");
    }
}

static void reboot_dialog_cb(lv_event_t *event)
{
    lv_obj_t *dialog = lv_event_get_current_target(event);
    const char *button = lv_msgbox_get_active_btn_text(dialog);
    if (!button) return;
    if (!strcmp(button, "Restart")) reboot_cb(NULL);
    lv_msgbox_close(dialog);
}

static void reboot_prompt_cb(lv_event_t *event)
{
    (void)event;
    if (bsp_display_is_therapy_active() || sd_storage_recording_active()) {
        bsp_display_set_notice("Stop therapy before restarting");
        return;
    }
    static const char *buttons[] = { "Cancel", "Restart", "" };
    lv_obj_t *dialog = lv_msgbox_create(
        NULL, "Restart SomnoTrace?",
        "The display, Bluetooth, and network services will be unavailable while the device restarts. Recorded nights are kept.",
        buttons, true);
    lv_obj_set_width(dialog, 620);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x172640), 0);
    lv_obj_set_style_text_color(dialog, lv_color_hex(0xe7edf7), 0);
    lv_obj_add_event_cb(dialog, reboot_dialog_cb, LV_EVENT_VALUE_CHANGED, NULL);
    track_manage_dialog(dialog);
    lv_obj_center(dialog);
}

typedef struct {
    char ssid[NETPROV_SSID_MAXLEN + 1];
    char password[NETPROV_PASS_MAXLEN + 1];
    bool keep_password;
} wifi_job_t;

static void wifi_save_task(void *arg)
{
    wifi_job_t *job = arg;
    struct netprov_config cfg = {0};
    netprov_load_config(&cfg);
    strlcpy(cfg.wifi[0].ssid, job->ssid, sizeof(cfg.wifi[0].ssid));
    if (!job->keep_password)
        strlcpy(cfg.wifi[0].pass, job->password, sizeof(cfg.wifi[0].pass));
    esp_err_t result = netprov_save_config(&cfg);
    free(job);
    if (result == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_wifi_save_busy = false;
        s_wifi_restart_pending = true;
        portEXIT_CRITICAL(&s_state_lock);
        if (bsp_display_is_therapy_active() || sd_storage_recording_active())
            bsp_display_set_notice("Wi-Fi saved; restart deferred while recording");
        else
            bsp_display_set_notice("Wi-Fi saved; restart required");
        vTaskDelete(NULL);
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_wifi_save_busy = false;
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_set_notice("Could not save Wi-Fi settings");
    vTaskDelete(NULL);
}

static void wifi_save_cb(lv_event_t *event)
{
    (void)event;
    if (!s_touch_services_ready) {
        bsp_display_set_notice("Network service is still starting");
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    bool restart_pending = s_wifi_restart_pending;
    bool busy = s_wifi_save_busy || s_reboot_busy;
    if (!busy && !restart_pending) s_wifi_save_busy = true;
    portEXIT_CRITICAL(&s_state_lock);
    if (busy) return;
    if (restart_pending) {
        reboot_prompt_cb(NULL);
        return;
    }
    const char *ssid = lv_textarea_get_text(s_wifi_ssid);
    const char *password = lv_textarea_get_text(s_wifi_password);
    if (!ssid || !ssid[0]) {
        portENTER_CRITICAL(&s_state_lock);
        s_wifi_save_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Enter a Wi-Fi network name");
        return;
    }
    wifi_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        portENTER_CRITICAL(&s_state_lock);
        s_wifi_save_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Unable to prepare Wi-Fi settings");
        return;
    }
    strlcpy(job->ssid, ssid, sizeof(job->ssid));
    strlcpy(job->password, password ? password : "", sizeof(job->password));
    job->keep_password = !s_wifi_force_clear_password && !job->password[0] &&
                         !strcmp(job->ssid, s_saved_wifi_ssid);
    close_keyboard_sheet(false);
    if (xTaskCreate(wifi_save_task, "ui_wifi_save", 4096, job, 4, NULL) != pdPASS) {
        free(job);
        portENTER_CRITICAL(&s_state_lock);
        s_wifi_save_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Unable to save Wi-Fi settings");
    }
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

    /* Modal controls can retain pointers into the current detail tree. Close
     * them while that tree is still valid, then release the destination when
     * Manage is no longer visible. */
    if (previous_page == 2) {
        close_keyboard_sheet(true);
        close_manage_dialog();
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
    if (section < 0 || section >= MANAGE_SECTION_COUNT) return;
    if (section == s_active_manage_section) return;

    /* Teardown must precede changing the selected index so callbacks and
     * periodic painters can no longer mistake the old tree for visible. */
    if (s_active_page == 2) {
        close_keyboard_sheet(true);
        close_manage_dialog();
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

static void storage_refresh_cb(lv_event_t *event)
{
    (void)event;
    start_storage_refresh();
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
    lv_obj_clear_flag(s_status_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_status_tray, LV_OBJ_FLAG_HIDDEN);
}

static void status_tray_route_cb(lv_event_t *event)
{
    int section = (int)(intptr_t)lv_event_get_user_data(event);
    status_tray_close_cb(NULL);
    /* Select while Manage is hidden so entering it constructs only the routed
     * destination instead of briefly constructing and discarding the prior
     * selection. */
    set_manage_section(section);
    set_active_page(2);
}

static void history_controller_changed(void *context)
{
    (void)context;
    /* This callback also runs on the History worker. It only schedules a
     * future LVGL-thread apply; task notification index 0 is deliberately not
     * used because the RGB driver owns it for its VSYNC handshake. */
    __atomic_store_n(&s_history_apply_scheduled, true, __ATOMIC_RELEASE);
}

static void history_route_card(void *context)
{
    (void)context;
    set_manage_section(MANAGE_STORAGE);
    set_active_page(2);
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

static void alert_test_task(void *arg)
{
    (void)arg;
    esp_err_t result = therapy_alert_send_test_push(NULL);
    portENTER_CRITICAL(&s_state_lock);
    s_alert_test_busy = false;
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_set_notice(result == ESP_OK ? "Test alert sent"
                                            : "Test alert could not be sent");
    vTaskDelete(NULL);
}

static void alert_test_cb(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_alert_test_busy;
    if (!busy) s_alert_test_busy = true;
    portEXIT_CRITICAL(&s_state_lock);
    if (busy) return;
    bsp_display_set_notice("Sending test alert...");
    if (xTaskCreate(alert_test_task, "ui_alert_test", 4096,
                    NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_alert_test_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Unable to start alert test");
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
            set_active_page(2);
            set_manage_section(MANAGE_DEVICES);
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

static void diagnostics_cb(lv_event_t *event)
{
    (void)event;
    ui_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_state_lock);

    const esp_app_desc_t *app = esp_app_get_description();
    UBaseType_t stack_free = s_lvgl_task ? uxTaskGetStackHighWaterMark(s_lvgl_task) : 0;
    char details[640];
    snprintf(details, sizeof(details),
             "Board        " UI_BOARD_NAME "\n"
             "Display      1024 x 600 RGB565\n"
             "Touch        %s (this tap was received)\n"
             "SD storage   %s\n"
             "Wi-Fi        %s\n"
             "AirSense 11  %s\n"
             "PSRAM free   %u KiB\n"
             "Internal RAM %u KiB\n"
             "UI stack min %u bytes free\n"
             "RGB frames   %lu (%lu sync timeouts)\n"
             "Touch errors %lu\n"
             "Backlight I2C errors %lu\n"
             "Firmware     %s",
             UI_TOUCH_STATUS,
             state.sd_ready ? "ready" : "not ready",
             state.wifi ? "connected" : "offline",
             state.paired ? "paired" : "not paired",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)stack_free,
             (unsigned long)s_flush_count,
             (unsigned long)s_flush_timeouts,
             (unsigned long)s_touch_read_errors,
             (unsigned long)s_backlight_write_errors,
             app ? app->version : "unknown");

    lv_obj_t *message = lv_msgbox_create(NULL, "Hardware diagnostics",
                                         details, NULL, true);
    lv_obj_set_width(message, 720);
    lv_obj_set_style_bg_color(message, lv_color_hex(0x121d32), 0);
    lv_obj_set_style_text_color(message, lv_color_hex(0xe7edf7), 0);
    track_manage_dialog(message);
    lv_obj_center(message);
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
static lv_obj_t *make_manage_section(lv_obj_t *section, int index,
                                     const char *title, const char *subtitle)
{
    make_label(section, title, 22, 17, 500, FONT_SCREEN_TITLE, COLOR_TEXT);
    lv_obj_t *sub = make_label(section, subtitle, 22, 45, 672,
                               FONT_BODY, COLOR_SECONDARY);
    if (index == MANAGE_DEVICES) s_device_section_subtitle = sub;
    if (index == MANAGE_CONNECTIVITY) s_connectivity_section_subtitle = sub;
    if (index == MANAGE_SYSTEM) s_system_section_subtitle = sub;
    s_manage_scrolls[index] = make_plain_container(
        section, 14, 76, UI_MANAGE_SCROLL_W, UI_MANAGE_SCROLL_H);
    /* Only panes that can exceed the viewport should participate in LVGL's
     * drag/throw machinery.  A vertical gesture on a short pane used to move
     * the entire surface elastically and redraw hundreds of thousands of
     * pixels even though there was nowhere useful to scroll. */
    bool can_overflow = index == MANAGE_DEVICES ||
                        index == MANAGE_CONNECTIVITY ||
                        index == MANAGE_UPLOADS ||
                        index == MANAGE_SYSTEM;
    if (can_overflow) {
        lv_obj_add_flag(s_manage_scrolls[index], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_manage_scrolls[index], LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_set_scroll_dir(s_manage_scrolls[index], LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_manage_scrolls[index],
                                  LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_width(s_manage_scrolls[index], 5, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(s_manage_scrolls[index],
                                  lv_color_hex(COLOR_TERTIARY),
                                  LV_PART_SCROLLBAR);
    }
    return s_manage_scrolls[index];
}

static lv_obj_t *make_manage_row(lv_obj_t *scroll, int y, int height)
{
    /* Only sections whose content actually overflows reserve the handoff's
     * 14 px scrollbar gutter. Short sections use the full 740 px column. */
    bool has_scroll_gutter = scroll == s_manage_scrolls[MANAGE_DEVICES] ||
                             scroll == s_manage_scrolls[MANAGE_CONNECTIVITY] ||
                             scroll == s_manage_scrolls[MANAGE_UPLOADS] ||
                             scroll == s_manage_scrolls[MANAGE_SYSTEM];
    lv_obj_t *row = make_card(scroll, 0, y,
                              has_scroll_gutter ? UI_MANAGE_ROW_W
                                                : UI_MANAGE_ROW_FULL_W,
                              height);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(row, 22, 0);
    lv_obj_set_style_pad_all(row, 16, 0);
    return row;
}

static void style_manage_surface(lv_obj_t *field)
{
    lv_obj_set_style_bg_color(field, lv_color_hex(COLOR_CONTROL), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0x454b58), 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(COLOR_LIVE),
                                  LV_STATE_FOCUSED);
    /* Do not grow the border on focus: changing it moves the content origin. */
    lv_obj_set_style_border_width(field, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_color(field, lv_color_hex(COLOR_LIVE),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(field, 8, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_opa(field, LV_OPA_20, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(field, 20, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_pad_left(field, 16, 0);
    lv_obj_set_style_pad_right(field, 16, 0);
}

static void style_manage_field(lv_obj_t *field)
{
    style_manage_surface(field);

    /* LVGL v8 starts dropdown and textarea copy at pad_top; it does not
     * vertically centre single-line content. Derive symmetric padding from
     * the actual control height and selected font so every field shares the
     * same optical centre. The one-pixel border is intentionally constant. */
    const lv_font_t *font = lv_obj_get_style_text_font(field, LV_PART_MAIN);
    /* Read the explicit style value: object coordinates still contain the
     * widget constructor's default height until LVGL's first layout pass. */
    lv_coord_t free_height = lv_obj_get_style_height(field, LV_PART_MAIN) -
                             lv_font_get_line_height(font) - 2;
    if (free_height < 0) free_height = 0;
    lv_obj_set_style_pad_top(field, free_height / 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(field, free_height - free_height / 2,
                                LV_PART_MAIN);
}

static void manage_textarea_scroll_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_FOCUSED ||
        code == LV_EVENT_SIZE_CHANGED) {
        /* LVGL's cursor visibility logic can introduce a vertical offset even
         * on one-line controls after a resize or password-mode transition. */
        lv_obj_scroll_to_y(lv_event_get_target(event), 0, LV_ANIM_OFF);
    }
}

static void style_manage_textarea(lv_obj_t *field)
{
    style_manage_field(field);
    /* The label and focused cursor need a little vertical scroll slack beyond
     * the font line box. Keep the centred top origin, leave the lower content
     * area open, and constrain one-line fields to horizontal scrolling. */
    lv_obj_set_style_pad_bottom(field, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(field, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(field, manage_textarea_scroll_cb, LV_EVENT_ALL, NULL);
}

static void build_devices_section(lv_obj_t *section)
{
    lv_obj_t *scroll = make_manage_section(section, MANAGE_DEVICES, "Devices",
                                            "Pair and manage bedside sensors");
    lv_obj_t *as = make_manage_row(scroll, 0, 230);
    s_as11_row = as;
    s_as11_dot = make_status_dot(as, 0, 23, 12);
    set_dot_tone(s_as11_dot, COLOR_LIVE, true);
    lv_obj_add_flag(s_as11_dot, LV_OBJ_FLAG_HIDDEN);
    s_as11_title = make_label(as, "AirSense 11", 0, 0, 180,
                              FONT_ROW_TITLE, COLOR_TEXT);
    s_as11_status = make_label(as, "Starting Bluetooth service...", 190, 2, 502,
                               FONT_BODY_SMALL, COLOR_SECONDARY);
    s_as11_badge = make_inner_card(as, 430, -3, 82, 32, 16);
    lv_obj_set_style_bg_color(s_as11_badge, lv_color_hex(0x123b40), 0);
    lv_obj_t *as_badge_label = make_label(s_as11_badge, "Paired", 0, 7, 82,
                                           FONT_BUTTON_SMALL, 0xbaf5f2);
    lv_obj_set_style_text_align(as_badge_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_as11_badge, LV_OBJ_FLAG_HIDDEN);
    static const char *steps[] = {
        "Searching", "Connecting", "Enter code", "Confirming", "Paired"
    };
    for (int i = 0; i < 5; ++i) {
        s_pair_steps[i] = lv_obj_create(as);
        lv_obj_set_pos(s_pair_steps[i], i * 134, 40);
        lv_obj_set_size(s_pair_steps[i], 124, 5);
        lv_obj_set_style_radius(s_pair_steps[i], 3, 0);
        lv_obj_set_style_border_width(s_pair_steps[i], 0, 0);
        lv_obj_set_style_bg_color(s_pair_steps[i], lv_color_hex(COLOR_CONTROL), 0);
        lv_obj_clear_flag(s_pair_steps[i],
                          LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_pair_step_labels[i] = make_label(as, steps[i], i * 134, 51, 124,
                                            FONT_BODY_SMALL,
                                            COLOR_TERTIARY);
    }
    s_as11_dropdown = lv_dropdown_create(as);
    lv_dropdown_set_options(s_as11_dropdown, "No devices found");
    lv_dropdown_set_symbol(s_as11_dropdown, NULL);
    lv_obj_set_pos(s_as11_dropdown, 0, 82);
    lv_obj_set_size(s_as11_dropdown, 324, 56);
    lv_obj_set_style_text_font(s_as11_dropdown, FONT_BODY_SMALL, 0);
    style_manage_field(s_as11_dropdown);
    make_manage_field_chevron(s_as11_dropdown);
    lv_obj_add_event_cb(s_as11_dropdown, manage_dropdown_list_ready_cb,
                        LV_EVENT_READY, NULL);
    s_ble_buttons[0] = make_touch_button(as, 336, 82, 94, 56,
                                         "AirSense is ready", COLOR_CONTROL,
                                         scan_cb, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(s_ble_buttons[0], 0),
                               FONT_BUTTON_COMPACT, 0);
    s_as11_pair_button = make_touch_button(as, 442, 82, 94, 56, "Pair",
                                           COLOR_INVERSE, device_action_cb,
                                           DEVICE_PAIR_AS11);
    s_ble_buttons[1] = s_as11_pair_button;
    lv_obj_set_style_text_color(lv_obj_get_child(s_as11_pair_button, 0),
                                lv_color_hex(COLOR_BASE), 0);
    s_ble_buttons[2] = make_touch_button(as, 548, 82, 146, 56,
                                         "Forget", COLOR_CONTROL,
                                         forget_prompt_cb, DEVICE_FORGET_AS11);
    set_button_surface(s_ble_buttons[2], 0x511e26, LV_OPA_COVER);
    lv_obj_set_style_border_width(s_ble_buttons[2], 1, 0);
    lv_obj_set_style_border_color(s_ble_buttons[2], lv_color_hex(COLOR_FAULT), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_ble_buttons[2], 0),
                                lv_color_hex(0xffd0ca), 0);
    s_passkey = lv_textarea_create(as);
    lv_textarea_set_one_line(s_passkey, true);
    lv_textarea_set_max_length(s_passkey, 4);
    lv_textarea_set_accepted_chars(s_passkey, "0123456789");
    lv_textarea_set_placeholder_text(s_passkey, "4-digit pairing code");
    lv_obj_set_pos(s_passkey, 0, 150);
    lv_obj_set_size(s_passkey, 324, 56);
    lv_obj_set_style_text_font(s_passkey, FONT_BODY, 0);
    style_manage_textarea(s_passkey);
    lv_obj_add_event_cb(s_passkey, passkey_focus_cb, LV_EVENT_FOCUSED, NULL);
    s_passkey_confirm_button = make_touch_button(as, 336, 150, 200, 56,
                                                  "Confirm code", COLOR_CONTROL,
                                                  device_action_cb,
                                                  DEVICE_CONFIRM_AS11);
    lv_obj_add_state(s_passkey, LV_STATE_DISABLED);
    lv_obj_add_state(s_passkey_confirm_button, LV_STATE_DISABLED);

    lv_obj_t *ox = make_manage_row(scroll, 238, 138);
    s_ox_row = ox;
    s_ox_dot = make_status_dot(ox, 0, 23, 12);
    set_dot_tone(s_ox_dot, COLOR_LIVE, true);
    lv_obj_add_flag(s_ox_dot, LV_OBJ_FLAG_HIDDEN);
    s_ox_title = make_label(ox, "O₂ Ring", 0, 0, 160,
                            FONT_ROW_TITLE, COLOR_TEXT);
    s_ox_status = make_label(ox, "Optional oxygen sensor", 170, 2, 522,
                             FONT_BODY_SMALL, COLOR_SECONDARY);
    s_ox_badge = make_inner_card(ox, 430, -3, 82, 32, 16);
    lv_obj_set_style_bg_color(s_ox_badge, lv_color_hex(0x123b40), 0);
    lv_obj_t *ox_badge_label = make_label(s_ox_badge, "Paired", 0, 7, 82,
                                           FONT_BUTTON_SMALL, 0xbaf5f2);
    lv_obj_set_style_text_align(ox_badge_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_ox_badge, LV_OBJ_FLAG_HIDDEN);
    s_ox_dropdown = lv_dropdown_create(ox);
    lv_dropdown_set_options(s_ox_dropdown, "No devices found");
    lv_dropdown_set_symbol(s_ox_dropdown, NULL);
    lv_obj_set_pos(s_ox_dropdown, 0, 48);
    lv_obj_set_size(s_ox_dropdown, 324, 56);
    lv_obj_set_style_text_font(s_ox_dropdown, FONT_BODY_SMALL, 0);
    style_manage_field(s_ox_dropdown);
    make_manage_field_chevron(s_ox_dropdown);
    lv_obj_add_event_cb(s_ox_dropdown, manage_dropdown_list_ready_cb,
                        LV_EVENT_READY, NULL);
    s_ble_buttons[3] = make_touch_button(ox, 336, 48, 94, 56,
                                         "Scan", COLOR_CONTROL, scan_cb, 1);
    s_ble_buttons[4] = make_touch_button(ox, 442, 48, 94, 56,
                                         "Pair", COLOR_INVERSE,
                                         device_action_cb, DEVICE_PAIR_OX);
    lv_obj_set_style_text_color(lv_obj_get_child(s_ble_buttons[4], 0),
                                lv_color_hex(COLOR_BASE), 0);
    s_ble_buttons[5] = make_touch_button(ox, 548, 48, 146, 56,
                                         "Forget", COLOR_CONTROL,
                                         forget_prompt_cb, DEVICE_FORGET_OX);
    set_button_surface(s_ble_buttons[5], 0x511e26, LV_OPA_COVER);
    lv_obj_set_style_border_width(s_ble_buttons[5], 1, 0);
    lv_obj_set_style_border_color(s_ble_buttons[5], lv_color_hex(COLOR_FAULT), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_ble_buttons[5], 0),
                                lv_color_hex(0xffd0ca), 0);

    s_device_change_row = make_manage_row(scroll, 384, 82);
    s_device_change_title = make_label(s_device_change_row, "Device changes", 0, 0,
                                       250, FONT_ROW_TITLE, COLOR_TEXT);
    s_device_change_detail = make_label(
        s_device_change_row,
        "Stop therapy first to pair or forget a device.",
        0, 29, 672, FONT_BODY_SMALL, COLOR_SECONDARY);
    lv_obj_add_flag(s_device_change_row, LV_OBJ_FLAG_HIDDEN);
}

static void __attribute__((unused)) build_connectivity_section(lv_obj_t *section)
{
    lv_obj_t *scroll = make_manage_section(section, MANAGE_CONNECTIVITY, "Connectivity",
                                            "Wi-Fi and local dashboard access");
    s_wifi_scan_button = make_touch_button(section, 616, 14, 134, 48,
                                            "Scan", COLOR_CONTROL,
                                            wifi_scan_request_cb, 0);
    lv_obj_set_style_radius(s_wifi_scan_button, 24, 0);
    s_wifi_scan_button_label = lv_obj_get_child(s_wifi_scan_button, 0);
    lv_obj_set_style_text_font(s_wifi_scan_button_label, FONT_BUTTON_COMPACT, 0);

    s_connectivity_rows[0] = make_manage_row(scroll, 0, 86);
    make_label(s_connectivity_rows[0], "Current network", 0, 0, 190,
               FONT_ROW_TITLE, COLOR_TEXT);
    s_network_status = make_label(s_connectivity_rows[0], "Checking network...", 200, 0, 492,
                                  FONT_BODY_SMALL, COLOR_SECONDARY);

    s_wifi_scan_row = make_manage_row(scroll, 94, 112);
    make_label(s_wifi_scan_row, "Nearby networks", 0, 0, 180,
               FONT_ROW_TITLE, COLOR_TEXT);
    s_wifi_scan_status = make_label(s_wifi_scan_row, "Preparing scan...", 0, 28, 180,
                                    FONT_BODY_SMALL, COLOR_SECONDARY);
    s_wifi_scan_dropdown = lv_dropdown_create(s_wifi_scan_row);
    lv_dropdown_set_options(s_wifi_scan_dropdown, "Scanning nearby networks...");
    lv_dropdown_set_symbol(s_wifi_scan_dropdown, NULL);
    lv_obj_set_pos(s_wifi_scan_dropdown, 190, 0);
    lv_obj_set_size(s_wifi_scan_dropdown, 354, 56);
    lv_obj_set_style_text_font(s_wifi_scan_dropdown, FONT_BODY_SMALL, 0);
    style_manage_field(s_wifi_scan_dropdown);
    make_manage_field_chevron(s_wifi_scan_dropdown);
    lv_obj_add_event_cb(s_wifi_scan_dropdown, manage_dropdown_list_ready_cb,
                        LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_wifi_scan_dropdown, wifi_scan_selection_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    s_wifi_scan_use_button = make_touch_button(s_wifi_scan_row, 556, 0, 138, 56,
                                                "Use", COLOR_INVERSE,
                                                wifi_scan_use_cb, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_wifi_scan_use_button, 0),
                                lv_color_hex(COLOR_BASE), 0);
    lv_obj_add_flag(s_wifi_scan_row, LV_OBJ_FLAG_HIDDEN);

    s_connectivity_rows[1] = make_manage_row(scroll, 94, 112);
    make_label(s_connectivity_rows[1], "Network name", 0, 0, 180,
               FONT_ROW_TITLE, COLOR_TEXT);
    s_wifi_ssid = lv_textarea_create(s_connectivity_rows[1]);
    lv_textarea_set_one_line(s_wifi_ssid, true);
    lv_textarea_set_max_length(s_wifi_ssid, NETPROV_SSID_MAXLEN);
    lv_textarea_set_placeholder_text(s_wifi_ssid, "Wi-Fi name (SSID)");
    lv_obj_set_pos(s_wifi_ssid, 190, 0);
    lv_obj_set_size(s_wifi_ssid, 504, 60);
    lv_obj_set_style_text_font(s_wifi_ssid, FONT_BODY, 0);
    style_manage_textarea(s_wifi_ssid);
    lv_obj_add_event_cb(s_wifi_ssid, text_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_wifi_ssid, wifi_field_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_connectivity_rows[2] = make_manage_row(scroll, 214, 112);
    make_label(s_connectivity_rows[2], "Password", 0, 0, 180,
               FONT_ROW_TITLE, COLOR_TEXT);
    s_wifi_password_helper = make_label(s_connectivity_rows[2],
                                        "Blank keeps the saved password",
                                        0, 28, 180, FONT_BODY_SMALL,
                                        COLOR_TERTIARY);
    s_wifi_password = lv_textarea_create(s_connectivity_rows[2]);
    lv_textarea_set_one_line(s_wifi_password, true);
    lv_textarea_set_password_mode(s_wifi_password, true);
    lv_textarea_set_max_length(s_wifi_password, NETPROV_PASS_MAXLEN);
    lv_textarea_set_placeholder_text(s_wifi_password, "Enter a new password");
    lv_obj_set_pos(s_wifi_password, 190, 0);
    lv_obj_set_size(s_wifi_password, 504, 60);
    lv_obj_set_style_text_font(s_wifi_password, FONT_BODY, 0);
    style_manage_textarea(s_wifi_password);
    lv_obj_set_style_pad_right(s_wifi_password, 128, 0);
    lv_obj_add_event_cb(s_wifi_password, text_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_wifi_password, wifi_field_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    s_wifi_password_reveal = make_touch_button(
        s_connectivity_rows[2], 574, 4, 112, 52, "Reveal",
        COLOR_CONTROL, wifi_password_reveal_cb, 0);
    lv_obj_set_style_radius(s_wifi_password_reveal, 26, 0);
    s_wifi_password_reveal_label = lv_obj_get_child(s_wifi_password_reveal, 0);
    lv_obj_set_style_text_font(s_wifi_password_reveal_label,
                               FONT_BUTTON_COMPACT, 0);

    s_connectivity_rows[3] = make_manage_row(scroll, 334, 82);
    make_label(s_connectivity_rows[3], "Setup hotspot", 0, 0, 360,
               FONT_ROW_TITLE, COLOR_TEXT);
    make_label(s_connectivity_rows[3], "Configure Wi-Fi from a phone", 0, 27, 390,
               FONT_BODY_SMALL, COLOR_SECONDARY);
    s_wifi_hotspot_button = make_touch_button(s_connectivity_rows[3], 528, -3, 166, 56,
                                               "Start", COLOR_CONTROL,
                                               action_cb, 3);

    s_connectivity_rows[4] = make_manage_row(scroll, 424, 82);
    make_label(s_connectivity_rows[4], "Apply network changes", 0, 0, 390,
               FONT_ROW_TITLE, COLOR_TEXT);
    s_wifi_restart_detail = make_label(
        s_connectivity_rows[4], "Save now; restart is requested separately.", 0, 27, 440,
        FONT_BODY_SMALL, COLOR_SECONDARY);
    s_wifi_save_button = make_touch_button(s_connectivity_rows[4], 494, -3, 200, 56,
                                            "Save changes", COLOR_INVERSE,
                                            wifi_save_cb, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_wifi_save_button, 0),
                                lv_color_hex(COLOR_BASE), 0);
}

static void __attribute__((unused)) build_alerts_section(lv_obj_t *section)
{
    lv_obj_t *scroll = make_manage_section(section, MANAGE_ALERTS, "Alerts",
                                            "If therapy stops unexpectedly overnight");
    lv_obj_t *status = make_manage_row(scroll, 0, 114);
    make_label(status, "Push alerts", 0, 0, 190,
               FONT_ROW_TITLE, COLOR_TEXT);
    s_alert_status = make_label(status, "Reading alert settings...", 200, 0, 494,
                                FONT_BODY_SMALL, COLOR_SECONDARY);
    make_label(status,
               "This 7-inch board has no onboard speaker; persistent on-screen alerts still wake the display.",
               0, 48, 672, FONT_BODY_SMALL, COLOR_SECONDARY);

    lv_obj_t *test = make_manage_row(scroll, 122, 82);
    make_label(test, "Test alert", 0, 0, 390,
               FONT_ROW_TITLE, COLOR_TEXT);
    make_label(test, "Sends one real test push notification", 0, 27, 430,
               FONT_BODY_SMALL, COLOR_SECONDARY);
    s_alert_test_button = make_touch_button(test, 494, -3, 200, 56,
                                             "Send test push", COLOR_INVERSE,
                                             alert_test_cb, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_alert_test_button, 0),
                                lv_color_hex(COLOR_BASE), 0);

    lv_obj_t *browser = make_manage_row(scroll, 212, 92);
    make_label(browser, "Alert schedule and delivery", 0, 0, 390,
               FONT_ROW_TITLE, COLOR_TEXT);
    make_label(browser,
               "Notification server, schedule, and escalation settings are configured in the browser dashboard.",
               0, 29, 470, FONT_BODY_SMALL, COLOR_SECONDARY);
    make_label(browser, "Browser dashboard only", 522, 20, 172,
               FONT_BODY_SMALL, COLOR_TERTIARY);
}

static void __attribute__((unused)) build_uploads_section(lv_obj_t *section)
{
    lv_obj_t *scroll = make_manage_section(section, MANAGE_UPLOADS, "Uploads",
                                            "Destination status and retry progress");

    for (int i = 0; i < UPLOADER_PROGRESS_MAX_BACKENDS; ++i) {
        lv_obj_t *row = make_manage_row(scroll, i * 100, 92);
        s_upload_rows[i] = row;
        s_upload_dots[i] = make_status_dot(row, 0, 7, 12);
        s_upload_titles[i] = make_label(row, "Upload destination", 28, 0, 330,
                                        FONT_ROW_TITLE, COLOR_TEXT);
        s_upload_states[i] = make_label(row, "Checking", 542, 1, 152,
                                        FONT_BODY, COLOR_TERTIARY);
        lv_obj_set_style_text_align(s_upload_states[i], LV_TEXT_ALIGN_RIGHT, 0);
        s_upload_details[i] = make_label(row, "Reading upload status...", 28, 29,
                                         642, FONT_BODY_SMALL, COLOR_SECONDARY);
        s_upload_meters[i] = lv_bar_create(row);
        lv_obj_set_pos(s_upload_meters[i], 28, 54);
        lv_obj_set_size(s_upload_meters[i], 642, 8);
        lv_bar_set_range(s_upload_meters[i], 0, 100);
        lv_bar_set_value(s_upload_meters[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_upload_meters[i], lv_color_hex(COLOR_CONTROL),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_upload_meters[i], lv_color_hex(COLOR_LIVE),
                                  LV_PART_INDICATOR);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    s_upload_browser_row = make_manage_row(scroll, 0, 96);
    make_label(s_upload_browser_row, "Upload configuration", 0, 0, 270,
               FONT_ROW_TITLE, COLOR_TEXT);
    make_label(s_upload_browser_row,
               "Credentials, FTP access, formatting, and bulk upload controls stay in the browser dashboard.",
               0, 29, 470, FONT_BODY_SMALL, COLOR_SECONDARY);
    make_label(s_upload_browser_row, "Browser dashboard only", 522, 22, 172,
               FONT_BODY_SMALL, COLOR_TERTIARY);
}

static void maintenance_navigate(maintenance_ui_navigation_t destination)
{
    static const int sections[] = { MANAGE_DEVICES, MANAGE_CONNECTIVITY,
                                    MANAGE_UPLOADS, MANAGE_LOGS };
    if ((unsigned)destination < sizeof(sections)/sizeof(sections[0]))
        set_manage_section(sections[destination]);
}

static void maintenance_restart(void) { reboot_prompt_cb(NULL); }
static void maintenance_screen_off(void)
{
    if (!screen_wake_input_available()) {
        bsp_display_set_notice("Touch is unavailable - screen kept on");
        return;
    }
    bsp_display_set_backlight(false);
}

static void build_maintenance_section(lv_obj_t *section,
                                      maintenance_ui_destination_t destination)
{
    const maintenance_ui_hooks_t hooks = {
        .navigate = maintenance_navigate, .restart = maintenance_restart,
        .screen_off = maintenance_screen_off, .wake_available = screen_wake_input_available,
    };
    esp_err_t result = touch_maintenance_ui_show(section, destination, &hooks);
    if (result == ESP_OK)
        touch_maintenance_ui_refresh(s_touch_services_ready);
    else
        make_label(section, "Maintenance pane unavailable: insufficient memory",
                   16, 24, 700, FONT_BODY, COLOR_FAULT);
}

static void build_storage_section(lv_obj_t *section)
{ build_maintenance_section(section, MAINT_UI_STORAGE); }
static void build_system_section(lv_obj_t *section)
{ build_maintenance_section(section, MAINT_UI_SYSTEM); }
static void build_advanced_section(lv_obj_t *section)
{ build_maintenance_section(section, MAINT_UI_ADVANCED); }

static void clear_manage_section_pointers(int section)
{
    if (section < 0 || section >= MANAGE_SECTION_COUNT) return;
    s_manage_sections[section] = NULL;
    s_manage_scrolls[section] = NULL;

    switch ((manage_section_t)section) {
    case MANAGE_DEVICES:
        s_as11_row = NULL;
        s_ox_row = NULL;
        s_as11_title = NULL;
        s_ox_title = NULL;
        s_as11_dot = NULL;
        s_ox_dot = NULL;
        s_as11_status = NULL;
        s_as11_badge = NULL;
        s_as11_dropdown = NULL;
        s_as11_pair_button = NULL;
        memset(s_ble_buttons, 0, sizeof(s_ble_buttons));
        memset(s_pair_steps, 0, sizeof(s_pair_steps));
        memset(s_pair_step_labels, 0, sizeof(s_pair_step_labels));
        s_passkey_confirm_button = NULL;
        s_passkey = NULL;
        s_ox_status = NULL;
        s_ox_badge = NULL;
        s_ox_dropdown = NULL;
        s_device_change_row = NULL;
        s_device_change_title = NULL;
        s_device_change_detail = NULL;
        s_device_section_subtitle = NULL;
        s_seen_as11_version = UINT_MAX;
        s_seen_ox_version = UINT_MAX;
        break;
    case MANAGE_CONNECTIVITY:
        s_network_status = NULL;
        s_wifi_ssid = NULL;
        s_wifi_password = NULL;
        memset(s_connectivity_rows, 0, sizeof(s_connectivity_rows));
        s_wifi_scan_button = NULL;
        s_wifi_scan_button_label = NULL;
        s_wifi_scan_row = NULL;
        s_wifi_scan_status = NULL;
        s_wifi_scan_dropdown = NULL;
        s_wifi_scan_use_button = NULL;
        s_wifi_password_helper = NULL;
        s_wifi_password_reveal = NULL;
        s_wifi_password_reveal_label = NULL;
        s_wifi_restart_detail = NULL;
        s_wifi_save_button = NULL;
        s_wifi_hotspot_button = NULL;
        s_connectivity_section_subtitle = NULL;
        s_wifi_password_revealed = false;
        s_wifi_scan_seen_generation = UINT32_MAX;
        s_wifi_scan_seen_state = NETPROV_SCAN_IDLE;
        s_connectivity_settings_synced = false;
        break;
    case MANAGE_ALERTS:
        s_alert_status = NULL;
        s_alert_test_button = NULL;
        break;
    case MANAGE_UPLOADS:
        memset(s_upload_rows, 0, sizeof(s_upload_rows));
        memset(s_upload_dots, 0, sizeof(s_upload_dots));
        memset(s_upload_titles, 0, sizeof(s_upload_titles));
        memset(s_upload_details, 0, sizeof(s_upload_details));
        memset(s_upload_states, 0, sizeof(s_upload_states));
        memset(s_upload_meters, 0, sizeof(s_upload_meters));
        s_upload_browser_row = NULL;
        break;
    case MANAGE_STORAGE:
        s_storage_status = NULL;
        s_storage_estimate = NULL;
        s_storage_meter = NULL;
        s_storage_refresh_button = NULL;
        s_storage_browser_row = NULL;
        break;
    case MANAGE_SYSTEM:
        s_system_health_title = NULL;
        s_system_health_dot = NULL;
        s_system_details = NULL;
        s_system_firmware = NULL;
        s_system_restart_detail = NULL;
        s_reboot_button = NULL;
        s_system_section_subtitle = NULL;
        break;
    case MANAGE_LOGS:
    case MANAGE_ADVANCED:
        break;
    }
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
    close_keyboard_sheet(true);
    close_manage_dialog();
    lv_obj_t *root = s_manage_sections[section];
    s_rendered_manage_section = -1;
    if (section == MANAGE_LOGS) touch_logs_controller_hide();
    if (section == MANAGE_CONNECTIVITY || section == MANAGE_ALERTS || section == MANAGE_UPLOADS)
        touch_manage_config_hide();

    if (section == MANAGE_STORAGE || section == MANAGE_SYSTEM ||
        section == MANAGE_ADVANCED) touch_maintenance_ui_destroy();
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
    case MANAGE_DEVICES:
        build_devices_section(destination);
        break;
    case MANAGE_CONNECTIVITY:
        touch_manage_config_show(destination, MC_WIFI);
        break;
    case MANAGE_ALERTS:
        touch_manage_config_show(destination, MC_ALERTS);
        break;
    case MANAGE_UPLOADS:
        touch_manage_config_show(destination, MC_UPLOADS);
        break;
    case MANAGE_STORAGE:
        build_storage_section(destination);
        break;
    case MANAGE_SYSTEM:
        build_system_section(destination);
        break;
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
    case MANAGE_ADVANCED:
        build_advanced_section(destination);
        break;
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
    static const char *section_names[] = {
        "Devices", "Connectivity", "Alerts", "Uploads",
        "Storage", "System", "Logs", "Advanced"
    };
    lv_obj_t *rail = make_card(manage, UI_PANEL_X, UI_PANEL_Y,
                               UI_MANAGE_RAIL_W, UI_PANEL_H);
    lv_obj_set_style_radius(rail, 28, 0);
    lv_obj_set_style_pad_all(rail, 8, 0);
    for (int i = 0; i < MANAGE_SECTION_COUNT; ++i) {
        s_manage_buttons[i] = make_destination_button(
            rail, 0, i * 52, 196, 46, section_names[i], COLOR_PANEL,
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

static const char *s_passkey_keyboard_map[] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", "Clear", "\n",
    "7", "8", "9", "0", ""
};

static const lv_btnmatrix_ctrl_t s_passkey_keyboard_ctrl[] = {
    1, 1, 1, 1,
    1, 1, 1, 1,
    1, 1, 1, 1,
};

/* Five compact rows reproduce the designer's 1024x600 text-entry sheet while
 * keeping every key inside the visible framebuffer. A literal space remains
 * a wide, intentionally blank key so LVGL inserts the correct character. */
static const char *s_text_keyboard_lower_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", ".", "123", "\n",
    "@", "space", "-", "_", ""
};

static const char *s_text_keyboard_upper_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", ".", "123", "\n",
    "@", "space", "-", "_", ""
};

static const lv_btnmatrix_ctrl_t s_text_keyboard_ctrl[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 8,
    7, 5, 5, 5, 5, 5, 5, 5, 5, 7,
    1, 5, 1, 1,
};

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
    static const char *tray_actions[] = {
        "Pair", "Manage", "Manage", "", "Pair"
    };
    static const int tray_sections[] = {
        MANAGE_DEVICES, MANAGE_STORAGE, MANAGE_CONNECTIVITY,
        MANAGE_UPLOADS, MANAGE_DEVICES
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
        s_status_tray_actions[i] = make_destination_button(
            row, 370, 11, 82, 44, tray_actions[i], COLOR_CONTROL,
            status_tray_route_cb, tray_sections[i]);
        set_destination_surface(s_status_tray_actions[i], COLOR_CONTROL,
                                LV_OPA_COVER);
        lv_obj_set_style_text_font(lv_obj_get_child(s_status_tray_actions[i], 0),
                                   FONT_BODY_SMALL, 0);
        lv_obj_add_flag(s_status_tray_actions[i], LV_OBJ_FLAG_HIDDEN);
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

    /* Text entry is a deliberate bottom sheet: the field behind it never
     * becomes the only way to commit or abandon an edit. Extending the card
     * below the framebuffer leaves square clipped bottom corners and the
     * handoff's 34 px rounded top corners. */
    s_keyboard_sheet = make_card(screen, 0, 314, 1024, 320);
    lv_obj_set_style_radius(s_keyboard_sheet, 34, 0);
    lv_obj_set_style_pad_all(s_keyboard_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_keyboard_sheet, lv_color_hex(0x1c202a), 0);
    lv_obj_set_style_bg_grad_dir(s_keyboard_sheet, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_shadow_width(s_keyboard_sheet,
                                  UI_DECORATIVE_SHADOW_WIDTH(50), 0);
    lv_obj_set_style_shadow_ofs_y(s_keyboard_sheet, -18, 0);
    lv_obj_set_style_shadow_opa(s_keyboard_sheet,
                                UI_DECORATIVE_SHADOW_OPA(LV_OPA_70), 0);
    s_keyboard_title = make_label(s_keyboard_sheet, "Network password",
                                  18, 24, 430,
                                  FONT_BUTTON, COLOR_TEXT);
    lv_obj_t *keyboard_cancel = make_touch_button(
        s_keyboard_sheet, 798, 14, 92, 42, "Cancel", COLOR_CONTROL,
        keyboard_sheet_action_cb, 0);
    lv_obj_set_style_radius(keyboard_cancel, 21, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(keyboard_cancel, 0),
                               FONT_BUTTON_COMPACT, 0);
    lv_obj_t *keyboard_done = make_touch_button(
        s_keyboard_sheet, 898, 14, 104, 42, "Done", COLOR_INVERSE,
        keyboard_sheet_action_cb, 1);
    lv_obj_set_style_radius(keyboard_done, 21, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(keyboard_done, 0),
                               FONT_BUTTON_COMPACT, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(keyboard_done, 0),
                                lv_color_hex(COLOR_BASE), 0);

    s_keyboard = lv_keyboard_create(s_keyboard_sheet);
    lv_obj_remove_event_cb(s_keyboard, lv_keyboard_def_event_cb);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_NUMBER,
                        s_passkey_keyboard_map, s_passkey_keyboard_ctrl);
    lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER,
                        s_text_keyboard_lower_map, s_text_keyboard_ctrl);
    lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER,
                        s_text_keyboard_upper_map, s_text_keyboard_ctrl);
    lv_obj_set_align(s_keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_keyboard, 14, 67);
    lv_obj_set_size(s_keyboard, 996, 203);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_keyboard, 9, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_keyboard, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(COLOR_CONTROL),
                              LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_keyboard, 16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard, lv_color_hex(COLOR_TEXT),
                                LV_PART_ITEMS);
    /* The shared map contains Shift/Backspace symbols absent from the custom
     * prose fonts. Keep those controls readable in every keyboard consumer. */
    lv_obj_set_style_text_font(s_keyboard, LV_FONT_DEFAULT, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_keyboard_sheet, LV_OBJ_FLAG_HIDDEN);

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

    set_manage_section(MANAGE_DEVICES);
    set_active_page(0);
    lv_obj_invalidate(screen);
}

static void refresh_device_dropdown(bool oxygen, const ui_service_state_t *services)
{
    unsigned version = oxygen ? services->ox_version : services->as11_version;
    unsigned *seen = oxygen ? &s_seen_ox_version : &s_seen_as11_version;
    if (*seen == version) return;
    *seen = version;
    const ui_device_result_t *items = oxygen ? services->ox : services->as11;
    size_t count = oxygen ? services->ox_count : services->as11_count;
    lv_obj_t *dropdown = oxygen ? s_ox_dropdown : s_as11_dropdown;
    char options[700] = {0};
    if (count == 0) {
        strlcpy(options, "No devices found", sizeof(options));
    } else {
        for (size_t i = 0; i < count; ++i) {
            size_t used = strlen(options);
            snprintf(options + used, sizeof(options) - used, "%s%s (%d dBm)",
                     i ? "\n" : "", items[i].name, items[i].rssi);
        }
    }
    lv_dropdown_set_options(dropdown, options);
}

static const char *friendly_as11_status(const char *status, bool scanning)
{
    if (scanning || !strcmp(status, AS11_STATUS_SCANNING))
        return "Searching for nearby machines";
    if (!strcmp(status, AS11_STATUS_CONNECTING)) return "Connecting securely";
    if (!strcmp(status, AS11_STATUS_WAIT_PASSKEY))
        return "Enter the 4-digit code shown on your AirSense";
    if (!strcmp(status, AS11_STATUS_CONFIRMING)) return "Confirming the code";
    if (!strcmp(status, AS11_STATUS_PAIRED)) return "Paired and ready";
    if (!strcmp(status, AS11_STATUS_ERROR))
        return "Pairing failed · enable pairing mode first";
    if (!strcmp(status, "simulated preview"))
        return "Paired · simulated device ready";
    return "Then tap AirSense is ready to scan";
}

static const char *friendly_ox_status(const char *status, bool scanning)
{
    if (scanning || !strcmp(status, OX_STATUS_SCANNING))
        return "Searching for nearby rings";
    if (!strcmp(status, OX_STATUS_CONNECTING)) return "Connecting to ring";
    if (!strcmp(status, OX_STATUS_PULLING)) return "Importing ring data";
    if (!strcmp(status, OX_STATUS_PAIRED)) return "Paired and ready";
    if (!strcmp(status, OX_STATUS_MONITORING)) return "Monitoring live data";
    if (!strcmp(status, OX_STATUS_ERROR)) return "Ring needs attention";
    if (!strcmp(status, "simulated preview"))
        return "Paired · simulated device ready";
    return "Ready to scan";
}

static int pairing_step(const char *status, bool scanning)
{
    if (scanning || !strcmp(status, AS11_STATUS_SCANNING)) return 0;
    if (!strcmp(status, AS11_STATUS_CONNECTING)) return 1;
    if (!strcmp(status, AS11_STATUS_WAIT_PASSKEY)) return 2;
    if (!strcmp(status, AS11_STATUS_CONFIRMING)) return 3;
    if (!strcmp(status, AS11_STATUS_PAIRED) ||
        !strcmp(status, "simulated preview")) return 4;
    return -1;
}

static const char *friendly_alert_state(alert_state_t state)
{
    switch (state) {
        case ALERT_ARMED: return "Push ready";
        case ALERT_PENDING: return "Therapy stop detected";
        case ALERT_PUSH_SENT: return "Push service accepted";
        case ALERT_SCREEN_ONLY: return "Screen alert only";
        case ALERT_PUSH_FAILED: return "Push failed; screen alert active";
        case ALERT_BUZZING: return "Escalated screen alert";
        case ALERT_ACKED: return "Acknowledged";
        case ALERT_DISARMED:
        default: return "Push unavailable";
    }
}

static void format_upload_day(const char *day, char *out, size_t out_len)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (day && strlen(day) == 8) {
        int month = (day[4] - '0') * 10 + day[5] - '0';
        int date = (day[6] - '0') * 10 + day[7] - '0';
        if (month >= 1 && month <= 12 && date >= 1 && date <= 31) {
            snprintf(out, out_len, "%d %s", date, months[month - 1]);
            return;
        }
    }
    strlcpy(out, "current night", out_len);
}

static void format_upload_success(const uploader_backend_progress_t *backend,
                                  char *out, size_t out_len)
{
    if (!backend->last_success_valid) {
        strlcpy(out, "No successful upload recorded yet", out_len);
        return;
    }
    time_t epoch = (time_t)backend->last_success_epoch_s;
    struct tm local;
    if (localtime_r(&epoch, &local)) {
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%e %b %H:%M", &local);
        snprintf(out, out_len, "Last success %s", stamp);
    } else {
        strlcpy(out, "Last success recorded", out_len);
    }
}

static void refresh_upload_destinations(const ui_service_state_t *services)
{
    const uploader_progress_snapshot_t *progress = &services->upload_progress;
    size_t count = services->upload_progress_result == ESP_OK
                       ? progress->backend_count : 0;
    if (count > UPLOADER_PROGRESS_MAX_BACKENDS)
        count = UPLOADER_PROGRESS_MAX_BACKENDS;

    if (services->storage_busy || count == 0) {
        count = 1;
        lv_label_set_text(s_upload_titles[0], "Upload destinations");
        lv_label_set_text(s_upload_states[0],
                          services->storage_busy ? "Checking" : "Unavailable");
        lv_label_set_text(s_upload_details[0],
                          services->storage_busy
                              ? "Reading synchronized upload status..."
                              : "Upload service status is not available yet");
        set_dot_tone(s_upload_dots[0], COLOR_DISABLED, false);
        lv_obj_add_flag(s_upload_meters[0], LV_OBJ_FLAG_HIDDEN);
    } else {
        for (size_t i = 0; i < count; ++i) {
            const uploader_backend_progress_t *backend = &progress->backends[i];
            char detail[160];
            uint32_t tone = COLOR_LIVE;
            const char *state_text = "Enabled";
            bool show_meter = false;
            int meter = 0;

            lv_label_set_text(s_upload_titles[i], backend->label[0]
                                                      ? backend->label
                                                      : backend->id);
            if (!backend->configured ||
                backend->state == UPLOADER_BACKEND_DISABLED) {
                state_text = "Disabled";
                tone = COLOR_DISABLED;
                strlcpy(detail,
                        "Not configured or disabled · configure in the browser dashboard",
                        sizeof(detail));
            } else if (backend->state == UPLOADER_BACKEND_UPLOADING) {
                state_text = "Uploading";
                char day[24];
                format_upload_day(backend->current_day, day, sizeof(day));
                if (backend->current_valid && backend->current_units > 0) {
                    snprintf(detail, sizeof(detail),
                             "%s · %d of %d parts complete · %d of %d nights",
                             day, backend->current_unit, backend->current_units,
                             backend->days_done, backend->days_total);
                    meter = backend->current_unit * 100 / backend->current_units;
                } else {
                    snprintf(detail, sizeof(detail), "%d of %d nights uploaded",
                             backend->days_done, backend->days_total);
                }
                show_meter = true;
            } else if (backend->state == UPLOADER_BACKEND_COOLDOWN) {
                state_text = backend->error_permanent ? "Needs setup" : "Retrying";
                tone = backend->error_permanent ? COLOR_FAULT : COLOR_AMBER;
                if (backend->retry_in_s >= 120) {
                    snprintf(detail, sizeof(detail), "Retry in %lu min%s%s",
                             (unsigned long)((backend->retry_in_s + 59) / 60),
                             backend->error_valid ? " · " : "",
                             backend->error_valid ? backend->error : "");
                } else {
                    snprintf(detail, sizeof(detail), "Retry in %lu s%s%s",
                             (unsigned long)backend->retry_in_s,
                             backend->error_valid ? " · " : "",
                             backend->error_valid ? backend->error : "");
                }
                show_meter = backend->days_total > 0;
            } else {
                char last_success[64];
                format_upload_success(backend, last_success, sizeof(last_success));
                state_text = backend->days_total > 0 &&
                                     backend->days_done >= backend->days_total
                                 ? "Up to date" : "Enabled";
                snprintf(detail, sizeof(detail), "%d of %d nights uploaded · %s",
                         backend->days_done, backend->days_total, last_success);
                show_meter = backend->days_total > 0;
            }

            if (show_meter && backend->state != UPLOADER_BACKEND_UPLOADING &&
                backend->days_total > 0)
                meter = backend->days_done * 100 / backend->days_total;
            if (meter < 0) meter = 0;
            if (meter > 100) meter = 100;
            lv_label_set_text(s_upload_states[i], state_text);
            lv_obj_set_style_text_color(s_upload_states[i], lv_color_hex(tone), 0);
            lv_label_set_text(s_upload_details[i], detail);
            set_dot_tone(s_upload_dots[i], tone, tone != COLOR_DISABLED);
            lv_bar_set_value(s_upload_meters[i], meter, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_upload_meters[i], lv_color_hex(tone),
                                      LV_PART_INDICATOR);
            set_hidden(s_upload_meters[i], !show_meter);
        }
    }

    for (size_t i = 0; i < UPLOADER_PROGRESS_MAX_BACKENDS; ++i)
        set_hidden(s_upload_rows[i], i >= count);
    lv_obj_set_y(s_upload_browser_row, (int)count * 100);
}

static void set_control_disabled(lv_obj_t *control, bool disabled)
{
    if (disabled) lv_obj_add_state(control, LV_STATE_DISABLED);
    else lv_obj_clear_state(control, LV_STATE_DISABLED);
}

static void __attribute__((unused)) refresh_wifi_scan_controls(void)
{
    netprov_scan_snapshot_t snapshot;
    wifi_scan_snapshot(&snapshot);
    bool running = snapshot.state == NETPROV_SCAN_RUNNING;
    const char *blocked = wifi_scan_interaction_reason(running);

    set_control_disabled(s_wifi_scan_button, blocked != NULL);
    if (!s_touch_services_ready)
        lv_label_set_text(s_wifi_scan_button_label, "Starting...");
    else if (bsp_display_is_therapy_active() || sd_storage_recording_active())
        lv_label_set_text(s_wifi_scan_button_label, "Stop therapy");
    else if (s_keyboard_target)
        lv_label_set_text(s_wifi_scan_button_label, "Editing");
    else if (running)
        lv_label_set_text(s_wifi_scan_button_label, "Scanning...");
    else {
        portENTER_CRITICAL(&s_state_lock);
        bool save_busy = s_wifi_save_busy;
        bool reboot_busy = s_reboot_busy;
        bool restart_pending = s_wifi_restart_pending;
        portEXIT_CRITICAL(&s_state_lock);
        lv_label_set_text(s_wifi_scan_button_label,
                          save_busy ? "Saving..." :
                          reboot_busy ? "Restarting" :
                          restart_pending ? "Restart first" : "Scan");
    }

    layout_connectivity_rows();
    if (!s_wifi_scan_requested) return;

    bool ready = snapshot.state == NETPROV_SCAN_READY && snapshot.count > 0;
    set_control_disabled(s_wifi_scan_dropdown, !ready || blocked != NULL);
    set_control_disabled(s_wifi_scan_use_button, !ready || blocked != NULL);

    if (snapshot.generation != s_wifi_scan_seen_generation ||
        snapshot.state != s_wifi_scan_seen_state) {
        char options[NETPROV_SCAN_MAX_APS * (NETPROV_SSID_MAXLEN + 24)] = {0};
        if (ready) {
            for (size_t i = 0; i < snapshot.count; ++i) {
                size_t used = strlen(options);
                snprintf(options + used, sizeof(options) - used,
                         "%s%s · %d dBm · %s",
                         i ? "\n" : "", snapshot.aps[i].ssid,
                         snapshot.aps[i].rssi,
                         snapshot.aps[i].secure ? "secured" : "open");
            }
        } else if (snapshot.state == NETPROV_SCAN_RUNNING) {
            strlcpy(options, "Scanning nearby networks...", sizeof(options));
        } else if (snapshot.state == NETPROV_SCAN_BLOCKED) {
            strlcpy(options, "Scan unavailable", sizeof(options));
        } else if (snapshot.state == NETPROV_SCAN_ERROR) {
            strlcpy(options, "Scan failed", sizeof(options));
        } else {
            strlcpy(options, "No networks found", sizeof(options));
        }
        lv_dropdown_set_options(s_wifi_scan_dropdown, options);
        s_wifi_scan_seen_generation = snapshot.generation;
        s_wifi_scan_seen_state = snapshot.state;
    }

    if (snapshot.state == NETPROV_SCAN_RUNNING) {
        lv_label_set_text(s_wifi_scan_status, "Scanning · please wait");
    } else if (snapshot.state == NETPROV_SCAN_READY && snapshot.count > 0) {
        if (s_wifi_scan_open_selected)
            lv_label_set_text(s_wifi_scan_status,
                              "Open selected · no password");
        else
            lv_label_set_text_fmt(s_wifi_scan_status, "%u found · choose one",
                                  (unsigned)snapshot.count);
    } else if (snapshot.state == NETPROV_SCAN_READY) {
        lv_label_set_text(s_wifi_scan_status, "No networks found · scan again");
    } else if (snapshot.state == NETPROV_SCAN_BLOCKED) {
        if (snapshot.blocked_by == NETPROV_SCAN_BLOCK_RECORDING)
            lv_label_set_text(s_wifi_scan_status, "Stop therapy to scan");
        else if (snapshot.blocked_by == NETPROV_SCAN_BLOCK_NOT_INITIALIZED)
            lv_label_set_text(s_wifi_scan_status, "Network service is starting");
        else
            lv_label_set_text(s_wifi_scan_status, "Wi-Fi is reconnecting");
    } else if (snapshot.state == NETPROV_SCAN_ERROR) {
        lv_label_set_text(s_wifi_scan_status, "Scan failed · try again");
    } else {
        lv_label_set_text(s_wifi_scan_status, "Tap Scan to search");
    }
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
    manage_config_health_t configuration;
    touch_manage_config_health(&configuration);
    bool channel_attention = configuration.ready && configuration.alerts_enabled &&
        (!configuration.push_configured || !configuration.push_verified || !state->wifi);
    /* Armed alone establishes the schedule, not push deliverability. */
    dots[MANAGE_ALERTS] = active_alert ? COLOR_FAULT :
                          alert == ALERT_ARMED ? COLOR_AMBER : COLOR_TERTIARY;
    if (configuration.ready && !active_alert) {
        dots[MANAGE_ALERTS] = configuration.history_storage_error ? COLOR_FAULT :
            !configuration.alerts_enabled ? COLOR_TERTIARY :
            channel_attention ? COLOR_AMBER : COLOR_LIVE;
    }
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
        badges[MANAGE_UPLOADS] || active_alert || channel_attention ||
        configuration.history_storage_error ? COLOR_AMBER : COLOR_LIVE;

    log_stream_retained_info_t logs;
    esp_err_t log_result = log_stream_retained_get_info(&logs);
    dots[MANAGE_LOGS] = log_result == ESP_OK && logs.available ?
        (touch_logs_controller_is_paused() ? COLOR_AMBER : COLOR_LIVE) :
        s_touch_services_ready ? COLOR_FAULT : COLOR_TERTIARY;
    for (size_t i = 0; i < MANAGE_SECTION_COUNT; ++i) {
        set_dot_tone(s_manage_dots[i], dots[i], dots[i] != COLOR_TERTIARY);
        set_hidden(s_manage_badges[i], badges[i] == 0);
        if (badges[i]) set_label_text_fmt_if_changed(s_manage_badges[i], "%u", badges[i]);
    }
}

static void refresh_secondary_pages(const ui_state_t *state, int active_tab)
{
    const ui_service_state_t *services = s_render_services;
    ble_ui_operation_t ble_operation;
    int64_t ble_started;
    bool reboot_busy;
    bool wifi_save_busy;
    bool pairing_mode_confirmed;
    portENTER_CRITICAL(&s_state_lock);
    ble_operation = s_ble_operation;
    ble_started = s_ble_operation_started_us;
    reboot_busy = s_reboot_busy;
    wifi_save_busy = s_wifi_save_busy;
    pairing_mode_confirmed = s_as11_pairing_mode_confirmed;
    portEXIT_CRITICAL(&s_state_lock);

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    const char *as_status = "simulated preview";
    const char *ox_status = "simulated preview";
#else
    const char *as_status = s_as11_service_ready ? as11_ble_get_status() : "unavailable";
    const char *ox_status = s_ox_service_ready ? oximeter_get_status() : "unavailable";
#endif

    /* Pairing can finish after the user leaves Manage, so completion polling
     * remains a background responsibility. Everything below this point only
     * paints Manage widgets and should not consume render time on Home or
     * History. */
    if (ble_started && esp_timer_get_time() - ble_started > 1000000) {
        bool as_done = ble_operation == BLE_UI_PAIR_AS11 &&
                       (!strcmp(as_status, AS11_STATUS_PAIRED) ||
                        !strcmp(as_status, AS11_STATUS_ERROR));
        bool ox_done = ble_operation == BLE_UI_PAIR_OX &&
                       (!strcmp(ox_status, OX_STATUS_PAIRED) ||
                        !strcmp(ox_status, OX_STATUS_MONITORING) ||
                        !strcmp(ox_status, OX_STATUS_ERROR));
        if (as_done && !strcmp(as_status, AS11_STATUS_ERROR)) {
            portENTER_CRITICAL(&s_state_lock);
            s_as11_pairing_mode_confirmed = false;
            portEXIT_CRITICAL(&s_state_lock);
            pairing_mode_confirmed = false;
        }
        if (as_done || ox_done) end_ble_operation();
    }

    reap_retired_logs_destination();
    if (active_tab != 2) return;
    refresh_manage_rail(state);
    int section = s_rendered_manage_section;
    if (section < 0 || section >= MANAGE_SECTION_COUNT ||
        section != s_active_manage_section || !s_manage_sections[section])
        return;
    if (section == MANAGE_STORAGE || section == MANAGE_SYSTEM ||
        section == MANAGE_ADVANCED) {
        touch_maintenance_ui_refresh(s_touch_services_ready);
        return;
    }
    lv_obj_t *active_scroll = s_manage_scrolls[section];
    if (active_scroll && lv_obj_is_scrolling(active_scroll)) {
        /* Static state catches up on the next 500 ms pass. Deferring it while
         * the finger is moving prevents unrelated labels and hidden sections
         * from competing with LVGL's scroll redraw. */
        return;
    }

    if (section == MANAGE_LOGS) {
        touch_logs_controller_refresh(state->sd_ready);
        return;
    }
    if (section == MANAGE_CONNECTIVITY || section == MANAGE_ALERTS || section == MANAGE_UPLOADS) {
        touch_manage_config_refresh();
        return;
    }
    if (section == MANAGE_DEVICES) {
        refresh_device_dropdown(false, services);
        refresh_device_dropdown(true, services);
    }

    if (!s_touch_services_ready) {
        if (section == MANAGE_DEVICES) {
            lv_label_set_text(s_as11_status, "Starting Bluetooth service...");
            lv_label_set_text(s_ox_status, "Starting Bluetooth service...");
            lv_label_set_text(s_device_section_subtitle,
                              "Pairing services are starting");
            for (int i = 0; i < 6; ++i)
                lv_obj_add_state(s_ble_buttons[i], LV_STATE_DISABLED);
            lv_obj_add_state(s_as11_dropdown, LV_STATE_DISABLED);
            lv_obj_add_state(s_ox_dropdown, LV_STATE_DISABLED);
            lv_obj_add_state(s_passkey, LV_STATE_DISABLED);
            lv_obj_add_state(s_passkey_confirm_button, LV_STATE_DISABLED);
        }
        return;
    }

    bool as_paired = state->paired;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    as_paired = s_as11_service_ready && as11_ble_is_paired();
#endif
    bool ox_paired = !strcmp(ox_status, OX_STATUS_PAIRED) ||
                     !strcmp(ox_status, OX_STATUS_MONITORING) ||
                     !strcmp(ox_status, "simulated preview");
    if (section == MANAGE_DEVICES) {
        lv_label_set_text(s_as11_status,
                      as_paired && !strcmp(as_status, AS11_STATUS_IDLE)
                          ? "Paired and ready"
                          : friendly_as11_status(as_status, services->as11_busy));
    lv_label_set_text(s_ox_status,
                      friendly_ox_status(ox_status, services->ox_busy));
    lv_label_set_text(s_device_section_subtitle,
                      as_paired && ox_paired ? "Both devices paired"
                      : as_paired ? "AirSense paired · oxygen sensor optional"
                      : !pairing_mode_confirmed ||
                        !strcmp(as_status, AS11_STATUS_ERROR)
                          ? "First on AirSense: More › MyAir App › OK, downloaded › Connect"
                          : "AirSense pairing mode confirmed · keep that screen open");
    bool waiting_passkey = !strcmp(as_status, AS11_STATUS_WAIT_PASSKEY);
    if (waiting_passkey) {
        lv_obj_set_style_border_color(s_passkey, lv_color_hex(0x43d7e8), 0);
    } else {
        lv_obj_set_style_border_color(s_passkey, lv_color_hex(0x454b58), 0);
    }
    if (waiting_passkey && !state->therapy) {
        lv_obj_clear_state(s_passkey, LV_STATE_DISABLED);
        lv_obj_clear_state(s_passkey_confirm_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_passkey, LV_STATE_DISABLED);
        lv_obj_add_state(s_passkey_confirm_button, LV_STATE_DISABLED);
    }
    bool ble_controls_blocked = state->therapy || ble_operation != BLE_UI_IDLE;
    for (int i = 0; i < 6; ++i) {
        if (ble_controls_blocked) lv_obj_add_state(s_ble_buttons[i], LV_STATE_DISABLED);
        else lv_obj_clear_state(s_ble_buttons[i], LV_STATE_DISABLED);
    }
    lv_label_set_text(lv_obj_get_child(s_ble_buttons[0], 0),
                      pairing_mode_confirmed ? "Scan again" : "AirSense is ready");
    if (!pairing_mode_confirmed)
        lv_obj_add_state(s_as11_pair_button, LV_STATE_DISABLED);
    if (ble_controls_blocked) {
        lv_obj_add_state(s_as11_dropdown, LV_STATE_DISABLED);
        lv_obj_add_state(s_ox_dropdown, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_as11_dropdown, LV_STATE_DISABLED);
        lv_obj_clear_state(s_ox_dropdown, LV_STATE_DISABLED);
    }
    int phase = as_paired && !strcmp(as_status, AS11_STATUS_IDLE)
                    ? 4 : pairing_step(as_status, services->as11_busy);
    bool pairing_error = !strcmp(as_status, AS11_STATUS_ERROR);
    bool as_pairing = !as_paired && phase >= 0;
    for (int i = 0; i < 5; ++i) {
        set_hidden(s_pair_steps[i], !as_pairing);
        set_hidden(s_pair_step_labels[i], !as_pairing);
        uint32_t color = pairing_error && i == (phase < 0 ? 0 : phase)
                             ? COLOR_FAULT
                             : phase >= i ? COLOR_LIVE : COLOR_CONTROL;
        lv_obj_set_style_bg_color(s_pair_steps[i], lv_color_hex(color), 0);
        lv_obj_set_style_text_color(s_pair_step_labels[i],
                                    lv_color_hex(phase >= i ? COLOR_SECONDARY
                                                            : COLOR_DISABLED), 0);
    }

    /* Render Devices as actual state-specific rows. In the settled paired
     * state, scan fields and the passkey disappear instead of contradicting
     * the status with a disabled "No devices found" form. */
    bool show_as_results = !as_paired && !as_pairing && services->as11_count > 0;
    bool show_as_idle = !as_paired && !as_pairing && !show_as_results;
    int as_height = as_paired ? 92 : waiting_passkey ? 162
                              : as_pairing ? 110
                              : show_as_results ? 114 : 92;
    lv_obj_set_height(s_as11_row, as_height);
    set_hidden(s_as11_dot, !as_paired);
    lv_obj_set_pos(s_as11_title, as_paired ? 28 : 0,
                   as_paired ? 4 : 0);
    lv_obj_set_pos(s_as11_status,
                   as_paired ? 28 : (as_pairing || show_as_results) ? 190 : 0,
                   as_paired ? 34 : (as_pairing || show_as_results) ? 2 : 29);
    lv_obj_set_width(s_as11_status,
                     (as_pairing || show_as_results) ? 502 : 522);
    set_hidden(s_as11_badge, !as_paired);
    if (as_paired) lv_obj_set_pos(s_as11_badge, 150, 0);
    set_hidden(s_as11_dropdown, !show_as_results);
    set_hidden(s_ble_buttons[0], !(show_as_idle || show_as_results));
    set_hidden(s_as11_pair_button, !show_as_results);
    set_hidden(s_ble_buttons[2], !as_paired);
    set_hidden(s_passkey, !waiting_passkey);
    set_hidden(s_passkey_confirm_button, !waiting_passkey);
    if (show_as_idle || as_paired) {
        lv_obj_set_pos(s_ble_buttons[show_as_idle ? 0 : 2], 548, -3);
        lv_obj_set_size(s_ble_buttons[show_as_idle ? 0 : 2], 146, 56);
    } else if (show_as_results) {
        lv_obj_set_pos(s_as11_dropdown, 0, 40);
        lv_obj_set_pos(s_ble_buttons[0], 336, 40);
        lv_obj_set_size(s_ble_buttons[0], 94, 56);
        lv_obj_set_pos(s_as11_pair_button, 442, 40);
    } else if (waiting_passkey) {
        lv_obj_set_pos(s_passkey, 0, 82);
        lv_obj_set_pos(s_passkey_confirm_button, 336, 82);
    }

    bool ox_pairing = services->ox_busy || ble_operation == BLE_UI_PAIR_OX ||
                      ble_operation == BLE_UI_SCAN_OX;
    bool show_ox_results = !ox_paired && !ox_pairing && services->ox_count > 0;
    bool show_ox_idle = !ox_paired && !ox_pairing && !show_ox_results;
    int ox_height = ox_paired ? 92 : show_ox_results ? 114 : 92;
    int ox_y = as_height + 8;
    lv_obj_set_pos(s_ox_row, 0, ox_y);
    lv_obj_set_height(s_ox_row, ox_height);
    set_hidden(s_ox_dot, !ox_paired);
    lv_obj_set_pos(s_ox_title, ox_paired ? 28 : 0,
                   ox_paired ? 4 : 0);
    lv_obj_set_pos(s_ox_status,
                   ox_paired ? 28 : show_ox_results || ox_pairing ? 170 : 0,
                   ox_paired ? 34 : show_ox_results || ox_pairing ? 2 : 29);
    lv_obj_set_width(s_ox_status, 522);
    set_hidden(s_ox_badge, !ox_paired);
    if (ox_paired) lv_obj_set_pos(s_ox_badge, 112, 0);
    set_hidden(s_ox_dropdown, !show_ox_results);
    set_hidden(s_ble_buttons[3], !(show_ox_idle || show_ox_results));
    set_hidden(s_ble_buttons[4], !show_ox_results);
    set_hidden(s_ble_buttons[5], !ox_paired);
    if (show_ox_idle || ox_paired) {
        lv_obj_set_pos(s_ble_buttons[show_ox_idle ? 3 : 5], 548, -3);
        lv_obj_set_size(s_ble_buttons[show_ox_idle ? 3 : 5], 146, 56);
    } else if (show_ox_results) {
        lv_obj_set_pos(s_ox_dropdown, 0, 40);
        lv_obj_set_pos(s_ble_buttons[3], 336, 40);
        lv_obj_set_size(s_ble_buttons[3], 94, 56);
        lv_obj_set_pos(s_ble_buttons[4], 442, 40);
    }

    bool show_device_change = as_paired || ox_paired;
    set_hidden(s_device_change_row, !show_device_change);
    if (show_device_change) {
        lv_obj_set_y(s_device_change_row, ox_y + ox_height + 8);
        lv_label_set_text(s_device_change_title, "Pairing while therapy runs");
        lv_label_set_text(s_device_change_detail,
                          "Device changes are blocked during therapy. Stop therapy first to pair or forget a device.");
    }
    }

    bool network_busy = reboot_busy || wifi_save_busy;
    if (section == MANAGE_DEVICES) {
        bool network_controls_blocked = state->therapy || network_busy;
        if (!network_controls_blocked && services->as11_count == 0)
            lv_obj_add_state(s_as11_pair_button, LV_STATE_DISABLED);
        if (!network_controls_blocked && services->ox_count == 0)
            lv_obj_add_state(s_ble_buttons[4], LV_STATE_DISABLED);
        if (!as_paired)
            lv_obj_add_state(s_ble_buttons[2], LV_STATE_DISABLED);
        if (!ox_paired)
            lv_obj_add_state(s_ble_buttons[5], LV_STATE_DISABLED);
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
    bool tray_action_visible[] = {
        !state.paired || airsense_stale,
        storage_degraded || storage_fault,
        !state.wifi,
        false,
        !ring_paired,
    };
    for (int i = 0; i < 5; ++i) {
        if (s_status_tray_actions[i])
            set_hidden(s_status_tray_actions[i], !tray_action_visible[i]);
    }

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
        : !state.paired ? "Pair your AirSense in Manage › Devices"
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
        : !state.paired ? "Pair a device"
                        : (state.therapy ? "Stop therapy" : "Start therapy"));
    bool therapy_button_disabled = therapy_command_busy;
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
    s_last_display_service_us = 0;
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
        .route_card = history_route_card,
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
    touch_maintenance_service_readiness(as11_ready, oximeter_ready);
    portENTER_CRITICAL(&s_state_lock);
    s_touch_services_ready = true;
    s_as11_service_ready = as11_ready;
    s_ox_service_ready = oximeter_ready;
    portEXIT_CRITICAL(&s_state_lock);
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

bool bsp_display_get_wake_snapshot(bsp_display_wake_snapshot_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_state_lock);
    *out = (bsp_display_wake_snapshot_t) {
        .requested_on = s_backlight_requested,
        .last_applied_on = s_backlight,
        .applied_known = s_backlight_known,
        .gesture_blocked = s_wake_gesture_pending,
        .write_errors = s_backlight_write_errors,
        .last_service_us = s_last_display_service_us,
    };
    portEXIT_CRITICAL(&s_state_lock);
    return out->last_service_us != 0;
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
    s_last_display_service_us = now_us;
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
    s_services.as11_count = 1;
    strlcpy(s_services.as11[0].addr, "AA:11:00:00:00:01",
            sizeof(s_services.as11[0].addr));
    strlcpy(s_services.as11[0].name, "AirSense 11 (simulated)",
            sizeof(s_services.as11[0].name));
    s_services.as11[0].rssi = -47;
    s_services.as11_version++;
    s_services.ox_count = 1;
    strlcpy(s_services.ox[0].addr, "02:00:00:00:00:02",
            sizeof(s_services.ox[0].addr));
    strlcpy(s_services.ox[0].name, "O2 Ring (simulated)",
            sizeof(s_services.ox[0].name));
    s_services.ox[0].rssi = -55;
    s_services.ox[0].driver = OX_DRIVER_AUTO;
    s_services.ox_version++;
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
