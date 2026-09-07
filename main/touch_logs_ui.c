/*
 * SomnoTrace - native retained Logs pane
 *
 * A fixed object tree is created lazily.  Ten row objects are reused forever;
 * retained scans and card exports are deliberately delegated to a controller.
 */

#include "touch_logs_ui.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "somnotrace_fonts.h"

#define UI_TOUCH_MIN       44
#define UI_HEADER_HEIGHT   64
#define UI_FILTER_Y        64
#define UI_FILTER_HEIGHT   54
#define UI_VIEW_X          16
#define UI_VIEW_Y         118
#define UI_VIEW_WIDTH     736
#define UI_VIEW_HEIGHT    286
#define UI_FOOTER_Y       404
#define UI_FOOTER_HEIGHT   46
#define UI_ROW_TOP          8
#define UI_ROW_HEIGHT      27

#define COLOR_PANEL       0x181c29
#define COLOR_VIEW        0x0b0e16
#define COLOR_CONTROL     0x303644
#define COLOR_CONTROL_OFF 0x222733
#define COLOR_TEXT        0xf0f2f5
#define COLOR_SECONDARY   0xa9aeb8
#define COLOR_TERTIARY    0x858b97
#define COLOR_DIM         0x666c78
#define COLOR_INVERSE     0xe8eeec
#define COLOR_INK         0x171b24
#define COLOR_CYAN        0x35d9dc
#define COLOR_AMBER       0xf3ba45
#define COLOR_ERROR       0xef5a52
#define COLOR_DEBUG       0x939aa6
#define COLOR_ERROR_ROW   0x402127
#define COLOR_WARN_ROW    0x3c3320
#define COLOR_BORDER      0x3a414e

#define FONT_TITLE       (&somnotrace_space_grotesk_semibold_19)
#define FONT_BUTTON      (&somnotrace_space_grotesk_semibold_15)
#define FONT_BODY        (&somnotrace_space_grotesk_medium_15)
#define FONT_SMALL       (&somnotrace_space_grotesk_medium_13)
#define FONT_MONO        (&somnotrace_ibm_plex_mono_medium_13)
#define FONT_MONO_SMALL  (&somnotrace_ibm_plex_mono_medium_13)
#define FONT_MONO_STRONG (&somnotrace_ibm_plex_mono_semibold_11)

typedef enum {
    ACTION_PAUSE = 1,
    ACTION_SEARCH_FOCUS,
    ACTION_SEARCH_DONE,
    ACTION_SEARCH_CLEAR,
    ACTION_LEVEL,
    ACTION_PAGE_OLDER,
    ACTION_PAGE_NEWER,
    ACTION_JUMP_NEWEST,
    ACTION_CLEAR_RAM,
    ACTION_CLEAR_CONFIRM,
    ACTION_CLEAR_CANCEL,
    ACTION_SAVE_CARD,
    ACTION_EMPTY,
    ACTION_QEMU_DISCONNECT,
} action_t;

typedef struct {
    lv_obj_t *surface;
    lv_obj_t *accent;
    lv_obj_t *time;
    lv_obj_t *level;
    lv_obj_t *tag;
    lv_obj_t *message;
} log_row_ui_t;

typedef struct {
    touch_logs_ui_controller_t controller;

    lv_obj_t *root;
    lv_obj_t *connection_dot;
    lv_obj_t *connection_label;
    lv_obj_t *pause_button;
    lv_obj_t *pause_label;
    lv_obj_t *clear_button;
    lv_obj_t *save_button;
    lv_obj_t *save_label;

    lv_obj_t *search_input;
    lv_obj_t *search_clear;
    lv_obj_t *level_buttons[4];
    lv_obj_t *level_dots[4];
    lv_obj_t *level_labels[4];

    lv_obj_t *viewport;
    log_row_ui_t rows[TOUCH_LOGS_UI_VISIBLE_ROWS];
    lv_obj_t *jump_button;
    lv_obj_t *jump_label;
    lv_obj_t *empty_group;
    lv_obj_t *empty_orb;
    lv_obj_t *empty_glyph;
    lv_obj_t *empty_title;
    lv_obj_t *empty_body;
    lv_obj_t *empty_action;
    lv_obj_t *empty_action_label;

    lv_obj_t *save_panel;
    lv_obj_t *save_status;
    lv_obj_t *save_progress;
    lv_obj_t *clear_panel;
    bool clear_pending;

    lv_obj_t *footer_left;
    lv_obj_t *newer_button;
    lv_obj_t *older_button;

    lv_obj_t *search_sheet;
    lv_obj_t *search_sheet_status;
    lv_obj_t *search_done;
    lv_obj_t *keyboard;

    log_stream_retained_info_t info;
    log_stream_retained_page_t page;
    esp_err_t snapshot_result;
    uint32_t level_mask;
    uint64_t pause_anchor_total_count;
    touch_logs_ui_save_state_t save_state;
    size_t save_processed_lines;
    size_t save_total_lines;
    size_t saved_line_count;
    esp_err_t save_result;
    char saved_path[96];
    char save_error[96];
    char query[TOUCH_LOGS_UI_QUERY_MAX];
    char last_line_time[20];
    char intent_error[80];
    bool paused;
    bool search_focused;
    bool view_forced_pause;
    bool retrying;
    bool card_available;
    bool visible;
    bool has_snapshot;
} touch_logs_ui_t;

static touch_logs_ui_t *s_ui;

static const uint32_t s_level_bits[4] = {
    LOG_STREAM_RETAINED_LEVEL_ERROR,
    LOG_STREAM_RETAINED_LEVEL_WARN,
    LOG_STREAM_RETAINED_LEVEL_INFO,
    LOG_STREAM_RETAINED_LEVEL_DEBUG,
};

static const char *const s_level_names[4] = {
    "Error", "Warn", "Info", "Debug",
};

static const uint32_t s_level_colors[4] = {
    COLOR_ERROR, COLOR_AMBER, COLOR_CYAN, COLOR_DEBUG,
};

static void render_all(void);
static void action_cb(lv_event_t *event);
static void viewport_gesture_cb(lv_event_t *event);

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void style_surface(lv_obj_t *obj, uint32_t color, int radius)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            int x, int y, int width,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_pos(label, x, y);
    if (width > 0) lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static void *event_token(action_t action, uint32_t value)
{
    return (void *)((((uintptr_t)action) << 16) | (value & 0xffffU));
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int x, int y, int width, int height,
                             bool inverse, action_t action, uint32_t value,
                             lv_obj_t **label_out)
{
    if (height < UI_TOUCH_MIN) height = UI_TOUCH_MIN;
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    style_surface(button, inverse ? COLOR_INVERSE : COLOR_CONTROL, height / 2);
    lv_obj_set_style_bg_opa(button, inverse ? LV_OPA_80 : LV_OPA_70,
                            LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_translate_x(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_CONTROL_OFF),
                              LV_STATE_DISABLED);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, action_cb, LV_EVENT_PRESSED,
                        event_token(action, value));

    lv_obj_t *label = make_label(button, text, 0, 0, width - 8,
                                 FONT_BUTTON,
                                 inverse ? COLOR_INK : COLOR_TEXT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return button;
}

