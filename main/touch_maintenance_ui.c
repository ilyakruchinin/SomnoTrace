#include "touch_maintenance_ui.h"
#include "bsp_display.h"
#include "device_settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "maintenance_model.h"
#include "maintenance_ota.h"
#include "maintenance_release.h"
#include "sd_storage.h"
#include "somnotrace_fonts.h"
#include "touch_maintenance.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define BG 0x181c29
#define CARD 0x101421
#define CONTROL 0x2d333f
#define INK 0xf0f2f6
#define DIM 0xa0a5af
#define CYAN 0x00e1e2
#define RED 0xf45249
#define AMBER 0xf8bd40
#define BODY (&somnotrace_space_grotesk_medium_15)
#define TITLE (&somnotrace_space_grotesk_semibold_19)
#define MONO (&somnotrace_ibm_plex_mono_medium_13)
#define ROW_TITLE (&somnotrace_space_grotesk_semibold_15)
#define SMALL (&somnotrace_space_grotesk_medium_13)

/* Timer refreshes must not invalidate unchanged glyphs on the RGB panel. */
static void label_set_if_changed(lv_obj_t *object, const char *text)
{
    if (object && strcmp(lv_label_get_text(object), text) != 0)
        lv_label_set_text(object, text);
}
static void label_set_fmt_if_changed(lv_obj_t *object, const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    label_set_if_changed(object, text);
}

