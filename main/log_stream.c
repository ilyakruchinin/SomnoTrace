/*
 * SomnoTrace - Log stream: ring-buffered log capture with SSE delivery
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

#include "log_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "sd_storage.h"
#include "psram_task.h"
#include "net_provision.h"
#include "uploader.h"
#include "as11_ble.h"
#include "oximeter.h"

static const char *TAG = "log_stream";

/* ── Ring Buffer ──────────────────────────────────────────────────── */

#define RINGBUF_SIZE_INTERNAL   (8  * 1024)
#define RINGBUF_SIZE_PSRAM      (16 * 1024)
#define LOG_LINE_MAX            256
#define LOGS_RECENT_RESPONSE_CAP (8 * 1024)
/* Includes the longest non-negative signed cursor and leaves room for NUL. */
#define LOGS_RECENT_SUFFIX_RESERVE (sizeof("],\"cursor\":2147483647}"))

/* ── SD Persistent Logging ───────────────────────────────────────── */

#define LOG_DIR             SD_LOG_DIR
#define LOG_FILE_PREFIX     "somnotrace.log."
#define LOG_MAX_FILES       4
#define LOG_FILE_MAX_SIZE   (128 * 1024)  /* 128 KB per file */
#define LOG_TOTAL_MAX       (LOG_FILE_MAX_SIZE * LOG_MAX_FILES)  /* 512 KB */
#define WRITEBUF_SIZE       (8 * 1024)    /* PSRAM write buffer */
#define FLUSH_INTERVAL_MS   2000          /* flush at least every 2 s */
#define FLUSH_THRESHOLD     4096          /* flush when buffer reaches 4 KB */

/* The native UI retains complete logical lines independently of the byte ring
 * drained by WebSocket/polling clients.  2,048 lines matches the Rev B screen
 * contract and costs about 430 KiB with the bounded text prefix below.  A tiny
 * internal-RAM fallback keeps diagnostics available on boards without usable
 * PSRAM without jeopardising the display's internal-RAM budget. */
#define RETAINED_CAPACITY_PSRAM          2048u
#define RETAINED_CAPACITY_PSRAM_FALLBACK  512u
#define RETAINED_CAPACITY_INTERNAL         32u
#define RETAINED_SAVE_FILE         "touchscreen-visible.log"
#define RETAINED_SAVE_TMP_FILE     RETAINED_SAVE_FILE ".tmp"
#define RETAINED_SAVE_BACKUP_FILE  RETAINED_SAVE_FILE ".bak"
#define RETAINED_SLOT_TRUNCATED    (1u << 0)

static RingbufHandle_t s_ringbuf;
static vprintf_like_t  s_orig_vprintf;
static bool s_init_attempted;
static esp_err_t s_init_result = ESP_ERR_INVALID_STATE;

/* Write buffer for SD persistence (separate from SSE ring buffer). */
static uint8_t *s_writebuf;          /* PSRAM buffer */
static size_t   s_writebuf_head;     /* write position (from vprintf hook) */
static size_t   s_writebuf_tail;     /* read position (from flush task) */
static SemaphoreHandle_t s_writebuf_mutex;
static TaskHandle_t s_flush_task;
static bool s_sd_ready;              /* SD card is mounted and log dir created */

typedef struct {
    uint64_t sequence;
    uint32_t captured_ms;
    uint16_t length;
    uint8_t level;
    uint8_t flags;
    char text[LOG_STREAM_RETAINED_TEXT_MAX];
} retained_slot_t;

typedef struct {
    size_t head;
    size_t count;
    size_t capacity;
    uint64_t generation;
    uint64_t total_count;
} retained_bounds_t;

static retained_slot_t *s_retained_slots;
static size_t s_retained_capacity;
static size_t s_retained_head;
static size_t s_retained_count;
static uint64_t s_retained_generation;
static uint64_t s_retained_total_count;
static volatile uint32_t s_retained_dropped_count;
static bool s_retained_in_psram;
static bool s_retained_retrying;
static esp_err_t s_retained_last_error = ESP_ERR_INVALID_STATE;
static portMUX_TYPE s_retained_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── WebSocket Live Tail ──────────────────────────────────────────── */
#define MAX_WS_CLIENTS 4

typedef struct {
    httpd_handle_t hd;
    int fd;
    bool paused;  /* per-client: when true, non-log pushes are skipped for this client */
} ws_client_t;

static ws_client_t       s_ws_clients[MAX_WS_CLIENTS];
static int               s_ws_client_count = 0;
static SemaphoreHandle_t s_ws_mutex;
static TaskHandle_t     s_ws_fwd_task;
static bool             s_ws_fwd_starting;
static portMUX_TYPE     s_ws_task_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Native touchscreen retained feed ─────────────────────────────── */

static uint8_t retained_level_for_line(const char *text, size_t length)
{
    size_t i = 0;
    while (i < length && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i >= length) return LOG_STREAM_RETAINED_LEVEL_UNKNOWN;

    switch (text[i]) {
    case 'E': return LOG_STREAM_RETAINED_LEVEL_ERROR;
    case 'W': return LOG_STREAM_RETAINED_LEVEL_WARN;
    case 'I': return LOG_STREAM_RETAINED_LEVEL_INFO;
    case 'D': return LOG_STREAM_RETAINED_LEVEL_DEBUG;
    case 'V': return LOG_STREAM_RETAINED_LEVEL_VERBOSE;
    default:  return LOG_STREAM_RETAINED_LEVEL_UNKNOWN;
    }
}

static void retained_set_last_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_retained_lock);
    s_retained_last_error = error;
    portEXIT_CRITICAL(&s_retained_lock);
}

static void retained_fill_info_locked(log_stream_retained_info_t *info)
{
    if (!info) return;
    info->available = s_retained_slots != NULL;
    info->in_psram = s_retained_in_psram;
    info->capacity = s_retained_capacity;
    info->retained_count = s_retained_count;
    info->generation = s_retained_generation;
    info->total_count = s_retained_total_count;
    info->retained_span_ms = 0;
    if (s_retained_slots && s_retained_capacity > 0 &&
        s_retained_count > 1) {
        size_t oldest = (s_retained_head + s_retained_capacity -
                         s_retained_count) % s_retained_capacity;
        size_t newest = (s_retained_head + s_retained_capacity - 1) %
                        s_retained_capacity;
        info->retained_span_ms =
            s_retained_slots[newest].captured_ms -
            s_retained_slots[oldest].captured_ms;
    }
    info->dropped_count = __atomic_load_n(&s_retained_dropped_count,
                                          __ATOMIC_RELAXED);
    info->last_error = s_retained_last_error;
}

static esp_err_t retained_read_bounds(retained_bounds_t *bounds,
                                      log_stream_retained_info_t *info)
{
    esp_err_t result;
    portENTER_CRITICAL(&s_retained_lock);
    retained_fill_info_locked(info);
    if (!s_retained_slots || s_retained_capacity == 0) {
        result = s_retained_last_error == ESP_OK
            ? ESP_ERR_INVALID_STATE : s_retained_last_error;
    } else {
        if (bounds) {
            bounds->head = s_retained_head;
            bounds->count = s_retained_count;
            bounds->capacity = s_retained_capacity;
            bounds->generation = s_retained_generation;
            bounds->total_count = s_retained_total_count;
        }
        result = ESP_OK;
    }
    portEXIT_CRITICAL(&s_retained_lock);
    return result;
}

static bool retained_copy_slot(size_t index,
                               log_stream_retained_line_t *line)
{
    bool copied = false;
    portENTER_CRITICAL(&s_retained_lock);
    if (s_retained_slots && index < s_retained_capacity) {
        const retained_slot_t *slot = &s_retained_slots[index];
        uint16_t length = slot->length;
        if (length >= LOG_STREAM_RETAINED_TEXT_MAX) {
            length = LOG_STREAM_RETAINED_TEXT_MAX - 1;
        }
        line->sequence = slot->sequence;
        line->length = length;
        line->level = slot->level;
        line->truncated = (slot->flags & RETAINED_SLOT_TRUNCATED) != 0;
        memcpy(line->text, slot->text, length);
        line->text[length] = '\0';
        copied = true;
    }
    portEXIT_CRITICAL(&s_retained_lock);
    return copied;
}

