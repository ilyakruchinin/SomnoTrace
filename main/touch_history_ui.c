/* Rev B native History surface. Storage and worker orchestration stay in the
 * BSP; this file owns only bounded LVGL presentation state and touch intents. */

#include "touch_history_ui.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "somnotrace_fonts.h"

#define HISTORY_UI_EM_DASH "\xE2\x80\x94"

#define HISTORY_UI_COLOR_BASE       0x05070e
#define HISTORY_UI_COLOR_PANEL      0x181c29
#define HISTORY_UI_COLOR_CARD       0x101421
#define HISTORY_UI_COLOR_ROW        0x151a28
#define HISTORY_UI_COLOR_ROW_ACTIVE 0x293142
#define HISTORY_UI_COLOR_CONTROL    0x2d333f
#define HISTORY_UI_COLOR_TEXT       0xf0f2f6
#define HISTORY_UI_COLOR_SECONDARY  0xa0a5af
#define HISTORY_UI_COLOR_TERTIARY   0x818691
#define HISTORY_UI_COLOR_DISABLED   0x5e636e
#define HISTORY_UI_COLOR_LIVE       0x00e1e2
#define HISTORY_UI_COLOR_LIVE_DIM   0x387a82
#define HISTORY_UI_COLOR_O2         0xdd70e8
#define HISTORY_UI_COLOR_EPR        0x54a5a9
#define HISTORY_UI_COLOR_AMBER      0xf8bd40
#define HISTORY_UI_COLOR_FAULT      0xf45249
#define HISTORY_UI_COLOR_BORDER     0x2b3241
#define HISTORY_UI_COLOR_GRID       0x272d39

#define HISTORY_UI_RADIUS 12
#define HISTORY_UI_HIT 44
#define HISTORY_UI_ROW_GAP 4
#define HISTORY_UI_LIST_ROW_H 60
#define HISTORY_UI_LIST_VIEWPORT_Y 92
#define HISTORY_UI_LIST_VIEWPORT_H 358
#define HISTORY_UI_CALENDAR_Y HISTORY_UI_HIT
#define HISTORY_UI_CALENDAR_H (TOUCH_HISTORY_UI_HEIGHT - HISTORY_UI_CALENDAR_Y)
#define HISTORY_UI_CALENDAR_GRID_Y 48
#define HISTORY_UI_CALENDAR_GRID_H 294

#define HISTORY_UI_DETAIL_HEADER_H 44
#define HISTORY_UI_SUMMARY_Y 64
#define HISTORY_UI_SUMMARY_H 44
#define HISTORY_UI_CHANNEL_Y 114
#define HISTORY_UI_CHANNEL_H 44
#define HISTORY_UI_CHANNEL_ROW_GAP 4
#define HISTORY_UI_GRAPH_Y 208
#define HISTORY_UI_GRAPH_H 216

#define HISTORY_UI_GRAPH_PAD_L 42
#define HISTORY_UI_GRAPH_PAD_R 10
#define HISTORY_UI_GRAPH_PAD_T 54
#define HISTORY_UI_GRAPH_PAD_B 38
#define HISTORY_UI_EVENT_LANE_H 18
#define HISTORY_UI_EVENT_MARKER_SIZE 9

typedef struct {
    struct touch_history_ui *ui;
    uint8_t index;
} history_ui_binding_t;

typedef struct {
    lv_obj_t *button;
    lv_obj_t *date;
    lv_obj_t *meta;
    lv_obj_t *usage;
    lv_obj_t *therapy_dot;
    lv_obj_t *o2_dot;
} history_ui_row_t;

typedef struct {
    lv_obj_t *button;
    lv_obj_t *pill;
    lv_obj_t *label;
} history_ui_channel_control_t;

typedef struct {
    char label[24];
    char unit[12];
    int32_t value_x100;
    bool available;
} history_ui_stat_t;

struct touch_history_ui {
    touch_history_ui_config_t config;

    lv_obj_t *root;
    lv_obj_t *left;
    lv_obj_t *detail;

    lv_obj_t *list_segment;
    lv_obj_t *list_segment_button;
    lv_obj_t *list_segment_label;
    lv_obj_t *calendar_button;
    lv_obj_t *calendar_button_label;
    lv_obj_t *list_title;
    lv_obj_t *jump_to_date;
    lv_obj_t *list_viewport;
    history_ui_row_t rows[TOUCH_HISTORY_UI_LIST_ROWS];
    history_ui_binding_t row_bindings[TOUCH_HISTORY_UI_LIST_ROWS];

    lv_obj_t *night_title;
    lv_obj_t *night_subtitle;
    lv_obj_t *target_badge;
    lv_obj_t *target_badge_label;
    lv_obj_t *night_previous;
    lv_obj_t *night_next;

    lv_obj_t *summary;
    lv_obj_t *summary_labels[5];
    lv_obj_t *summary_values[5];

    history_ui_channel_control_t channels[TOUCH_HISTORY_UI_CHANNEL_CONTROLS];
    history_ui_binding_t channel_bindings[TOUCH_HISTORY_UI_CHANNEL_CONTROLS];

    lv_obj_t *graph;
    lv_obj_t *graph_title;
    lv_obj_t *graph_source;
    lv_obj_t *stat_labels[TOUCH_HISTORY_UI_STAT_COUNT];
    lv_obj_t *stat_values[TOUCH_HISTORY_UI_STAT_COUNT];
    lv_obj_t *stats_warning;
    lv_obj_t *fit_button;
    lv_obj_t *zoom_out_button;
    lv_obj_t *zoom_in_button;
    lv_obj_t *therapy_only_button;
    lv_obj_t *zoom_overlay;
    lv_obj_t *zoom_overlay_text;
    lv_obj_t *marker_legend;
    lv_obj_t *safety_footer;

    lv_obj_t *state_overlay;
    lv_obj_t *state_icon;
    lv_obj_t *state_title;
    lv_obj_t *state_body;
    lv_obj_t *state_progress;
    lv_obj_t *state_primary;
    lv_obj_t *state_primary_label;
    lv_obj_t *state_secondary;
    lv_obj_t *state_secondary_label;

    lv_obj_t *degraded_banner;
    lv_obj_t *degraded_label;

    /* Calendar is intentionally absent until the first calendar snapshot. */
    lv_obj_t *calendar_overlay;
    lv_obj_t *calendar_title;
    lv_obj_t *calendar_previous;
    lv_obj_t *calendar_next;
    lv_obj_t *calendar_grid;
    lv_obj_t *calendar_status;

    touch_history_ui_state_t state;
    touch_history_ui_rail_mode_t rail_mode;
    touch_history_day_t days[TOUCH_HISTORY_UI_LIST_ROWS];
    size_t day_count;
    touch_history_index_page_t page;
    size_t selected_row;
    touch_history_night_t night;
    bool has_night;
    touch_history_session_t sessions[TOUCH_HISTORY_UI_MAX_SESSIONS];
    size_t session_count;
    touch_history_overview_t overview;
    bool has_overview;
    touch_history_event_t events[TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS];
    size_t event_count;
    size_t event_total_count;
    touch_history_ui_event_state_t event_state;
    bool events_truncated;
    touch_history_month_t month;
    bool has_month;
    bool can_previous_month;
    bool can_next_month;
    bool calendar_loading;
    bool calendar_read_error;
    touch_history_signal_t signal;
    history_ui_stat_t stats[TOUCH_HISTORY_UI_STAT_COUNT];
    bool can_previous_night;
    bool can_next_night;
    bool usage_target_known;
    bool usage_on_target;
    bool therapy_only;
    bool cursor_valid;
    int64_t cursor_ms;
    lv_coord_t graph_press_x;
    int32_t graph_drag_delta_x;
    bool graph_dragged;
    uint16_t progress_per_mille;
    char status_text[TOUCH_HISTORY_UI_TEXT_MAX];
    char error_text[TOUCH_HISTORY_UI_TEXT_MAX];
    char degraded_text[TOUCH_HISTORY_UI_TEXT_MAX];
    char stats_warning_text[TOUCH_HISTORY_UI_TEXT_MAX];
};

static const char *const s_signal_names[TOUCH_HISTORY_SIGNAL_COUNT] = {
    "Breathing / Flow", "Pressure", "Leak", "Flow limit",
    "Snore", "SpO₂", "Pulse", "Motion",
};

static const char *const s_signal_units[TOUCH_HISTORY_SIGNAL_COUNT] = {
    "L/s", "cmH₂O", "L/min", "index", "index", "%", "bpm", "index",
};

static lv_color_t history_ui_color(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static void history_ui_copy_text(char *destination, size_t capacity,
                                 const char *source)
{
    if (!destination || capacity == 0)
        return;
    if (!source)
        source = "";
    snprintf(destination, capacity, "%s", source);
}

static lv_obj_t *history_ui_container(lv_obj_t *parent, lv_coord_t x,
                                      lv_coord_t y, lv_coord_t width,
                                      lv_coord_t height, uint32_t background,
                                      uint32_t border)
{
    lv_obj_t *object = lv_obj_create(parent);
    if (!object)
        return NULL;
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, HISTORY_UI_RADIUS, 0);
    lv_obj_set_style_bg_color(object, history_ui_color(background), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, border ? 1 : 0, 0);
    if (border)
        lv_obj_set_style_border_color(object, history_ui_color(border), 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *history_ui_label(lv_obj_t *parent, const char *text,
                                  const lv_font_t *font, uint32_t color,
                                  lv_coord_t x, lv_coord_t y,
                                  lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *label = lv_label_create(parent);
    if (!label)
        return NULL;
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, history_ui_color(color), 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text ? text : "");
    return label;
}

static lv_obj_t *history_ui_button(lv_obj_t *parent, lv_coord_t x,
                                   lv_coord_t y, lv_coord_t width,
                                   lv_coord_t height, uint32_t background)
{
    lv_obj_t *button = lv_btn_create(parent);
    if (!button)
        return NULL;
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, history_ui_color(background), 0);
    lv_obj_set_style_bg_color(button,
                              history_ui_color(HISTORY_UI_COLOR_ROW_ACTIVE),
                              LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    return button;
}

static lv_obj_t *history_ui_button_label(lv_obj_t *button, const char *text,
                                         const lv_font_t *font,
                                         uint32_t color)
{
    lv_obj_t *label = lv_label_create(button);
    if (!label)
        return NULL;
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, history_ui_color(color), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return label;
}

static void history_ui_set_hidden(lv_obj_t *object, bool hidden)
{
    if (!object)
        return;
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hidden)
        return;
    if (hidden)
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void history_ui_set_enabled(lv_obj_t *object, bool enabled)
{
    if (!object)
        return;
    bool currently_enabled =
        !lv_obj_has_state(object, LV_STATE_DISABLED);
    if (currently_enabled == enabled)
        return;
    if (enabled) {
        lv_obj_clear_state(object, LV_STATE_DISABLED);
        lv_obj_set_style_opa(object, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(object, LV_STATE_DISABLED);
        lv_obj_set_style_opa(object, LV_OPA_40, 0);
    }
}

static void history_ui_set_label_text_if_changed(lv_obj_t *label,
                                                 const char *text)
{
    if (!label)
        return;
    const char *next = text ? text : "";
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, next))
        lv_label_set_text(label, next);
}

static void history_ui_emit(touch_history_ui_t *ui,
                            touch_history_ui_intent_type_t type,
                            int64_t relative, int64_t timestamp_ms,
                            size_t row_index,
                            touch_history_signal_t signal,
                            const char *day)
{
    if (!ui || !ui->config.on_intent)
        return;
    touch_history_ui_intent_t intent = {
        .type = type,
        .relative = relative,
        .timestamp_ms = timestamp_ms,
        .row_index = row_index,
        .signal = signal,
    };
    history_ui_copy_text(intent.day, sizeof(intent.day), day);
    ui->config.on_intent(ui->config.intent_context, &intent);
}

static int history_ui_first_weekday(unsigned year, unsigned month);

static bool history_ui_parse_day(const char day[9], unsigned *year,
                                 unsigned *month, unsigned *date)
{
    unsigned parsed_year = 0, parsed_month = 0, parsed_date = 0;
    if (!day || strlen(day) != 8 ||
        sscanf(day, "%4u%2u%2u",
               &parsed_year, &parsed_month, &parsed_date) != 3 ||
        parsed_month < 1 || parsed_month > 12 ||
        parsed_date < 1 || parsed_date > 31)
        return false;
    if (year)
        *year = parsed_year;
    if (month)
        *month = parsed_month;
    if (date)
        *date = parsed_date;
    return true;
}

static void history_ui_format_day(const char day[9], char *output,
                                  size_t capacity)
{
    static const char *const months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };
    static const char *const weekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday",
    };
    unsigned year = 0, month = 0, date = 0;
    if (!history_ui_parse_day(day, &year, &month, &date)) {
        history_ui_copy_text(output, capacity, HISTORY_UI_EM_DASH);
        return;
    }
    unsigned weekday =
        ((unsigned)history_ui_first_weekday(year, month) + date - 1U) % 7U;
    snprintf(output, capacity, "%s %u %s %u",
             weekdays[weekday], date, months[month - 1], year);
}

static void history_ui_format_day_short(const char day[9], char *output,
                                        size_t capacity)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    static const char *const weekdays[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    };
    unsigned year = 0, month = 0, date = 0;
    if (!history_ui_parse_day(day, &year, &month, &date)) {
        history_ui_copy_text(output, capacity, HISTORY_UI_EM_DASH);
        return;
    }
    unsigned weekday =
        ((unsigned)history_ui_first_weekday(year, month) + date - 1U) % 7U;
    snprintf(output, capacity, "%s %u %s",
             weekdays[weekday], date, months[month - 1]);
}

static void history_ui_format_day_compact(const char day[9], char *output,
                                          size_t capacity)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    static const char *const weekdays[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    };
    unsigned year = 0, month = 0, date = 0;
    if (!history_ui_parse_day(day, &year, &month, &date)) {
        history_ui_copy_text(output, capacity, HISTORY_UI_EM_DASH);
        return;
    }
    unsigned weekday =
        ((unsigned)history_ui_first_weekday(year, month) + date - 1U) % 7U;
    snprintf(output, capacity, "%s %u %s %u",
             weekdays[weekday], date, months[month - 1], year);
}

static void history_ui_format_clock_minutes(int minutes, bool available,
                                            char *output, size_t capacity)
{
    if (!available || minutes < 0) {
        history_ui_copy_text(output, capacity, HISTORY_UI_EM_DASH);
        return;
    }
    snprintf(output, capacity, "%d:%02d", minutes / 60, minutes % 60);
}