typedef enum {
    VIEW_STORAGE,
    VIEW_FILES,
    VIEW_FILE,
    VIEW_SYSTEM,
    VIEW_FIRMWARE,
    VIEW_IMAGES,
    VIEW_URL,
    VIEW_OTA,
    VIEW_ADVANCED,
    VIEW_HOLD,
    VIEW_DISPLAY,
    VIEW_ERRORS,
    VIEW_NOTES
} view_t;
enum {
    BUTTON_BACK = 1,
    BUTTON_SCAN,
    BUTTON_OLDER,
    BUTTON_FIRST,
    BUTTON_ROW = 10,
    BUTTON_FIRMWARE = 30,
    BUTTON_DISPLAY,
    BUTTON_EXPORT,
    BUTTON_ERRORS,
    BUTTON_CHECK,
    BUTTON_UPDATE,
    BUTTON_SD,
    BUTTON_URL,
    BUTTON_INSTALL_URL,
    BUTTON_CANCEL_OTA,
    BUTTON_NOTES,
    BUTTON_UPLOAD_RESET,
    BUTTON_RECREATE,
    BUTTON_RESTART,
    BUTTON_DELETE,
    BUTTON_RESET,
    BUTTON_FORMAT,
    BUTTON_CONFIRM_CANCEL,
    BUTTON_NAV_DEVICES,
    BUTTON_NAV_WIFI,
    BUTTON_NAV_UPLOADS,
    BUTTON_NAV_LOGS,
    BUTTON_SCREEN_OFF
};
typedef struct {
    lv_obj_t *root, *body, *status, *title, *diag, *services[6], *hold_label, *hold_button,
        *hold_bar;
    lv_obj_t *ota_label, *ota_bar, *cancel_ota, *url, *keyboard, *brightness_value;
    lv_obj_t *brightness_slider, *timeout_dropdown, *mode_dropdown;
    lv_timer_t *timer;
    maintenance_ui_hooks_t hooks;
    maintenance_snapshot_t snapshot;
    maintenance_diagnostics_t diagnostics;
    maintenance_ota_snapshot_t ota;
    maintenance_hold_t hold;
    maintenance_action_t confirm_action;
    maintenance_ui_destination_t destination;
    maintenance_entry_t selected;
    view_t view;
    bool rebuild, ready;
    bool read_pending;
    maintenance_action_t read_action;
    uint32_t view_generation, read_view_generation, read_job_id;
    uint32_t rendered_view_generation, rendered_read_job_id;
    char read_argument[MAINTENANCE_NAME_MAX];
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    bool frame_marker_pending;
    int frame_scroll, frame_ota_stage;
#endif
    uint32_t shown_revision, last_diagnostics;
    char notice[192], day[9], url_value[512];
} ui_t;
static ui_t *s;
static void render(void);
static void event(lv_event_t *e);
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    label_set_if_changed(o, text);
    return o;
}
static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int h, int action)
{
    lv_obj_t *o = lv_btn_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(CONTROL), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(CONTROL), LV_STATE_DISABLED);
    lv_obj_set_style_opa(o, LV_OPA_70, LV_STATE_DISABLED);
    lv_obj_set_style_color_filter_opa(o, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 8, 0);
    lv_obj_t *l = label(o, text, 0, 0, w - 16, BODY, INK);
    lv_obj_center(l);
    lv_obj_add_event_cb(o, event, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    return o;
}
static void disabled(lv_obj_t *o, bool value)
{
    if (value)
        lv_obj_add_state(o, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(o, LV_STATE_DISABLED);
}
static bool blocked(void)
{
    return !s->ready || s->snapshot.busy || bsp_display_is_therapy_active() ||
           sd_storage_recording_active();
}
static void show(view_t view)
{
    touch_maintenance_cancel_reads();
    maintenance_hold_reset(&s->hold);
    ++s->view_generation;
    s->read_pending = false;
    s->read_job_id = 0;
    s->read_action = MAINT_NONE;
    s->snapshot.count = 0;
    s->snapshot.has_more = false;
    s->view = view;
    s->rebuild = true;
    s->notice[0] = 0;
}
static bool is_read_action(maintenance_action_t action)
{
    return action == MAINT_SCAN || action == MAINT_FILES || action == MAINT_IMAGES;
}
static bool read_result_current(void)
{
    return !s->read_pending && s->read_job_id &&
           s->read_view_generation == s->view_generation &&
           s->snapshot.job_id == s->read_job_id && s->snapshot.action == s->read_action;
}
static bool read_rows_current(void)
{
    return read_result_current() && s->rendered_view_generation == s->view_generation &&
           s->rendered_read_job_id == s->read_job_id;
}
static void read_rows_rendered(void)
{
    s->rendered_view_generation = s->view_generation;
    s->rendered_read_job_id = read_result_current() ? s->read_job_id : 0;
}
static void refresh_read_snapshot(void)
{
    touch_maintenance_snapshot(&s->snapshot);
    if (s->read_pending && s->read_view_generation != s->view_generation)
        s->read_pending = false;
    if (s->read_pending && !s->snapshot.busy) {
        esp_err_t result = touch_maintenance_request_tracked(
            s->read_action, s->read_argument, &s->read_job_id);
        touch_maintenance_snapshot(&s->snapshot);
        /* A competing owner can win admission after our snapshot. Retain one
         * view-scoped intent; retry only after that owner has retired. */
        if (result == ESP_OK) {
            s->read_pending = false;
            strlcpy(s->notice, "Working...", sizeof(s->notice));
        } else if (result != ESP_ERR_INVALID_STATE || !s->snapshot.busy) {
            s->read_pending = false;
            snprintf(s->notice, sizeof(s->notice), "Request not started: %s",
                     esp_err_to_name(result));
        }
        s->rebuild = true;
    }
    if (!read_result_current()) {
        s->snapshot.count = 0;
        s->snapshot.has_more = false;
        if (is_read_action(s->snapshot.action)) {
            s->snapshot.census_valid = false;
            s->snapshot.message[0] = 0;
        }
    }
    s->snapshot.busy |= s->read_pending;
}
static void request(maintenance_action_t action, const char *argument)
{
    if (is_read_action(action)) {
        if (argument && strlen(argument) >= sizeof(s->read_argument)) {
            strlcpy(s->notice, "Read request is too long", sizeof(s->notice));
            return;
        }
        touch_maintenance_cancel_reads();
        s->read_action = action;
        s->read_view_generation = s->view_generation;
        s->read_job_id = 0;
        s->read_pending = true;
        strlcpy(s->read_argument, argument ? argument : "", sizeof(s->read_argument));
        strlcpy(s->notice, "Waiting for the current card operation...", sizeof(s->notice));
        refresh_read_snapshot();
        s->rebuild = true;
        return;
    }
    esp_err_t result = touch_maintenance_request(action, argument);
    if (result != ESP_OK)
        snprintf(s->notice, sizeof(s->notice), "Request not started: %s", esp_err_to_name(result));
    else
        strlcpy(s->notice, "Working...", sizeof(s->notice));
}
static lv_obj_t *row(int y, const char *title, const char *subtitle, int action, bool off)
{
    lv_obj_t *o = button(s->body, "", 0, y, 720, 46, action);
    lv_obj_set_style_bg_color(o, lv_color_hex(CARD), 0);
    lv_obj_set_style_radius(o, 12, 0);
    label(o, title, 5, -5, 680, ROW_TITLE, off ? DIM : INK);
    lv_obj_t *sublabel = label(o, subtitle, 5, 16, 680, SMALL, DIM);
    disabled(o, off);
    return sublabel;
}
static void hold_event(lv_event_t *e)
{
    if (!s || s->view != VIEW_HOLD)
        return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && !blocked())
        maintenance_hold_press(&s->hold, lv_tick_get());
    else if (code == LV_EVENT_PRESSING &&
             maintenance_hold_ready(&s->hold, lv_tick_get(), !blocked()))
        request(s->confirm_action, NULL);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE)
        maintenance_hold_reset(&s->hold);
}
static void display_event(lv_event_t *e)
{
    if (!s)
        return;
    intptr_t kind = (intptr_t)lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (kind == 1) {
        if (code == LV_EVENT_VALUE_CHANGED) {
            int value = lv_slider_get_value(o);
            device_settings_set_brightness(value);
            label_set_fmt_if_changed(s->brightness_value, "%d%%", (value + 1) / 2);
        } else if (code == LV_EVENT_RELEASED)
            request(MAINT_SAVE_DISPLAY, NULL);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (kind == 2) {
            static const uint16_t values[] = {0, 60, 120, 300, 900, 1800};
            device_settings_set_screen_timeout_s(values[lv_dropdown_get_selected(o)]);
        } else {
            static const backlight_mode_t values[] = {
                BACKLIGHT_MODE_ON,
                BACKLIGHT_MODE_OFF_THRP,
                BACKLIGHT_MODE_ALWAYS_OFF,
            };
            device_settings_set_backlight_mode(values[lv_dropdown_get_selected(o)]);
        }
        request(MAINT_SAVE_DISPLAY, NULL);
    }
}
static void event(lv_event_t *e)
{
    if (!s)
        return;
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action >= BUTTON_ROW && action < BUTTON_ROW + MAINTENANCE_PAGE_SIZE) {
        int i = action - BUTTON_ROW;
        if (!read_rows_current() || i >= s->snapshot.count || s->snapshot.busy)
            return;
        s->selected = s->snapshot.entries[i];
        if (s->view == VIEW_STORAGE) {
            strlcpy(s->day, s->selected.name, sizeof(s->day));
            show(VIEW_FILES);
            request(MAINT_FILES, s->day);
        } else if (s->view == VIEW_IMAGES) {
            esp_err_t result = maintenance_ota_start_sd(s->selected.name);
            if (result == ESP_OK)
                show(VIEW_OTA);
            else
                snprintf(s->notice, sizeof(s->notice), "Install not started: %s",
                         esp_err_to_name(result));
        } else
            show(VIEW_FILE);
        return;
    }
    switch (action) {
    case BUTTON_SCREEN_OFF:
        if (s->hooks.screen_off)
            s->hooks.screen_off();
        break;
    case BUTTON_BACK:
        if (s->view == VIEW_FILES || s->view == VIEW_FILE) {
            show(VIEW_STORAGE);
            request(MAINT_SCAN, NULL);
        } else if (s->view == VIEW_HOLD)
            show(VIEW_ADVANCED);
        else if (s->view == VIEW_IMAGES || s->view == VIEW_URL || s->view == VIEW_NOTES ||
                 s->view == VIEW_OTA)
            show(VIEW_FIRMWARE);
        else
            show(VIEW_SYSTEM);
        break;
    case BUTTON_SCAN:
    case BUTTON_FIRST:
        request(s->view == VIEW_FILES    ? MAINT_FILES
                : s->view == VIEW_IMAGES ? MAINT_IMAGES
                                         : MAINT_SCAN,
                s->view == VIEW_FILES ? s->day : NULL);
        break;
    case BUTTON_OLDER:
        if (read_rows_current() && s->snapshot.count) {
            char argument[MAINTENANCE_NAME_MAX];
            const char *last = s->snapshot.entries[s->snapshot.count - 1].name;
            if (s->view == VIEW_FILES)
                snprintf(argument, sizeof(argument), "%s/%s", s->day, last);
            else
                strlcpy(argument, last, sizeof(argument));
            request(s->view == VIEW_FILES    ? MAINT_FILES
                    : s->view == VIEW_IMAGES ? MAINT_IMAGES
                                             : MAINT_SCAN,
                    argument);
        }
        break;
    case BUTTON_FIRMWARE:
        show(VIEW_FIRMWARE);
        break;
    case BUTTON_DISPLAY:
        show(VIEW_DISPLAY);
        break;
    case BUTTON_EXPORT:
        request(MAINT_EXPORT_REPORT, NULL);
        break;
    case BUTTON_ERRORS:
        show(VIEW_ERRORS);
        break;
    case BUTTON_CHECK:
        request(MAINT_CHECK_FIRMWARE, NULL);
        break;
    case BUTTON_UPDATE: {
        esp_err_t result = maintenance_ota_start_url(s->snapshot.release_url);
        if (result == ESP_OK)
            show(VIEW_OTA);
        else
            snprintf(s->notice, sizeof(s->notice), "Install not started: %s",
                     esp_err_to_name(result));
        break;
    }
    case BUTTON_SD:
        show(VIEW_IMAGES);
        request(MAINT_IMAGES, NULL);
        break;
    case BUTTON_URL:
        show(VIEW_URL);
        break;
    case BUTTON_INSTALL_URL: {
        strlcpy(s->url_value, lv_textarea_get_text(s->url), sizeof(s->url_value));
        esp_err_t result = maintenance_ota_start_url(s->url_value);
        if (result == ESP_OK)
            show(VIEW_OTA);
        else
            snprintf(s->notice, sizeof(s->notice), "Install not started: %s",
                     esp_err_to_name(result));
        break;
    }
    case BUTTON_CANCEL_OTA:
        if (!maintenance_ota_cancel())
            strlcpy(s->notice, "Cancel unavailable: boot selection has begun.", sizeof(s->notice));
        break;
    case BUTTON_NOTES:
        show(VIEW_NOTES);
        break;
    case BUTTON_UPLOAD_RESET:
        request(MAINT_RESET_UPLOAD, NULL);
        break;
    case BUTTON_RECREATE:
        request(MAINT_RECREATE_EDF, NULL);
        break;
    case BUTTON_RESTART:
        if (!blocked() && s->hooks.restart)
            s->hooks.restart();
        break;
    case BUTTON_DELETE:
    case BUTTON_RESET:
    case BUTTON_FORMAT:
        s->confirm_action = action == BUTTON_DELETE  ? MAINT_DELETE_EDF
                            : action == BUTTON_RESET ? MAINT_FACTORY_RESET
                                                     : MAINT_FORMAT;
        show(VIEW_HOLD);
        request(MAINT_SCAN, NULL);
        break;
    case BUTTON_CONFIRM_CANCEL:
        show(VIEW_ADVANCED);
        break;
    case BUTTON_NAV_DEVICES:
        if (s->hooks.navigate)
            s->hooks.navigate(MAINT_NAV_DEVICES);
        break;
    case BUTTON_NAV_WIFI:
        if (s->hooks.navigate)
            s->hooks.navigate(MAINT_NAV_CONNECTIVITY);
        break;
    case BUTTON_NAV_UPLOADS:
        if (s->hooks.navigate)
            s->hooks.navigate(MAINT_NAV_UPLOADS);
        break;
    case BUTTON_NAV_LOGS:
        if (s->hooks.navigate)
            s->hooks.navigate(MAINT_NAV_LOGS);
        break;
    }
}
static void dropdown_ready(lv_event_t *event)
{
    lv_obj_t *list = lv_dropdown_get_list(lv_event_get_target(event));
    if (!list)
        return;
    lv_obj_set_style_text_font(list, BODY, 0);
    lv_obj_set_style_text_line_space(list, 28, 0);
    lv_obj_set_style_max_height(list, 250, 0);
    lv_obj_set_style_pad_top(list, 12, 0);
    lv_obj_set_style_pad_bottom(list, 12, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(CARD), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(INK), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(CYAN), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(BG), LV_PART_SELECTED);
}
static void dropdown_style(lv_obj_t *control)
{
    /* LVGL treats ASCII symbols as image filenames; use its icon font for the arrow. */
    lv_dropdown_set_symbol(control, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(control, LV_FONT_DEFAULT, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(control, lv_color_hex(INK), LV_PART_INDICATOR);
    lv_obj_set_style_text_font(control, BODY, 0);
    lv_obj_set_style_text_color(control, lv_color_hex(INK), 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(CONTROL), 0);
    lv_obj_set_style_border_width(control, 0, 0);
    lv_obj_set_style_radius(control, 12, 0);
}
static void display(void)
{
    device_settings_t cfg;
    device_settings_snapshot(&cfg);
    disabled(button(s->root, "Off now", 478, 5, 150, 44, BUTTON_SCREEN_OFF),
             !s->hooks.wake_available || !s->hooks.wake_available());
    label(s->body, "Brightness", 0, 0, 250, TITLE, INK);
    s->brightness_value = label(s->body, "", 600, 0, 100, MONO, INK);
    label_set_fmt_if_changed(s->brightness_value, "%d%%", (cfg.brightness + 1) / 2);
    lv_obj_t *slider = lv_slider_create(s->body);
    s->brightness_slider = slider;
    lv_obj_set_pos(slider, 20, 45);
    lv_obj_set_size(slider, 670, 16);
    lv_slider_set_range(slider, 1, 200);
    lv_slider_set_value(slider, cfg.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(CYAN), LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 20);
    lv_obj_set_style_pad_all(slider, 12, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, display_event, LV_EVENT_ALL, (void *)1);
    label(s->body, "Screen timeout", 0, 88, 290, TITLE, INK);
    lv_obj_t *timeout = lv_dropdown_create(s->body);
    s->timeout_dropdown = timeout;
    lv_obj_set_pos(timeout, 365, 75);
    lv_obj_set_size(timeout, 340, 48);
    lv_dropdown_set_options(timeout,
                            "Never\n1 minute\n2 minutes\n5 minutes\n15 minutes\n30 minutes");
    static const uint16_t options[] = {0, 60, 120, 300, 900, 1800};
    unsigned selected = 0;
    for (unsigned i = 0; i < 6; i++)
        if (options[i] == cfg.screen_timeout_s)
            selected = i;
    lv_dropdown_set_selected(timeout, selected);
    dropdown_style(timeout);
    lv_obj_add_event_cb(timeout, display_event, LV_EVENT_VALUE_CHANGED, (void *)2);
    lv_obj_add_event_cb(timeout, dropdown_ready, LV_EVENT_READY, NULL);
    disabled(timeout, !s->hooks.wake_available || !s->hooks.wake_available());
    label(s->body, "Overnight behaviour", 0, 150, 330, TITLE, INK);
    lv_obj_t *mode = lv_dropdown_create(s->body);
    s->mode_dropdown = mode;
    lv_obj_set_pos(mode, 365, 137);
    lv_obj_set_size(mode, 340, 48);
    lv_dropdown_set_options(mode, "Live therapy detail\nOff during therapy\nOff except alerts");
    lv_dropdown_set_selected(mode, cfg.backlight_mode == BACKLIGHT_MODE_OFF_THRP     ? 1
                                   : cfg.backlight_mode == BACKLIGHT_MODE_ALWAYS_OFF ? 2
                                                                                      : 0);
    dropdown_style(mode);
    lv_obj_add_event_cb(mode, display_event, LV_EVENT_VALUE_CHANGED, (void *)3);
    lv_obj_add_event_cb(mode, dropdown_ready, LV_EVENT_READY, NULL);
    disabled(mode, !s->hooks.wake_available || !s->hooks.wake_available());
    label(s->body,
          "Touch wakes the screen; alerts can wake it. Fixed landscape.\n\nInformation-only Home, "
          "day/night theme and larger text are deferred.\nLive therapy detail is the available "
          "Home layout.",
          0, 211, 710, BODY, DIM);
}
static void render(void)
{
    s->rebuild = false;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    s->frame_marker_pending = true;
#endif
    maintenance_hold_reset(&s->hold);
    lv_obj_clean(s->root);
    s->diag = NULL;
    s->hold_label = NULL;
    s->ota_label = NULL;
    memset(s->services, 0, sizeof(s->services));
    const char *titles[] = {"Recorded files",      "Night files",     "File details",
                            "System status",       "Firmware",        "Install from microSD",
                            "Install from a URL",  "Firmware update", "Advanced",
                            "Confirm maintenance", "Display",         "Controller error history",
                            "Release notes"};
    s->title = label(s->root, titles[s->view], 16, 10, 420, TITLE, INK);
    bool top = s->view == VIEW_STORAGE || s->view == VIEW_SYSTEM || s->view == VIEW_ADVANCED;
    if (!top)
        button(s->root, "Back", 646, 5, 104, 44, BUTTON_BACK);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    label(s->root, "QEMU service simulation", 442, 52, 305, MONO, AMBER);
#endif
    s->body = lv_obj_create(s->root);
    lv_obj_set_pos(s->body, 16, 64);
    lv_obj_set_size(s->body, 736, 336);
    lv_obj_set_style_bg_opa(s->body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->body, 0, 0);
    lv_obj_set_style_pad_all(s->body, 0, 0);
    lv_obj_set_scroll_dir(s->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s->body, LV_SCROLLBAR_MODE_AUTO);
    s->status = label(s->root, "", 16, 405, 730, BODY, DIM);
    char text[1300];
    switch (s->view) {
    case VIEW_STORAGE:
    case VIEW_FILES:
    case VIEW_IMAGES:
        if (s->view == VIEW_STORAGE) {
            button(s->root, "Refresh", 642, 5, 110, 44, BUTTON_SCAN);
            if (s->snapshot.capacity_valid && s->snapshot.census_valid)
                snprintf(text, sizeof(text),
                         "%.2f / %.2f GiB free  |  %llu AirSense nights  |  %llu data files\n"
                         "Exact capacity: %llu / %llu bytes free",
                         s->snapshot.free_bytes / 1073741824.0,
                         s->snapshot.total_bytes / 1073741824.0,
                         (unsigned long long)s->snapshot.nights,
                         (unsigned long long)s->snapshot.files,
                         (unsigned long long)s->snapshot.free_bytes,
                         (unsigned long long)s->snapshot.total_bytes);
            else
                strlcpy(text,
                        "Capacity and file counts unavailable; refresh when the card is idle.",
                        sizeof(text));
            label(s->body, text, 0, 0, 715, MONO, INK);
            if (s->snapshot.census_valid && s->snapshot.estimate_samples)
                snprintf(text, sizeof(text),
                         "About %llu AirSense-only nights: largest of %lu completed raw+EDF "
                         "nights.\nExcludes shared metadata; actual use varies. O2 needs "
                         "additional space.",
                         (unsigned long long)maintenance_estimated_nights(
                             s->snapshot.free_bytes, s->snapshot.largest_air_night,
                             s->snapshot.estimate_samples),
                         (unsigned long)s->snapshot.estimate_samples);
            else
                strlcpy(text,
                        "Nights estimate unavailable: no representative AirSense-only sample.\nO2 "
                        "storage and variable session lengths affect capacity.",
                        sizeof(text));
            label(s->body, text, 0, 42, 715, BODY, DIM);
        } else if (s->view == VIEW_FILES) {
            snprintf(text, sizeof(text), "Night %s: actual raw session and generated EDF files",
                     s->day);
            label(s->body, text, 0, 0, 715, BODY, DIM);
        } else
            label(s->body,
                  "Select somnotrace-*.bin from the card root. Board identity is verified.\nStop "
                  "therapy before installing; duration depends on image size and card speed.",
                  0, 0, 710, BODY, DIM);
        for (size_t i = 0; i < s->snapshot.count; i++) {
            char detail[100];
            snprintf(detail, sizeof(detail), "%llu bytes%s",
                     (unsigned long long)s->snapshot.entries[i].bytes,
                     s->view == VIEW_STORAGE ? " / raw + EDF" : "");
            row((s->view == VIEW_STORAGE ? 90 : 54) + i * 54, s->snapshot.entries[i].name, detail,
                BUTTON_ROW + i, s->snapshot.busy || (s->view == VIEW_IMAGES && blocked()));
        }
        if (!s->snapshot.count)
            label(s->body, s->snapshot.busy ? "Reading card..." : "No matching files in this page.",
                  0, 105, 700, TITLE, DIM);
        button(s->root, "First", 412, 5, 104, 44, BUTTON_FIRST);
        disabled(button(s->root, "Next", 528, 5, 104, 44, BUTTON_OLDER),
                 !s->snapshot.has_more || s->snapshot.busy);
        break;
    case VIEW_FILE:
        snprintf(text, sizeof(text), "%s\n\nExact size: %llu bytes\nNight: %s\n\n%s",
                 s->selected.name, (unsigned long long)s->selected.bytes, s->day,
                 !strncmp(s->selected.name, "EDF/", 4)
                     ? "Generated EDF export. Source session files are retained separately."
                     : "Source session file. Recreating EDF does not delete this data.");
        label(s->body, text, 0, 0, 700, MONO, INK);
        break;
    case VIEW_SYSTEM:
        button(s->root, "Export report", 588, 5, 164, 44, BUTTON_EXPORT);
        s->diag = label(s->body, "Collecting current measurements...", 0, 0, 714, MONO, INK);
        s->services[0] = row(174, "AirSense link", "", BUTTON_NAV_DEVICES, false);
        s->services[1] = row(232, "O2 Ring", "", BUTTON_NAV_DEVICES, false);
        s->services[2] = row(290, "Wi-Fi", "", BUTTON_NAV_WIFI, false);
        s->services[3] = row(348, "Upload scheduler", "", BUTTON_NAV_UPLOADS, false);
        s->services[4] = row(406, "Touch and display controllers", "Observed errors since boot",
                             BUTTON_ERRORS, false);
        row(464, "Firmware", "Check release notes, update over Wi-Fi or from card", BUTTON_FIRMWARE,
            false);
        row(522, "Display", "Brightness, timeout and overnight behaviour", BUTTON_DISPLAY, false);
        break;
    case VIEW_FIRMWARE:
        button(s->root, "Check now", 478, 5, 150, 44, BUTTON_CHECK);
        if (s->snapshot.checked_uptime_us) {
            if (s->snapshot.checked_epoch_s >= 1577836800) {
                time_t checked = s->snapshot.checked_epoch_s;
                struct tm tm;
                localtime_r(&checked, &tm);
                char timestamp[40];
                strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
                snprintf(text, sizeof(text), "Last check attempted %s%s", timestamp,
                         s->snapshot.release_valid ? "" : " - no release result");
            } else
                snprintf(text, sizeof(text),
                         "Last check attempted at uptime %lld s; wall clock unavailable",
                         (long long)s->snapshot.checked_uptime_us / 1000000);
        } else
            strlcpy(text, "Firmware has not been checked.", sizeof(text));
        label(s->body, text, 0, 0, 712, MONO, DIM);
        label(s->root, "Source: " MAINTENANCE_RELEASE_SOURCE, 16, 48, 410, MONO, DIM);
        snprintf(text, sizeof(text), "Installed %s\nRelease %s%s", s->diagnostics.version,
                 s->snapshot.release_valid ? s->snapshot.release : "unknown",
                 s->snapshot.release_valid && !s->snapshot.compatible_asset
                     ? " - no explicitly marked 7B asset"
                     : "");
        label(s->body, text, 0, 40, 712, TITLE, INK);
        row(104, "Release notes",
            s->snapshot.release_valid ? s->snapshot.published : "Check a release first",
            BUTTON_NOTES, !s->snapshot.release_valid);
        row(162, "Update over Wi-Fi",
            "Therapy must be stopped. Time estimate appears after byte progress.", BUTTON_UPDATE,
            blocked() || !s->snapshot.compatible_asset);
        row(220, "Install from microSD card", "Looks for somnotrace-*.bin in the card root",
            BUTTON_SD, blocked());
        row(278, "Install from a URL", "Advanced: HTTPS URL for a compatible 7B image", BUTTON_URL,
            blocked());
        break;
    case VIEW_NOTES:
        label(s->body, s->snapshot.notes, 0, 0, 710, BODY, INK);
        break;
    case VIEW_URL:
        label(s->body, "Advanced: HTTPS firmware URL. Image target must match this board.", 0, 0,
              700, BODY, DIM);
        s->url = lv_textarea_create(s->body);
        lv_obj_set_pos(s->url, 0, 32);
        lv_obj_set_size(s->url, 535, 54);
        lv_textarea_set_one_line(s->url, true);
        lv_textarea_set_max_length(s->url, 511);
        lv_textarea_set_text(s->url, s->url_value);
        lv_obj_set_style_text_font(s->url, BODY, 0);
        lv_obj_set_style_text_color(s->url, lv_color_hex(INK), 0);
        lv_obj_set_style_bg_color(s->url, lv_color_hex(CONTROL), 0);
        lv_obj_set_style_border_width(s->url, 0, 0);
        disabled(button(s->body, "Install", 550, 32, 158, 54, BUTTON_INSTALL_URL), blocked());
        s->keyboard = lv_keyboard_create(s->body);
        lv_obj_set_align(s->keyboard, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(s->keyboard, 0, 100);
        lv_obj_set_size(s->keyboard, 710, 236);
        lv_obj_set_style_pad_all(s->keyboard, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(s->keyboard, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_column(s->keyboard, 3, LV_PART_MAIN);
        lv_keyboard_set_textarea(s->keyboard, s->url);
        /* Default font supplies the keyboard shift/backspace/accept symbols. */
        lv_obj_set_style_text_font(s->keyboard, LV_FONT_DEFAULT, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(s->keyboard, lv_color_hex(BG), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s->keyboard, lv_color_hex(CONTROL), LV_PART_ITEMS);
        lv_obj_set_style_text_color(s->keyboard, lv_color_hex(INK), LV_PART_ITEMS);
        break;
    case VIEW_OTA:
        s->ota_label = label(s->body, "Reading updater state...", 0, 0, 710, TITLE, INK);
        s->ota_bar = lv_bar_create(s->body);
        lv_obj_set_pos(s->ota_bar, 0, 150);
        lv_obj_set_size(s->ota_bar, 710, 18);
        lv_bar_set_range(s->ota_bar, 0, 100);
        lv_obj_set_style_bg_color(s->ota_bar, lv_color_hex(CONTROL), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s->ota_bar, lv_color_hex(CYAN), LV_PART_INDICATOR);
        s->cancel_ota = button(s->body, "Cancel update", 0, 184, 250, 52, BUTTON_CANCEL_OTA);
        button(s->body, "Use microSD instead", 270, 184, 260, 52, BUTTON_SD);
        button(s->body, "Open logs", 550, 184, 160, 52, BUTTON_NAV_LOGS);
        label(s->body,
              "The download writes the inactive OTA slot. Cancel is available before\nboot "
              "selection begins. Keep power connected; a selected image restarts\nonly after "
              "therapy and storage are idle.",
              0, 250, 710, BODY, DIM);
        break;
    case VIEW_ADVANCED:
        label(s->body, "SAFE", 0, 0, 650, MONO, CYAN);
        row(22, "Reset upload state", "Forget upload receipts; recording files are retained",
            BUTTON_UPLOAD_RESET, blocked());
        row(70, "Recreate EDF files",
            "Transactional per-night rebuild; duration depends on retained data", BUTTON_RECREATE,
            blocked());
        row(118, "Restart", "Confirmation required; blocked during therapy", BUTTON_RESTART,
            blocked());
        label(s->body, "DESTRUCTIVE  /  Cannot be undone", 0, 174, 710, MONO, RED);
        row(194, "Delete generated EDF files  /  Hold",
            "Source sessions, O2 raw files, settings and pairing are retained", BUTTON_DELETE,
            blocked());
        row(242, "Reset all data  /  Hold", "Every recording, every setting, every paired device",
            BUTTON_RESET, blocked());
        row(290, "Format the microSD card  /  Hold",
            "Erase everything on the card; retain device settings and pairing", BUTTON_FORMAT,
            blocked());
        break;
    case VIEW_HOLD: {
        const char *name = s->confirm_action == MAINT_DELETE_EDF ? "Delete generated EDF files"
                           : s->confirm_action == MAINT_FORMAT   ? "Format the microSD card"
                                                                 : "Reset all data";
        label(s->body, name, 0, 0, 710, TITLE, RED);
        if (s->snapshot.census_valid)
            snprintf(text, sizeof(text),
                     "Latest scan: %llu AirSense nights, %llu data files, %llu generated "
                     "EDFs.\nO2 raw files: %llu. Per-night upload delivery count is unavailable.",
                     (unsigned long long)s->snapshot.nights, (unsigned long long)s->snapshot.files,
                     (unsigned long long)s->snapshot.generated_edfs,
                     (unsigned long long)s->snapshot.ox_files);
        else
            strlcpy(text,
                    "Recording counts unavailable. Do not assume any night has been uploaded.",
                    sizeof(text));
        label(s->body, text, 0, 36, 710, MONO, INK);
        label(
            s->body,
            s->confirm_action == MAINT_DELETE_EDF
                ? "Only generated STR/BRP/PLD/SA2/EVE/CSL EDF files are removed.\nSource sessions "
                  "and O2 raw files stay; EDF exports can be rebuilt."
            : s->confirm_action == MAINT_FORMAT
                ? "Everything on the card is lost, including recordings, card logs and "
                  "update\nimages. Device settings and paired devices stay. Recording is "
                  "unavailable\nwhile formatting; therapy starting before admission cancels the "
                  "operation."
                : "All managed recordings, card logs, upload receipts, device settings "
                  "and\nBluetooth pairing are removed. Wi-Fi, alerts, clock and display "
                  "settings\nreset. Unrelated card-root files stay. Restarts into first-run setup.",
            0, 106, 710, BODY, DIM);
        s->hold_button = button(s->body, "Hold continuously for 3 seconds", 0, 202, 510, 58, 0);
        lv_obj_add_event_cb(s->hold_button, hold_event, LV_EVENT_ALL, NULL);
        disabled(s->hold_button, blocked());
        button(s->body, "Cancel", 528, 202, 180, 58, BUTTON_CONFIRM_CANCEL);
        s->hold_bar = lv_bar_create(s->body);
        lv_obj_set_pos(s->hold_bar, 0, 272);
        lv_obj_set_size(s->hold_bar, 710, 10);
        lv_bar_set_range(s->hold_bar, 0, MAINTENANCE_HOLD_MS);
        lv_obj_set_style_bg_color(s->hold_bar, lv_color_hex(CONTROL), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s->hold_bar, lv_color_hex(RED), LV_PART_INDICATOR);
        s->hold_label = label(s->body, "Release, cancellation or navigation resets the hold.", 0,
                              292, 710, BODY, DIM);
        break;
    }
    case VIEW_DISPLAY:
        display();
        break;
    case VIEW_ERRORS:
        snprintf(text, sizeof(text),
                 "Controller observations since boot: %s\nTimes are monotonic uptime. Unobserved "
                 "operations are unknown.\nNo task stack-health measurements are supplied.",
                 s->diagnostics.controllers.simulated ? "SIMULATED" : "physical driver results");
        label(s->body, text, 0, 0, 710, MONO, INK);
        for (size_t i = 0; i < CONTROLLER_OPERATION_COUNT; i++) {
            controller_operation_status_t *op = &s->diagnostics.controllers.operations[i];
            snprintf(text, sizeof(text), "%s / %s / %lu errors since boot",
                     controller_diagnostics_operation_name(i),
                     op->observed ? esp_err_to_name(op->last_result) : "unobserved",
                     (unsigned long)op->error_count);
            label(s->body, text, 0, 80 + i * 28, 710, MONO, DIM);
        }
        for (size_t i = 0; i < s->diagnostics.controllers.history_count; i++) {
            controller_error_episode_t *ep = &s->diagnostics.controllers.history[i];
            snprintf(text, sizeof(text), "%lld..%lld ms  %s  %s x%lu",
                     (long long)ep->first_us / 1000, (long long)ep->last_us / 1000,
                     controller_diagnostics_operation_name(ep->operation),
                     esp_err_to_name(ep->result), (unsigned long)ep->occurrences);
            label(s->body, text, 0, 88 + CONTROLLER_OPERATION_COUNT * 28 + i * 44,
                  710, MONO, DIM);
        }
        break;
    }
    read_rows_rendered();
}
static bool subtree_interacting(lv_obj_t *object)
{
    if (lv_obj_has_state(object, LV_STATE_PRESSED) || lv_obj_is_scrolling(object)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i)
        if (subtree_interacting(lv_obj_get_child(object, i))) return true;
    return false;
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s)
        return;
    bool was_busy = s->snapshot.busy;
    refresh_read_snapshot();
    if (s->snapshot.revision != s->shown_revision || was_busy != s->snapshot.busy) {
        s->shown_revision = s->snapshot.revision;
        if (s->view != VIEW_URL && s->view != VIEW_DISPLAY && s->view != VIEW_OTA)
            s->rebuild = true;
        if (!s->snapshot.busy && s->snapshot.message[0])
            strlcpy(s->notice, s->snapshot.message, sizeof(s->notice));
    }
    if (s->rebuild && !subtree_interacting(s->root))
        render();
    if (s->hold_label) {
        bool allowed = !blocked();
        disabled(s->hold_button, !allowed);
        if (!allowed)
            maintenance_hold_reset(&s->hold);
        uint32_t elapsed = maintenance_hold_elapsed(&s->hold, lv_tick_get());
        lv_bar_set_value(s->hold_bar, elapsed > MAINTENANCE_HOLD_MS ? MAINTENANCE_HOLD_MS : elapsed,
                         LV_ANIM_OFF);
        if (s->hold.pressed)
            label_set_fmt_if_changed(
                s->hold_label, "Keep holding: %lu ms remaining. Release to cancel.",
                (unsigned long)(elapsed >= MAINTENANCE_HOLD_MS ? 0
                                                               : MAINTENANCE_HOLD_MS - elapsed));
        else
            label_set_if_changed(s->hold_label,
                              allowed ? "Release, cancellation or navigation resets the hold."
                                      : "Unavailable while services, therapy or storage are busy.");
    }
    if (s->ota_label) {
        maintenance_ota_snapshot(&s->ota);
        char text[640];
        static const char *stage[] = {"Not started",     "Transferring to inactive slot",
                                      "Verifying image", "Verifying / selecting boot image",
                                      "Restart pending", "Failed",
                                      "Cancelled"};
        const char *health =
            s->ota.boot_selected
                ? "New image selected; running firmware changes after restart."
                : "Running firmware unchanged; this update has not confirmed a new boot selection.";
        if (s->ota.total > 0) {
            int64_t elapsed = esp_timer_get_time() - s->ota.started_us;
            long long eta =
                s->ota.transferred > 0 && s->ota.transferred < s->ota.total
                    ? elapsed * (s->ota.total - s->ota.transferred) / s->ota.transferred / 1000000
                    : -1;
            snprintf(text, sizeof(text), "%s\n%d / %d bytes%s\n%s\n%s", stage[s->ota.stage],
                     s->ota.transferred, s->ota.total,
                     s->ota.active && eta >= 0 ? " - estimate based on observed transfer rate" : "",
                     health, s->ota.error);
            if (s->ota.active && eta >= 0) {
                size_t n = strlen(text);
                snprintf(text + n, sizeof(text) - n,
                         "\nAbout %lld s of transfer remaining; verification time unknown.", eta);
            }
            lv_bar_set_value(s->ota_bar, (int)((int64_t)s->ota.transferred * 100 / s->ota.total),
                             LV_ANIM_OFF);
        } else
            snprintf(text, sizeof(text), "%s\n%d bytes transferred; total size unknown\n%s\n%s",
                     stage[s->ota.stage], s->ota.transferred, health, s->ota.error);
        if (s->ota.done && !s->ota.ok && s->ota.failed_stage < 8) {
            size_t n = strlen(text);
            snprintf(text + n, sizeof(text) - n, "\nStopped at: %s", stage[s->ota.failed_stage]);
        }
        label_set_if_changed(s->ota_label, text);
        disabled(s->cancel_ota, !s->ota.cancellable || !s->ota.active);
    }
    if (lv_tick_elaps(s->last_diagnostics) > 1000 || !s->last_diagnostics) {
        s->last_diagnostics = lv_tick_get();
        touch_maintenance_diagnostics(&s->diagnostics);
        if (s->view == VIEW_DISPLAY) {
            device_settings_t current;
            device_settings_snapshot(&current);
            if (!lv_obj_has_state(s->brightness_slider, LV_STATE_PRESSED)) {
                lv_slider_set_value(s->brightness_slider, current.brightness, LV_ANIM_OFF);
                label_set_fmt_if_changed(s->brightness_value, "%d%%", (current.brightness + 1) / 2);
            }
            if (!lv_dropdown_is_open(s->timeout_dropdown)) {
                static const uint16_t values[] = {0, 60, 120, 300, 900, 1800};
                for (unsigned i = 0; i < 6; i++)
                    if (values[i] == current.screen_timeout_s)
                        lv_dropdown_set_selected(s->timeout_dropdown, i);
            }
            if (!lv_dropdown_is_open(s->mode_dropdown))
                lv_dropdown_set_selected(s->mode_dropdown,
                                         current.backlight_mode == BACKLIGHT_MODE_OFF_THRP     ? 1
                                         : current.backlight_mode == BACKLIGHT_MODE_ALWAYS_OFF ? 2
                                                                                                : 0);
        }
        if (s->diag) {
            maintenance_diagnostics_t *d = &s->diagnostics;
            char capacity[96], signal[40];
            if (d->sd_capacity_valid)
                snprintf(capacity, sizeof(capacity), "%llu / %llu bytes free",
                         (unsigned long long)d->sd_free, (unsigned long long)d->sd_total);
            else
                strlcpy(capacity, "unknown; no valid capacity sample", sizeof(capacity));
            if (d->rssi_valid)
                snprintf(signal, sizeof(signal), "%d dBm", d->rssi);
            else
                strlcpy(signal, "RSSI unavailable", sizeof(signal));
            label_set_fmt_if_changed(
                s->diag,
                "%s / %s / build %s\nInternal free/min/largest  %lu / %lu / %lu B\nPSRAM "
                "free/min/largest     %lu / %lu / %lu B\nTasks %lu   Uptime %llu s   Card "
                "%s\nTherapy %s / recording %s\nCapacity: %s",
                d->version, d->target, d->build, (unsigned long)d->free_internal,
                (unsigned long)d->min_internal, (unsigned long)d->largest_internal,
                (unsigned long)d->free_psram, (unsigned long)d->min_psram,
                (unsigned long)d->largest_psram, (unsigned long)d->tasks,
                (unsigned long long)d->uptime_s, d->card_ready ? "mounted" : "unavailable",
                d->therapy ? "active" : "inactive", d->recording ? "active" : "inactive",
                capacity);
            label_set_if_changed(s->services[0], d->airsense);
            label_set_if_changed(s->services[1], d->oxygen);
            label_set_fmt_if_changed(s->services[2], "%s / %s / %s",
                                  d->wifi ? d->ssid : "Offline", d->wifi ? d->ip : "IP unavailable",
                                  signal);
            label_set_if_changed(s->services[3], d->uploads);
            const char *controller = "Open measured operations and error history";
            for (size_t i = 0; i < CONTROLLER_OPERATION_COUNT; i++) {
                if (d->controllers.operations[i].observed &&
                    d->controllers.operations[i].last_result != ESP_OK) {
                    controller = controller_diagnostics_operation_name(i);
                    break;
                }
            }
            label_set_if_changed(s->services[4], controller);
        }
    }
    if (s->status)
        label_set_if_changed(s->status,
                          s->snapshot.busy
                              ? "Operation in progress; controls are disabled until completion."
                              : s->notice);
}
esp_err_t touch_maintenance_ui_show(lv_obj_t *parent, maintenance_ui_destination_t destination,
                                    const maintenance_ui_hooks_t *hooks)
{
    if (s || !parent || !hooks)
        return ESP_ERR_INVALID_STATE;
    s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    if (!s)
        return ESP_ERR_NO_MEM;
    s->hooks = *hooks;
    s->destination = destination;
    s->view_generation = 1;
    s->view = destination == MAINT_UI_STORAGE  ? VIEW_STORAGE
              : destination == MAINT_UI_SYSTEM ? VIEW_SYSTEM
                                               : VIEW_ADVANCED;
    s->root = lv_obj_create(parent);
    lv_obj_set_pos(s->root, 0, 0);
    lv_obj_set_size(s->root, 768, 450);
    lv_obj_set_style_bg_color(s->root, lv_color_hex(BG), 0);
    lv_obj_set_style_radius(s->root, 28, 0);
    lv_obj_set_style_clip_corner(s->root, true, 0);
    lv_obj_set_style_border_width(s->root, 0, 0);
    lv_obj_set_style_pad_all(s->root, 0, 0);
    lv_obj_clear_flag(s->root, LV_OBJ_FLAG_SCROLLABLE);
    touch_maintenance_snapshot(&s->snapshot);
    bool cached_scan = !s->snapshot.busy && s->snapshot.job_id &&
                       s->snapshot.action == MAINT_SCAN && s->snapshot.census_valid &&
                       s->snapshot.census_generation == sd_storage_content_generation();
    if (destination == MAINT_UI_STORAGE) {
        if (cached_scan) {
            s->read_action = MAINT_SCAN;
            s->read_view_generation = s->view_generation;
            s->read_job_id = s->snapshot.job_id;
        } else {
            request(MAINT_SCAN, NULL);
        }
    } else {
        refresh_read_snapshot();
    }
    s->shown_revision = s->snapshot.revision;
    touch_maintenance_diagnostics(&s->diagnostics);
    render();
    s->timer = lv_timer_create(tick, 100, NULL);
    return ESP_OK;
}
void touch_maintenance_ui_refresh(bool ready)
{
    if (s) {
        if (s->ready != ready) {
            s->ready = ready;
            s->rebuild = true;
        }
        tick(NULL);
    }
}
void touch_maintenance_ui_destroy(void)
{
    if (!s)
        return;
    touch_maintenance_cancel_reads();
    maintenance_hold_reset(&s->hold);
    if (s->timer)
        lv_timer_del(s->timer);
    lv_obj_del(s->root);
    free(s);
    s = NULL;
}

void touch_maintenance_ui_frame_published(void)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    /* Called on the LVGL thread only after successful final framebuffer submission. */
    if (!s || !lv_obj_is_visible(s->root))
        return;
    int scroll = lv_obj_get_scroll_y(s->body);
    int ota_stage = s->view == VIEW_OTA ? (int)s->ota.stage : -1;
    if (s->frame_marker_pending || scroll != s->frame_scroll || ota_stage != s->frame_ota_stage) {
        static const char *names[] = {"storage", "files", "file", "system", "firmware",
                                      "images", "url", "ota", "advanced", "hold", "display",
                                      "controllers", "notes"};
        ESP_LOGI("maintenance_ui", "QEMU maintenance frame view=%s scroll=%d revision=%lu busy=%d ota_stage=%d",
                 names[s->view], scroll, (unsigned long)s->snapshot.revision,
                 s->snapshot.busy, ota_stage);
        s->frame_marker_pending = false;
        s->frame_scroll = scroll;
        s->frame_ota_stage = ota_stage;
    }
#endif
}