static bool retained_contains_case_insensitive(const char *haystack,
                                               size_t haystack_len,
                                               const char *needle)
{
    if (!needle || needle[0] == '\0') return true;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return true;
    if (needle_len > haystack_len) return false;

    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        size_t j = 0;
        while (j < needle_len) {
            unsigned char a = (unsigned char)haystack[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (j == needle_len) return true;
    }
    return false;
}

/* ESP-IDF text logs are `I (timestamp) tag: message`.  Search intentionally
 * excludes the severity/timestamp prefix so the touchscreen contract remains
 * "tag + message", while gracefully falling back to the whole line for text
 * written through the hook in a different format. */
static const char *retained_search_start(const log_stream_retained_line_t *line,
                                         size_t *search_length)
{
    for (size_t i = 0; i + 1 < line->length; i++) {
        if (line->text[i] == ')' && line->text[i + 1] == ' ') {
            *search_length = line->length - (i + 2);
            return line->text + i + 2;
        }
    }
    *search_length = line->length;
    return line->text;
}

static bool retained_filter_matches(const log_stream_retained_line_t *line,
                                    const log_stream_retained_filter_t *filter)
{
    if (!filter) return true;
    uint32_t mask = filter->level_mask == 0
        ? LOG_STREAM_RETAINED_LEVEL_ALL : filter->level_mask;
    if ((mask & line->level) == 0) return false;
    if (line->sequence <= filter->after_sequence) return false;
    if (filter->before_sequence != 0 &&
        line->sequence >= filter->before_sequence) return false;

    size_t search_length;
    const char *search = retained_search_start(line, &search_length);
    return retained_contains_case_insensitive(search, search_length,
                                              filter->query);
}

static bool retained_line_is_in_bounds(const log_stream_retained_line_t *line,
                                       const retained_bounds_t *bounds)
{
    if (bounds->count == 0 || bounds->total_count == 0) return false;
    uint64_t first = bounds->total_count - bounds->count + 1;
    return line->sequence >= first && line->sequence <= bounds->total_count;
}

static size_t retained_snapshot_from_bounds(
    const retained_bounds_t *bounds,
    log_stream_retained_line_t *lines,
    size_t line_capacity,
    const log_stream_retained_filter_t *filter,
    bool *unstable)
{
    size_t copied = 0;
    bool oldest_first = filter &&
        filter->order == LOG_STREAM_RETAINED_OLDEST_FIRST;

    if (unstable) *unstable = false;
    for (size_t offset = 0;
         offset < bounds->count && copied < line_capacity;
         offset++) {
        size_t index;
        if (oldest_first) {
            index = (bounds->head + bounds->capacity - bounds->count + offset)
                % bounds->capacity;
        } else {
            index = (bounds->head + bounds->capacity - 1 - offset)
                % bounds->capacity;
        }

        log_stream_retained_line_t candidate;
        if (!retained_copy_slot(index, &candidate) ||
            !retained_line_is_in_bounds(&candidate, bounds)) {
            if (unstable) *unstable = true;
            continue;
        }
        if (!retained_filter_matches(&candidate, filter)) continue;
        lines[copied++] = candidate;
    }
    return copied;
}

static void retained_append_line(const char *text, size_t length,
                                 bool source_truncated)
{
    if (!s_retained_slots || s_retained_capacity == 0) {
        __atomic_fetch_add(&s_retained_dropped_count, 1, __ATOMIC_RELAXED);
        return;
    }
    if (length == 0) return;

    /* Never wait in the global vprintf path.  A simultaneous snapshot read or
     * Clear costs this line, which is counted and visible to the UI. */
    if (portTRY_ENTER_CRITICAL(&s_retained_lock, 0) != pdTRUE) {
        __atomic_fetch_add(&s_retained_dropped_count, 1, __ATOMIC_RELAXED);
        return;
    }

    retained_slot_t *slot = &s_retained_slots[s_retained_head];
    size_t copy_length = length;
    bool truncated = source_truncated;
    if (copy_length >= LOG_STREAM_RETAINED_TEXT_MAX) {
        copy_length = LOG_STREAM_RETAINED_TEXT_MAX - 1;
        truncated = true;
    }
    memcpy(slot->text, text, copy_length);
    slot->text[copy_length] = '\0';
    slot->length = (uint16_t)copy_length;
    slot->level = retained_level_for_line(slot->text, copy_length);
    slot->flags = truncated ? RETAINED_SLOT_TRUNCATED : 0;
    slot->sequence = ++s_retained_total_count;
    slot->captured_ms = (uint32_t)(esp_timer_get_time() / 1000);

    s_retained_head = (s_retained_head + 1) % s_retained_capacity;
    if (s_retained_count < s_retained_capacity) s_retained_count++;
    s_retained_generation++;
    portEXIT_CRITICAL(&s_retained_lock);
}

static void retained_capture_text(const char *text, size_t length,
                                  bool source_truncated)
{
    size_t start = 0;
    while (start < length) {
        size_t end = start;
        while (end < length && text[end] != '\n') end++;
        size_t line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') line_end--;
        if (line_end > start) {
            bool last_was_truncated = source_truncated && end == length;
            retained_append_line(text + start, line_end - start,
                                 last_was_truncated);
        }
        start = end < length ? end + 1 : length;
    }
}

static void retained_init(void)
{
    size_t bytes = RETAINED_CAPACITY_PSRAM * sizeof(retained_slot_t);
    s_retained_slots = heap_caps_calloc(1, bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_retained_slots) {
        s_retained_capacity = RETAINED_CAPACITY_PSRAM;
        s_retained_in_psram = true;
        s_retained_last_error = ESP_OK;
        return;
    }

    /* A fragmented PSRAM heap may no longer have the roughly 430 KiB required
     * for the full ring even though it can still retain a useful history. */
    bytes = RETAINED_CAPACITY_PSRAM_FALLBACK * sizeof(retained_slot_t);
    s_retained_slots = heap_caps_calloc(1, bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_retained_slots) {
        s_retained_capacity = RETAINED_CAPACITY_PSRAM_FALLBACK;
        s_retained_in_psram = true;
        s_retained_last_error = ESP_OK;
        return;
    }

    bytes = RETAINED_CAPACITY_INTERNAL * sizeof(retained_slot_t);
    s_retained_slots = heap_caps_calloc(1, bytes,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_retained_slots) {
        s_retained_capacity = RETAINED_CAPACITY_INTERNAL;
        s_retained_in_psram = false;
        s_retained_last_error = ESP_OK;
    } else {
        s_retained_capacity = 0;
        s_retained_in_psram = false;
        s_retained_last_error = ESP_ERR_NO_MEM;
    }
}

esp_err_t log_stream_retained_get_info(log_stream_retained_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    return retained_read_bounds(NULL, info);
}

esp_err_t log_stream_retained_retry(void)
{
    portENTER_CRITICAL(&s_retained_lock);
    if (s_retained_slots && s_retained_capacity > 0) {
        portEXIT_CRITICAL(&s_retained_lock);
        return ESP_OK;
    }
    if (s_retained_retrying) {
        portEXIT_CRITICAL(&s_retained_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_retained_retrying = true;
    portEXIT_CRITICAL(&s_retained_lock);

    retained_slot_t *slots = NULL;
    size_t capacity = RETAINED_CAPACITY_PSRAM;
    bool in_psram = true;
    slots = heap_caps_calloc(1, capacity * sizeof(*slots),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!slots) {
        capacity = RETAINED_CAPACITY_PSRAM_FALLBACK;
        slots = heap_caps_calloc(1, capacity * sizeof(*slots),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    /* Do not consume the display's already-tight internal heap from a late UI
     * retry.  The tiny internal fallback is safe only during early boot,
     * before LVGL and BLE have established their budgets. */

    esp_err_t result = slots ? ESP_OK : ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_retained_lock);
    /* Only one retry owns allocation, but preserve a concurrently restored
     * feed defensively instead of replacing it. */
    if (!s_retained_slots && slots) {
        s_retained_slots = slots;
        s_retained_capacity = capacity;
        s_retained_head = 0;
        s_retained_count = 0;
        s_retained_in_psram = in_psram;
        s_retained_generation++;
        slots = NULL;
    }
    s_retained_retrying = false;
    s_retained_last_error = s_retained_slots ? ESP_OK : result;
    result = s_retained_slots ? ESP_OK : result;
    portEXIT_CRITICAL(&s_retained_lock);
    free(slots);
    return result;
}

esp_err_t log_stream_retained_snapshot(
    log_stream_retained_line_t *lines,
    size_t line_capacity,
    const log_stream_retained_filter_t *filter,
    size_t *line_count,
    log_stream_retained_info_t *info)
{
    if (!line_count || (line_capacity > 0 && !lines) ||
        (filter && filter->order != LOG_STREAM_RETAINED_NEWEST_FIRST &&
         filter->order != LOG_STREAM_RETAINED_OLDEST_FIRST)) {
        return ESP_ERR_INVALID_ARG;
    }
    *line_count = 0;

    retained_bounds_t bounds;
    esp_err_t error = retained_read_bounds(&bounds, info);
    if (error != ESP_OK || line_capacity == 0 || bounds.count == 0) {
        return error;
    }

    /* Copying one bounded slot under the feed lock prevents torn PSRAM reads.
     * A slot can still age out between the metadata snapshot and its copy;
     * skip it rather than returning a newer line.  Generation lets the UI
     * request again. */
    *line_count = retained_snapshot_from_bounds(&bounds, lines, line_capacity,
                                                filter, NULL);
    return ESP_OK;
}

esp_err_t log_stream_retained_snapshot_page(
    log_stream_retained_line_t *lines,
    size_t line_capacity,
    const log_stream_retained_filter_t *filter,
    size_t match_offset,
    log_stream_retained_page_t *page,
    log_stream_retained_info_t *info)
{
    if (!page || (line_capacity > 0 && !lines) ||
        (filter && filter->order != LOG_STREAM_RETAINED_NEWEST_FIRST &&
         filter->order != LOG_STREAM_RETAINED_OLDEST_FIRST)) {
        return ESP_ERR_INVALID_ARG;
    }
    *page = (log_stream_retained_page_t) {
        .match_offset = match_offset,
    };

    retained_bounds_t bounds;
    esp_err_t error = retained_read_bounds(&bounds, info);
    if (error != ESP_OK || bounds.count == 0) return error;

    bool oldest_first = filter &&
        filter->order == LOG_STREAM_RETAINED_OLDEST_FIRST;
    size_t matched = 0;
    for (size_t offset = 0; offset < bounds.count; offset++) {
        size_t index;
        if (oldest_first) {
            index = (bounds.head + bounds.capacity - bounds.count + offset)
                % bounds.capacity;
        } else {
            index = (bounds.head + bounds.capacity - 1 - offset)
                % bounds.capacity;
        }

        log_stream_retained_line_t candidate;
        if (!retained_copy_slot(index, &candidate) ||
            !retained_line_is_in_bounds(&candidate, &bounds) ||
            !retained_filter_matches(&candidate, filter)) {
            continue;
        }

        if (matched >= match_offset && page->returned < line_capacity) {
            lines[page->returned++] = candidate;
            if (page->returned == 1) page->first_sequence = candidate.sequence;
            page->last_sequence = candidate.sequence;
        }
        matched++;
    }

    page->matching_count = matched;
    page->has_previous_page = match_offset > 0 && matched > 0;
    page->has_next_page = match_offset < matched &&
        page->returned < matched - match_offset;
    return ESP_OK;
}

esp_err_t log_stream_retained_clear(void)
{
    portENTER_CRITICAL(&s_retained_lock);
    if (!s_retained_slots || s_retained_capacity == 0) {
        esp_err_t error = s_retained_last_error == ESP_OK
            ? ESP_ERR_INVALID_STATE : s_retained_last_error;
        portEXIT_CRITICAL(&s_retained_lock);
        return error;
    }
    s_retained_head = 0;
    s_retained_count = 0;
    s_retained_generation++;
    s_retained_last_error = ESP_OK;
    portEXIT_CRITICAL(&s_retained_lock);
    return ESP_OK;
}

/**
 * Custom vprintf hook installed via esp_log_set_vprintf().
 *
 * 1. Forward to the original UART handler (so serial console keeps working).
 * 2. Render the formatted string into a scratch buffer.
 * 3. Push it into the ring buffer (best-effort, drop if full).
 * 4. Notify the SD flush task if enough data has accumulated.
 */

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Two consumers, two independent va_lists.
     *
     * A va_list is consumed by a vprintf-family call: after s_orig_vprintf()
     * returns, `args` is indeterminate and reusing it for vsnprintf() is
     * undefined behaviour, not merely unportable.  Each consumer therefore
     * gets its own va_copy, and the caller's list is never touched here. */
    va_list uart_args;
    va_copy(uart_args, args);
    int ret = s_orig_vprintf(fmt, uart_args);
    va_end(uart_args);

    /* Render into a stack-local scratch buffer. */
    char buf[LOG_LINE_MAX];
    va_list buf_args;
    va_copy(buf_args, args);
    int rendered_len = vsnprintf(buf, sizeof(buf), fmt, buf_args);
    va_end(buf_args);
    if (rendered_len <= 0) {
        return ret;
    }
    bool rendered_truncated = rendered_len >= (int)sizeof(buf);
    int len = rendered_len;
    if (len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    retained_capture_text(buf, (size_t)len, rendered_truncated);

    /* Push to SSE ring buffer (best-effort, drop if full). */
    if (s_ringbuf) {
        xRingbufferSend(s_ringbuf, buf, (size_t)len, 0);
    }

    /* Push to SD write buffer (best-effort, drop if full or mutex busy). */
    if (s_writebuf && xSemaphoreTake(s_writebuf_mutex, 0) == pdTRUE) {
        size_t avail = WRITEBUF_SIZE - (s_writebuf_head - s_writebuf_tail);
        if ((size_t)len <= avail) {
            size_t pos = s_writebuf_head % WRITEBUF_SIZE;
            size_t chunk1 = WRITEBUF_SIZE - pos;
            if ((size_t)len <= chunk1) {
                memcpy(s_writebuf + pos, buf, (size_t)len);
            } else {
                memcpy(s_writebuf + pos, buf, chunk1);
                memcpy(s_writebuf, buf + chunk1, (size_t)len - chunk1);
            }
            s_writebuf_head += (size_t)len;
        }
        xSemaphoreGive(s_writebuf_mutex);
    }

    /* Wake the SD flush task if enough data has accumulated. */
    if (s_flush_task && s_writebuf_head - s_writebuf_tail >= FLUSH_THRESHOLD) {
        xTaskNotifyGive(s_flush_task);
    }

    return ret;
}

/* ── SD Persistent Logging ───────────────────────────────────────── */

/* Ensure the log directory exists on the SD card. */
static void ensure_log_dir(void)
{
    if (!sd_storage_is_ready()) return;
    mkdir(SD_APP_DIR, 0777);  /* parent .somnotrace/ (non-recursive mkdir) */
    mkdir(LOG_DIR, 0777);     /* ignore EEXIST */
    s_sd_ready = true;
}

/* Rotate log files: delete oldest, shift others up, create new .0 */
static void rotate_logs(void)
{
    char path[64];

    /* Delete the oldest file (somnotrace.log.2) */
    snprintf(path, sizeof(path), "%s/%s%d", LOG_DIR, LOG_FILE_PREFIX, LOG_MAX_FILES - 1);
    remove(path);

    /* Shift: .1 -> .2, .0 -> .1 */
    for (int i = LOG_MAX_FILES - 2; i >= 0; i--) {
        char old_path[64], new_path[64];
        snprintf(old_path, sizeof(old_path), "%s/%s%d", LOG_DIR, LOG_FILE_PREFIX, i);
        snprintf(new_path, sizeof(new_path), "%s/%s%d", LOG_DIR, LOG_FILE_PREFIX, i + 1);
        rename(old_path, new_path);
    }
}

/* Get current log file size. */
static long log_file_size(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s0", LOG_DIR, LOG_FILE_PREFIX);
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return 0;
}

/* Advance only the prefix which stdio accepted.  Keeping the unwritten suffix
 * behind tail makes a short/error write retryable on the next flush instead of
 * silently dropping it.  The flush task is the sole tail writer. */
static void writebuf_acknowledge(size_t written)
{
    if (!written || !s_writebuf_mutex) return;
    xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
    size_t pending = s_writebuf_head - s_writebuf_tail;
    if (written > pending) written = pending;
    s_writebuf_tail += written;
    xSemaphoreGive(s_writebuf_mutex);
}

static void log_flush_once(void)
{
    if (!s_writebuf || !s_writebuf_mutex || !sd_storage_is_ready()) return;

    /* Persistent logs are independent of raw session files, so use EXPORT:
     * this serialises rotation/open/write against card readers and destructive
     * maintenance while remaining explicitly allowed during therapy. */
    if (!sd_storage_lease_acquire(SD_LEASE_EXPORT, 250)) return;

    FILE *file = NULL;
    if (!sd_storage_is_ready()) goto done;
    if (!s_sd_ready) {
        ensure_log_dir();
        if (!s_sd_ready) goto done;
    }

    size_t available;
    xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
    available = s_writebuf_head - s_writebuf_tail;
    xSemaphoreGive(s_writebuf_mutex);
    if (available == 0) goto done;

    long current_size = log_file_size();
    if (current_size >= LOG_FILE_MAX_SIZE) {
        rotate_logs();
        current_size = 0;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s0", LOG_DIR, LOG_FILE_PREFIX);
    file = fopen(path, "ab");
    if (!file) {
        ESP_LOGE(TAG, "flush: cannot open %s (errno %d)", path, errno);
        goto done;
    }

    uint8_t temporary[LOG_LINE_MAX];
    size_t total_written = 0;
    while (true) {
        xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
        size_t pending = s_writebuf_head - s_writebuf_tail;
        if (pending == 0) {
            xSemaphoreGive(s_writebuf_mutex);
            break;
        }
        size_t position = s_writebuf_tail % WRITEBUF_SIZE;
        size_t chunk = WRITEBUF_SIZE - position;
        if (chunk > pending) chunk = pending;
        if (chunk > sizeof(temporary)) chunk = sizeof(temporary);
        memcpy(temporary, s_writebuf + position, chunk);
        xSemaphoreGive(s_writebuf_mutex);

        size_t written = fwrite(temporary, 1, chunk, file);
        writebuf_acknowledge(written);
        total_written += written;
        if (written != chunk) {
            ESP_LOGW(TAG, "flush: short write (%u/%u, errno %d); retaining suffix",
                     (unsigned)written, (unsigned)chunk, errno);
            break;
        }

        if (current_size + (long)total_written >= LOG_FILE_MAX_SIZE) {
            if (fclose(file) != 0) {
                ESP_LOGW(TAG, "flush: close before rotation failed (errno %d)",
                         errno);
                file = NULL;
                break;
            }
            file = NULL;
            rotate_logs();
            current_size = 0;
            total_written = 0;
            file = fopen(path, "ab");
            if (!file) {
                ESP_LOGE(TAG, "flush: cannot reopen %s (errno %d)", path,
                         errno);
                break;
            }
        }
    }

done:
    if (file && fclose(file) != 0)
        ESP_LOGW(TAG, "flush: close failed (errno %d)", errno);
    sd_storage_lease_release_unchanged(SD_LEASE_EXPORT);
}

/* Background flush task: drains write buffer to SD card. */
static void log_flush_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_INTERVAL_MS));
        log_flush_once();

        /* One-shot high-water mark after the first flush attempt verifies the
         * 4 KB PSRAM stack remains sufficient for the FATFS write path. */
        static bool hwm_logged = false;
        if (!hwm_logged) {
            hwm_logged = true;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "log_flush: stack high-water = %u bytes",
                     (unsigned)(hwm * sizeof(StackType_t)));
        }
    }
}