static void history_ui_format_x100(int32_t value_x100, bool available,
                                   char *output, size_t capacity)
{
    if (!available) {
        history_ui_copy_text(output, capacity, HISTORY_UI_EM_DASH);
        return;
    }
    int64_t magnitude = value_x100;
    const char *sign = "";
    if (magnitude < 0) {
        sign = "-";
        magnitude = -magnitude;
    }
    snprintf(output, capacity, "%s%" PRId64 ".%02" PRId64,
             sign, magnitude / 100, magnitude % 100);
}

static const touch_history_day_t *history_ui_selected_day(
    const touch_history_ui_t *ui)
{
    if (!ui || ui->selected_row >= ui->day_count)
        return NULL;
    return &ui->days[ui->selected_row];
}

static void history_ui_row_pressed(lv_event_t *event)
{
    history_ui_binding_t *binding = lv_event_get_user_data(event);
    if (!binding || !binding->ui || binding->index >= binding->ui->day_count)
        return;
    const touch_history_day_t *day = &binding->ui->days[binding->index];
    history_ui_emit(binding->ui, TOUCH_HISTORY_UI_INTENT_SELECT_DAY, 0, 0,
                    binding->index, binding->ui->signal, day->day);
}

static void history_ui_channel_pressed(lv_event_t *event)
{
    history_ui_binding_t *binding = lv_event_get_user_data(event);
    if (!binding || !binding->ui ||
        binding->index >= TOUCH_HISTORY_SIGNAL_COUNT)
        return;
    history_ui_emit(binding->ui, TOUCH_HISTORY_UI_INTENT_SELECT_CHANNEL, 0, 0,
                    binding->ui->selected_row,
                    (touch_history_signal_t)binding->index,
                    binding->ui->has_night ? binding->ui->night.day : NULL);
}

static void history_ui_list_scroll_end(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || !ui->list_viewport)
        return;
    int64_t direction = 0;
    if (lv_obj_get_scroll_bottom(ui->list_viewport) <= 2 &&
        ui->page.has_more)
        direction = 1;
    else if (lv_obj_get_scroll_y(ui->list_viewport) <= 2 &&
             ui->page.offset > 0)
        direction = -1;
    if (direction)
        history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_PAGE_RELATIVE,
                        direction, 0, SIZE_MAX, ui->signal, NULL);
}

static void history_ui_open_calendar_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_OPEN_CALENDAR, 0, 0,
                    SIZE_MAX, ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW, NULL);
}

static void history_ui_close_calendar_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_CLOSE_CALENDAR, 0, 0,
                    SIZE_MAX, ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW, NULL);
}

static void history_ui_month_previous_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_MONTH_RELATIVE, -1, 0,
                    SIZE_MAX, ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW, NULL);
}

static void history_ui_month_next_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_MONTH_RELATIVE, 1, 0,
                    SIZE_MAX, ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW, NULL);
}

static void history_ui_previous_night_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_PREVIOUS_NIGHT, -1, 0,
                    ui ? ui->selected_row : SIZE_MAX,
                    ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW,
                    ui && ui->has_night ? ui->night.day : NULL);
}

static void history_ui_next_night_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_NEXT_NIGHT, 1, 0,
                    ui ? ui->selected_row : SIZE_MAX,
                    ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW,
                    ui && ui->has_night ? ui->night.day : NULL);
}

static void history_ui_fit_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_FIT_NIGHT, 0, 0,
                    ui ? ui->selected_row : SIZE_MAX,
                    ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW,
                    ui && ui->has_night ? ui->night.day : NULL);
}

static void history_ui_zoom_out_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_ZOOM_RELATIVE, -1, 0,
                    ui ? ui->selected_row : SIZE_MAX,
                    ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW,
                    ui && ui->has_night ? ui->night.day : NULL);
}

static void history_ui_zoom_in_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_ZOOM_RELATIVE, 1, 0,
                    ui ? ui->selected_row : SIZE_MAX,
                    ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW,
                    ui && ui->has_night ? ui->night.day : NULL);
}

static void history_ui_therapy_only_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_TOGGLE_THERAPY_ONLY,
                    ui && ui->therapy_only ? 0 : 1, 0,
                    ui ? ui->selected_row : SIZE_MAX,
                    ui ? ui->signal : TOUCH_HISTORY_SIGNAL_SPO2,
                    ui && ui->has_night ? ui->night.day : NULL);
}

static void history_ui_cancel_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || ui->state != TOUCH_HISTORY_UI_STATE_AUTO_LOADING)
        return;
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_CANCEL_AUTO_LOAD, 0, 0,
                    ui->selected_row, ui->signal,
                    ui->has_night ? ui->night.day : NULL);
}

static void history_ui_retry_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_RETRY_READ, 0, 0,
                    ui ? ui->selected_row : SIZE_MAX,
                    ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW,
                    ui && ui->has_night ? ui->night.day : NULL);
}

static void history_ui_open_card_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_OPEN_CARD, 0, 0,
                    ui ? ui->selected_row : SIZE_MAX,
                    ui ? ui->signal : TOUCH_HISTORY_SIGNAL_FLOW,
                    ui && ui->has_night ? ui->night.day : NULL);
}

static int history_ui_days_in_month(unsigned year, unsigned month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12)
        return 0;
    int result = days[month - 1];
    bool leap = (year % 4U == 0U && year % 100U != 0U) ||
                (year % 400U == 0U);
    if (month == 2 && leap)
        result++;
    return result;
}

/* Gregorian weekday, 0=Sunday. */
static int history_ui_first_weekday(unsigned year, unsigned month)
{
    static const int offsets[] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4,
    };
    if (month < 3)
        year--;
    return (int)((year + year / 4U - year / 100U + year / 400U +
                  (unsigned)offsets[month - 1] + 1U) % 7U);
}

/* The general date formatter above uses Sunday=0. The History handoff's
 * calendar is Monday-first, so translate only the calendar column index. */
static int history_ui_calendar_first_column(unsigned year, unsigned month)
{
    return (history_ui_first_weekday(year, month) + 6) % 7;
}

static int history_ui_calendar_day_at(const touch_history_ui_t *ui,
                                      lv_coord_t x, lv_coord_t y)
{
    if (!ui || !ui->has_month || !ui->calendar_grid)
        return 0;
    lv_area_t area;
    lv_obj_get_content_coords(ui->calendar_grid, &area);
    x -= area.x1;
    y -= area.y1;
    if (x < 0 || y < 30)
        return 0;
    int col = x * 7 / lv_area_get_width(&area);
    int row = (y - 30) / 44;
    if (col < 0 || col > 6 || row < 0 || row > 5)
        return 0;
    int day = row * 7 + col -
              history_ui_calendar_first_column(
                  ui->month.year, ui->month.month) + 1;
    int count = history_ui_days_in_month(ui->month.year, ui->month.month);
    return day >= 1 && day <= count ? day : 0;
}

static void history_ui_calendar_grid_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_get_act();
    /* The old month remains painted while its replacement is read. Ignore
     * coordinates until the requested month is coherent and published. */
    if (!ui || !indev || ui->calendar_loading)
        return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int day_number = history_ui_calendar_day_at(ui, point.x, point.y);
    if (day_number == 0)
        return;
    uint32_t bit = 1UL << (unsigned)(day_number - 1);
    if (((ui->month.therapy_days | ui->month.oximetry_days) & bit) == 0)
        return;
    unsigned year = ui->month.year;
    unsigned month = ui->month.month;
    unsigned number = (unsigned)day_number;
    char day[9] = {
        (char)('0' + year / 1000U % 10U),
        (char)('0' + year / 100U % 10U),
        (char)('0' + year / 10U % 10U),
        (char)('0' + year % 10U),
        (char)('0' + month / 10U),
        (char)('0' + month % 10U),
        (char)('0' + number / 10U),
        (char)('0' + number % 10U),
        '\0',
    };
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_SELECT_CALENDAR_DAY, 0, 0,
                    SIZE_MAX, ui->signal, day);
}

static int64_t history_ui_graph_time_at_x(const touch_history_ui_t *ui,
                                          lv_coord_t absolute_x)
{
    if (!ui || !ui->has_overview)
        return 0;
    lv_area_t area;
    lv_obj_get_content_coords(ui->graph, &area);
    lv_coord_t left = area.x1 + HISTORY_UI_GRAPH_PAD_L;
    lv_coord_t right = area.x2 - HISTORY_UI_GRAPH_PAD_R;
    if (right <= left)
        return ui->overview.axis_start_ms;
    if (absolute_x < left)
        absolute_x = left;
    if (absolute_x > right)
        absolute_x = right;
    int64_t duration = ui->overview.axis_end_ms - ui->overview.axis_start_ms;
    return ui->overview.axis_start_ms +
           (duration * (absolute_x - left)) / (right - left);
}

static void history_ui_finish_graph_pan(touch_history_ui_t *ui)
{
    if (!ui)
        return;

    bool dragged = ui->graph_dragged;
    int32_t delta_x = ui->graph_drag_delta_x;
    ui->graph_drag_delta_x = 0;
    if (!dragged || delta_x == 0 || !ui->has_overview)
        return;

    lv_area_t area;
    lv_obj_get_content_coords(ui->graph, &area);
    lv_coord_t width = lv_area_get_width(&area) -
                       HISTORY_UI_GRAPH_PAD_L - HISTORY_UI_GRAPH_PAD_R;
    int64_t duration = ui->overview.axis_end_ms - ui->overview.axis_start_ms;
    if (width <= 0 || duration <= 0)
        return;

    int64_t delta_ms = -((int64_t)delta_x * duration) / width;
    if (delta_ms == 0)
        return;
    history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_PAN_RELATIVE, delta_ms,
                    ui->cursor_ms, ui->selected_row, ui->signal,
                    ui->has_night ? ui->night.day : NULL);
}

static void history_ui_graph_touch(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    if (!ui)
        return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        history_ui_finish_graph_pan(ui);
        return;
    }
    if (!ui->has_overview)
        return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev)
        return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED) {
        ui->graph_press_x = point.x;
        ui->graph_drag_delta_x = 0;
        ui->graph_dragged = false;
        return;
    }
    if (code == LV_EVENT_SHORT_CLICKED) {
        /* LVGL sends RELEASED before SHORT_CLICKED. Keep the drag marker
         * through the release handler so a completed pan cannot also place a
         * cursor at the release point. */
        if (ui->graph_dragged) {
            ui->graph_dragged = false;
            return;
        }
        bool clear_cursor = false;
        if (ui->cursor_valid &&
            ui->overview.axis_end_ms > ui->overview.axis_start_ms) {
            lv_area_t area;
            lv_obj_get_content_coords(ui->graph, &area);
            lv_coord_t left = area.x1 + HISTORY_UI_GRAPH_PAD_L;
            lv_coord_t right = area.x2 - HISTORY_UI_GRAPH_PAD_R;
            lv_coord_t cursor_x = left + (lv_coord_t)(
                ((ui->cursor_ms - ui->overview.axis_start_ms) *
                 (right - left)) /
                (ui->overview.axis_end_ms - ui->overview.axis_start_ms));
            lv_coord_t distance = point.x - cursor_x;
            if (distance < 0) distance = -distance;
            clear_cursor = distance <= 18;
        }
        if (clear_cursor) {
            ui->cursor_valid = false;
            ui->cursor_ms = 0;
            history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_CLEAR_CURSOR, 0, 0,
                            ui->selected_row, ui->signal,
                            ui->has_night ? ui->night.day : NULL);
        } else {
            ui->cursor_ms = history_ui_graph_time_at_x(ui, point.x);
            ui->cursor_valid = true;
            history_ui_emit(ui, TOUCH_HISTORY_UI_INTENT_SET_CURSOR, 0,
                            ui->cursor_ms, ui->selected_row, ui->signal,
                            ui->has_night ? ui->night.day : NULL);
        }
        lv_obj_invalidate(ui->graph);
        return;
    }
    if (code != LV_EVENT_PRESSING)
        return;
    ui->graph_drag_delta_x = (int32_t)point.x - (int32_t)ui->graph_press_x;
    if (ui->graph_drag_delta_x <= -4 || ui->graph_drag_delta_x >= 4)
        ui->graph_dragged = true;
}

static void history_ui_draw_line(lv_draw_ctx_t *draw_ctx, uint32_t color,
                                 lv_opa_t opacity, lv_coord_t width,
                                 lv_coord_t x1, lv_coord_t y1,
                                 lv_coord_t x2, lv_coord_t y2)
{
    lv_draw_line_dsc_t descriptor;
    lv_draw_line_dsc_init(&descriptor);
    descriptor.color = history_ui_color(color);
    descriptor.opa = opacity;
    descriptor.width = width;
    descriptor.round_start = true;
    descriptor.round_end = true;
    lv_point_t points[2] = {{x1, y1}, {x2, y2}};
    lv_draw_line(draw_ctx, &descriptor, &points[0], &points[1]);
}

static void history_ui_draw_rect(lv_draw_ctx_t *draw_ctx, const lv_area_t *area,
                                 uint32_t color, lv_opa_t opacity,
                                 lv_coord_t radius, uint32_t border_color,
                                 lv_coord_t border_width)
{
    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.bg_color = history_ui_color(color);
    descriptor.bg_opa = opacity;
    descriptor.radius = radius;
    descriptor.border_color = history_ui_color(border_color);
    descriptor.border_width = border_width;
    descriptor.border_opa = border_width ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_draw_rect(draw_ctx, &descriptor, area);
}

static void history_ui_draw_text(lv_draw_ctx_t *draw_ctx, const lv_area_t *area,
                                 const char *text, const lv_font_t *font,
                                 uint32_t color, lv_text_align_t alignment)
{
    lv_draw_label_dsc_t descriptor;
    lv_draw_label_dsc_init(&descriptor);
    descriptor.font = font;
    descriptor.color = history_ui_color(color);
    descriptor.align = alignment;
    lv_draw_label(draw_ctx, &descriptor, area, text, NULL);
}

static lv_coord_t history_ui_plot_x(const touch_history_overview_t *overview,
                                    size_t index, lv_coord_t left,
                                    lv_coord_t right)
{
    if (!overview || right <= left)
        return left;
    int64_t timestamp = overview->timestamp_ms[index];
    int64_t duration = overview->axis_end_ms - overview->axis_start_ms;
    if (duration > 0 && timestamp >= overview->axis_start_ms)
        return left + (lv_coord_t)(((timestamp - overview->axis_start_ms) *
                                    (right - left)) / duration);
    size_t count = overview->point_count ? overview->point_count : 1;
    if (count < 2)
        return left;
    return left + (lv_coord_t)((index * (size_t)(right - left)) / (count - 1));
}