static void set_enabled(lv_obj_t *obj, bool enabled)
{
    if (!obj) return;
    if (enabled) lv_obj_clear_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static uint32_t effective_level_mask(void)
{
    return s_ui->level_mask == 0
        ? LOG_STREAM_RETAINED_LEVEL_ALL
        : s_ui->level_mask;
}

static uint64_t new_line_count(void)
{
    if (!s_ui->paused || s_ui->info.total_count <= s_ui->pause_anchor_total_count)
        return 0;
    return s_ui->info.total_count - s_ui->pause_anchor_total_count;
}

static bool stream_connected(void)
{
    return s_ui->has_snapshot && s_ui->snapshot_result == ESP_OK &&
           s_ui->info.available;
}

static const char *level_word(uint8_t level)
{
    switch (level) {
    case LOG_STREAM_RETAINED_LEVEL_ERROR: return "ERROR";
    case LOG_STREAM_RETAINED_LEVEL_WARN: return "WARN";
    case LOG_STREAM_RETAINED_LEVEL_INFO: return "INFO";
    case LOG_STREAM_RETAINED_LEVEL_DEBUG: return "DEBUG";
    case LOG_STREAM_RETAINED_LEVEL_VERBOSE: return "VRB";
    default: return "LOG";
    }
}

static uint32_t level_color(uint8_t level)
{
    switch (level) {
    case LOG_STREAM_RETAINED_LEVEL_ERROR: return COLOR_ERROR;
    case LOG_STREAM_RETAINED_LEVEL_WARN: return COLOR_AMBER;
    case LOG_STREAM_RETAINED_LEVEL_INFO: return COLOR_CYAN;
    default: return COLOR_DEBUG;
    }
}

static void format_uptime(char *out, size_t out_size,
                          const char *stamp, size_t stamp_len)
{
    bool digits = stamp_len > 0;
    for (size_t i = 0; i < stamp_len; ++i) {
        if (!isdigit((unsigned char)stamp[i])) {
            digits = false;
            break;
        }
    }
    if (!digits || stamp_len >= 24) {
        snprintf(out, out_size, "%.*s", (int)stamp_len, stamp);
        return;
    }

    char number[24];
    memcpy(number, stamp, stamp_len);
    number[stamp_len] = '\0';
    uint64_t ms = strtoull(number, NULL, 10);
    unsigned hours = (unsigned)((ms / 3600000ULL) % 100ULL);
    unsigned minutes = (unsigned)((ms / 60000ULL) % 60ULL);
    unsigned seconds = (unsigned)((ms / 1000ULL) % 60ULL);
    unsigned millis = (unsigned)(ms % 1000ULL);
    snprintf(out, out_size, "%02u:%02u:%02u.%03u",
             hours, minutes, seconds, millis);
}

static void parse_line(const log_stream_retained_line_t *line,
                       char *time_out, size_t time_size,
                       char *tag_out, size_t tag_size,
                       char *message_out, size_t message_size)
{
    copy_text(time_out, time_size, "--:--:--.---");
    copy_text(tag_out, tag_size, "system");
    copy_text(message_out, message_size, line ? line->text : "");
    if (!line) return;

    const char *begin = line->text;
    const char *limit = begin + strnlen(begin, LOG_STREAM_RETAINED_TEXT_MAX);
    const char *open = memchr(begin, '(', (size_t)(limit - begin));
    const char *close = open
        ? memchr(open + 1, ')', (size_t)(limit - open - 1))
        : NULL;
    const char *payload = begin;
    if (open && close && close > open + 1) {
        const char *stamp = open + 1;
        while (stamp < close && isspace((unsigned char)*stamp)) ++stamp;
        const char *stamp_end = close;
        while (stamp_end > stamp && isspace((unsigned char)stamp_end[-1]))
            --stamp_end;
        format_uptime(time_out, time_size, stamp,
                      (size_t)(stamp_end - stamp));
        payload = close + 1;
    }

    while (payload < limit && isspace((unsigned char)*payload)) ++payload;
    const char *colon = memchr(payload, ':', (size_t)(limit - payload));
    if (!colon) return;

    const char *tag_end = colon;
    while (tag_end > payload && isspace((unsigned char)tag_end[-1])) --tag_end;
    if (tag_end > payload) {
        snprintf(tag_out, tag_size, "%.*s", (int)(tag_end - payload), payload);
    }
    const char *message = colon + 1;
    while (message < limit && isspace((unsigned char)*message)) ++message;
    snprintf(message_out, message_size, "%.*s", (int)(limit - message), message);
    if (line->truncated && message_size > 4) {
        size_t len = strlen(message_out);
        if (len + 3 < message_size) strcat(message_out, "...");
    }
}

static void remember_intent_result(esp_err_t result, const char *action)
{
    if (result == ESP_OK) {
        s_ui->intent_error[0] = '\0';
        return;
    }
    snprintf(s_ui->intent_error, sizeof(s_ui->intent_error),
             "%s could not start · %s", action, esp_err_to_name(result));
}

static void style_connection(bool connected, bool paused)
{
    uint32_t color = connected ? (paused ? COLOR_AMBER : COLOR_CYAN)
                               : COLOR_ERROR;
    lv_obj_set_style_bg_color(s_ui->connection_dot, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_color(s_ui->connection_dot, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(s_ui->connection_dot, connected ? 7 : 5, 0);
}

static void render_header(void)
{
    bool connected = stream_connected();
    style_connection(connected, s_ui->paused);

    char status[112];
    if (!connected) {
        if (s_ui->retrying) {
            copy_text(status, sizeof(status), "Reconnecting log stream…");
        } else if (s_ui->last_line_time[0]) {
            snprintf(status, sizeof(status),
                     "Stream disconnected · last line %s", s_ui->last_line_time);
        } else {
            copy_text(status, sizeof(status), "Stream disconnected");
        }
    } else if (s_ui->paused) {
        uint64_t count = new_line_count();
        snprintf(status, sizeof(status), "Paused · %" PRIu64 " new line%s",
                 count, count == 1 ? "" : "s");
    } else {
        snprintf(status, sizeof(status), "Live · %u-line ring buffer",
                 (unsigned)s_ui->info.capacity);
    }
    lv_label_set_text(s_ui->connection_label, status);
    lv_obj_set_style_text_color(s_ui->connection_label,
        lv_color_hex(!connected ? COLOR_ERROR
                     : s_ui->paused ? COLOR_AMBER : COLOR_SECONDARY), 0);

    lv_label_set_text(s_ui->pause_label, s_ui->paused ? "Resume" : "Pause");
    lv_obj_set_style_bg_color(s_ui->pause_button,
        lv_color_hex(s_ui->paused ? COLOR_INVERSE : COLOR_CONTROL), 0);
    lv_obj_set_style_text_color(s_ui->pause_label,
        lv_color_hex(s_ui->paused ? COLOR_INK : COLOR_TEXT), 0);
    set_enabled(s_ui->pause_button,
                s_ui->controller.set_paused != NULL && connected);
    set_enabled(s_ui->clear_button,
                s_ui->controller.clear_ram_only != NULL &&
                s_ui->save_state != TOUCH_LOGS_UI_SAVE_RUNNING &&
                s_ui->info.retained_count > 0);

    const char *save_text = "Save to card";
    if (s_ui->save_state == TOUCH_LOGS_UI_SAVE_RUNNING) save_text = "Saving…";
    else if (s_ui->save_state == TOUCH_LOGS_UI_SAVE_FAILED) save_text = "Retry save";
    lv_label_set_text(s_ui->save_label, save_text);
    set_enabled(s_ui->save_button,
                s_ui->controller.save_card_snapshot != NULL &&
                s_ui->card_available &&
                s_ui->save_state != TOUCH_LOGS_UI_SAVE_RUNNING &&
                s_ui->info.retained_count > 0);
}

static void render_filters(void)
{
    if (!s_ui->search_focused) {
        const char *current = lv_textarea_get_text(s_ui->search_input);
        if (strcmp(current ? current : "", s_ui->query) != 0)
            lv_textarea_set_text(s_ui->search_input, s_ui->query);
    }
    if (s_ui->query[0]) lv_obj_clear_flag(s_ui->search_clear, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_ui->search_clear, LV_OBJ_FLAG_HIDDEN);
    set_enabled(s_ui->search_input,
                s_ui->controller.begin_search != NULL &&
                s_ui->controller.search_query != NULL && stream_connected());

    uint32_t mask = effective_level_mask();
    for (unsigned i = 0; i < 4; ++i) {
        bool active = (mask & s_level_bits[i]) != 0;
        lv_obj_set_style_bg_color(s_ui->level_buttons[i],
            lv_color_hex(active ? COLOR_CONTROL : COLOR_CONTROL_OFF), 0);
        lv_obj_set_style_bg_opa(s_ui->level_buttons[i],
            active ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_set_style_bg_color(s_ui->level_dots[i],
            lv_color_hex(active ? s_level_colors[i] : COLOR_DIM), 0);
        lv_obj_set_style_text_color(s_ui->level_labels[i],
            lv_color_hex(active ? COLOR_TEXT : COLOR_TERTIARY), 0);
        set_enabled(s_ui->level_buttons[i],
                    s_ui->controller.toggle_level != NULL && stream_connected());
    }
}

static void render_rows(const log_stream_retained_line_t *lines,
                        size_t line_count)
{
    for (size_t i = 0; i < TOUCH_LOGS_UI_VISIBLE_ROWS; ++i) {
        log_row_ui_t *row = &s_ui->rows[i];
        if (i >= line_count) {
            lv_obj_add_flag(row->surface, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const log_stream_retained_line_t *line = &lines[i];
        char stamp[20];
        char tag[40];
        char message[LOG_STREAM_RETAINED_TEXT_MAX + 4];
        parse_line(line, stamp, sizeof(stamp), tag, sizeof(tag),
                   message, sizeof(message));
        if (i == 0) copy_text(s_ui->last_line_time,
                              sizeof(s_ui->last_line_time), stamp);

        lv_label_set_text(row->time, stamp);
        lv_label_set_text(row->level, level_word(line->level));
        lv_label_set_text(row->tag, tag);
        lv_label_set_text(row->message, message);

        uint32_t tone = level_color(line->level);
        bool error = line->level == LOG_STREAM_RETAINED_LEVEL_ERROR;
        bool warning = line->level == LOG_STREAM_RETAINED_LEVEL_WARN;
        lv_obj_set_style_bg_color(row->accent, lv_color_hex(tone), 0);
        lv_obj_set_style_bg_opa(row->accent,
            (error || warning) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(row->level, lv_color_hex(tone), 0);
        lv_obj_set_style_text_color(row->message,
            lv_color_hex(line->level == LOG_STREAM_RETAINED_LEVEL_DEBUG
                         ? COLOR_SECONDARY : COLOR_TEXT), 0);
        lv_obj_set_style_bg_color(row->surface,
            lv_color_hex(error ? COLOR_ERROR_ROW
                         : warning ? COLOR_WARN_ROW : COLOR_VIEW), 0);
        lv_obj_set_style_bg_opa(row->surface,
            (error || warning) ? LV_OPA_70 : LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row->surface, LV_OBJ_FLAG_HIDDEN);
    }
}

static void active_levels_text(char *out, size_t out_size)
{
    uint32_t mask = effective_level_mask();
    size_t used = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < 4; ++i) {
        if ((mask & s_level_bits[i]) == 0) continue;
        int wrote = snprintf(out + used, out_size - used, "%s%s",
                             used ? " · " : "", s_level_names[i]);
        if (wrote < 0 || (size_t)wrote >= out_size - used) break;
        used += (size_t)wrote;
    }
    uint32_t other = LOG_STREAM_RETAINED_LEVEL_UNKNOWN |
                     LOG_STREAM_RETAINED_LEVEL_VERBOSE;
    if ((mask & other) != 0 && used < out_size) {
        snprintf(out + used, out_size - used, "%sOther", used ? " · " : "");
    }
    if (out[0] == '\0') copy_text(out, out_size, "None");
}

static void render_empty(void)
{
    bool disconnected = !stream_connected();
    bool empty = !disconnected && s_ui->page.matching_count == 0;
    if (!disconnected && !empty) {
        lv_obj_add_flag(s_ui->empty_group, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_ui->empty_group, LV_OBJ_FLAG_HIDDEN);
    /* Retain the last readable rows above a compact recovery panel. */
    lv_obj_set_pos(s_ui->empty_group, 0, disconnected ? 85 : 0);
    lv_obj_set_height(s_ui->empty_group, disconnected ? 201 : UI_VIEW_HEIGHT);
    if (disconnected) lv_obj_add_flag(s_ui->empty_orb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_ui->empty_orb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_ui->empty_title, disconnected ? 12 : 103);
    lv_obj_set_y(s_ui->empty_body, disconnected ? 45 : 139);
    lv_obj_set_y(s_ui->empty_action, disconnected ? 125 : 210);
    if (disconnected) {
        lv_label_set_text(s_ui->empty_glyph, "!");
        lv_label_set_text(s_ui->empty_title, "Log stream disconnected");
        lv_label_set_text(s_ui->empty_body,
            "The retained log view is unavailable. This does not stop therapy or recording. Existing card files are unchanged.");
        lv_label_set_text(s_ui->empty_action_label,
            s_ui->retrying ? "Reconnecting…" : "Reconnect");
        lv_obj_set_style_bg_color(s_ui->empty_orb,
                                  lv_color_hex(COLOR_ERROR_ROW), 0);
        lv_obj_set_style_text_color(s_ui->empty_glyph,
                                    lv_color_hex(COLOR_ERROR), 0);
        set_enabled(s_ui->empty_action,
                    !s_ui->retrying &&
                    s_ui->controller.retry_connection != NULL);
    } else if (s_ui->query[0]) {
        char title[112];
        char body[256];
        char levels[96];
        active_levels_text(levels, sizeof(levels));
        snprintf(title, sizeof(title), "No lines match “%s”", s_ui->query);
        if ((effective_level_mask() & LOG_STREAM_RETAINED_LEVEL_DEBUG) == 0) {
            snprintf(body, sizeof(body),
                     "Active levels: %s. The filter checks tag and message. Debug is off — enable Debug if the event may only be logged there.",
                     levels);
        } else {
            snprintf(body, sizeof(body),
                     "Active levels: %s. The filter checks the tag and message text.",
                     levels);
        }
        lv_label_set_text(s_ui->empty_glyph, "?");
        lv_label_set_text(s_ui->empty_title, title);
        lv_label_set_text(s_ui->empty_body, body);
        lv_label_set_text(s_ui->empty_action_label, "Clear filter");
        lv_obj_set_style_bg_color(s_ui->empty_orb,
                                  lv_color_hex(COLOR_CONTROL), 0);
        lv_obj_set_style_text_color(s_ui->empty_glyph,
                                    lv_color_hex(COLOR_SECONDARY), 0);
        set_enabled(s_ui->empty_action,
                    s_ui->controller.search_query != NULL);
    } else {
        char levels[96];
        char body[192];
        active_levels_text(levels, sizeof(levels));
        lv_label_set_text(s_ui->empty_glyph, "·");
        lv_label_set_text(s_ui->empty_title,
            s_ui->info.retained_count ? "No lines at these levels" : "No retained lines yet");
        snprintf(body, sizeof(body), "Active levels: %s. %s", levels,
            effective_level_mask() == LOG_STREAM_RETAINED_LEVEL_NONE
            ? "Turn on a level to see retained lines."
            : "Matching captured lines will appear here.");
        lv_label_set_text(s_ui->empty_body, body);
        lv_label_set_text(s_ui->empty_action_label, "Waiting for logs");
        lv_obj_set_style_bg_color(s_ui->empty_orb,
                                  lv_color_hex(COLOR_CONTROL), 0);
        lv_obj_set_style_text_color(s_ui->empty_glyph,
                                    lv_color_hex(COLOR_CYAN), 0);
        set_enabled(s_ui->empty_action, false);
    }
    lv_obj_move_foreground(s_ui->empty_group);
}

static void render_jump(void)
{
    uint64_t count = new_line_count();
    bool show = stream_connected() && s_ui->page.matching_count > 0 && s_ui->paused &&
                (count > 0 || s_ui->page.match_offset > 0);
    if (!show) {
        lv_obj_add_flag(s_ui->jump_button, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char text[80];
    if (count > 0) {
        snprintf(text, sizeof(text), "Jump to newest · %" PRIu64 " new", count);
    } else {
        copy_text(text, sizeof(text), "Jump to newest");
    }
    lv_label_set_text(s_ui->jump_label, text);
    set_enabled(s_ui->jump_button, s_ui->controller.jump_newest != NULL);
    lv_obj_clear_flag(s_ui->jump_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui->jump_button);
}

static void render_save(void)
{
    if (s_ui->save_state == TOUCH_LOGS_UI_SAVE_IDLE) {
        lv_obj_add_flag(s_ui->save_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char text[180];
    int percent = 0;
    if (s_ui->save_total_lines > 0) {
        size_t done = s_ui->save_processed_lines > s_ui->save_total_lines
                    ? s_ui->save_total_lines : s_ui->save_processed_lines;
        percent = (int)((done * 100U) / s_ui->save_total_lines);
    }
    if (s_ui->save_state == TOUCH_LOGS_UI_SAVE_RUNNING) {
        /* Scanning is not durable publication: never show completion before
         * flush, close and rename have all succeeded in the service worker. */
        if (percent > 99) percent = 99;
        snprintf(text, sizeof(text), "Saving retained snapshot… %d%%", percent);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
        snprintf(text, sizeof(text), "QEMU simulated save… %d%%", percent);
#endif
        lv_obj_clear_flag(s_ui->save_progress, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_ui->save_progress, percent, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_ui->save_panel,
                                  lv_color_hex(COLOR_CONTROL), 0);
    } else if (s_ui->save_state == TOUCH_LOGS_UI_SAVE_SUCCEEDED) {
        if (s_ui->saved_path[0]) {
            snprintf(text, sizeof(text), "Saved %u lines · %s",
                     (unsigned)s_ui->saved_line_count, s_ui->saved_path);
        } else {
            snprintf(text, sizeof(text), "Saved %u lines to card",
                     (unsigned)s_ui->saved_line_count);
        }
#if CONFIG_SOMNOTRACE_BOARD_QEMU
        snprintf(text, sizeof(text), "QEMU: processed %u lines; no card file written",
                 (unsigned)s_ui->saved_line_count);
#endif
        lv_obj_add_flag(s_ui->save_progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_ui->save_panel,
                                  lv_color_hex(0x17352f), 0);
    } else {
        const char *detail = s_ui->save_error[0]
                           ? s_ui->save_error
                           : esp_err_to_name(s_ui->save_result);
        snprintf(text, sizeof(text), "Save failed · %s", detail);
        lv_obj_add_flag(s_ui->save_progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_ui->save_panel,
                                  lv_color_hex(COLOR_ERROR_ROW), 0);
    }
    lv_label_set_text(s_ui->save_status, text);
    lv_obj_set_width(s_ui->save_status,
        s_ui->save_state == TOUCH_LOGS_UI_SAVE_RUNNING ? 430 : 680);
    lv_obj_clear_flag(s_ui->save_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui->save_panel);
}

static void render_footer(void)
{
    char text[192];
    if (s_ui->intent_error[0]) {
        copy_text(text, sizeof(text), s_ui->intent_error);
    } else {
        snprintf(text, sizeof(text),
                 "%u/%u matching · %u retained · observed %u min %02u s · %u dropped\nClear affects RAM only; card files stay intact. %s",
                 (unsigned)s_ui->page.returned,
                 (unsigned)s_ui->page.matching_count,
                 (unsigned)s_ui->info.retained_count,
                 (unsigned)(s_ui->info.retained_span_ms / 60000U),
                 (unsigned)((s_ui->info.retained_span_ms / 1000U) % 60U),
                 (unsigned)s_ui->info.dropped_count,
                 s_ui->paused ? "Capture continues; swipe to page."
                              : "Older lines roll off the buffer.");
    }
    lv_label_set_text(s_ui->footer_left, text);

    set_enabled(s_ui->newer_button,
                stream_connected() && s_ui->page.has_previous_page &&
                s_ui->controller.page_newer != NULL);
    set_enabled(s_ui->older_button,
                stream_connected() && s_ui->page.has_next_page &&
                s_ui->controller.page_older != NULL);
}

static void viewport_gesture_cb(lv_event_t *event)
{
    if (!s_ui || lv_event_get_code(event) != LV_EVENT_GESTURE ||
        !stream_connected()) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    /* The hidden 44px action proxies keep every controller invocation inside
     * the same PRESSED-only dispatcher as visible controls.  The viewport
     * itself never scrolls or creates rows: swipe up requests the next older
     * retained page, and swipe down requests the previous/newer page. */
    if (direction == LV_DIR_TOP && s_ui->page.has_next_page &&
        s_ui->controller.page_older) {
        lv_event_send(s_ui->older_button, LV_EVENT_PRESSED, NULL);
    } else if (direction == LV_DIR_BOTTOM && s_ui->page.has_previous_page &&
               s_ui->controller.page_newer) {
        lv_event_send(s_ui->newer_button, LV_EVENT_PRESSED, NULL);
    }
}

static void render_search_sheet(void)
{
    if (!s_ui->search_focused) {
        lv_obj_add_flag(s_ui->search_sheet, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char text[112];
    snprintf(text, sizeof(text),
             "Search paused · %" PRIu64 " new line%s buffered",
             new_line_count(), new_line_count() == 1 ? "" : "s");
    lv_label_set_text(s_ui->search_sheet_status, text);
    lv_keyboard_set_textarea(s_ui->keyboard, s_ui->search_input);
    lv_obj_clear_flag(s_ui->search_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui->search_sheet);
}

static void render_all(void)
{
    if (!s_ui || !s_ui->root) return;
    render_header();
    render_filters();
    render_empty();
    render_jump();
    render_save();
    render_footer();
    render_search_sheet();
    if (s_ui->clear_pending) {
        lv_obj_clear_flag(s_ui->clear_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui->clear_panel);
    } else {
        lv_obj_add_flag(s_ui->clear_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void action_cb(lv_event_t *event)
{
    if (!s_ui || lv_event_get_code(event) != LV_EVENT_PRESSED) return;
    uintptr_t token = (uintptr_t)lv_event_get_user_data(event);
    action_t action = (action_t)(token >> 16);
    uint32_t value = (uint32_t)(token & 0xffffU);
    esp_err_t result = ESP_ERR_NOT_SUPPORTED;

    switch (action) {
    case ACTION_PAUSE: {
        bool pause = !s_ui->paused;
        if (!s_ui->controller.set_paused) break;
        result = s_ui->controller.set_paused(s_ui->controller.context, pause);
        remember_intent_result(result, pause ? "Pause" : "Resume");
        if (result == ESP_OK) {
            s_ui->paused = pause;
            if (pause) s_ui->pause_anchor_total_count = s_ui->info.total_count;
            else s_ui->view_forced_pause = false;
        }
        break;
    }
    case ACTION_SEARCH_FOCUS:
        if (!s_ui->controller.begin_search || !s_ui->controller.search_query)
            break;
        result = s_ui->controller.begin_search(s_ui->controller.context);
        remember_intent_result(result, "Search");
        if (result == ESP_OK) {
            s_ui->paused = true;
            s_ui->view_forced_pause = true;
            s_ui->pause_anchor_total_count = s_ui->info.total_count;
            s_ui->search_focused = true;
        }
        break;
    case ACTION_SEARCH_DONE: {
        if (!s_ui->controller.search_query) break;
        const char *query = lv_textarea_get_text(s_ui->search_input);
        copy_text(s_ui->query, sizeof(s_ui->query), query);
        result = s_ui->controller.search_query(s_ui->controller.context,
                                               s_ui->query);
        remember_intent_result(result, "Search filter");
        if (result == ESP_OK) {
            /* Search dismissal intentionally keeps view_forced_pause set. */
            s_ui->search_focused = false;
        }
        break;
    }
    case ACTION_SEARCH_CLEAR:
        if (!s_ui->controller.search_query) break;
        result = s_ui->controller.search_query(s_ui->controller.context, "");
        remember_intent_result(result, "Clear filter");
        if (result == ESP_OK) {
            s_ui->query[0] = '\0';
            lv_textarea_set_text(s_ui->search_input, "");
            s_ui->search_focused = false;
        }
        break;
    case ACTION_LEVEL: {
        if (value >= 4 || !s_ui->controller.toggle_level) break;
        uint32_t bit = s_level_bits[value];
        bool enabled = (effective_level_mask() & bit) == 0;
        result = s_ui->controller.toggle_level(s_ui->controller.context,
                                               bit, enabled);
        remember_intent_result(result, "Level filter");
        if (result == ESP_OK) {
            uint32_t mask = effective_level_mask();
            if (enabled) mask |= bit;
            else mask &= ~bit;
            mask &= LOG_STREAM_RETAINED_LEVEL_ALL;
            s_ui->level_mask = mask ? mask : LOG_STREAM_RETAINED_LEVEL_NONE;
        }
        break;
    }
    case ACTION_PAGE_OLDER:
        if (!s_ui->controller.page_older) break;
        result = s_ui->controller.page_older(s_ui->controller.context);
        remember_intent_result(result, "Older page");
        if (result == ESP_OK) {
            if (!s_ui->paused)
                s_ui->pause_anchor_total_count = s_ui->info.total_count;
            s_ui->paused = true;
            s_ui->view_forced_pause = true;
        }
        break;
    case ACTION_PAGE_NEWER:
        if (!s_ui->controller.page_newer) break;
        result = s_ui->controller.page_newer(s_ui->controller.context);
        remember_intent_result(result, "Newer page");
        if (result == ESP_OK) {
            s_ui->paused = true;
            s_ui->view_forced_pause = true;
        }
        break;
    case ACTION_JUMP_NEWEST:
        if (!s_ui->controller.jump_newest) break;
        result = s_ui->controller.jump_newest(s_ui->controller.context);
        remember_intent_result(result, "Jump to newest");
        if (result == ESP_OK) {
            s_ui->pause_anchor_total_count = s_ui->info.total_count;
            /* Deliberately do not clear paused/view_forced_pause. */
        }
        break;
    case ACTION_CLEAR_RAM:
        s_ui->clear_pending = true;
        break;
    case ACTION_CLEAR_CANCEL:
        s_ui->clear_pending = false;
        break;
    case ACTION_CLEAR_CONFIRM:
        if (!s_ui->controller.clear_ram_only) break;
        result = s_ui->controller.clear_ram_only(s_ui->controller.context);
        remember_intent_result(result, "Clear RAM buffer");
        if (result == ESP_OK) s_ui->clear_pending = false;
        break;
    case ACTION_SAVE_CARD:
        if (!s_ui->controller.save_card_snapshot) break;
        result = s_ui->controller.save_card_snapshot(s_ui->controller.context);
        remember_intent_result(result, "Card save");
        if (result != ESP_OK) {
            s_ui->save_state = TOUCH_LOGS_UI_SAVE_FAILED;
            s_ui->save_result = result;
            s_ui->save_error[0] = '\0';
        }
        break;
    case ACTION_EMPTY:
        if (!stream_connected()) {
            if (!s_ui->controller.retry_connection) break;
            result = s_ui->controller.retry_connection(s_ui->controller.context);
            remember_intent_result(result, "Reconnect");
            if (result == ESP_OK) s_ui->retrying = true;
        } else if (s_ui->query[0] && s_ui->controller.search_query) {
            result = s_ui->controller.search_query(s_ui->controller.context, "");
            remember_intent_result(result, "Clear filter");
            if (result == ESP_OK) {
                s_ui->query[0] = '\0';
                lv_textarea_set_text(s_ui->search_input, "");
            }
        }
        break;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    case ACTION_QEMU_DISCONNECT:
        if (s_ui->controller.simulate_disconnect)
            s_ui->controller.simulate_disconnect(s_ui->controller.context);
        break;
#endif
    default:
        break;
    }
    render_all();
}

static void create_header(void)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    lv_obj_t *title = make_label(s_ui->root, "Logs · QEMU", 16, 8, 250, FONT_TITLE, COLOR_TEXT);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, action_cb, LV_EVENT_PRESSED,
                       event_token(ACTION_QEMU_DISCONNECT, 0));
#else
    make_label(s_ui->root, "Logs", 16, 8, 250, FONT_TITLE, COLOR_TEXT);
#endif
    s_ui->connection_dot = lv_obj_create(s_ui->root);
    lv_obj_set_pos(s_ui->connection_dot, 16, 42);
    lv_obj_set_size(s_ui->connection_dot, 7, 7);
    style_surface(s_ui->connection_dot, COLOR_ERROR, 4);
    s_ui->connection_label = make_label(s_ui->root, "Stream disconnected",
        31, 35, 370, FONT_SMALL, COLOR_ERROR);

    s_ui->pause_button = make_button(s_ui->root, "Pause",
        410, 10, 98, 44, false, ACTION_PAUSE, 0, &s_ui->pause_label);
    s_ui->clear_button = make_button(s_ui->root, "Clear",
        516, 10, 96, 44, false, ACTION_CLEAR_RAM, 0, NULL);
    s_ui->save_button = make_button(s_ui->root, "Save to card",
        620, 10, 132, 44, false, ACTION_SAVE_CARD, 0, &s_ui->save_label);
}

static void style_search_input(lv_obj_t *input)
{
    lv_obj_set_style_bg_color(input, lv_color_hex(COLOR_CONTROL_OFF), 0);
    lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(input, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_radius(input, 22, 0);
    lv_obj_set_style_pad_left(input, 16, 0);
    lv_obj_set_style_pad_right(input, 50, 0);
    lv_obj_set_style_pad_top(input, 11, 0);
    lv_obj_set_style_text_font(input, FONT_MONO, 0);
    lv_obj_set_style_text_color(input, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_color(input, lv_color_hex(COLOR_TERTIARY),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(input, lv_color_hex(COLOR_CYAN),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(input, 2, LV_STATE_FOCUSED);
    lv_obj_clear_flag(input, LV_OBJ_FLAG_SCROLLABLE);
}

static void create_filters(void)
{
    s_ui->search_input = lv_textarea_create(s_ui->root);
    lv_obj_set_pos(s_ui->search_input, 16, UI_FILTER_Y + 5);
    lv_obj_set_size(s_ui->search_input, 300, 44);
    lv_textarea_set_one_line(s_ui->search_input, true);
    lv_textarea_set_max_length(s_ui->search_input, TOUCH_LOGS_UI_QUERY_MAX - 1);
    lv_textarea_set_placeholder_text(s_ui->search_input,
                                     "Filter by tag or message");
    style_search_input(s_ui->search_input);
    lv_obj_add_event_cb(s_ui->search_input, action_cb, LV_EVENT_PRESSED,
                        event_token(ACTION_SEARCH_FOCUS, 0));

    s_ui->search_clear = make_button(s_ui->root, "Clear",
        260, UI_FILTER_Y + 5, 56, 44, false, ACTION_SEARCH_CLEAR, 0, NULL);
    lv_obj_add_flag(s_ui->search_clear, LV_OBJ_FLAG_HIDDEN);

    for (unsigned i = 0; i < 4; ++i) {
        int x = 324 + (int)i * 107;
        s_ui->level_buttons[i] = make_button(s_ui->root, "",
            x, UI_FILTER_Y + 5, 103, 44, false, ACTION_LEVEL, i, NULL);
        s_ui->level_dots[i] = lv_obj_create(s_ui->level_buttons[i]);
        lv_obj_set_pos(s_ui->level_dots[i], 11, 18);
        lv_obj_set_size(s_ui->level_dots[i], 7, 7);
        style_surface(s_ui->level_dots[i], s_level_colors[i], 4);
        s_ui->level_labels[i] = make_label(s_ui->level_buttons[i],
            s_level_names[i], 24, 12, 72, FONT_SMALL, COLOR_TEXT);
    }
}

static void create_rows(void)
{
    for (size_t i = 0; i < TOUCH_LOGS_UI_VISIBLE_ROWS; ++i) {
        log_row_ui_t *row = &s_ui->rows[i];
        row->surface = lv_obj_create(s_ui->viewport);
        lv_obj_set_pos(row->surface, 0, UI_ROW_TOP + (int)i * UI_ROW_HEIGHT);
        lv_obj_set_size(row->surface, UI_VIEW_WIDTH, UI_ROW_HEIGHT);
        style_surface(row->surface, COLOR_VIEW, 0);
        lv_obj_set_style_bg_opa(row->surface, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row->surface, LV_OBJ_FLAG_CLICKABLE);

        row->accent = lv_obj_create(row->surface);
        lv_obj_set_pos(row->accent, 10, 5);
        lv_obj_set_size(row->accent, 3, 17);
        style_surface(row->accent, COLOR_CYAN, 2);
        lv_obj_set_style_bg_opa(row->accent, LV_OPA_TRANSP, 0);

        row->time = make_label(row->surface, "--:--:--.---",
            21, 6, 96, FONT_MONO, COLOR_TERTIARY);
        row->level = make_label(row->surface, "INFO",
            125, 7, 50, FONT_MONO_STRONG, COLOR_CYAN);
        row->tag = make_label(row->surface, "system",
            183, 6, 92, FONT_MONO_SMALL, COLOR_SECONDARY);
        row->message = make_label(row->surface, "",
            285, 6, 435, FONT_MONO, COLOR_TEXT);
        lv_obj_add_flag(row->surface, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_empty_state(void)
{
    s_ui->empty_group = lv_obj_create(s_ui->viewport);
    lv_obj_set_pos(s_ui->empty_group, 0, 0);
    lv_obj_set_size(s_ui->empty_group, UI_VIEW_WIDTH, UI_VIEW_HEIGHT);
    style_surface(s_ui->empty_group, COLOR_VIEW, 0);

    s_ui->empty_orb = lv_obj_create(s_ui->empty_group);
    lv_obj_set_pos(s_ui->empty_orb, 344, 45);
    lv_obj_set_size(s_ui->empty_orb, 48, 48);
    style_surface(s_ui->empty_orb, COLOR_CONTROL, 24);
    s_ui->empty_glyph = make_label(s_ui->empty_orb, "?", 0, 0, 48,
                                   FONT_TITLE, COLOR_SECONDARY);
    lv_obj_set_style_text_align(s_ui->empty_glyph, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_ui->empty_glyph);

    s_ui->empty_title = make_label(s_ui->empty_group,
        "Log stream disconnected", 108, 103, 520, FONT_TITLE, COLOR_TEXT);
    lv_obj_set_style_text_align(s_ui->empty_title, LV_TEXT_ALIGN_CENTER, 0);
    s_ui->empty_body = make_label(s_ui->empty_group, "", 108, 139, 520,
                                  FONT_BODY, COLOR_SECONDARY);
    lv_obj_set_height(s_ui->empty_body, 54);
    lv_label_set_long_mode(s_ui->empty_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ui->empty_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_ui->empty_body, 3, 0);
    s_ui->empty_action = make_button(s_ui->empty_group, "Reconnect",
        281, 210, 174, 50, true, ACTION_EMPTY, 0,
        &s_ui->empty_action_label);
}

static void create_viewport(void)
{
    s_ui->viewport = lv_obj_create(s_ui->root);
    lv_obj_set_pos(s_ui->viewport, UI_VIEW_X, UI_VIEW_Y);
    lv_obj_set_size(s_ui->viewport, UI_VIEW_WIDTH, UI_VIEW_HEIGHT);
    style_surface(s_ui->viewport, COLOR_VIEW, 16);
    lv_obj_set_style_border_width(s_ui->viewport, 1, 0);
    lv_obj_set_style_border_color(s_ui->viewport, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_clip_corner(s_ui->viewport, true, 0);
    lv_obj_add_flag(s_ui->viewport, LV_OBJ_FLAG_CLICKABLE);
    /* LVGL bubbles gestures to the first ancestor without this flag. Stop
     * here so the fixed Logs viewport, rather than the screen, receives it. */
    lv_obj_clear_flag(s_ui->viewport, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_ui->viewport, viewport_gesture_cb,
                        LV_EVENT_GESTURE, NULL);

    create_rows();
    create_empty_state();
    s_ui->jump_button = make_button(s_ui->viewport, "Jump to newest",
        260, 230, 216, 44, true, ACTION_JUMP_NEWEST, 0,
        &s_ui->jump_label);
    lv_obj_add_flag(s_ui->jump_button, LV_OBJ_FLAG_HIDDEN);

    s_ui->save_panel = lv_obj_create(s_ui->viewport);
    lv_obj_set_pos(s_ui->save_panel, 8, 8);
    lv_obj_set_size(s_ui->save_panel, UI_VIEW_WIDTH - 16, 48);
    style_surface(s_ui->save_panel, COLOR_CONTROL, 14);
    s_ui->save_status = make_label(s_ui->save_panel, "", 14, 15, 430,
                                   FONT_SMALL, COLOR_TEXT);
    s_ui->save_progress = lv_bar_create(s_ui->save_panel);
    lv_obj_set_pos(s_ui->save_progress, 472, 17);
    lv_obj_set_size(s_ui->save_progress, 230, 14);
    lv_bar_set_range(s_ui->save_progress, 0, 100);
    lv_bar_set_value(s_ui->save_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui->save_progress,
                              lv_color_hex(COLOR_CONTROL_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui->save_progress,
                              lv_color_hex(COLOR_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ui->save_progress, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui->save_progress, 7, LV_PART_INDICATOR);
    lv_obj_add_flag(s_ui->save_panel, LV_OBJ_FLAG_HIDDEN);

    s_ui->clear_panel = lv_obj_create(s_ui->viewport);
    lv_obj_set_pos(s_ui->clear_panel, 40, 38);
    lv_obj_set_size(s_ui->clear_panel, 656, 210);
    style_surface(s_ui->clear_panel, COLOR_PANEL, 16);
    make_label(s_ui->clear_panel, "Clear retained log buffer?", 24, 20, 600,
               FONT_TITLE, COLOR_TEXT);
    lv_obj_t *body = make_label(s_ui->clear_panel,
        "Removes retained RAM lines at every level, including hidden lines. Card files and recordings stay unchanged.",
        24, 65, 600, FONT_BODY, COLOR_SECONDARY);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    make_button(s_ui->clear_panel, "Cancel", 282, 142, 140, 44,
                false, ACTION_CLEAR_CANCEL, 0, NULL);
    make_button(s_ui->clear_panel, "Clear buffer", 438, 142, 190, 44,
                true, ACTION_CLEAR_CONFIRM, 0, NULL);
    lv_obj_add_flag(s_ui->clear_panel, LV_OBJ_FLAG_HIDDEN);
}

static void create_footer(void)
{
    s_ui->footer_left = make_label(s_ui->root, "", 16, UI_FOOTER_Y + 5,
                                   736, FONT_SMALL, COLOR_TERTIARY);
    lv_obj_set_height(s_ui->footer_left, 35);
    lv_label_set_long_mode(s_ui->footer_left, LV_LABEL_LONG_WRAP);
    /* Paging is gesture-first. These hidden touch-sized proxies translate a
     * vertical gesture into the shared PRESSED intent dispatcher without
     * putting permanent Newer/Older controls in the handoff footer. */
    s_ui->newer_button = make_button(s_ui->root, "Newer",
        0, 0, 100, 44, false, ACTION_PAGE_NEWER, 0, NULL);
    s_ui->older_button = make_button(s_ui->root, "Older",
        0, 0, 100, 44, false, ACTION_PAGE_OLDER, 0, NULL);
    lv_obj_add_flag(s_ui->newer_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui->older_button, LV_OBJ_FLAG_HIDDEN);
}

static void keyboard_input_cb(lv_event_t *event)
{
    if (!s_ui || !s_ui->search_focused) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        lv_event_send(s_ui->search_done, LV_EVENT_PRESSED, NULL);
    } else if (code == LV_EVENT_CANCEL) {
        s_ui->search_focused = false;
        lv_textarea_set_text(s_ui->search_input, s_ui->query);
        render_all();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        const char *text = lv_btnmatrix_get_btn_text(s_ui->keyboard,
            lv_btnmatrix_get_selected_btn(s_ui->keyboard));
        if (!text) return;
        /* The shared shell installs its Rev B maps globally in LVGL. Honor
         * their legends rather than inserting "space" or the Shift glyph. */
        if (!strcmp(text, "space")) {
            lv_textarea_add_char(s_ui->search_input, ' ');
        } else if (!strcmp(text, LV_SYMBOL_UP)) {
            lv_keyboard_set_mode(s_ui->keyboard,
                lv_keyboard_get_mode(s_ui->keyboard) == LV_KEYBOARD_MODE_TEXT_UPPER
                ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
        } else if (!strcmp(text, "123")) {
            lv_keyboard_set_mode(s_ui->keyboard, LV_KEYBOARD_MODE_SPECIAL);
        } else {
            lv_keyboard_def_event_cb(event);
        }
    }
}

static void create_search_sheet(void)
{
    s_ui->search_sheet = lv_obj_create(s_ui->root);
    lv_obj_set_pos(s_ui->search_sheet, 0, UI_VIEW_Y);
    lv_obj_set_size(s_ui->search_sheet, TOUCH_LOGS_UI_WIDTH,
                    TOUCH_LOGS_UI_HEIGHT - UI_VIEW_Y);
    style_surface(s_ui->search_sheet, COLOR_PANEL, 0);

    make_label(s_ui->search_sheet, "Filter tag and message", 16, 13, 250,
               FONT_TITLE, COLOR_TEXT);
    s_ui->search_sheet_status = make_label(s_ui->search_sheet,
        "Search paused", 274, 18, 330, FONT_SMALL, COLOR_AMBER);
    s_ui->search_done = make_button(s_ui->search_sheet, "Done",
        652, 5, 100, 44, true, ACTION_SEARCH_DONE, 0, NULL);

    s_ui->keyboard = lv_keyboard_create(s_ui->search_sheet);
    lv_obj_remove_event_cb(s_ui->keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_ui->keyboard, keyboard_input_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ui->keyboard, keyboard_input_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ui->keyboard, keyboard_input_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_set_align(s_ui->keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_ui->keyboard, 8, 54);
    lv_obj_set_size(s_ui->keyboard, 752, 272);
    lv_keyboard_set_mode(s_ui->keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_ui->keyboard, s_ui->search_input);
    lv_obj_set_style_bg_color(s_ui->keyboard, lv_color_hex(COLOR_VIEW),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui->keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui->keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui->keyboard, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui->keyboard, lv_color_hex(COLOR_CONTROL),
                              LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_ui->keyboard, lv_color_hex(COLOR_TEXT),
                                LV_PART_ITEMS);
    /* LVGL's built-in keyboard needs its symbol glyphs for Shift/Backspace. */
    lv_obj_set_style_text_font(s_ui->keyboard, LV_FONT_DEFAULT, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_ui->keyboard, 7, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_ui->keyboard, 0, LV_PART_ITEMS);
    lv_obj_add_flag(s_ui->search_sheet, LV_OBJ_FLAG_HIDDEN);
}

esp_err_t touch_logs_ui_create(
    lv_obj_t *parent, const touch_logs_ui_controller_t *controller)
{
    if (!parent || !controller) return ESP_ERR_INVALID_ARG;
    if (s_ui) return ESP_ERR_INVALID_STATE;

    touch_logs_ui_t *ui = heap_caps_calloc(
        1, sizeof(*ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ui) return ESP_ERR_NO_MEM;
    s_ui = ui;
    s_ui->controller = *controller;
    s_ui->snapshot_result = ESP_ERR_INVALID_STATE;
    s_ui->save_result = ESP_OK;

    s_ui->root = lv_obj_create(parent);
    if (!s_ui->root) {
        heap_caps_free(s_ui);
        s_ui = NULL;
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(s_ui->root, 0, 0);
    lv_obj_set_size(s_ui->root, TOUCH_LOGS_UI_WIDTH, TOUCH_LOGS_UI_HEIGHT);
    style_surface(s_ui->root, COLOR_PANEL, 26);
    lv_obj_set_style_clip_corner(s_ui->root, true, 0);
    lv_obj_add_flag(s_ui->root, LV_OBJ_FLAG_HIDDEN);

    create_header();
    create_filters();
    create_viewport();
    create_footer();
    create_search_sheet();
    render_all();
    return ESP_OK;
}

void touch_logs_ui_destroy(void)
{
    if (!s_ui) return;
    touch_logs_ui_t *released = s_ui;
    s_ui = NULL;
    if (released->root) lv_obj_del(released->root);
    heap_caps_free(released);
}

esp_err_t touch_logs_ui_show(void)
{
    if (!s_ui || !s_ui->root) return ESP_ERR_INVALID_STATE;
    render_all();
    lv_obj_clear_flag(s_ui->root, LV_OBJ_FLAG_HIDDEN);
    s_ui->visible = true;
    return ESP_OK;
}

void touch_logs_ui_hide(void)
{
    if (!s_ui || !s_ui->root) return;
    lv_obj_add_flag(s_ui->root, LV_OBJ_FLAG_HIDDEN);
    s_ui->visible = false;
    s_ui->clear_pending = false;
}

bool touch_logs_ui_is_visible(void)
{
    return s_ui && s_ui->visible;
}

esp_err_t touch_logs_ui_update(const touch_logs_ui_update_t *snapshot)
{
    if (!s_ui || !s_ui->root) return ESP_ERR_INVALID_STATE;
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    if (snapshot->filter.order != LOG_STREAM_RETAINED_NEWEST_FIRST)
        return ESP_ERR_INVALID_ARG;
    if (snapshot->line_count > TOUCH_LOGS_UI_VISIBLE_ROWS)
        return ESP_ERR_INVALID_SIZE;
    if (snapshot->line_count > 0 && !snapshot->lines)
        return ESP_ERR_INVALID_ARG;
    if (snapshot->snapshot_result == ESP_OK &&
        snapshot->page.returned != snapshot->line_count)
        return ESP_ERR_INVALID_SIZE;

    s_ui->snapshot_result = snapshot->snapshot_result;
    s_ui->info = snapshot->info;
    s_ui->page = snapshot->page;
    s_ui->level_mask = snapshot->filter.level_mask;
    s_ui->retrying = snapshot->retrying;
    s_ui->card_available = snapshot->card_available;
    s_ui->save_state = snapshot->save_state;
    s_ui->save_processed_lines = snapshot->save_processed_lines;
    s_ui->save_total_lines = snapshot->save_total_lines;
    s_ui->saved_line_count = snapshot->saved_line_count;
    s_ui->save_result = snapshot->save_result;
    copy_text(s_ui->saved_path, sizeof(s_ui->saved_path), snapshot->saved_path);
    copy_text(s_ui->save_error, sizeof(s_ui->save_error), snapshot->save_error);

    if (!s_ui->search_focused) {
        copy_text(s_ui->query, sizeof(s_ui->query), snapshot->filter.query);
    }
    s_ui->paused = snapshot->paused || s_ui->view_forced_pause;
    if (snapshot->paused &&
        snapshot->pause_anchor_total_count <= snapshot->info.total_count) {
        s_ui->pause_anchor_total_count = snapshot->pause_anchor_total_count;
    } else if (!s_ui->paused) {
        s_ui->pause_anchor_total_count = snapshot->info.total_count;
    }
    s_ui->has_snapshot = true;
    if (snapshot->snapshot_result == ESP_OK) s_ui->intent_error[0] = '\0';

    render_rows(snapshot->lines, snapshot->line_count);
    render_all();
    return ESP_OK;
}

lv_obj_t *touch_logs_ui_root(void)
{
    return s_ui ? s_ui->root : NULL;
}