/* ── Initialisation ───────────────────────────────────────────────── */

/* Publish a completed retained-log snapshot without first discarding the last
 * good one.  The backup also makes a reset between the two renames recoverable
 * on the next Save attempt. */
static esp_err_t retained_publish_snapshot(const char *temporary_path,
                                           const char *final_path,
                                           const char *backup_path)
{
    struct stat status;
    errno = 0;
    bool final_exists = stat(final_path, &status) == 0;
    if (!final_exists && errno != ENOENT) return ESP_FAIL;

    errno = 0;
    bool backup_exists = stat(backup_path, &status) == 0;
    if (!backup_exists && errno != ENOENT) return ESP_FAIL;

    /* Recover an interrupted earlier publication before starting a new one.
     * If both names exist, final is already the committed copy and the stale
     * backup can be discarded. */
    if (backup_exists) {
        if (final_exists) {
            if (remove(backup_path) != 0) return ESP_FAIL;
        } else {
            if (rename(backup_path, final_path) != 0) return ESP_FAIL;
            final_exists = true;
        }
    }

    bool moved_previous = false;
    if (final_exists) {
        if (rename(final_path, backup_path) != 0) return ESP_FAIL;
        moved_previous = true;
    }

    if (rename(temporary_path, final_path) != 0) {
        int publish_errno = errno;
        if (moved_previous && rename(backup_path, final_path) != 0) {
            /* Keep the backup name intact if rollback itself fails. A future
             * Save can recover it rather than deleting the last good bytes. */
            ESP_LOGE(TAG, "retained save rollback failed (errno %d)", errno);
        }
        errno = publish_errno;
        return ESP_FAIL;
    }

    if (moved_previous && remove(backup_path) != 0 && errno != ENOENT) {
        /* The new final is already complete. A stale backup is harmless and is
         * reconciled by the next call, so do not misreport this save as lost. */
        ESP_LOGW(TAG, "retained save left stale backup (errno %d)", errno);
    }
    return ESP_OK;
}