static lv_coord_t history_ui_plot_y(int32_t value_x100, int32_t minimum,
                                    int32_t maximum, lv_coord_t top,
                                    lv_coord_t bottom)
{
    if (maximum <= minimum || bottom <= top)
        return top + (bottom - top) / 2;
    int64_t numerator = (int64_t)(maximum - value_x100) * (bottom - top);
    return top + (lv_coord_t)(numerator / (maximum - minimum));
}

static bool history_ui_point_visible(const touch_history_ui_t *ui,
                                     size_t index)
{
    if (!ui || index >= ui->overview.point_count ||
        !(ui->overview.flags[index] & TOUCH_HISTORY_POINT_VALID))
        return false;
    if (ui->signal == TOUCH_HISTORY_SIGNAL_SPO2 && ui->therapy_only &&
        !(ui->overview.flags[index] & TOUCH_HISTORY_POINT_THERAPY))
        return false;
    return true;
}

static void history_ui_format_clock(int64_t timestamp_ms, bool seconds,
                                    char *output, size_t capacity)
{
    time_t timestamp = (time_t)(timestamp_ms / 1000);
    struct tm local = {0};
    if (!localtime_r(&timestamp, &local)) {
        history_ui_copy_text(output, capacity, HISTORY_UI_EM_DASH);
        return;
    }
    if (seconds)
        snprintf(output, capacity, "%02d:%02d:%02d",
                 local.tm_hour, local.tm_min, local.tm_sec);
    else
        snprintf(output, capacity, "%02d:%02d", local.tm_hour, local.tm_min);
}

static const char *history_ui_event_code(touch_history_event_type_t type)
{
    switch (type) {
    case TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA:
        return "OA";
    case TOUCH_HISTORY_EVENT_CENTRAL_APNEA:
        return "CA";
    case TOUCH_HISTORY_EVENT_HYPOPNEA:
        return "H";
    case TOUCH_HISTORY_EVENT_GENERIC_APNEA:
        return "A";
    case TOUCH_HISTORY_EVENT_RERA:
        return "RERA";
    default:
        return "?";
    }
}

static uint32_t history_ui_event_color(touch_history_event_type_t type)
{
    switch (type) {
    case TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA:
        return HISTORY_UI_COLOR_FAULT;
    case TOUCH_HISTORY_EVENT_CENTRAL_APNEA:
        return 0x7ca9ff;
    case TOUCH_HISTORY_EVENT_HYPOPNEA:
        return HISTORY_UI_COLOR_AMBER;
    case TOUCH_HISTORY_EVENT_GENERIC_APNEA:
        return 0xe38b55;
    case TOUCH_HISTORY_EVENT_RERA:
        return HISTORY_UI_COLOR_O2;
    default:
        return HISTORY_UI_COLOR_TERTIARY;
    }
}

static void history_ui_draw_event_lane(const touch_history_ui_t *ui,
                                       lv_draw_ctx_t *draw_ctx,
                                       lv_coord_t left, lv_coord_t right,
                                       lv_coord_t lane_y)
{
    history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_GRID, LV_OPA_70, 1,
                         left, lane_y, right, lane_y);
    if (!ui || ui->event_state == TOUCH_HISTORY_UI_EVENT_STATE_UNAVAILABLE ||
        !ui->has_overview ||
        ui->overview.axis_end_ms <= ui->overview.axis_start_ms)
        return;

    const lv_coord_t half = HISTORY_UI_EVENT_MARKER_SIZE / 2;
    for (size_t i = 0; i < ui->event_count; ++i) {
        const touch_history_event_t *marker = &ui->events[i];
        if ((int)marker->type < 0 ||
            marker->type >= TOUCH_HISTORY_EVENT_TYPE_COUNT ||
            marker->end_ms < ui->overview.axis_start_ms ||
            marker->end_ms >= ui->overview.axis_end_ms)
            continue;

        /* The source notification is an event-end report, so its end time is
         * the actual lane timestamp. A filled square survives the lower
         * contrast and viewing distance of the physical bedside panel. */
        lv_coord_t x = left + (lv_coord_t)(
            ((marker->end_ms - ui->overview.axis_start_ms) *
             (right - left)) /
            (ui->overview.axis_end_ms - ui->overview.axis_start_ms));
        if (x < left + half)
            x = left + half;
        if (x > right - half)
            x = right - half;
        lv_area_t marker_area = {
            x - half, lane_y - half, x + half, lane_y + half,
        };
        uint32_t color = history_ui_event_color(marker->type);
        history_ui_draw_rect(draw_ctx, &marker_area, color, LV_OPA_COVER,
                             2, color, 0);
    }
}

static const touch_history_event_t *history_ui_cursor_event(
    const touch_history_ui_t *ui)
{
    if (!ui || !ui->cursor_valid)
        return NULL;
    for (size_t i = 0; i < ui->event_count; ++i) {
        const touch_history_event_t *event = &ui->events[i];
        if (ui->cursor_ms >= event->start_ms &&
            ui->cursor_ms <= event->end_ms)
            return event;
    }
    /* Do not attach a merely nearby event to the cursor. That would imply an
     * exact temporal relationship which the selected display bin cannot prove. */
    return NULL;
}

static void history_ui_draw_unreadable_spans(touch_history_ui_t *ui,
                                             lv_draw_ctx_t *draw_ctx,
                                             lv_coord_t left, lv_coord_t right,
                                             lv_coord_t top, lv_coord_t bottom)
{
    if (!ui || ui->state != TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN)
        return;
    size_t count = ui->overview.point_count;
    for (size_t begin = 0; begin < count;) {
        bool unreadable =
            !(ui->overview.flags[begin] & TOUCH_HISTORY_POINT_VALID) &&
            (ui->overview.flags[begin] & TOUCH_HISTORY_POINT_THERAPY);
        if (!unreadable) {
            begin++;
            continue;
        }
        size_t end = begin + 1;
        while (end < count &&
               !(ui->overview.flags[end] & TOUCH_HISTORY_POINT_VALID) &&
               (ui->overview.flags[end] & TOUCH_HISTORY_POINT_THERAPY))
            end++;
        lv_coord_t x1 = history_ui_plot_x(&ui->overview, begin, left, right);
        lv_coord_t x2 = history_ui_plot_x(&ui->overview, end - 1, left, right);
        if (x2 <= x1)
            x2 = x1 + 2;
        lv_area_t region = {x1, top, x2, bottom};
        history_ui_draw_rect(draw_ctx, &region, HISTORY_UI_COLOR_DISABLED,
                             LV_OPA_20, 0, HISTORY_UI_COLOR_DISABLED, 0);
        lv_coord_t height = bottom - top;
        for (lv_coord_t base = x1 - height; base < x2; base += 10) {
            lv_coord_t start = base < x1 ? x1 - base : 0;
            lv_coord_t finish = height;
            if (base + finish > x2)
                finish = x2 - base;
            if (finish > start)
                history_ui_draw_line(
                    draw_ctx, HISTORY_UI_COLOR_DISABLED, LV_OPA_40, 1,
                    base + start, bottom - start,
                    base + finish, bottom - finish);
        }
        begin = end;
    }
}