esp_err_t log_stream_retained_save_to_sd(
    const log_stream_retained_filter_t *filter,
    char *saved_path,
    size_t saved_path_size,
    size_t *saved_line_count,
    log_stream_retained_progress_fn progress_fn,
    void *progress_ctx)
{
    if ((saved_path && saved_path_size == 0) ||
        (!saved_path && saved_path_size != 0) ||
        (filter && filter->order != LOG_STREAM_RETAINED_NEWEST_FIRST &&
         filter->order != LOG_STREAM_RETAINED_OLDEST_FIRST)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (saved_path) saved_path[0] = '\0';
    if (saved_line_count) *saved_line_count = 0;

    const size_t final_path_len = strlen(LOG_DIR) + 1 +
        strlen(RETAINED_SAVE_FILE);
    if (saved_path && saved_path_size <= final_path_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    log_stream_retained_info_t info;
    esp_err_t error = log_stream_retained_get_info(&info);
    if (error != ESP_OK) return error;
    if (!sd_storage_is_ready()) {
        retained_set_last_error(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lease_acquire(SD_LEASE_EXPORT, 5000)) {
        retained_set_last_error(ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }

    char final_path[96];
    char tmp_path[100];
    char backup_path[100];
    snprintf(final_path, sizeof(final_path), "%s/%s", LOG_DIR,
             RETAINED_SAVE_FILE);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s", LOG_DIR,
             RETAINED_SAVE_TMP_FILE);
    snprintf(backup_path, sizeof(backup_path), "%s/%s", LOG_DIR,
             RETAINED_SAVE_BACKUP_FILE);

    FILE *file = NULL;
    bool tmp_exists = false;
    size_t written_lines = 0;

    if (!sd_storage_is_ready()) {
        error = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if ((mkdir(SD_APP_DIR, 0777) != 0 && errno != EEXIST) ||
        (mkdir(LOG_DIR, 0777) != 0 && errno != EEXIST)) {
        error = ESP_FAIL;
        goto done;
    }

    retained_bounds_t bounds;
    error = retained_read_bounds(&bounds, NULL);
    if (error != ESP_OK) goto done;
    if (progress_fn) progress_fn(0, bounds.count, progress_ctx);

    /* A temporary sibling prevents a failed/partial write from replacing the
     * last successful touchscreen export.  Capture remains available while
     * FATFS is busy; if the bounded ring wraps over this exact snapshot, fail
     * explicitly and let the UI offer Retry instead of publishing a partial
     * file as complete. */
    remove(tmp_path);
    file = fopen(tmp_path, "wb");
    if (!file) {
        error = ESP_FAIL;
        goto done;
    }
    tmp_exists = true;

    log_stream_retained_filter_t chronological = {0};
    if (filter) chronological = *filter;
    chronological.order = LOG_STREAM_RETAINED_OLDEST_FIRST;

    for (size_t offset = 0; offset < bounds.count; offset++) {
        size_t index = (bounds.head + bounds.capacity - bounds.count + offset)
            % bounds.capacity;
        log_stream_retained_line_t line;
        if (!retained_copy_slot(index, &line) ||
            !retained_line_is_in_bounds(&line, &bounds)) {
            error = ESP_ERR_INVALID_STATE;
            goto done;
        }
        if (retained_filter_matches(&line, &chronological)) {
            if ((line.length > 0 &&
                 fwrite(line.text, 1, line.length, file) != line.length) ||
                fwrite("\n", 1, 1, file) != 1) {
                error = ESP_FAIL;
                goto done;
            }
            written_lines++;
        }
        if (progress_fn) progress_fn(offset + 1, bounds.count, progress_ctx);
    }

    /* Filtering can skip the final slot's write; progress still describes the
     * bounded snapshot scan rather than only matching output lines. */
    if (progress_fn && bounds.count == 0)
        progress_fn(0, 0, progress_ctx);

    bool close_failed = fflush(file) != 0;
    if (fclose(file) != 0) close_failed = true;
    file = NULL;
    if (close_failed) {
        error = ESP_FAIL;
        goto done;
    }

    error = retained_publish_snapshot(tmp_path, final_path, backup_path);
    if (error != ESP_OK) goto done;
    tmp_exists = false;
    error = ESP_OK;
    if (progress_fn) progress_fn(bounds.count, bounds.count, progress_ctx);

done:
    if (file && fclose(file) != 0 && error == ESP_OK) error = ESP_FAIL;
    if (tmp_exists) remove(tmp_path);
    sd_storage_lease_release_unchanged(SD_LEASE_EXPORT);
    retained_set_last_error(error);

    if (error == ESP_OK) {
        if (saved_path) snprintf(saved_path, saved_path_size, "%s", final_path);
        if (saved_line_count) *saved_line_count = written_lines;
    }
    return error;
}

esp_err_t log_stream_init(void)
{
    if (s_init_attempted) return s_init_result;
    s_init_attempted = true;
    s_init_result = ESP_ERR_NO_MEM;

    /* Prefer PSRAM (larger buffer) if available, else internal RAM. */
    size_t buf_sz = 0;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > RINGBUF_SIZE_PSRAM * 2) {
        s_ringbuf = xRingbufferCreateWithCaps(RINGBUF_SIZE_PSRAM,
                                              RINGBUF_TYPE_BYTEBUF,
                                              MALLOC_CAP_SPIRAM);
        buf_sz = RINGBUF_SIZE_PSRAM;
    } else {
        s_ringbuf = xRingbufferCreateWithCaps(RINGBUF_SIZE_INTERNAL,
                                              RINGBUF_TYPE_BYTEBUF,
                                              MALLOC_CAP_INTERNAL);
        buf_sz = RINGBUF_SIZE_INTERNAL;
    }
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "failed to create log ring buffer");
        return s_init_result;
    }

    retained_init();

    /* Create this before installing the hook or exposing handlers. Every WS
     * entry point also checks it, so a degraded boot never passes NULL into a
     * FreeRTOS semaphore primitive. */
    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex)
        ESP_LOGE(TAG, "failed to create WebSocket mutex; live WS disabled");

    /* Allocate SD write buffer in PSRAM. */
    s_writebuf = heap_caps_malloc(WRITEBUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_writebuf) {
        s_writebuf = heap_caps_malloc(WRITEBUF_SIZE, MALLOC_CAP_INTERNAL);
    }
    if (s_writebuf) {
        s_writebuf_mutex = xSemaphoreCreateMutex();
        if (!s_writebuf_mutex) {
            free(s_writebuf);
            s_writebuf = NULL;
            ESP_LOGW(TAG, "failed to create write buffer mutex, SD logging disabled");
        }
    } else {
        ESP_LOGW(TAG, "failed to allocate write buffer, SD logging disabled");
    }

    /* Install our hook; stash the original handler. */
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);

    /* Start the SD flush task (low priority, core 0). If task creation fails,
     * release its otherwise-unserviceable buffer instead of pinning 8 KiB for
     * the rest of the boot. */
    if (s_writebuf) {
        s_flush_task = psram_task_create(log_flush_task, "log_flush", 4096, NULL, 3, 0, NULL, NULL);
        if (!s_flush_task) {
            vSemaphoreDelete(s_writebuf_mutex);
            s_writebuf_mutex = NULL;
            free(s_writebuf);
            s_writebuf = NULL;
            ESP_LOGE(TAG, "failed to create log flush task; SD logging disabled");
        }
    }

    ESP_LOGI(TAG, "log stream init: %u-byte ring buffer (%s), %s SD logging, "
             "%u-line retained feed (%s)",
             (unsigned)buf_sz,
             buf_sz == RINGBUF_SIZE_PSRAM ? "PSRAM" : "internal",
             s_flush_task ? "with" : "without",
             (unsigned)s_retained_capacity,
             s_retained_in_psram ? "PSRAM" :
                 s_retained_slots ? "internal" : "unavailable");

    bool complete = s_ringbuf && s_retained_slots && s_writebuf &&
                    s_writebuf_mutex && s_flush_task && s_ws_mutex;
    s_init_result = complete ? ESP_OK : ESP_ERR_NO_MEM;
    if (!complete)
        ESP_LOGW(TAG, "log stream started in degraded mode (%s)",
                 esp_err_to_name(s_init_result));
    return s_init_result;
}

/* Check if any non-paused WebSocket client is connected.
 * Used by the forwarder loop to skip building JSON when all clients
 * are paused (e.g. background tabs).  Log messages bypass this check. */
static bool ws_has_active_client(void)
{
    bool active = false;
    if (!s_ws_mutex) return false;
    if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
        for (int i = 0; i < s_ws_client_count; i++) {
            if (!s_ws_clients[i].paused) {
                active = true;
                break;
            }
        }
        xSemaphoreGive(s_ws_mutex);
    }
    return active;
}

/* Push-now flags — set by external event sources, consumed by the forwarder
 * task.  Volatile because they are written from other tasks/contexts. */
static volatile bool s_push_status_now = false;
static volatile bool s_push_upload_now = false;
static volatile bool s_push_ble_now    = false;
static volatile bool s_push_ox_now     = false;

/* Request an immediate upload-progress push on the next forwarder cycle
 * (e.g. on a backend state transition).  Safe to call from any task. */
void log_stream_request_upload_push(void)
{
    s_push_upload_now = true;
}

/* Request an immediate BLE-state push on the next forwarder cycle
 * (e.g. on a pairing state change).  Safe to call from any task. */
void log_stream_request_ble_push(void)
{
    s_push_ble_now = true;
}

/* Request an immediate oximeter-state push on the next forwarder cycle
 * (e.g. on a pairing or sync state change).  Safe to call from any task. */
void log_stream_request_ox_push(void)
{
    s_push_ox_now = true;
}

static esp_err_t ws_queue_send(httpd_handle_t hd, int fd, uint8_t type,
                               const char *payload, size_t len);

static void ws_remove_client_locked(int fd)
{
    for (int i = 0; i < s_ws_client_count; i++) {
        if (s_ws_clients[i].fd == fd) {
            for (int j = i; j < s_ws_client_count - 1; j++) {
                s_ws_clients[j] = s_ws_clients[j + 1];
            }
            s_ws_client_count--;
            break;
        }
    }
}

static void ws_add_client_locked(httpd_handle_t hd, int fd)
{
    for (int i = 0; i < s_ws_client_count; i++) {
        if (s_ws_clients[i].fd == fd) return;
    }
    if (s_ws_client_count >= MAX_WS_CLIENTS) {
        httpd_handle_t old_hd = s_ws_clients[0].hd;
        int old_fd = s_ws_clients[0].fd;
        ESP_LOGI(TAG, "ws: evicting oldest client (fd=%d) for new client (fd=%d)", old_fd, fd);

        const char *evict_msg = "{\"type\":\"evicted\",\"reason\":\"max_clients_exceeded\"}";
        ws_queue_send(old_hd, old_fd, HTTPD_WS_TYPE_TEXT, evict_msg,
                      strlen(evict_msg));
        ws_queue_send(old_hd, old_fd, HTTPD_WS_TYPE_CLOSE, NULL, 0);

        for (int i = 0; i < s_ws_client_count - 1; i++) {
            s_ws_clients[i] = s_ws_clients[i + 1];
        }
        s_ws_client_count--;
    }
    s_ws_clients[s_ws_client_count].hd = hd;
    s_ws_clients[s_ws_client_count].fd = fd;
    s_ws_clients[s_ws_client_count].paused = false;
    s_ws_client_count++;
    ESP_LOGI(TAG, "ws: client connected (fd=%d, total=%d)", fd, s_ws_client_count);
}

typedef struct {
    httpd_handle_t hd;
    int fd;
    uint8_t type;
    char *payload;
    size_t len;
} ws_send_work_t;

static void ws_send_work(void *arg)
{
    ws_send_work_t *work = arg;
    if (!work) return;
    httpd_ws_frame_t pkt = {
        .final = true,
        .type = work->type,
        .payload = (uint8_t *)work->payload,
        .len = work->len,
    };
    esp_err_t err = httpd_ws_send_frame_async(work->hd, work->fd, &pkt);
    if (s_ws_mutex && err != ESP_OK && err != ESP_ERR_TIMEOUT &&
        err != ESP_ERR_NO_MEM) {
        if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
            ws_remove_client_locked(work->fd);
            xSemaphoreGive(s_ws_mutex);
        }
    }
    free(work->payload);
    free(work);
}