static void history_ui_graph_draw(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_obj_t *graph = lv_event_get_target(event);
    if (!ui || !draw_ctx || !graph)
        return;

    lv_area_t area;
    lv_obj_get_content_coords(graph, &area);
    lv_coord_t left = area.x1 + HISTORY_UI_GRAPH_PAD_L;
    lv_coord_t right = area.x2 - HISTORY_UI_GRAPH_PAD_R;
    lv_coord_t top = area.y1 + HISTORY_UI_GRAPH_PAD_T;
    lv_coord_t bottom = area.y2 - HISTORY_UI_GRAPH_PAD_B -
                        HISTORY_UI_EVENT_LANE_H;
    if (right <= left || bottom <= top)
        return;

    for (unsigned i = 0; i <= 4; ++i) {
        lv_coord_t y = top + (lv_coord_t)((bottom - top) * i / 4U);
        history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_GRID, LV_OPA_60, 1,
                             left, y, right, y);
    }
    for (unsigned i = 0; i <= 4; ++i) {
        lv_coord_t x = left + (lv_coord_t)((right - left) * i / 4U);
        history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_GRID, LV_OPA_40, 1,
                             x, top, x, bottom);
    }

    /* The event lane is independent of the selected signal. Keep it visible
     * even when that signal has no readable samples. */
    lv_coord_t lane_y = top - 7;
    history_ui_draw_event_lane(ui, draw_ctx, left, right, lane_y);

    if (!ui->has_overview || !ui->overview.loaded ||
        !ui->overview.has_data || ui->overview.point_count == 0) {
        lv_area_t message_area = {
            left, top + (bottom - top) / 2 - 10,
            right, top + (bottom - top) / 2 + 16,
        };
        history_ui_draw_text(draw_ctx, &message_area,
                             "No recorded samples for this channel",
                             &somnotrace_space_grotesk_medium_15,
                             HISTORY_UI_COLOR_SECONDARY,
                             LV_TEXT_ALIGN_CENTER);
        return;
    }

    int32_t minimum = INT32_MAX;
    int32_t maximum = INT32_MIN;
    for (size_t i = 0; i < ui->overview.point_count; ++i) {
        if (!history_ui_point_visible(ui, i))
            continue;
        int32_t low = ui->overview.value_x100[i];
        int32_t high = low;
        if (ui->overview.flags[i] & TOUCH_HISTORY_POINT_UPPER_VALID)
            high = ui->overview.upper_x100[i];
        if (low < minimum)
            minimum = low;
        if (high > maximum)
            maximum = high;
        if (ui->overview.flags[i] & TOUCH_HISTORY_POINT_COMPANION_VALID) {
            int32_t companion = ui->overview.companion_x100[i];
            if (companion < minimum)
                minimum = companion;
            if (companion > maximum)
                maximum = companion;
        }
    }
    if (minimum == INT32_MAX || maximum == INT32_MIN) {
        lv_area_t message_area = {
            left, top + (bottom - top) / 2 - 10,
            right, top + (bottom - top) / 2 + 16,
        };
        history_ui_draw_text(draw_ctx, &message_area,
                             ui->therapy_only
                                 ? "No SpO₂ samples during therapy"
                                 : "No valid samples in this window",
                             &somnotrace_space_grotesk_medium_15,
                             HISTORY_UI_COLOR_SECONDARY,
                             LV_TEXT_ALIGN_CENTER);
        return;
    }
    if (minimum == maximum) {
        int32_t padding = minimum == 0 ? 100 : abs(minimum) / 10;
        if (padding < 25)
            padding = 25;
        minimum -= padding;
        maximum += padding;
    } else {
        int32_t padding = (maximum - minimum) / 12;
        if (padding < 10)
            padding = 10;
        minimum -= padding;
        maximum += padding;
    }
    if (ui->signal == TOUCH_HISTORY_SIGNAL_FLOW &&
        minimum < 0 && maximum > 0) {
        int32_t magnitude = -minimum > maximum ? -minimum : maximum;
        minimum = -magnitude;
        maximum = magnitude;
    }

    char scale[24];
    for (unsigned tick = 0; tick <= 4; ++tick) {
        int32_t value = maximum -
                        (int32_t)(((int64_t)(maximum - minimum) * tick) / 4);
        history_ui_format_x100(value, true, scale, sizeof(scale));
        lv_coord_t y = top + (lv_coord_t)((bottom - top) * tick / 4U);
        lv_area_t scale_area = {
            area.x1 + 2, y - 8, left - 5, y + 10,
        };
        history_ui_draw_text(draw_ctx, &scale_area, scale,
                             &somnotrace_ibm_plex_mono_medium_11,
                             HISTORY_UI_COLOR_TERTIARY,
                             LV_TEXT_ALIGN_RIGHT);
    }

    history_ui_draw_unreadable_spans(ui, draw_ctx, left, right, top, bottom);

    const bool envelope =
        ui->signal == TOUCH_HISTORY_SIGNAL_FLOW &&
        ui->overview.aggregation == TOUCH_HISTORY_AGGREGATION_ENVELOPE;
    size_t previous = SIZE_MAX;
    size_t previous_companion = SIZE_MAX;
    for (size_t i = 0; i < ui->overview.point_count; ++i) {
        lv_coord_t x = history_ui_plot_x(&ui->overview, i, left, right);
        if (history_ui_point_visible(ui, i)) {
            lv_coord_t y = history_ui_plot_y(ui->overview.value_x100[i],
                                             minimum, maximum, top, bottom);
            if (envelope &&
                (ui->overview.flags[i] & TOUCH_HISTORY_POINT_UPPER_VALID)) {
                lv_coord_t upper = history_ui_plot_y(
                    ui->overview.upper_x100[i], minimum, maximum, top, bottom);
                history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_LIVE,
                                     LV_OPA_60, 2, x, upper, x, y);
                if (previous != SIZE_MAX &&
                    (ui->overview.flags[previous] &
                     TOUCH_HISTORY_POINT_UPPER_VALID)) {
                    bool adjacent =
                        ui->overview.bin_width_ms == 0 ||
                        ui->overview.timestamp_ms[i] -
                                ui->overview.timestamp_ms[previous] <=
                            (int64_t)ui->overview.bin_width_ms * 2;
                    if (adjacent) {
                        lv_coord_t previous_x = history_ui_plot_x(
                            &ui->overview, previous, left, right);
                        lv_coord_t previous_low = history_ui_plot_y(
                            ui->overview.value_x100[previous],
                            minimum, maximum, top, bottom);
                        lv_coord_t previous_upper = history_ui_plot_y(
                            ui->overview.upper_x100[previous],
                            minimum, maximum, top, bottom);
                        history_ui_draw_line(
                            draw_ctx, HISTORY_UI_COLOR_LIVE, LV_OPA_80, 1,
                            previous_x, previous_low, x, y);
                        history_ui_draw_line(
                            draw_ctx, HISTORY_UI_COLOR_LIVE, LV_OPA_80, 1,
                            previous_x, previous_upper, x, upper);
                    }
                }
            } else if (previous != SIZE_MAX) {
                bool adjacent =
                    ui->overview.bin_width_ms == 0 ||
                    ui->overview.timestamp_ms[i] -
                            ui->overview.timestamp_ms[previous] <=
                        (int64_t)ui->overview.bin_width_ms * 2;
                if (adjacent) {
                    lv_coord_t previous_x = history_ui_plot_x(
                        &ui->overview, previous, left, right);
                    lv_coord_t previous_y = history_ui_plot_y(
                        ui->overview.value_x100[previous], minimum, maximum,
                        top, bottom);
                    history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_LIVE,
                                         LV_OPA_COVER, 2, previous_x,
                                         previous_y, x, y);
                }
            }
            previous = i;
        } else {
            previous = SIZE_MAX;
        }

        if (ui->signal == TOUCH_HISTORY_SIGNAL_PRESSURE &&
            (ui->overview.flags[i] &
             (TOUCH_HISTORY_POINT_VALID |
              TOUCH_HISTORY_POINT_COMPANION_VALID)) ==
                (TOUCH_HISTORY_POINT_VALID |
                 TOUCH_HISTORY_POINT_COMPANION_VALID)) {
            lv_coord_t companion_y = history_ui_plot_y(
                ui->overview.companion_x100[i], minimum, maximum, top, bottom);
            if (previous_companion != SIZE_MAX) {
                bool adjacent =
                    ui->overview.bin_width_ms == 0 ||
                    ui->overview.timestamp_ms[i] -
                            ui->overview.timestamp_ms[previous_companion] <=
                        (int64_t)ui->overview.bin_width_ms * 2;
                if (adjacent) {
                    lv_coord_t previous_x = history_ui_plot_x(
                        &ui->overview, previous_companion, left, right);
                    lv_coord_t previous_y = history_ui_plot_y(
                        ui->overview.companion_x100[previous_companion],
                        minimum, maximum, top, bottom);
                    history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_EPR,
                                         LV_OPA_70, 2, previous_x, previous_y,
                                         x, companion_y);
                }
            }
            previous_companion = i;
        } else {
            previous_companion = SIZE_MAX;
        }
    }

    for (size_t i = 0; i < ui->session_count; ++i) {
        int64_t boundaries[2] = {
            ui->sessions[i].start_ms, ui->sessions[i].end_ms,
        };
        for (size_t boundary = 0; boundary < 2; ++boundary) {
            int64_t timestamp = boundaries[boundary];
            if (timestamp <= ui->overview.axis_start_ms ||
                timestamp >= ui->overview.axis_end_ms)
                continue;
            lv_coord_t x = left + (lv_coord_t)(
                ((timestamp - ui->overview.axis_start_ms) * (right - left)) /
                (ui->overview.axis_end_ms - ui->overview.axis_start_ms));
            history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_SECONDARY,
                                 LV_OPA_30, 1, x, top, x, bottom);
        }
    }

    if (ui->signal == TOUCH_HISTORY_SIGNAL_SPO2 ||
        ui->signal == TOUCH_HISTORY_SIGNAL_PULSE) {
        lv_coord_t availability_y = bottom + 3;
        history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_DISABLED,
                             LV_OPA_50, 3, left, availability_y,
                             right, availability_y);
        for (size_t i = 0; i < ui->overview.point_count; ++i) {
            if (!history_ui_point_visible(ui, i))
                continue;
            lv_coord_t x1 = history_ui_plot_x(
                &ui->overview, i, left, right);
            lv_coord_t x2 = i + 1 < ui->overview.point_count
                                ? history_ui_plot_x(
                                      &ui->overview, i + 1, left, right)
                                : x1 + 1;
            history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_O2,
                                 LV_OPA_COVER, 3, x1, availability_y,
                                 x2, availability_y);
        }
    }

    for (size_t i = 0; i < ui->session_count; ++i) {
        int64_t start = ui->sessions[i].start_ms;
        int64_t end = ui->sessions[i].end_ms;
        if (end <= ui->overview.axis_start_ms ||
            start >= ui->overview.axis_end_ms)
            continue;
        if (start < ui->overview.axis_start_ms)
            start = ui->overview.axis_start_ms;
        if (end > ui->overview.axis_end_ms)
            end = ui->overview.axis_end_ms;
        lv_coord_t x1 = left + (lv_coord_t)(
            ((start - ui->overview.axis_start_ms) * (right - left)) /
            (ui->overview.axis_end_ms - ui->overview.axis_start_ms));
        lv_coord_t x2 = left + (lv_coord_t)(
            ((end - ui->overview.axis_start_ms) * (right - left)) /
            (ui->overview.axis_end_ms - ui->overview.axis_start_ms));
        if (x2 - x1 < 72)
            continue;
        char start_time[12], end_time[12], caption[64];
        history_ui_format_clock(start, false, start_time, sizeof(start_time));
        history_ui_format_clock(end, false, end_time, sizeof(end_time));
        snprintf(caption, sizeof(caption), "SESSION %u · %s–%s",
                 (unsigned)(i + 1), start_time, end_time);
        lv_area_t caption_area = {
            x1 + 3, bottom + 7, x2 - 3, bottom + 23,
        };
        history_ui_draw_text(draw_ctx, &caption_area, caption,
                             &somnotrace_ibm_plex_mono_semibold_11,
                             HISTORY_UI_COLOR_TERTIARY,
                             LV_TEXT_ALIGN_LEFT);
    }

    for (unsigned tick = 0; tick <= 4; ++tick) {
        int64_t timestamp = ui->overview.axis_start_ms +
                            ((ui->overview.axis_end_ms -
                              ui->overview.axis_start_ms) * tick) / 4;
        char clock[12];
        history_ui_format_clock(timestamp, false, clock, sizeof(clock));
        lv_coord_t x = left + (lv_coord_t)((right - left) * tick / 4U);
        lv_area_t clock_area = {
            x - 36, area.y2 - 18, x + 36, area.y2 - 1,
        };
        if (clock_area.x1 < left)
            clock_area.x1 = left;
        if (clock_area.x2 > right)
            clock_area.x2 = right;
        history_ui_draw_text(draw_ctx, &clock_area, clock,
                             &somnotrace_ibm_plex_mono_medium_11,
                             HISTORY_UI_COLOR_TERTIARY,
                             tick == 0 ? LV_TEXT_ALIGN_LEFT
                                       : (tick == 4 ? LV_TEXT_ALIGN_RIGHT
                                                    : LV_TEXT_ALIGN_CENTER));
    }

    if (!ui->cursor_valid || ui->cursor_ms < ui->overview.axis_start_ms ||
        ui->cursor_ms > ui->overview.axis_end_ms)
        return;
    lv_coord_t cursor_x = left + (lv_coord_t)(
        ((ui->cursor_ms - ui->overview.axis_start_ms) * (right - left)) /
        (ui->overview.axis_end_ms - ui->overview.axis_start_ms));
    history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_TEXT, LV_OPA_80, 1,
                         cursor_x, lane_y - 5, cursor_x, bottom);

    size_t nearest = 0;
    uint64_t nearest_distance = UINT64_MAX;
    for (size_t i = 0; i < ui->overview.point_count; ++i) {
        int64_t delta = ui->cursor_ms - ui->overview.timestamp_ms[i];
        uint64_t distance = delta < 0 ? (uint64_t)-delta : (uint64_t)delta;
        if (distance < nearest_distance) {
            nearest = i;
            nearest_distance = distance;
        }
    }
    char clock[16], value[24], upper[24], summary[64], tooltip[128];
    history_ui_format_clock(ui->cursor_ms, true, clock, sizeof(clock));
    bool value_available = history_ui_point_visible(ui, nearest);
    history_ui_format_x100(ui->overview.value_x100[nearest], value_available,
                           value, sizeof(value));
    bool cursor_envelope = value_available &&
                           ui->overview.aggregation ==
                               TOUCH_HISTORY_AGGREGATION_ENVELOPE &&
                           (ui->overview.flags[nearest] &
                            TOUCH_HISTORY_POINT_UPPER_VALID);
    if (cursor_envelope) {
        history_ui_format_x100(ui->overview.upper_x100[nearest], true,
                               upper, sizeof(upper));
        snprintf(summary, sizeof(summary), "Bin min/max %s/%s %s",
                 value, upper, s_signal_units[ui->signal]);
    } else {
        const char *summary_label = "Bin mean";
        if (ui->overview.aggregation == TOUCH_HISTORY_AGGREGATION_MINIMUM)
            summary_label = "Bin min";
        else if (ui->overview.aggregation == TOUCH_HISTORY_AGGREGATION_MAXIMUM)
            summary_label = "Bin max";
        else if (ui->overview.aggregation == TOUCH_HISTORY_AGGREGATION_ENVELOPE)
            summary_label = "Bin min/max";
        snprintf(summary, sizeof(summary), "%s %s %s",
                 summary_label, value, s_signal_units[ui->signal]);
    }
    const touch_history_event_t *cursor_event = history_ui_cursor_event(ui);
    snprintf(tooltip, sizeof(tooltip), "%s  %s%s%s%s",
             clock, ui->overview.preview ? "Preview " : "", summary,
             cursor_event ? "  Event " : "",
             cursor_event ? history_ui_event_code(cursor_event->type) : "");
    lv_coord_t tooltip_width = 360;
    lv_coord_t tooltip_x = cursor_x + 8;
    if (tooltip_x + tooltip_width > right)
        tooltip_x = cursor_x - tooltip_width - 8;
    if (tooltip_x < left)
        tooltip_x = left;
    lv_area_t tooltip_area = {
        tooltip_x, top + 5, tooltip_x + tooltip_width, top + 38,
    };
    history_ui_draw_rect(draw_ctx, &tooltip_area, HISTORY_UI_COLOR_PANEL,
                         LV_OPA_COVER, 7, HISTORY_UI_COLOR_BORDER, 1);
    lv_area_t tooltip_text_area = {
        tooltip_area.x1 + 8, tooltip_area.y1 + 7,
        tooltip_area.x2 - 8, tooltip_area.y2 - 4,
    };
    history_ui_draw_text(draw_ctx, &tooltip_text_area, tooltip,
                         &somnotrace_ibm_plex_mono_medium_11,
                         HISTORY_UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
}

static void history_ui_calendar_draw(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_obj_t *grid = lv_event_get_target(event);
    if (!ui || !draw_ctx || !grid || !ui->has_month)
        return;

    static const char *const weekday[] = {"M", "T", "W", "T", "F", "S", "S"};
    lv_area_t area;
    lv_obj_get_content_coords(grid, &area);
    lv_coord_t width = lv_area_get_width(&area);
    lv_coord_t cell_width = width / 7;
    for (unsigned col = 0; col < 7; ++col) {
        lv_area_t label_area = {
            area.x1 + (lv_coord_t)(col * cell_width), area.y1 + 4,
            area.x1 + (lv_coord_t)((col + 1) * cell_width) - 1, area.y1 + 25,
        };
        history_ui_draw_text(draw_ctx, &label_area, weekday[col],
                             &somnotrace_space_grotesk_semibold_13,
                             HISTORY_UI_COLOR_TERTIARY, LV_TEXT_ALIGN_CENTER);
    }

    unsigned selected_year = 0;
    unsigned selected_month = 0;
    unsigned selected_day = 0;
    bool selected_date_valid = ui->has_night && history_ui_parse_day(
        ui->night.day, &selected_year, &selected_month, &selected_day);
    int first = history_ui_calendar_first_column(
        ui->month.year, ui->month.month);
    int days = history_ui_days_in_month(ui->month.year, ui->month.month);
    for (int day = 1; day <= days; ++day) {
        int slot = first + day - 1;
        int row = slot / 7;
        int col = slot % 7;
        lv_coord_t x1 = area.x1 + col * cell_width + 3;
        lv_coord_t y1 = area.y1 + 30 + row * 44 + 2;
        lv_area_t cell = {x1, y1, x1 + cell_width - 7, y1 + 39};
        uint32_t bit = 1UL << (unsigned)(day - 1);
        bool therapy = (ui->month.therapy_days & bit) != 0;
        bool oximetry = (ui->month.oximetry_days & bit) != 0;
        bool has_data = therapy || oximetry;
        bool selected = selected_date_valid &&
                        selected_year == ui->month.year &&
                        selected_month == ui->month.month &&
                        selected_day == (unsigned)day;
        history_ui_draw_rect(draw_ctx, &cell,
                             selected ? 0xe9edf3
                                      : has_data ? HISTORY_UI_COLOR_ROW
                                      : HISTORY_UI_COLOR_BASE,
                             selected || has_data ? LV_OPA_COVER : LV_OPA_50,
                             8, HISTORY_UI_COLOR_BORDER,
                             selected ? 0 : (has_data ? 1 : 0));
        char number[4];
        snprintf(number, sizeof(number), "%d", day);
        lv_area_t number_area = {
            cell.x1, cell.y1 + 7, cell.x2, cell.y2 - 7,
        };
        history_ui_draw_text(draw_ctx, &number_area, number,
                             &somnotrace_space_grotesk_medium_15,
                             selected ? HISTORY_UI_COLOR_CARD
                                      : has_data ? HISTORY_UI_COLOR_TEXT
                                      : HISTORY_UI_COLOR_DISABLED,
                             LV_TEXT_ALIGN_CENTER);
        lv_coord_t dot_y = cell.y2 - 5;
        lv_coord_t dot_x = cell.x1 + (cell.x2 - cell.x1) / 2;
        if (therapy)
            history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_LIVE,
                                 LV_OPA_COVER, 4,
                                 dot_x - (oximetry ? 4 : 0), dot_y,
                                 dot_x - (oximetry ? 4 : 0), dot_y);
        if (oximetry)
            history_ui_draw_line(draw_ctx, HISTORY_UI_COLOR_O2,
                                 LV_OPA_COVER, 4,
                                 dot_x + (therapy ? 4 : 0), dot_y,
                                 dot_x + (therapy ? 4 : 0), dot_y);
    }
}

static void history_ui_state_primary_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    if (!ui)
        return;
    if (ui->state == TOUCH_HISTORY_UI_STATE_AUTO_LOADING)
        history_ui_cancel_pressed(event);
    else if (ui->state == TOUCH_HISTORY_UI_STATE_READ_ERROR)
        history_ui_retry_pressed(event);
}

static void history_ui_state_secondary_pressed(lv_event_t *event)
{
    touch_history_ui_t *ui = lv_event_get_user_data(event);
    if (ui && ui->state == TOUCH_HISTORY_UI_STATE_READ_ERROR)
        history_ui_open_card_pressed(event);
}