static esp_err_t ws_queue_send(httpd_handle_t hd, int fd, uint8_t type,
                               const char *payload, size_t len)
{
    ws_send_work_t *work = heap_caps_calloc(1, sizeof(*work), MALLOC_CAP_SPIRAM);
    if (!work) work = calloc(1, sizeof(*work));
    if (!work) return ESP_ERR_NO_MEM;
    if (len > 0) {
        work->payload = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!work->payload) work->payload = malloc(len);
        if (!work->payload) { free(work); return ESP_ERR_NO_MEM; }
        memcpy(work->payload, payload, len);
    }
    work->hd = hd; work->fd = fd; work->type = type; work->len = len;
    esp_err_t err = httpd_queue_work(hd, ws_send_work, work);
    if (err != ESP_OK) {
        free(work->payload);
        free(work);
    }
    return err;
}

static esp_err_t ws_send_frame_internal(const char *payload_str, size_t len,
                                        bool skip_paused)
{
    if (!s_ws_mutex) return ESP_ERR_INVALID_STATE;
    if (!payload_str && len > 0) return ESP_ERR_INVALID_ARG;

    ws_client_t clients[MAX_WS_CLIENTS];
    int count = 0;

    if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = s_ws_client_count;
        if (count > 0) {
            memcpy(clients, s_ws_clients, sizeof(ws_client_t) * count);
        }
        xSemaphoreGive(s_ws_mutex);
    }
    if (count <= 0) return ESP_OK;

    for (int i = 0; i < count; i++) {
        /* Skip paused clients for non-log messages (per-client pause). */
        if (skip_paused && clients[i].paused)
            continue;
        esp_err_t err = ws_queue_send(clients[i].hd, clients[i].fd,
                                      HTTPD_WS_TYPE_TEXT, payload_str, len);
        if (err != ESP_OK) {
            bool transient = (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NO_MEM);
            if (!transient) {
                ESP_LOGW(TAG, "ws: send failed (err=0x%x), removing fd=%d", err, clients[i].fd);
                if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
                    ws_remove_client_locked(clients[i].fd);
                    xSemaphoreGive(s_ws_mutex);
                }
            }
        }
    }
    return ESP_OK;
}

esp_err_t log_stream_ws_send_json(const char *type, cJSON *data_obj)
{
    if (!type) {
        if (data_obj) cJSON_Delete(data_obj);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ringbuf || !s_ws_mutex) {
        if (data_obj) cJSON_Delete(data_obj);
        return ESP_ERR_INVALID_STATE;
    }

    bool is_log = (strcmp(type, "log") == 0);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        if (data_obj) cJSON_Delete(data_obj);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", type);
    if (data_obj) {
        cJSON_AddItemToObject(root, "data", data_obj);
    }
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return ESP_ERR_NO_MEM;

    esp_err_t err = ws_send_frame_internal(str, strlen(str), !is_log);
    free(str);
    return err;
}

esp_err_t log_stream_ws_send_json_raw(const char *type, const char *data_json_str)
{
    if (!type) return ESP_ERR_INVALID_ARG;
    if (!s_ringbuf || !s_ws_mutex) return ESP_ERR_INVALID_STATE;

    bool is_log = (strcmp(type, "log") == 0);
    size_t type_len = strlen(type);
    size_t data_len = data_json_str ? strlen(data_json_str) : 4;
    size_t total = type_len + data_len + 32;
    char *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;

    int len = snprintf(buf, total, "{\"type\":\"%s\",\"data\":%s}", type, data_json_str ? data_json_str : "null");
    esp_err_t err = ws_send_frame_internal(buf, len, !is_log);
    free(buf);
    return err;
}

/* psram_task_create() may schedule the new task before it returns its handle
 * to the creator.  Keep the task behind this tiny publication gate so every
 * self-exit can reliably clear the exact handle that was published. */
static bool ws_forwarder_wait_for_publication(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    while (true) {
        bool published;
        bool abandoned;
        portENTER_CRITICAL(&s_ws_task_lock);
        published = s_ws_fwd_task == self;
        abandoned = !s_ws_fwd_starting && !published;
        portEXIT_CRITICAL(&s_ws_task_lock);
        if (published) return true;
        if (abandoned) return false;
        vTaskDelay(1);
    }
}

static void ws_forwarder_clear_current(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_ws_task_lock);
    if (s_ws_fwd_task == self) s_ws_fwd_task = NULL;
    portEXIT_CRITICAL(&s_ws_task_lock);
}

static void ws_forwarder_exit(void)
{
    ws_forwarder_clear_current();
    psram_task_delete(NULL);
}

static void ws_forwarder_task(void *arg);

static esp_err_t ws_forwarder_start(void)
{
    bool create = false;
    portENTER_CRITICAL(&s_ws_task_lock);
    if (!s_ws_fwd_task && !s_ws_fwd_starting) {
        s_ws_fwd_starting = true;
        create = true;
    }
    portEXIT_CRITICAL(&s_ws_task_lock);
    if (!create) return ESP_OK;

    TaskHandle_t task = psram_task_create(ws_forwarder_task, "ws_fwd", 12288,
                                          NULL, 3, 0, NULL, NULL);
    portENTER_CRITICAL(&s_ws_task_lock);
    if (task) s_ws_fwd_task = task;
    s_ws_fwd_starting = false;
    portEXIT_CRITICAL(&s_ws_task_lock);
    return task ? ESP_OK : ESP_ERR_NO_MEM;
}

static void ws_forwarder_task(void *arg)
{
    (void)arg;
    if (!ws_forwarder_wait_for_publication() || !s_ringbuf || !s_ws_mutex) {
        ws_forwarder_exit();
        return;
    }

    char *frame_buf = heap_caps_malloc(LOG_LINE_MAX * 16, MALLOC_CAP_SPIRAM);
    if (!frame_buf) {
        ESP_LOGE(TAG, "ws_fwd: failed to allocate frame buffer");
        ws_forwarder_exit();
        return;
    }
    size_t frame_cap = LOG_LINE_MAX * 16;
    size_t frame_pos = 0;
    const TickType_t status_interval = pdMS_TO_TICKS(3000);
    const TickType_t upload_interval = pdMS_TO_TICKS(5000);
    TickType_t last_status_tick = xTaskGetTickCount();
    TickType_t last_upload_tick = xTaskGetTickCount();
    bool hwm_logged = false;

    while (true) {
        int client_count = 0;
        if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            client_count = s_ws_client_count;
            xSemaphoreGive(s_ws_mutex);
        }

        /* One-shot high-water mark after the first status push to verify
         * the 12 KB PSRAM stack is sufficient for the JSON build path. */
        if (!hwm_logged && client_count > 0) {
            hwm_logged = true;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "ws_fwd: stack high-water = %u bytes",
                     (unsigned)(hwm * sizeof(StackType_t)));
        }

        if (client_count <= 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            frame_pos = 0;
            last_status_tick = xTaskGetTickCount();
            last_upload_tick = xTaskGetTickCount();
            s_push_status_now = false;
            s_push_upload_now = false;
            s_push_ble_now    = false;
            s_push_ox_now     = false;
            continue;
        }

        /* Status push: periodic (~3 s) or on-demand (resume). */
        TickType_t now = xTaskGetTickCount();
        if (ws_has_active_client() &&
            ((now - last_status_tick) >= status_interval || s_push_status_now)) {
            last_status_tick = now;
            s_push_status_now = false;
            cJSON *st = netprov_build_status_json();
            if (st) {
                log_stream_ws_send_json("status", st);
            }
        }

        /* Upload progress push: periodic (~5 s) or on-demand (transition). */
        if (ws_has_active_client() &&
            ((now - last_upload_tick) >= upload_interval || s_push_upload_now)) {
            last_upload_tick = now;
            s_push_upload_now = false;
            char *up_json = NULL;
            if (uploader_get_progress_json(&up_json) == ESP_OK && up_json) {
                log_stream_ws_send_json_raw("upload", up_json);
                free(up_json);
            }
        }

        /* BLE state push: event-driven only (no periodic polling). */
        if (ws_has_active_client() && s_push_ble_now) {
            s_push_ble_now = false;
            cJSON *ble = cJSON_CreateObject();
            if (ble) {
                cJSON_AddStringToObject(ble, "state", as11_ble_get_status());
                cJSON_AddStringToObject(ble, "error", as11_ble_get_error());
                cJSON_AddBoolToObject(ble, "paired", as11_ble_is_paired());
                if (as11_ble_is_paired()) {
                    cJSON *info = as11_ble_get_paired_info();
                    if (info) {
                        cJSON_AddItemToObject(ble, "device", info);
                    }
                }
                log_stream_ws_send_json("ble", ble);
            }
        }

        /* Oximeter state push: event-driven only (no periodic polling). */
        if (ws_has_active_client() && s_push_ox_now) {
            s_push_ox_now = false;
            cJSON *ox = cJSON_CreateObject();
            if (ox) {
                cJSON_AddStringToObject(ox, "state", oximeter_get_status());
                cJSON_AddStringToObject(ox, "error", oximeter_get_error());
                cJSON_AddBoolToObject(ox, "paired", oximeter_is_paired());
                if (oximeter_is_paired()) {
                    cJSON *info = oximeter_get_paired_info();
                    if (info) {
                        cJSON_AddItemToObject(ox, "device", info);
                    }
                }
                log_stream_ws_send_json("oximeter", ox);
            }
        }

        /* Try to drain available ring-buffer data (non-blocking). */
        size_t item_sz = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz, 0, LOG_LINE_MAX);
        if (item) {
            size_t copy_len = item_sz;
            if (frame_pos + copy_len > frame_cap - 1) {
                copy_len = frame_cap - 1 - frame_pos;
            }
            memcpy(frame_buf + frame_pos, item, copy_len);
            frame_pos += copy_len;
            vRingbufferReturnItem(s_ringbuf, item);

            if (frame_pos >= 256) {
                frame_buf[frame_pos] = '\0';
                cJSON *cjs = cJSON_CreateString(frame_buf);
                if (cjs) {
                    log_stream_ws_send_json("log", cjs);
                }
                frame_pos = 0;
            }
        } else {
            if (frame_pos > 0) {
                frame_buf[frame_pos] = '\0';
                cJSON *cjs = cJSON_CreateString(frame_buf);
                if (cjs) {
                    log_stream_ws_send_json("log", cjs);
                }
                frame_pos = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static esp_err_t logs_ws_handler(httpd_req_t *req)
{
    if (!s_ringbuf || !s_ws_mutex) {
        if (req->method == HTTP_GET) httpd_resp_send_500(req);
        return ESP_ERR_INVALID_STATE;
    }

    if (req->method == HTTP_GET) {
        if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            ws_add_client_locked(req->handle, httpd_req_to_sockfd(req));
            xSemaphoreGive(s_ws_mutex);
        }

        esp_err_t start_error = ws_forwarder_start();
        if (start_error != ESP_OK) {
            if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                ws_remove_client_locked(httpd_req_to_sockfd(req));
                xSemaphoreGive(s_ws_mutex);
            }
            return start_error;
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t buf[32] = {0};
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    if (httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf) - 1) != ESP_OK ||
        ws_pkt.len >= sizeof(buf)) {
        return ESP_FAIL;
    }

    int req_fd = httpd_req_to_sockfd(req);

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_pkt.len > 0) {
        buf[ws_pkt.len] = '\0';
        if (strncmp((char *)buf, "pause", 5) == 0) {
            if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
                for (int i = 0; i < s_ws_client_count; i++) {
                    if (s_ws_clients[i].fd == req_fd) {
                        s_ws_clients[i].paused = true;
                        break;
                    }
                }
                xSemaphoreGive(s_ws_mutex);
            }
        } else if (strncmp((char *)buf, "resume", 6) == 0) {
            if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
                for (int i = 0; i < s_ws_client_count; i++) {
                    if (s_ws_clients[i].fd == req_fd) {
                        s_ws_clients[i].paused = false;
                        break;
                    }
                }
                xSemaphoreGive(s_ws_mutex);
            }
            /* Defer the status push to the forwarder task (single-writer model). */
            s_push_status_now = true;
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
            ws_remove_client_locked(req_fd);
            xSemaphoreGive(s_ws_mutex);
        }
        ESP_LOGI(TAG, "ws: client closed connection (fd=%d)", req_fd);
    }
    return ESP_OK;
}

/* ── Polling Endpoint: GET /api/logs/recent ───────────────────────── */
/* Replaces the old SSE /api/logs/stream endpoint.  Each request drains
 * available ring-buffer data and returns it as a JSON array, then closes
 * immediately — no long-lived connection, no blocking of the single httpd
 * worker task.  The frontend polls this every 1-2 seconds.
 *
 * Optional query parameter: ?since=<cursor>  — returns only lines produced
 * after the given cursor value (monotonic counter).  Omit or pass 0 to get
 * all currently-buffered lines.  Response includes the new cursor value so
 * the client can pass it on the next poll.
 *
 * Response format:
 *   {"lines":["line1","line2",...],"cursor":N}
 */

static esp_err_t logs_recent_handler(httpd_req_t *req)
{
    /* Parse optional ?since=<cursor> query parameter. */
    int since = 0;
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = atoi(val);
            if (since < 0) since = 0;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!s_ringbuf) {
        httpd_resp_send_500(req);
        return ESP_ERR_INVALID_STATE;
    }

    /* Build the entire JSON response in a single buffer to avoid
     * hundreds of tiny chunked sends (each one a socket write that
     * generates ENOTCONN spam if the client has disconnected). */
    char *buf = heap_caps_malloc(LOGS_RECENT_RESPONSE_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t cap = LOGS_RECENT_RESPONSE_CAP;
    size_t pos = 0;

    static const char prefix[] = "{\"lines\":[";
    memcpy(buf, prefix, sizeof(prefix) - 1);
    pos = sizeof(prefix) - 1;

    int local_cursor = since;
    int chunks_sent = 0;
    bool response_full = false;

    /* Drain available ring-buffer data, non-blocking. */
    while (s_ringbuf && !response_full) {
        size_t item_sz = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz, 0, LOG_LINE_MAX);
        if (!item) break;

        if (local_cursor < INT_MAX) local_cursor++;

        /* Split chunk into individual lines on '\n' and emit as JSON strings. */
        const char *p   = (const char *)item;
        const char *end = p + item_sz;

        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

            /* Strip trailing CR from CRLF lines. */
            if (line_len > 0 && p[line_len - 1] == '\r') {
                line_len--;
            }

            if (line_len > 0) {
                /* Size the complete escaped line before writing anything.
                 * A response that fills mid-line is not valid JSON.  Keep a
                 * suffix reserve at all times so snprintf can never report a
                 * length beyond the allocation passed to httpd_resp_send. */
                size_t escaped_len = 0;
                for (size_t i = 0; i < line_len; i++) {
                    unsigned char c = (unsigned char)p[i];
                    escaped_len += (c == '\\' || c == '"') ? 2u :
                                   (c < 0x20) ? 6u : 1u;
                }
                size_t needed = escaped_len + 2u + (chunks_sent > 0 ? 1u : 0u);
                if (needed + LOGS_RECENT_SUFFIX_RESERVE > cap - pos) {
                    response_full = true;
                    break;
                }

                if (chunks_sent > 0) buf[pos++] = ',';
                buf[pos++] = '"';

                /* Escape JSON-special characters. */
                static const char hex[] = "0123456789abcdef";
                for (size_t i = 0; i < line_len; i++) {
                    unsigned char c = (unsigned char)p[i];
                    if (c == '\\' || c == '"') {
                        buf[pos++] = '\\';
                        buf[pos++] = (char)c;
                    } else if (c < 0x20) {
                        buf[pos++] = '\\';
                        buf[pos++] = 'u';
                        buf[pos++] = '0';
                        buf[pos++] = '0';
                        buf[pos++] = hex[c >> 4];
                        buf[pos++] = hex[c & 0x0f];
                    } else {
                        buf[pos++] = (char)c;
                    }
                }

                buf[pos++] = '"';
                chunks_sent++;
            }

            p = nl ? nl + 1 : end;
        }

        /* Every receive must have exactly one return, including the bounded
         * response path above. */
        vRingbufferReturnItem(s_ringbuf, item);
    }

    int suffix_len = snprintf(buf + pos, cap - pos,
                              "],\"cursor\":%d}", local_cursor);
    if (suffix_len < 0 || (size_t)suffix_len >= cap - pos) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    pos += (size_t)suffix_len;

    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

/* ── History Endpoint: GET /api/logs/history ─────────────────────── */

/* Copy pending bytes while the producer is locked, then release the mutex
 * before any socket write.  When called while holding the EXPORT lease the
 * flush task cannot advance tail, so the SD-file prefix and this snapshot are
 * one consistent archive view. */