static lv_obj_t *history_ui_dot(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    if (!dot)
        return NULL;
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, history_ui_color(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

static void history_ui_discard_calendar(touch_history_ui_t *ui)
{
    if (!ui)
        return;
    if (ui->calendar_overlay)
        lv_obj_del(ui->calendar_overlay);
    ui->calendar_overlay = NULL;
    ui->calendar_title = NULL;
    ui->calendar_previous = NULL;
    ui->calendar_next = NULL;
    ui->calendar_grid = NULL;
    ui->calendar_status = NULL;
}

static esp_err_t history_ui_ensure_calendar(touch_history_ui_t *ui)
{
    if (!ui)
        return ESP_ERR_INVALID_ARG;
    if (ui->calendar_overlay)
        return ESP_OK;

    ui->calendar_overlay = history_ui_container(
        ui->left, 0, HISTORY_UI_CALENDAR_Y, TOUCH_HISTORY_UI_LIST_WIDTH,
        HISTORY_UI_CALENDAR_H, HISTORY_UI_COLOR_PANEL, 0);
    if (!ui->calendar_overlay)
        return ESP_ERR_NO_MEM;

    ui->calendar_title = history_ui_label(
        ui->calendar_overlay, "Recorded nights",
        &somnotrace_space_grotesk_semibold_15, HISTORY_UI_COLOR_TEXT,
        HISTORY_UI_HIT, 10,
        TOUCH_HISTORY_UI_LIST_WIDTH - 2 * HISTORY_UI_HIT, 24);
    if (!ui->calendar_title)
        goto no_memory;
    lv_obj_set_style_text_align(ui->calendar_title, LV_TEXT_ALIGN_CENTER, 0);

    ui->calendar_previous = history_ui_button(
        ui->calendar_overlay, 0, 0, HISTORY_UI_HIT, HISTORY_UI_HIT,
        HISTORY_UI_COLOR_CONTROL);
    if (!ui->calendar_previous)
        goto no_memory;
    lv_obj_set_style_radius(ui->calendar_previous, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *previous_label = history_ui_button_label(
        ui->calendar_previous, "<", &somnotrace_space_grotesk_semibold_19,
        HISTORY_UI_COLOR_TEXT);
    if (!previous_label)
        goto no_memory;
    lv_obj_add_event_cb(ui->calendar_previous,
                        history_ui_month_previous_pressed,
                        LV_EVENT_PRESSED, ui);

    ui->calendar_next = history_ui_button(
        ui->calendar_overlay, TOUCH_HISTORY_UI_LIST_WIDTH - HISTORY_UI_HIT,
        0, HISTORY_UI_HIT, HISTORY_UI_HIT,
        HISTORY_UI_COLOR_CONTROL);
    if (!ui->calendar_next)
        goto no_memory;
    lv_obj_set_style_radius(ui->calendar_next, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *next_label = history_ui_button_label(
        ui->calendar_next, ">", &somnotrace_space_grotesk_semibold_19,
        HISTORY_UI_COLOR_TEXT);
    if (!next_label)
        goto no_memory;
    lv_obj_add_event_cb(ui->calendar_next, history_ui_month_next_pressed,
                        LV_EVENT_PRESSED, ui);

    /* One touch/draw grid keeps 42 calendar cells from becoming 42 objects. */
    ui->calendar_grid = history_ui_container(
        ui->calendar_overlay, 0, HISTORY_UI_CALENDAR_GRID_Y,
        TOUCH_HISTORY_UI_LIST_WIDTH, HISTORY_UI_CALENDAR_GRID_H,
        HISTORY_UI_COLOR_CARD, 0);
    if (!ui->calendar_grid)
        goto no_memory;
    lv_obj_add_flag(ui->calendar_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->calendar_grid, history_ui_calendar_draw,
                        LV_EVENT_DRAW_MAIN, ui);
    lv_obj_add_event_cb(ui->calendar_grid, history_ui_calendar_grid_pressed,
                        LV_EVENT_PRESSED, ui);

    lv_obj_t *therapy_dot = history_ui_dot(
        ui->calendar_overlay, 16, 365, HISTORY_UI_COLOR_LIVE);
    lv_obj_t *therapy_label = history_ui_label(
        ui->calendar_overlay, "AirSense", &somnotrace_space_grotesk_medium_13,
        HISTORY_UI_COLOR_SECONDARY, 30, 358, 96, 22);
    lv_obj_t *oximetry_dot = history_ui_dot(
        ui->calendar_overlay, 150, 365, HISTORY_UI_COLOR_O2);
    lv_obj_t *oximetry_label = history_ui_label(
        ui->calendar_overlay, "O₂ Ring", &somnotrace_space_grotesk_medium_13,
        HISTORY_UI_COLOR_SECONDARY, 164, 358, 104, 22);
    if (!therapy_dot || !therapy_label || !oximetry_dot || !oximetry_label)
        goto no_memory;
    ui->calendar_status = history_ui_label(
        ui->calendar_overlay, "Could not read this month",
        &somnotrace_ibm_plex_mono_semibold_11, HISTORY_UI_COLOR_FAULT,
        10, 384, TOUCH_HISTORY_UI_LIST_WIDTH - 20, 18);
    if (!ui->calendar_status)
        goto no_memory;
    lv_obj_set_style_text_align(ui->calendar_status, LV_TEXT_ALIGN_CENTER, 0);
    history_ui_set_hidden(ui->calendar_status, true);
    history_ui_set_hidden(ui->calendar_overlay, true);
    return ESP_OK;

no_memory:
    history_ui_discard_calendar(ui);
    return ESP_ERR_NO_MEM;
}

static esp_err_t history_ui_build_objects(touch_history_ui_t *ui,
                                          lv_obj_t *parent)
{
    ui->root = history_ui_container(
        parent, 0, 0, TOUCH_HISTORY_UI_WIDTH, TOUCH_HISTORY_UI_HEIGHT,
        HISTORY_UI_COLOR_BASE, 0);
    if (!ui->root)
        return ESP_ERR_NO_MEM;

    ui->left = history_ui_container(
        ui->root, 0, 0, TOUCH_HISTORY_UI_LIST_WIDTH, TOUCH_HISTORY_UI_HEIGHT,
        HISTORY_UI_COLOR_PANEL, HISTORY_UI_COLOR_BORDER);
    ui->detail = history_ui_container(
        ui->root, TOUCH_HISTORY_UI_DETAIL_X, 0,
        TOUCH_HISTORY_UI_DETAIL_WIDTH, TOUCH_HISTORY_UI_HEIGHT,
        HISTORY_UI_COLOR_BASE, 0);

    ui->list_segment = history_ui_container(
        ui->left, 0, 0, TOUCH_HISTORY_UI_LIST_WIDTH, HISTORY_UI_HIT,
        HISTORY_UI_COLOR_CARD, HISTORY_UI_COLOR_BORDER);
    ui->list_segment_button = history_ui_button(
        ui->list_segment, 0, 0, TOUCH_HISTORY_UI_LIST_WIDTH / 2,
        HISTORY_UI_HIT, HISTORY_UI_COLOR_ROW_ACTIVE);
    ui->list_segment_label = history_ui_button_label(
        ui->list_segment_button, "List",
        &somnotrace_space_grotesk_semibold_13, HISTORY_UI_COLOR_CARD);
    lv_obj_add_event_cb(ui->list_segment_button,
                        history_ui_close_calendar_pressed,
                        LV_EVENT_PRESSED, ui);
    ui->calendar_button = history_ui_button(
        ui->list_segment, TOUCH_HISTORY_UI_LIST_WIDTH / 2, 0,
        TOUCH_HISTORY_UI_LIST_WIDTH / 2, HISTORY_UI_HIT,
        HISTORY_UI_COLOR_CARD);
    ui->calendar_button_label = history_ui_button_label(
        ui->calendar_button, "Calendar",
        &somnotrace_space_grotesk_semibold_13, HISTORY_UI_COLOR_SECONDARY);
    lv_obj_add_event_cb(ui->calendar_button,
                        history_ui_open_calendar_pressed,
                        LV_EVENT_PRESSED, ui);

    ui->list_title = history_ui_label(
        ui->left, "ALL RECORDED NIGHTS",
        &somnotrace_ibm_plex_mono_semibold_11,
        HISTORY_UI_COLOR_SECONDARY, 10, 53, 150, 34);
    lv_label_set_long_mode(ui->list_title, LV_LABEL_LONG_WRAP);
    ui->jump_to_date = history_ui_button(
        ui->left, 160, 44, 128, HISTORY_UI_HIT, HISTORY_UI_COLOR_PANEL);
    history_ui_button_label(ui->jump_to_date, "Jump to date",
                            &somnotrace_space_grotesk_semibold_13,
                            HISTORY_UI_COLOR_LIVE);
    lv_obj_add_event_cb(ui->jump_to_date,
                        history_ui_open_calendar_pressed,
                        LV_EVENT_PRESSED, ui);

    ui->list_viewport = history_ui_container(
        ui->left, 0, HISTORY_UI_LIST_VIEWPORT_Y,
        TOUCH_HISTORY_UI_LIST_WIDTH, HISTORY_UI_LIST_VIEWPORT_H,
        HISTORY_UI_COLOR_PANEL, 0);
    lv_obj_add_flag(ui->list_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui->list_viewport, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui->list_viewport, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(ui->list_viewport, history_ui_list_scroll_end,
                        LV_EVENT_SCROLL_END, ui);

    for (size_t i = 0; i < TOUCH_HISTORY_UI_LIST_ROWS; ++i) {
        history_ui_row_t *row = &ui->rows[i];
        lv_coord_t y = (lv_coord_t)i *
                       (HISTORY_UI_LIST_ROW_H + HISTORY_UI_ROW_GAP);
        row->button = history_ui_button(
            ui->list_viewport, 0, y, TOUCH_HISTORY_UI_LIST_WIDTH,
            HISTORY_UI_LIST_ROW_H,
            HISTORY_UI_COLOR_ROW);
        row->date = history_ui_label(
            row->button, HISTORY_UI_EM_DASH,
            &somnotrace_space_grotesk_semibold_15,
            HISTORY_UI_COLOR_TEXT, 13, 8, 205, 20);
        row->meta = history_ui_label(
            row->button, HISTORY_UI_EM_DASH,
            &somnotrace_ibm_plex_mono_medium_11,
            HISTORY_UI_COLOR_SECONDARY, 48, 35, 162, 16);
        row->usage = history_ui_label(
            row->button, HISTORY_UI_EM_DASH,
            &somnotrace_ibm_plex_mono_semibold_15,
            HISTORY_UI_COLOR_SECONDARY, 216, 17, 60, 22);
        lv_obj_set_style_text_align(row->usage, LV_TEXT_ALIGN_RIGHT, 0);
        row->therapy_dot = history_ui_dot(
            row->button, 20, 39, HISTORY_UI_COLOR_LIVE);
        row->o2_dot = history_ui_dot(
            row->button, 34, 39, HISTORY_UI_COLOR_O2);
        ui->row_bindings[i].ui = ui;
        ui->row_bindings[i].index = (uint8_t)i;
        lv_obj_add_event_cb(row->button, history_ui_row_pressed,
                            LV_EVENT_SHORT_CLICKED, &ui->row_bindings[i]);
    }

    ui->night_title = history_ui_label(
        ui->detail, "Select a recorded night",
        &somnotrace_space_grotesk_semibold_19, HISTORY_UI_COLOR_TEXT,
        58, 2, 244, 26);
    lv_label_set_long_mode(ui->night_title, LV_LABEL_LONG_CLIP);
    ui->night_subtitle = history_ui_label(
        ui->detail, "Newest completed night opens automatically",
        &somnotrace_ibm_plex_mono_medium_11, HISTORY_UI_COLOR_TERTIARY,
        58, 28, 244, 16);
    ui->target_badge = history_ui_container(
        ui->detail, 310, 4, 282, 36, 0x12393c, HISTORY_UI_COLOR_LIVE_DIM);
    ui->target_badge_label = history_ui_label(
        ui->target_badge, HISTORY_UI_EM_DASH,
        &somnotrace_space_grotesk_semibold_13, HISTORY_UI_COLOR_LIVE,
        10, 8, 262, 20);
    lv_obj_set_style_text_align(ui->target_badge_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    ui->night_previous = history_ui_button(
        ui->detail, 0, 0,
        HISTORY_UI_HIT, HISTORY_UI_HIT, HISTORY_UI_COLOR_CONTROL);
    history_ui_button_label(ui->night_previous, "<",
                            &somnotrace_space_grotesk_semibold_19,
                            HISTORY_UI_COLOR_TEXT);
    lv_obj_add_event_cb(ui->night_previous,
                        history_ui_previous_night_pressed,
                        LV_EVENT_PRESSED, ui);
    ui->night_next = history_ui_button(
        ui->detail, TOUCH_HISTORY_UI_DETAIL_WIDTH - HISTORY_UI_HIT, 0,
        HISTORY_UI_HIT, HISTORY_UI_HIT, HISTORY_UI_COLOR_CONTROL);
    history_ui_button_label(ui->night_next, ">",
                            &somnotrace_space_grotesk_semibold_19,
                            HISTORY_UI_COLOR_TEXT);
    lv_obj_add_event_cb(ui->night_next, history_ui_next_night_pressed,
                        LV_EVENT_PRESSED, ui);

    ui->summary = history_ui_container(
        ui->detail, 0, HISTORY_UI_SUMMARY_Y,
        TOUCH_HISTORY_UI_DETAIL_WIDTH, HISTORY_UI_SUMMARY_H,
        HISTORY_UI_COLOR_CARD, HISTORY_UI_COLOR_BORDER);
    static const char *const summary_names[] = {
        "Usage", "ST AHI", "Device AHI", "Recorded", "O₂ coverage",
    };
    for (size_t i = 0; i < 5; ++i) {
        lv_coord_t x = (lv_coord_t)(i * 138);
        ui->summary_labels[i] = history_ui_label(
            ui->summary, summary_names[i],
            &somnotrace_ibm_plex_mono_semibold_11,
            i == 2 ? HISTORY_UI_COLOR_TERTIARY
                   : HISTORY_UI_COLOR_SECONDARY,
            x + 10, 4, 124, 15);
        ui->summary_values[i] = history_ui_label(
            ui->summary, HISTORY_UI_EM_DASH,
            &somnotrace_ibm_plex_mono_semibold_15,
            i == 2 ? HISTORY_UI_COLOR_TERTIARY : HISTORY_UI_COLOR_TEXT,
            x + 10, 21, 124, 19);
    }

    const lv_coord_t channel_width =
        (TOUCH_HISTORY_UI_DETAIL_WIDTH - 12) / 4;
    for (size_t i = 0; i < TOUCH_HISTORY_UI_CHANNEL_CONTROLS; ++i) {
        lv_coord_t col = (lv_coord_t)(i % 4);
        lv_coord_t row = (lv_coord_t)(i / 4);
        lv_coord_t x = col * (channel_width + 4);
        lv_coord_t y = HISTORY_UI_CHANNEL_Y +
                       row * (HISTORY_UI_CHANNEL_H +
                              HISTORY_UI_CHANNEL_ROW_GAP);
        ui->channels[i].button = history_ui_button(
            ui->detail, x, y, channel_width, HISTORY_UI_CHANNEL_H,
            HISTORY_UI_COLOR_BASE);
        lv_obj_set_style_bg_opa(ui->channels[i].button,
                                LV_OPA_TRANSP, 0);
        ui->channels[i].pill = history_ui_container(
            ui->channels[i].button, 4, 6, channel_width - 8, 31,
            HISTORY_UI_COLOR_ROW, 0);
        lv_obj_set_style_radius(ui->channels[i].pill,
                                LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(ui->channels[i].pill, LV_OBJ_FLAG_CLICKABLE);
        ui->channels[i].label = history_ui_button_label(
            ui->channels[i].pill, s_signal_names[i],
            &somnotrace_space_grotesk_semibold_13,
            HISTORY_UI_COLOR_SECONDARY);
        ui->channel_bindings[i].ui = ui;
        ui->channel_bindings[i].index = (uint8_t)i;
        lv_obj_add_event_cb(ui->channels[i].button,
                            history_ui_channel_pressed, LV_EVENT_PRESSED,
                            &ui->channel_bindings[i]);
    }

    ui->graph = history_ui_container(
        ui->detail, 0, HISTORY_UI_GRAPH_Y,
        TOUCH_HISTORY_UI_DETAIL_WIDTH, HISTORY_UI_GRAPH_H,
        HISTORY_UI_COLOR_CARD, HISTORY_UI_COLOR_BORDER);
    lv_obj_add_flag(ui->graph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->graph, history_ui_graph_draw,
                        LV_EVENT_DRAW_MAIN, ui);
    lv_obj_add_event_cb(ui->graph, history_ui_graph_touch,
                        LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->graph, history_ui_graph_touch,
                        LV_EVENT_PRESSING, ui);
    lv_obj_add_event_cb(ui->graph, history_ui_graph_touch,
                        LV_EVENT_SHORT_CLICKED, ui);
    lv_obj_add_event_cb(ui->graph, history_ui_graph_touch,
                        LV_EVENT_RELEASED, ui);
    lv_obj_add_event_cb(ui->graph, history_ui_graph_touch,
                        LV_EVENT_PRESS_LOST, ui);
    ui->graph_title = history_ui_label(
        ui->graph, "Breathing / Flow · L/s",
        &somnotrace_space_grotesk_semibold_15, HISTORY_UI_COLOR_TEXT,
        12, 7, 132, 20);
    ui->graph_source = history_ui_label(
        ui->graph, "Full night · min/max envelope",
        &somnotrace_ibm_plex_mono_medium_11, HISTORY_UI_COLOR_TERTIARY,
        12, 29, 132, 16);

    for (size_t i = 0; i < TOUCH_HISTORY_UI_STAT_COUNT; ++i) {
        lv_coord_t x = 150 + (lv_coord_t)i * 68;
        ui->stat_labels[i] = history_ui_label(
            ui->graph, HISTORY_UI_EM_DASH,
            &somnotrace_ibm_plex_mono_semibold_11,
            HISTORY_UI_COLOR_TERTIARY, x, 5, 64, 16);
        ui->stat_values[i] = history_ui_label(
            ui->graph, HISTORY_UI_EM_DASH,
            &somnotrace_ibm_plex_mono_semibold_13,
            HISTORY_UI_COLOR_TEXT, x, 24, 64, 19);
    }
    ui->stats_warning = history_ui_label(
        ui->graph, "Percentiles unavailable",
        &somnotrace_ibm_plex_mono_semibold_11,
        HISTORY_UI_COLOR_AMBER, 150, 12, 268, 24);
    history_ui_set_hidden(ui->stats_warning, true);

    ui->therapy_only_button = history_ui_button(
        ui->graph, 422, 4, 106, HISTORY_UI_HIT, HISTORY_UI_COLOR_CONTROL);
    history_ui_button_label(ui->therapy_only_button, "Therapy: On",
                            &somnotrace_space_grotesk_semibold_13,
                            HISTORY_UI_COLOR_TEXT);
    lv_obj_add_event_cb(ui->therapy_only_button,
                        history_ui_therapy_only_pressed,
                        LV_EVENT_PRESSED, ui);
    history_ui_set_hidden(ui->therapy_only_button, true);

    ui->zoom_out_button = history_ui_button(
        ui->graph, 532, 4, HISTORY_UI_HIT, HISTORY_UI_HIT,
        HISTORY_UI_COLOR_CONTROL);
    history_ui_button_label(ui->zoom_out_button, "-",
                            &somnotrace_space_grotesk_semibold_19,
                            HISTORY_UI_COLOR_TEXT);
    lv_obj_add_event_cb(ui->zoom_out_button, history_ui_zoom_out_pressed,
                        LV_EVENT_PRESSED, ui);
    ui->zoom_in_button = history_ui_button(
        ui->graph, 580, 4, HISTORY_UI_HIT, HISTORY_UI_HIT,
        HISTORY_UI_COLOR_CONTROL);
    history_ui_button_label(ui->zoom_in_button, "+",
                            &somnotrace_space_grotesk_semibold_19,
                            HISTORY_UI_COLOR_TEXT);
    lv_obj_add_event_cb(ui->zoom_in_button, history_ui_zoom_in_pressed,
                        LV_EVENT_PRESSED, ui);
    ui->fit_button = history_ui_button(
        ui->graph, 628, 4, 64, HISTORY_UI_HIT, HISTORY_UI_COLOR_CONTROL);
    history_ui_button_label(ui->fit_button, "Fit",
                            &somnotrace_space_grotesk_semibold_13,
                            HISTORY_UI_COLOR_TEXT);
    lv_obj_add_event_cb(ui->fit_button, history_ui_fit_pressed,
                        LV_EVENT_PRESSED, ui);

    /* Compact status badge; never intercept graph gestures. */
    ui->zoom_overlay = history_ui_container(ui->graph, 410, 50, 126, 24,
        HISTORY_UI_COLOR_PANEL, HISTORY_UI_COLOR_BORDER);
    lv_obj_clear_flag(ui->zoom_overlay, LV_OBJ_FLAG_CLICKABLE);
    ui->zoom_overlay_text = history_ui_label(
        ui->zoom_overlay, "Updating…",
        &somnotrace_space_grotesk_semibold_13, HISTORY_UI_COLOR_TEXT,
        6, 3, 114, 18);
    lv_obj_clear_flag(ui->zoom_overlay_text, LV_OBJ_FLAG_CLICKABLE);
    history_ui_set_hidden(ui->zoom_overlay, true);

    ui->degraded_banner = history_ui_container(
        ui->graph, HISTORY_UI_GRAPH_PAD_L, HISTORY_UI_GRAPH_PAD_T,
        TOUCH_HISTORY_UI_DETAIL_WIDTH - HISTORY_UI_GRAPH_PAD_L -
            HISTORY_UI_GRAPH_PAD_R,
        36, 0x493817, HISTORY_UI_COLOR_AMBER);
    ui->degraded_label = history_ui_label(
        ui->degraded_banner, "Some history details are unavailable",
        &somnotrace_space_grotesk_semibold_13, HISTORY_UI_COLOR_AMBER,
        10, 8, 616, 20);
    history_ui_set_hidden(ui->degraded_banner, true);

    ui->marker_legend = history_ui_label(
        ui->detail, "Markers: OA · CA · H · A · RERA",
        &somnotrace_ibm_plex_mono_medium_11,
        HISTORY_UI_COLOR_TERTIARY, 2, 430, 300, 18);
    ui->safety_footer = history_ui_label(
        ui->detail, "Trend review only. Not a diagnosis or a prescription.",
        &somnotrace_space_grotesk_medium_13,
        HISTORY_UI_COLOR_TERTIARY, 306, 428, 384, 20);
    lv_obj_set_style_text_align(ui->safety_footer, LV_TEXT_ALIGN_RIGHT, 0);

    ui->state_overlay = history_ui_container(
        ui->detail, 0, 0, TOUCH_HISTORY_UI_DETAIL_WIDTH,
        TOUCH_HISTORY_UI_HEIGHT, HISTORY_UI_COLOR_CARD,
        HISTORY_UI_COLOR_BORDER);
    ui->state_icon = history_ui_label(
        ui->state_overlay, "...",
        &somnotrace_ibm_plex_mono_semibold_29, HISTORY_UI_COLOR_LIVE,
        286, 82, 120, 42);
    lv_obj_set_style_text_align(ui->state_icon, LV_TEXT_ALIGN_CENTER, 0);
    ui->state_title = history_ui_label(
        ui->state_overlay, "Loading night",
        &somnotrace_space_grotesk_semibold_23, HISTORY_UI_COLOR_TEXT,
        96, 132, 500, 34);
    lv_obj_set_style_text_align(ui->state_title, LV_TEXT_ALIGN_CENTER, 0);
    ui->state_body = history_ui_label(
        ui->state_overlay, "Combining sessions and O₂ data",
        &somnotrace_space_grotesk_medium_15,
        HISTORY_UI_COLOR_SECONDARY, 96, 174, 500, 52);
    lv_obj_set_style_text_align(ui->state_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui->state_body, LV_LABEL_LONG_WRAP);
    ui->state_progress = lv_bar_create(ui->state_overlay);
    lv_obj_set_pos(ui->state_progress, 146, 238);
    lv_obj_set_size(ui->state_progress, 400, 8);
    lv_obj_set_style_bg_color(ui->state_progress,
                              history_ui_color(HISTORY_UI_COLOR_CONTROL),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->state_progress,
                              history_ui_color(HISTORY_UI_COLOR_LIVE),
                              LV_PART_INDICATOR);
    lv_bar_set_range(ui->state_progress, 0, 1000);
    ui->state_primary = history_ui_button(
        ui->state_overlay, 230, 278, 232, HISTORY_UI_HIT,
        HISTORY_UI_COLOR_CONTROL);
    ui->state_primary_label = history_ui_button_label(
        ui->state_primary, "Cancel",
        &somnotrace_space_grotesk_semibold_15, HISTORY_UI_COLOR_TEXT);
    lv_obj_add_event_cb(ui->state_primary, history_ui_state_primary_pressed,
                        LV_EVENT_PRESSED, ui);
    ui->state_secondary = history_ui_button(
        ui->state_overlay, 230, 330, 232, HISTORY_UI_HIT,
        HISTORY_UI_COLOR_CONTROL);
    ui->state_secondary_label = history_ui_button_label(
        ui->state_secondary, "Card status",
        &somnotrace_space_grotesk_semibold_15, HISTORY_UI_COLOR_TEXT);
    lv_obj_add_event_cb(ui->state_secondary,
                        history_ui_state_secondary_pressed,
                        LV_EVENT_PRESSED, ui);
    history_ui_set_hidden(ui->state_overlay, true);
    return ESP_OK;
}

esp_err_t touch_history_ui_create(lv_obj_t *parent,
                                  const touch_history_ui_config_t *config,
                                  touch_history_ui_t **out_ui)
{
    if (!parent || !out_ui)
        return ESP_ERR_INVALID_ARG;
    *out_ui = NULL;

    touch_history_ui_t *ui = heap_caps_calloc(
        1, sizeof(*ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ui)
        return ESP_ERR_NO_MEM;
    if (config)
        ui->config = *config;
    ui->selected_row = SIZE_MAX;
    ui->signal = TOUCH_HISTORY_SIGNAL_FLOW;
    ui->state = TOUCH_HISTORY_UI_STATE_EMPTY;

    esp_err_t result = history_ui_build_objects(ui, parent);
    if (result != ESP_OK) {
        touch_history_ui_destroy(ui);
        return result;
    }
    *out_ui = ui;
    return ESP_OK;
}

void touch_history_ui_destroy(touch_history_ui_t *ui)
{
    if (!ui)
        return;
    if (ui->root && lv_obj_is_valid(ui->root))
        lv_obj_del(ui->root);
    ui->root = NULL;
    heap_caps_free(ui);
}

lv_obj_t *touch_history_ui_root(touch_history_ui_t *ui)
{
    return ui ? ui->root : NULL;
}

static const char *history_ui_default_stat_label(touch_history_signal_t signal,
                                                 size_t index)
{
    static const char *const labels[TOUCH_HISTORY_SIGNAL_COUNT][4] = {
        {"P50 |Flow|", "P95 |Flow|", "P99.5 |Flow|", ""},
        {"P50", "P95", "P99.5", ""},
        {"P50", "P95", "P99.5", ""},
        {"P50", "P95", "P99.5", ""},
        {"P50", "P95", "P99.5", ""},
        {"Minimum", "P5", "P0.5", "Time <88%"},
        {"Minimum", "Median", "Maximum", ""},
        {"Active", "Mean", "Peak", ""},
    };
    return signal < TOUCH_HISTORY_SIGNAL_COUNT &&
                   index < TOUCH_HISTORY_UI_STAT_COUNT
               ? labels[signal][index]
               : HISTORY_UI_EM_DASH;
}

static void history_ui_update_list(touch_history_ui_t *ui)
{
    bool calendar = ui->rail_mode == TOUCH_HISTORY_UI_RAIL_CALENDAR;
    history_ui_set_hidden(ui->list_title, calendar);
    history_ui_set_hidden(ui->jump_to_date, calendar);
    history_ui_set_hidden(ui->list_viewport, calendar);
    lv_obj_set_style_bg_color(
        ui->list_segment_button,
        history_ui_color(calendar ? HISTORY_UI_COLOR_CARD : 0xe9edf3), 0);
    lv_obj_set_style_bg_color(
        ui->calendar_button,
        history_ui_color(calendar ? 0xe9edf3 : HISTORY_UI_COLOR_CARD), 0);
    lv_obj_set_style_text_color(
        ui->list_segment_label,
        history_ui_color(calendar ? HISTORY_UI_COLOR_SECONDARY
                                  : HISTORY_UI_COLOR_CARD), 0);
    lv_obj_set_style_text_color(
        ui->calendar_button_label,
        history_ui_color(calendar ? HISTORY_UI_COLOR_CARD
                                  : HISTORY_UI_COLOR_SECONDARY), 0);

    /* The Calendar owns this rail below the persistent segment. Keep the
     * recycled List tree dormant until List is selected again; rewriting its
     * hidden labels on graph/loading revisions only burns LVGL time and heap. */
    if (calendar)
        return;

    for (size_t i = 0; i < TOUCH_HISTORY_UI_LIST_ROWS; ++i) {
        history_ui_row_t *row = &ui->rows[i];
        bool present = i < ui->day_count;
        history_ui_set_hidden(row->button, !present);
        if (!present)
            continue;

        const touch_history_day_t *day = &ui->days[i];
        char date[32], usage[24], meta[64];
        history_ui_format_day_short(day->day, date, sizeof(date));
        history_ui_format_clock_minutes(day->usage_min, day->has_usage,
                                        usage, sizeof(usage));
        if (day->sessions > 0)
            snprintf(meta, sizeof(meta), "%d session%s",
                     day->sessions, day->sessions == 1 ? "" : "s");
        else
            snprintf(meta, sizeof(meta), "sessions %s", HISTORY_UI_EM_DASH);
        lv_label_set_text(row->date, date);
        lv_label_set_text(row->meta, meta);
        lv_label_set_text(row->usage, usage);
        history_ui_set_hidden(row->therapy_dot, !day->has_therapy);
        history_ui_set_hidden(row->o2_dot, !day->has_oximetry);
        bool selected = i == ui->selected_row;
        lv_obj_set_style_bg_color(
            row->button,
            history_ui_color(selected ? 0xe9edf3
                                      : HISTORY_UI_COLOR_ROW),
            0);
        lv_obj_set_style_border_width(row->button, 0, 0);
        lv_obj_set_style_text_color(
            row->date,
            history_ui_color(selected ? HISTORY_UI_COLOR_CARD
                                      : HISTORY_UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_color(
            row->meta,
            history_ui_color(selected ? HISTORY_UI_COLOR_CONTROL
                                      : HISTORY_UI_COLOR_SECONDARY), 0);
        lv_obj_set_style_text_color(
            row->usage,
            history_ui_color(selected ? HISTORY_UI_COLOR_CONTROL
                                      : HISTORY_UI_COLOR_SECONDARY), 0);
    }

    char count[48];
    snprintf(count, sizeof(count), "ALL %u RECORDED\nNIGHTS",
             (unsigned)ui->page.total_days);
    lv_label_set_text(ui->list_title, count);
    history_ui_set_enabled(ui->jump_to_date, ui->page.total_days > 0);
}

static void history_ui_update_header(touch_history_ui_t *ui)
{
    const touch_history_day_t *day = history_ui_selected_day(ui);
    char title[40];
    const char *selected_day = ui->has_night
                                   ? ui->night.day
                                   : (day ? day->day : NULL);
    if (selected_day) {
        history_ui_format_day(selected_day, title, sizeof(title));
        lv_point_t title_size = {0};
        lv_txt_get_size(&title_size, title,
                        &somnotrace_space_grotesk_semibold_19,
                        0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (title_size.x > lv_obj_get_content_width(ui->night_title))
            history_ui_format_day_compact(selected_day, title,
                                          sizeof(title));
    } else {
        history_ui_copy_text(title, sizeof(title), "Select a recorded night");
    }
    lv_label_set_text(ui->night_title, title);

    char subtitle[96];
    if (ui->has_night) {
        char start[12], end[12];
        history_ui_format_clock(ui->night.axis_start_ms, false,
                                start, sizeof(start));
        history_ui_format_clock(ui->night.axis_end_ms, false,
                                end, sizeof(end));
        snprintf(subtitle, sizeof(subtitle), "Review window %s–%s", start, end);
    } else {
        history_ui_copy_text(subtitle, sizeof(subtitle),
                             "Newest completed night opens automatically");
    }
    lv_label_set_text(ui->night_subtitle, subtitle);

    char usage[56];
    if (day && day->has_usage) {
        if (ui->usage_target_known)
            snprintf(usage, sizeof(usage), "%d h %02d m · %s",
                     day->usage_min / 60, day->usage_min % 60,
                     ui->usage_on_target ? "≥4 h adherence"
                                         : "<4 h adherence");
        else
            snprintf(usage, sizeof(usage), "%d h %02d m",
                     day->usage_min / 60, day->usage_min % 60);
        lv_label_set_text(ui->target_badge_label, usage);
        history_ui_set_hidden(ui->target_badge, false);
        uint32_t badge_color =
            ui->usage_target_known && !ui->usage_on_target
                ? HISTORY_UI_COLOR_AMBER : HISTORY_UI_COLOR_LIVE;
        lv_obj_set_style_text_color(
            ui->target_badge_label, history_ui_color(badge_color), 0);
        lv_obj_set_style_border_color(
            ui->target_badge, history_ui_color(badge_color), 0);
    } else {
        history_ui_set_hidden(ui->target_badge, true);
    }
    history_ui_set_enabled(ui->night_previous, ui->can_previous_night);
    history_ui_set_enabled(ui->night_next, ui->can_next_night);
}

static void history_ui_update_summary(touch_history_ui_t *ui)
{
    const touch_history_day_t *day = history_ui_selected_day(ui);
    char values[5][32];
    if (day && day->has_usage)
        snprintf(values[0], sizeof(values[0]), "%.1f h",
                 (double)day->usage_min / 60.0);
    else
        history_ui_copy_text(values[0], sizeof(values[0]),
                             HISTORY_UI_EM_DASH);
    bool has_st_ahi = (ui->has_night && ui->night.has_st_ahi) ||
                      (day && day->has_st_ahi);
    float st_ahi = ui->has_night ? ui->night.st_ahi
                                 : (day ? day->st_ahi : 0.0f);
    if (has_st_ahi)
        snprintf(values[1], sizeof(values[1]), "%.1f /h", (double)st_ahi);
    else
        history_ui_copy_text(values[1], sizeof(values[1]),
                             HISTORY_UI_EM_DASH);
    bool has_device_ahi =
        (ui->has_night && ui->night.has_device_ahi) ||
        (day && day->has_device_ahi);
    float device_ahi = ui->has_night ? ui->night.device_ahi
                                     : (day ? day->device_ahi : 0.0f);
    if (has_device_ahi)
        snprintf(values[2], sizeof(values[2]), "%.1f /h",
                 (double)device_ahi);
    else
        history_ui_copy_text(values[2], sizeof(values[2]),
                             HISTORY_UI_EM_DASH);
    if (ui->has_night)
        snprintf(values[3], sizeof(values[3]), "%u session%s",
                 (unsigned)ui->night.session_count,
                 ui->night.session_count == 1 ? "" : "s");
    else
        history_ui_copy_text(values[3], sizeof(values[3]),
                             HISTORY_UI_EM_DASH);
    if (ui->has_night && ui->night.has_o2_coverage &&
        day && day->has_usage) {
        int coverage_minutes =
            (int)(((int64_t)day->usage_min *
                   ui->night.o2_coverage_per_mille + 500) / 1000);
        history_ui_format_clock_minutes(coverage_minutes, true,
                                        values[4], sizeof(values[4]));
    } else {
        history_ui_copy_text(values[4], sizeof(values[4]),
                             HISTORY_UI_EM_DASH);
    }
    for (size_t i = 0; i < 5; ++i)
        lv_label_set_text(ui->summary_values[i], values[i]);
}

static bool history_ui_signal_available(const touch_history_ui_t *ui,
                                        touch_history_signal_t signal)
{
    if (!ui->has_night)
        return false;
    return (ui->night.available_signals &
            TOUCH_HISTORY_SIGNAL_BIT(signal)) != 0;
}

static void history_ui_update_channels(touch_history_ui_t *ui)
{
    for (size_t i = 0; i < TOUCH_HISTORY_UI_CHANNEL_CONTROLS; ++i) {
        bool selected = i == (size_t)ui->signal;
        bool available = history_ui_signal_available(
            ui, (touch_history_signal_t)i);
        lv_obj_set_style_bg_color(
            ui->channels[i].pill,
            history_ui_color(selected ? HISTORY_UI_COLOR_LIVE_DIM
                                      : HISTORY_UI_COLOR_ROW),
            0);
        lv_obj_set_style_text_color(
            ui->channels[i].label,
            history_ui_color(selected ? HISTORY_UI_COLOR_TEXT
                                      : HISTORY_UI_COLOR_SECONDARY),
            0);
        history_ui_set_enabled(ui->channels[i].button, available);
    }
}

static void history_ui_update_graph_header(touch_history_ui_t *ui)
{
    char graph_title[48];
    snprintf(graph_title, sizeof(graph_title), "%s · %s",
             s_signal_names[ui->signal], s_signal_units[ui->signal]);
    lv_label_set_text(ui->graph_title, graph_title);

    /* The accepted Flow identity is intentionally longer than the other
     * channel names. Flow has three stats, so use its otherwise-empty fourth
     * stat column to keep the complete title readable without adding objects. */
    bool breathing_flow = ui->signal == TOUCH_HISTORY_SIGNAL_FLOW;
    lv_obj_set_width(ui->graph_title, breathing_flow ? 200 : 132);
    lv_obj_set_width(ui->graph_source, breathing_flow ? 200 : 132);
    for (size_t i = 0; i < TOUCH_HISTORY_UI_STAT_COUNT; ++i) {
        lv_coord_t stat_x = (breathing_flow ? 218 : 150) +
                            (lv_coord_t)i * 68;
        lv_obj_set_x(ui->stat_labels[i], stat_x);
        lv_obj_set_x(ui->stat_values[i], stat_x);
    }
    lv_obj_set_x(ui->stats_warning, breathing_flow ? 218 : 150);
    lv_obj_set_width(ui->stats_warning, breathing_flow ? 306 : 268);

    char source[96];
    if (!ui->has_overview) {
        history_ui_copy_text(source, sizeof(source), "Not loaded");
    } else {
        uint64_t duration = ui->overview.axis_end_ms >
                                    ui->overview.axis_start_ms
                                ? (uint64_t)(ui->overview.axis_end_ms -
                                             ui->overview.axis_start_ms)
                                : 0;
        unsigned minutes = (unsigned)((duration + 30000ULL) / 60000ULL);
        if (ui->overview.preview) {
            snprintf(source, sizeof(source), "%um · preview", minutes);
        } else if (ui->signal == TOUCH_HISTORY_SIGNAL_FLOW &&
                   ui->overview.source_raw) {
            snprintf(source, sizeof(source),
                     "%um · raw 25 Hz", minutes);
        } else if (ui->signal == TOUCH_HISTORY_SIGNAL_FLOW &&
                   ui->overview.source_fallback) {
            snprintf(source, sizeof(source),
                     "%um · 1 Hz fallback", minutes);
        } else if (ui->signal == TOUCH_HISTORY_SIGNAL_FLOW &&
                   ui->overview.aggregation ==
                       TOUCH_HISTORY_AGGREGATION_ENVELOPE) {
            if (ui->has_night && ui->overview.axis_start_ms == ui->night.axis_start_ms &&
                ui->overview.axis_end_ms == ui->night.axis_end_ms)
                history_ui_copy_text(source, sizeof(source), "Full night · envelope");
            else
                snprintf(source, sizeof(source), "%um · envelope", minutes);
        } else if (ui->signal == TOUCH_HISTORY_SIGNAL_PRESSURE &&
                   ui->overview.has_companion) {
            history_ui_copy_text(source, sizeof(source),
                                 "Pressure + EPR");
        } else {
            snprintf(source, sizeof(source), "%um window", minutes);
        }
    }
    lv_label_set_text(ui->graph_source, source);

    bool o2_filter = ui->signal == TOUCH_HISTORY_SIGNAL_SPO2;
    history_ui_set_hidden(ui->therapy_only_button, !o2_filter);
    lv_obj_t *therapy_label = o2_filter
                                  ? lv_obj_get_child(ui->therapy_only_button, 0)
                                  : NULL;
    if (therapy_label)
        lv_label_set_text(therapy_label,
                          ui->therapy_only ? "Therapy: On"
                                           : "Therapy: Off");

    bool stats_warning = ui->stats_warning_text[0] != '\0';
    history_ui_set_hidden(ui->stats_warning, !stats_warning);
    if (stats_warning)
        lv_label_set_text(ui->stats_warning, ui->stats_warning_text);
    for (size_t i = 0; i < TOUCH_HISTORY_UI_STAT_COUNT; ++i) {
        bool fourth_visible =
            i < 3 || ui->signal == TOUCH_HISTORY_SIGNAL_SPO2;
        history_ui_set_hidden(ui->stat_labels[i],
                              stats_warning || !fourth_visible);
        history_ui_set_hidden(ui->stat_values[i],
                              stats_warning || !fourth_visible);
        lv_label_set_text(ui->stat_labels[i],
                          ui->stats[i].label[0]
                              ? ui->stats[i].label
                              : history_ui_default_stat_label(ui->signal, i));
        char value[40], numeric[24];
        history_ui_format_x100(ui->stats[i].value_x100,
                               ui->stats[i].available,
                               numeric, sizeof(numeric));
        if (ui->stats[i].available && ui->stats[i].unit[0])
            snprintf(value, sizeof(value), "%s %s",
                     numeric, ui->stats[i].unit);
        else if (ui->stats[i].available)
            history_ui_copy_text(value, sizeof(value), numeric);
        else
            history_ui_copy_text(value, sizeof(value), HISTORY_UI_EM_DASH);
        lv_label_set_text(ui->stat_values[i], value);
    }

    bool graph_ready = ui->has_overview;
    history_ui_set_enabled(ui->fit_button, graph_ready);
    history_ui_set_enabled(ui->zoom_out_button, graph_ready);
    history_ui_set_enabled(ui->zoom_in_button, graph_ready);
}

static void history_ui_update_event_status(touch_history_ui_t *ui)
{
    const char *text = "Event data unavailable";
    uint32_t color = HISTORY_UI_COLOR_AMBER;

    if (ui->event_state == TOUCH_HISTORY_UI_EVENT_STATE_INCOMPLETE) {
        text = ui->events_truncated
            ? "Event data incomplete · view truncated"
            : "Event data incomplete · markers missing";
    } else if (ui->event_state == TOUCH_HISTORY_UI_EVENT_STATE_COMPLETE) {
        color = HISTORY_UI_COLOR_TERTIARY;
        if (ui->events_truncated) {
            text = "Event markers truncated · zoom in";
            color = HISTORY_UI_COLOR_AMBER;
        } else if (ui->event_total_count == 0) {
            text = "No OA/CA/H/A/RERA events recorded";
        } else if (ui->event_count == 0) {
            text = "No respiratory events in this window";
        } else {
            text = "Markers: OA · CA · H · A · RERA";
        }
    }

    lv_label_set_text(ui->marker_legend, text);
    lv_obj_set_style_text_color(ui->marker_legend,
                                history_ui_color(color), 0);
}

static void history_ui_update_state(touch_history_ui_t *ui)
{
    bool full_overlay = ui->state == TOUCH_HISTORY_UI_STATE_EMPTY ||
                        ui->state == TOUCH_HISTORY_UI_STATE_AUTO_LOADING ||
                        ui->state == TOUCH_HISTORY_UI_STATE_READ_ERROR;
    history_ui_set_hidden(ui->state_overlay, !full_overlay);
    history_ui_set_hidden(ui->zoom_overlay,
                          ui->state != TOUCH_HISTORY_UI_STATE_ZOOM_LOADING);
    history_ui_set_hidden(
        ui->degraded_banner,
        ui->state != TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN);

    if (ui->state == TOUCH_HISTORY_UI_STATE_ZOOM_LOADING) {
        lv_label_set_text(
            ui->zoom_overlay_text,
            ui->status_text[0] ? ui->status_text
                               : "Updating…");
        /* Intentionally no cancel control exists in this overlay. */
    } else if (ui->state == TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN) {
        lv_label_set_text(
            ui->degraded_label,
            ui->degraded_text[0] ? ui->degraded_text
                                 : "Some history details are unavailable");
    }

    if (!full_overlay)
        return;
    if (ui->state == TOUCH_HISTORY_UI_STATE_AUTO_LOADING) {
        lv_label_set_text(ui->state_icon, "...");
        lv_obj_set_style_text_color(
            ui->state_icon, history_ui_color(HISTORY_UI_COLOR_LIVE), 0);
        lv_label_set_text(ui->state_title, "Loading recorded night");
        lv_label_set_text(
            ui->state_body,
            ui->status_text[0] ? ui->status_text
                               : "Combining all completed sessions and O₂ data");
        history_ui_set_hidden(ui->state_progress, false);
        lv_bar_set_value(ui->state_progress, ui->progress_per_mille,
                         LV_ANIM_OFF);
        history_ui_set_hidden(ui->state_primary, false);
        lv_label_set_text(ui->state_primary_label, "Cancel");
        history_ui_set_hidden(ui->state_secondary, true);
    } else if (ui->state == TOUCH_HISTORY_UI_STATE_READ_ERROR) {
        lv_label_set_text(ui->state_icon, "!");
        lv_obj_set_style_text_color(
            ui->state_icon, history_ui_color(HISTORY_UI_COLOR_FAULT), 0);
        lv_label_set_text(ui->state_title, "Could not read the card");
        char body[220];
        snprintf(body, sizeof(body), "%s\nLive therapy is unaffected.",
                 ui->error_text[0]
                     ? ui->error_text
                     : "History data did not respond.");
        lv_label_set_text(ui->state_body, body);
        history_ui_set_hidden(ui->state_progress, true);
        history_ui_set_hidden(ui->state_primary, false);
        lv_label_set_text(ui->state_primary_label, "Retry");
        history_ui_set_hidden(ui->state_secondary, false);
        lv_label_set_text(ui->state_secondary_label, "Card status");
    } else {
        lv_label_set_text(ui->state_icon, HISTORY_UI_EM_DASH);
        lv_obj_set_style_text_color(
            ui->state_icon, history_ui_color(HISTORY_UI_COLOR_TERTIARY), 0);
        lv_label_set_text(ui->state_title, "No recorded nights yet");
        lv_label_set_text(
            ui->state_body,
            "Completed nights will appear here automatically.");
        history_ui_set_hidden(ui->state_progress, true);
        history_ui_set_hidden(ui->state_primary, true);
        history_ui_set_hidden(ui->state_secondary, true);
    }
}

static void history_ui_update_calendar(touch_history_ui_t *ui,
                                       bool grid_content_changed)
{
    if (!ui->calendar_overlay)
        return;
    history_ui_set_hidden(ui->calendar_overlay,
                          ui->rail_mode != TOUCH_HISTORY_UI_RAIL_CALENDAR);
    if (ui->rail_mode != TOUCH_HISTORY_UI_RAIL_CALENDAR)
        return;
    char title[48];
    static const char *const months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };
    if (ui->has_month && ui->month.month >= 1 && ui->month.month <= 12)
        snprintf(title, sizeof(title), "%s %u",
                 months[ui->month.month - 1], (unsigned)ui->month.year);
    else
        history_ui_copy_text(title, sizeof(title), "Recorded nights");
    history_ui_set_label_text_if_changed(ui->calendar_title, title);
    history_ui_set_enabled(ui->calendar_previous,
                           ui->can_previous_month && !ui->calendar_loading);
    history_ui_set_enabled(ui->calendar_next,
                           ui->can_next_month && !ui->calendar_loading);
    if (ui->calendar_loading) {
        history_ui_set_label_text_if_changed(
            ui->calendar_status, "Loading month…");
        lv_obj_set_style_text_color(
            ui->calendar_status, history_ui_color(HISTORY_UI_COLOR_LIVE), 0);
    } else if (ui->calendar_read_error) {
        history_ui_set_label_text_if_changed(
            ui->calendar_status, "Could not read this month");
        lv_obj_set_style_text_color(
            ui->calendar_status, history_ui_color(HISTORY_UI_COLOR_FAULT), 0);
    }
    history_ui_set_hidden(
        ui->calendar_status,
        !ui->calendar_loading && !ui->calendar_read_error);
    if (grid_content_changed)
        lv_obj_invalidate(ui->calendar_grid);
}

static esp_err_t history_ui_validate_snapshot(
    const touch_history_ui_snapshot_t *snapshot)
{
    if (!snapshot || snapshot->state > TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN ||
        snapshot->rail_mode > TOUCH_HISTORY_UI_RAIL_CALENDAR ||
        snapshot->day_count > TOUCH_HISTORY_UI_LIST_ROWS ||
        snapshot->session_count > TOUCH_HISTORY_UI_MAX_SESSIONS ||
        snapshot->event_count > TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS ||
        snapshot->event_state > TOUCH_HISTORY_UI_EVENT_STATE_INCOMPLETE ||
        snapshot->event_total_count < snapshot->event_count ||
        snapshot->selected_signal >= TOUCH_HISTORY_SIGNAL_COUNT ||
        snapshot->progress_per_mille > 1000)
        return ESP_ERR_INVALID_ARG;
    if ((snapshot->day_count && !snapshot->days) ||
        (snapshot->session_count && !snapshot->sessions) ||
        (snapshot->event_count && !snapshot->events) ||
        (snapshot->event_state == TOUCH_HISTORY_UI_EVENT_STATE_UNAVAILABLE &&
         (snapshot->event_count || snapshot->event_total_count ||
          snapshot->events_truncated)) ||
        (snapshot->selected_row != SIZE_MAX &&
         snapshot->selected_row >= snapshot->day_count))
        return ESP_ERR_INVALID_ARG;
    if (snapshot->overview &&
        (snapshot->overview->point_count > TOUCH_HISTORY_OVERVIEW_POINTS ||
         snapshot->overview->signal >= TOUCH_HISTORY_SIGNAL_COUNT ||
         snapshot->overview->signal != snapshot->selected_signal ||
         (snapshot->overview->has_data &&
          snapshot->overview->axis_end_ms <=
              snapshot->overview->axis_start_ms)))
        return ESP_ERR_INVALID_ARG;
    if (snapshot->month &&
        (snapshot->month->year > 9999 ||
         snapshot->month->month < 1 || snapshot->month->month > 12))
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static bool history_ui_graph_content_changed(
    const touch_history_ui_t *ui,
    const touch_history_ui_snapshot_t *snapshot)
{
    bool next_has_overview = snapshot->overview != NULL;
    if (ui->has_overview != next_has_overview ||
        ui->signal != snapshot->selected_signal ||
        ui->therapy_only != snapshot->therapy_only ||
        ui->cursor_valid != snapshot->cursor_valid ||
        (ui->cursor_valid && ui->cursor_ms != snapshot->cursor_ms) ||
        ui->session_count != snapshot->session_count ||
        ui->event_count != snapshot->event_count ||
        (ui->state == TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN) !=
            (snapshot->state == TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN))
        return true;
    if (next_has_overview &&
        memcmp(&ui->overview, snapshot->overview,
               sizeof(ui->overview)) != 0)
        return true;
    if (snapshot->session_count &&
        memcmp(ui->sessions, snapshot->sessions,
               snapshot->session_count * sizeof(ui->sessions[0])) != 0)
        return true;
    if (snapshot->event_count &&
        memcmp(ui->events, snapshot->events,
               snapshot->event_count * sizeof(ui->events[0])) != 0)
        return true;
    return false;
}

static bool history_ui_calendar_grid_content_changed(
    const touch_history_ui_t *ui,
    const touch_history_ui_snapshot_t *snapshot)
{
    if (!ui->calendar_overlay || ui->rail_mode != snapshot->rail_mode)
        return true;
    bool next_has_month = snapshot->month != NULL;
    if (ui->has_month != next_has_month)
        return true;
    if (next_has_month &&
        (ui->month.year != snapshot->month->year ||
         ui->month.month != snapshot->month->month ||
         ui->month.days_in_month != snapshot->month->days_in_month ||
         ui->month.therapy_days != snapshot->month->therapy_days ||
         ui->month.oximetry_days != snapshot->month->oximetry_days))
        return true;
    bool next_has_night = snapshot->night != NULL;
    if (ui->has_night != next_has_night)
        return true;
    return next_has_night && strcmp(ui->night.day, snapshot->night->day) != 0;
}

esp_err_t touch_history_ui_apply(touch_history_ui_t *ui,
                                 const touch_history_ui_snapshot_t *snapshot)
{
    if (!ui)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = history_ui_validate_snapshot(snapshot);
    if (result != ESP_OK)
        return result;
    bool graph_content_changed =
        history_ui_graph_content_changed(ui, snapshot);
    bool calendar_grid_content_changed =
        history_ui_calendar_grid_content_changed(ui, snapshot);

    ui->state = snapshot->state;
    ui->rail_mode = snapshot->rail_mode;
    ui->day_count = snapshot->day_count;
    memset(ui->days, 0, sizeof(ui->days));
    if (snapshot->day_count)
        memcpy(ui->days, snapshot->days,
               snapshot->day_count * sizeof(ui->days[0]));
    ui->page = snapshot->page;
    ui->selected_row = snapshot->selected_row;

    if (snapshot->night) {
        ui->night = *snapshot->night;
        ui->has_night = true;
    } else if (snapshot->state != TOUCH_HISTORY_UI_STATE_ZOOM_LOADING) {
        memset(&ui->night, 0, sizeof(ui->night));
        ui->has_night = false;
    }
    ui->session_count = snapshot->session_count;
    if (snapshot->session_count)
        memcpy(ui->sessions, snapshot->sessions,
               snapshot->session_count * sizeof(ui->sessions[0]));

    if (snapshot->overview) {
        ui->overview = *snapshot->overview;
        ui->has_overview = true;
    } else if (snapshot->state != TOUCH_HISTORY_UI_STATE_ZOOM_LOADING) {
        memset(&ui->overview, 0, sizeof(ui->overview));
        ui->has_overview = false;
    }
    ui->event_count = snapshot->event_count;
    ui->event_total_count = snapshot->event_total_count;
    ui->event_state = snapshot->event_state;
    ui->events_truncated = snapshot->events_truncated;
    if (snapshot->event_count)
        memcpy(ui->events, snapshot->events,
               snapshot->event_count * sizeof(ui->events[0]));

    if (snapshot->month) {
        ui->month = *snapshot->month;
        ui->has_month = true;
    } else {
        memset(&ui->month, 0, sizeof(ui->month));
        ui->has_month = false;
    }
    ui->can_previous_month = snapshot->can_previous_month;
    ui->can_next_month = snapshot->can_next_month;
    ui->calendar_loading = snapshot->calendar_loading;
    ui->calendar_read_error = snapshot->calendar_read_error;
    ui->signal = snapshot->selected_signal;
    for (size_t i = 0; i < TOUCH_HISTORY_UI_STAT_COUNT; ++i) {
        history_ui_copy_text(ui->stats[i].label,
                             sizeof(ui->stats[i].label),
                             snapshot->stats[i].label);
        history_ui_copy_text(ui->stats[i].unit,
                             sizeof(ui->stats[i].unit),
                             snapshot->stats[i].unit);
        ui->stats[i].value_x100 = snapshot->stats[i].value_x100;
        ui->stats[i].available = snapshot->stats[i].available;
    }
    ui->can_previous_night = snapshot->can_previous_night;
    ui->can_next_night = snapshot->can_next_night;
    ui->usage_target_known = snapshot->usage_target_known;
    ui->usage_on_target = snapshot->usage_on_target;
    ui->therapy_only = snapshot->therapy_only;
    ui->progress_per_mille = snapshot->progress_per_mille;
    history_ui_copy_text(ui->status_text, sizeof(ui->status_text),
                         snapshot->status_text);
    history_ui_copy_text(ui->error_text, sizeof(ui->error_text),
                         snapshot->error_text);
    history_ui_copy_text(ui->degraded_text, sizeof(ui->degraded_text),
                         snapshot->degraded_text);
    history_ui_copy_text(ui->stats_warning_text,
                         sizeof(ui->stats_warning_text),
                         snapshot->stats_warning_text);

    ui->cursor_valid = snapshot->cursor_valid;
    ui->cursor_ms = snapshot->cursor_valid ? snapshot->cursor_ms : 0;

    if (ui->rail_mode == TOUCH_HISTORY_UI_RAIL_CALENDAR) {
        result = history_ui_ensure_calendar(ui);
        if (result != ESP_OK)
            return result;
    }

    history_ui_update_list(ui);
    history_ui_update_header(ui);
    history_ui_update_summary(ui);
    history_ui_update_channels(ui);
    history_ui_update_graph_header(ui);
    history_ui_update_event_status(ui);
    history_ui_update_state(ui);
    history_ui_update_calendar(ui, calendar_grid_content_changed);
    if (graph_content_changed)
        lv_obj_invalidate(ui->graph);
    return ESP_OK;
}