static esp_err_t snapshot_pending_log_bytes(uint8_t **bytes, size_t *length)
{
    if (!bytes || !length) return ESP_ERR_INVALID_ARG;
    *bytes = NULL;
    *length = 0;
    if (!s_writebuf || !s_writebuf_mutex) return ESP_OK;

    uint8_t *copy = heap_caps_malloc(WRITEBUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!copy) copy = malloc(WRITEBUF_SIZE);
    if (!copy) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
    size_t pending = s_writebuf_head - s_writebuf_tail;
    if (pending > WRITEBUF_SIZE) pending = WRITEBUF_SIZE;
    if (pending > 0) {
        size_t position = s_writebuf_tail % WRITEBUF_SIZE;
        size_t first = WRITEBUF_SIZE - position;
        if (first > pending) first = pending;
        memcpy(copy, s_writebuf + position, first);
        if (pending > first)
            memcpy(copy + first, s_writebuf, pending - first);
    }
    xSemaphoreGive(s_writebuf_mutex);

    if (pending == 0) {
        free(copy);
        return ESP_OK;
    }
    *bytes = copy;
    *length = pending;
    return ESP_OK;
}

/* Stream every persistent file under one full-lifetime EXPORT lease.  This
 * prevents a concurrent flush from rotating or appending a file between its
 * open and final read, but (by SD lease contract) does not block raw therapy
 * recording.  The pending RAM suffix is snapped before releasing the lease
 * to avoid duplicate/missing bytes at the file/buffer boundary. */
static esp_err_t stream_persistent_log_files(httpd_req_t *req,
                                             bool *got_any,
                                             uint8_t **pending_bytes,
                                             size_t *pending_length)
{
    if (!req || !got_any || !pending_bytes || !pending_length)
        return ESP_ERR_INVALID_ARG;
    *pending_bytes = NULL;
    *pending_length = 0;

    bool card_ready = s_sd_ready || sd_storage_is_ready();
    bool leased = false;
    esp_err_t result = ESP_OK;
    FILE *file = NULL;

    if (card_ready) {
        leased = sd_storage_lease_acquire(SD_LEASE_EXPORT, 1000);
        if (!leased) result = ESP_ERR_TIMEOUT;
    }

    if (leased) {
        if (!sd_storage_is_ready()) {
            result = ESP_ERR_INVALID_STATE;
            goto snapshot;
        }

        for (int i = LOG_MAX_FILES - 1; i >= 0; i--) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s%d", LOG_DIR,
                     LOG_FILE_PREFIX, i);
            file = fopen(path, "rb");
            if (!file) continue;

            char chunk[1024];
            size_t count;
            while ((count = fread(chunk, 1, sizeof(chunk), file)) > 0) {
                if (httpd_resp_send_chunk(req, chunk, (ssize_t)count) !=
                    ESP_OK) {
                    result = ESP_FAIL;
                    goto done;
                }
                *got_any = true;
            }
            if (ferror(file)) result = ESP_FAIL;
            if (fclose(file) != 0 && result == ESP_OK) result = ESP_FAIL;
            file = NULL;
            if (result != ESP_OK) goto done;
        }
    }

snapshot:
    /* Also snapshot when no card/lease is available. In that case the mutex
     * still makes this a safe best-effort view of the live pending suffix. */
    {
        esp_err_t snapshot_error =
            snapshot_pending_log_bytes(pending_bytes, pending_length);
        if (result == ESP_OK && snapshot_error != ESP_OK)
            result = snapshot_error;
    }

done:
    if (file) fclose(file);
    if (leased) sd_storage_lease_release_unchanged(SD_LEASE_EXPORT);
    return result;
}

static esp_err_t stream_complete_log_archive(httpd_req_t *req,
                                             const char *empty_message)
{
    bool got_any = false;
    uint8_t *pending = NULL;
    size_t pending_length = 0;
    esp_err_t result = stream_persistent_log_files(
        req, &got_any, &pending, &pending_length);

    if (result != ESP_FAIL && pending_length > 0) {
        if (httpd_resp_send_chunk(req, (const char *)pending,
                                  (ssize_t)pending_length) == ESP_OK) {
            got_any = true;
        } else {
            result = ESP_FAIL;
        }
    }
    free(pending);

    if (result == ESP_ERR_TIMEOUT && !got_any) {
        static const char busy[] =
            "(persistent log files busy; retry this request)\n";
        if (httpd_resp_send_chunk(req, busy, sizeof(busy) - 1) == ESP_OK)
            got_any = true;
        else
            result = ESP_FAIL;
    } else if (result == ESP_ERR_NO_MEM && !got_any) {
        httpd_resp_send_500(req);
        return result;
    }

    if (!got_any && result != ESP_FAIL) {
        if (httpd_resp_send_chunk(req, empty_message, -1) != ESP_OK)
            result = ESP_FAIL;
    }
    if (result != ESP_FAIL &&
        httpd_resp_send_chunk(req, NULL, 0) != ESP_OK)
        result = ESP_FAIL;
    return result == ESP_FAIL ? ESP_FAIL : ESP_OK;
}

static esp_err_t logs_history_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return stream_complete_log_archive(
        req, "(no log history available)\n");
}

/* ── Download Endpoint: GET /api/logs/download ────────────────────── */

static esp_err_t logs_download_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"somnotrace-logs.txt\"");

    return stream_complete_log_archive(req, "(no log data available)\n");
}

/* ── Log Level Endpoint: POST /api/logs/level ─────────────────────── */

static const char *level_to_str(esp_log_level_t level)
{
    switch (level) {
        case ESP_LOG_NONE:    return "none";
        case ESP_LOG_ERROR:   return "error";
        case ESP_LOG_WARN:    return "warn";
        case ESP_LOG_INFO:    return "info";
        case ESP_LOG_DEBUG:   return "debug";
        case ESP_LOG_VERBOSE: return "verbose";
        default:              return "unknown";
    }
}

static esp_log_level_t str_to_level(const char *s)
{
    if (strcmp(s, "error")   == 0) return ESP_LOG_ERROR;
    if (strcmp(s, "warn")    == 0) return ESP_LOG_WARN;
    if (strcmp(s, "info")    == 0) return ESP_LOG_INFO;
    if (strcmp(s, "debug")   == 0) return ESP_LOG_DEBUG;
    if (strcmp(s, "verbose") == 0) return ESP_LOG_VERBOSE;
    if (strcmp(s, "none")    == 0) return ESP_LOG_NONE;
    return (esp_log_level_t)-1;
}

/* Cached current global level so we can report it back to the UI.
 * ESP-IDF doesn't expose a getter for the global default. */
static esp_log_level_t s_current_level = ESP_LOG_INFO;

static esp_err_t logs_level_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Return current level as JSON. */
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"level\":\"%s\"}", level_to_str(s_current_level));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    /* POST — set the global log level. */
    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *lvl_item = cJSON_GetObjectItem(json, "level");
    if (!cJSON_IsString(lvl_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'level' string");
        return ESP_FAIL;
    }

    esp_log_level_t new_level = str_to_level(lvl_item->valuestring);
    cJSON_Delete(json);

    if ((int)new_level < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid level — use: none, error, warn, info, debug, verbose");
        return ESP_FAIL;
    }

    /* Apply globally (wildcard "*" sets the default for all tags). */
    esp_log_level_set("*", new_level);
    s_current_level = new_level;

    ESP_LOGI(TAG, "global log level changed to %s", level_to_str(new_level));

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"level\":\"%s\"}", level_to_str(new_level));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── Handler Registration ─────────────────────────────────────────── */

void log_stream_register_handlers(httpd_handle_t server)
{
    httpd_uri_t recent = {
        .uri     = "/api/logs/recent",
        .method  = HTTP_GET,
        .handler = logs_recent_handler,
    };
    httpd_register_uri_handler(server, &recent);

    httpd_uri_t download = {
        .uri     = "/api/logs/download",
        .method  = HTTP_GET,
        .handler = logs_download_handler,
    };
    httpd_register_uri_handler(server, &download);

    httpd_uri_t level_get = {
        .uri     = "/api/logs/level",
        .method  = HTTP_GET,
        .handler = logs_level_handler,
    };
    httpd_register_uri_handler(server, &level_get);

    httpd_uri_t level_post = {
        .uri     = "/api/logs/level",
        .method  = HTTP_POST,
        .handler = logs_level_handler,
    };
    httpd_register_uri_handler(server, &level_post);

    httpd_uri_t history = {
        .uri     = "/api/logs/history",
        .method  = HTTP_GET,
        .handler = logs_history_handler,
    };
    httpd_register_uri_handler(server, &history);

    httpd_uri_t ws = {
        .uri        = "/api/ws",
        .method     = HTTP_GET,
        .handler    = logs_ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server, &ws);

    ESP_LOGI(TAG, "registered /api/ws and /api/logs/{recent,download,history,level}");
}
