/*
 * SomnoTrace - Log stream: ring-buffered log capture with WebSocket delivery
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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "cJSON.h"

/**
 * Initialise the log stream subsystem.
 *
 * Allocates a ring buffer (PSRAM if available, otherwise internal RAM) and
 * installs a custom vprintf hook so every ESP_LOGx() call is captured.
 * Must be called once, before the HTTP server registers the log endpoints.
 */
/* Returns ESP_OK when all live-stream primitives are available.  A degraded
 * capture may still retain or persist logs after a non-OK result, but every
 * public endpoint remains safe to call.  Repeated calls are idempotent. */
esp_err_t log_stream_init(void);

/* ── Native touchscreen retained log feed ─────────────────────────────────────
 *
 * This feed is deliberately independent of the WebSocket byte ring.  Reading
 * it never consumes lines that a browser client is waiting for, and browser
 * polling never removes lines from the touchscreen history.
 */

/* Includes the trailing NUL.  Longer rendered ESP_LOG lines are retained with
 * a deterministic prefix and `truncated == true`. */
#define LOG_STREAM_RETAINED_TEXT_MAX 192

typedef enum {
    LOG_STREAM_RETAINED_LEVEL_UNKNOWN = (1u << 0),
    LOG_STREAM_RETAINED_LEVEL_ERROR   = (1u << 1),
    LOG_STREAM_RETAINED_LEVEL_WARN    = (1u << 2),
    LOG_STREAM_RETAINED_LEVEL_INFO    = (1u << 3),
    LOG_STREAM_RETAINED_LEVEL_DEBUG   = (1u << 4),
    LOG_STREAM_RETAINED_LEVEL_VERBOSE = (1u << 5),
    LOG_STREAM_RETAINED_LEVEL_ALL     = ((1u << 6) - 1u),
    /* Explicit empty selection; zero retains its existing all-level meaning. */
    LOG_STREAM_RETAINED_LEVEL_NONE    = (1u << 6),
} log_stream_retained_level_t;

typedef enum {
    /* Zero is the UI-friendly default for a zero-initialised filter. */
    LOG_STREAM_RETAINED_NEWEST_FIRST = 0,
    LOG_STREAM_RETAINED_OLDEST_FIRST,
} log_stream_retained_order_t;

typedef struct {
    uint64_t sequence; /* Monotonic for the lifetime of this boot. */
    uint16_t length;   /* Bytes in text, excluding the trailing NUL. */
    uint8_t level;     /* One log_stream_retained_level_t bit. */
    bool truncated;
    char text[LOG_STREAM_RETAINED_TEXT_MAX];
} log_stream_retained_line_t;

typedef struct {
    /* Zero means all levels.  Otherwise OR log_stream_retained_level_t bits. */
    uint32_t level_mask;
    /* Optional case-insensitive substring matched against tag + message. */
    const char *query;
    /* Only return lines with sequence strictly greater than this value. */
    uint64_t after_sequence;
    /* When non-zero, only return lines with sequence strictly below this
     * value.  A paused UI can anchor this to newest_sequence + 1 so incoming
     * lines do not move the page being read. */
    uint64_t before_sequence;
    log_stream_retained_order_t order;
} log_stream_retained_filter_t;

typedef struct {
    /* Zero-based offset among lines matching the filter, in filter order. */
    size_t match_offset;
    size_t returned;
    size_t matching_count;
    bool has_previous_page;
    bool has_next_page;
    uint64_t first_sequence;
    uint64_t last_sequence;
} log_stream_retained_page_t;

typedef struct {
    bool available;
    bool in_psram;
    size_t capacity;
    size_t retained_count;
    /* Increments on every accepted line and Clear operation. */
    uint64_t generation;
    /* Accepted line count; Clear deliberately does not reset it. */
    uint64_t total_count;
    /* Wall-clock-independent span between the oldest and newest retained
     * captures. Wrap-safe for the ESP timer's 32-bit millisecond projection. */
    uint32_t retained_span_ms;
    /* Lines dropped because capture storage was unavailable or contended. */
    uint32_t dropped_count;
    esp_err_t last_error;
} log_stream_retained_info_t;

/* Optional progress notification for a synchronous card snapshot.  Calls are
 * made from the caller's worker task, never from the vprintf hook. */
typedef void (*log_stream_retained_progress_fn)(size_t processed_lines,
                                                size_t total_lines,
                                                void *ctx);

/** Read retained-feed status without allocating. */
esp_err_t log_stream_retained_get_info(log_stream_retained_info_t *info);

/**
 * Retry allocation only when the native retained feed is unavailable.  This
 * is intended for the explicit Reconnect action on the Logs screen; existing
 * contents are never replaced and capture remains best-effort while the
 * allocation is attempted.
 */
esp_err_t log_stream_retained_retry(void);

/**
 * Copy a filtered snapshot into caller-owned storage.  The call allocates
 * nothing and never consumes either log ring.  A NULL filter selects all
 * levels in newest-first order.  `line_capacity == 0` is valid with `lines`
 * set to NULL and can be used to obtain metadata only.  The filter and its
 * optional query string are borrowed only for the duration of the call.
 */
esp_err_t log_stream_retained_snapshot(
    log_stream_retained_line_t *lines,
    size_t line_capacity,
    const log_stream_retained_filter_t *filter,
    size_t *line_count,
    log_stream_retained_info_t *info);

/**
 * Copy one bounded page without creating an LVGL row per retained line.
 * Unlike the fast live-tail snapshot above, this scans the current retained
 * bounds to report an exact filtered count.  It is intended for explicit
 * pause/search/scroll actions, not every live refresh tick.  Page offsets are
 * interpreted in `filter->order`; a non-zero before_sequence keeps paused
 * pages stable while capture continues.
 */
esp_err_t log_stream_retained_snapshot_page(
    log_stream_retained_line_t *lines,
    size_t line_capacity,
    const log_stream_retained_filter_t *filter,
    size_t match_offset,
    log_stream_retained_page_t *page,
    log_stream_retained_info_t *info);

/**
 * Clear only the touchscreen-visible retained RAM ring.  The WebSocket ring,
 * pending persistent-log write buffer, and files already on the card are not
 * touched.  Generation and total_count remain monotonic.
 */
esp_err_t log_stream_retained_clear(void);

/**
 * Copy the current retained snapshot (optionally filtered) to the dedicated
 * touchscreen-visible log file below SD_LOG_DIR.  The operation uses the SD
 * export lease, writes through a temporary file, and publishes only a
 * completed copy.  `saved_path` is optional; if supplied it receives the full
 * mounted path.  This function may block on storage and must not run on the
 * LVGL/touch handler or the vprintf hook.  The filter and query string must
 * remain valid until the synchronous call returns.  `progress_fn` is optional
 * and reports scanned source lines, so filtered saves still reach 100%; the
 * callback runs synchronously on the caller's worker task.
 */
esp_err_t log_stream_retained_save_to_sd(
    const log_stream_retained_filter_t *filter,
    char *saved_path,
    size_t saved_path_size,
    size_t *saved_line_count,
    log_stream_retained_progress_fn progress_fn,
    void *progress_ctx);

/**
 * Register the system WebSocket & log HTTP endpoints on the given server:
 *
 *   GET  /api/ws            — Unified WebSocket real-time channel (JSON envelopes)
 *   GET  /api/logs/recent    — polling fallback for live log lines (JSON)
 *   GET  /api/logs/download  — plain-text download of buffered logs
 *   GET  /api/logs/history   — plain-text history from SD card files
 *   GET  /api/logs/level     — get current log level (JSON)
 *   POST /api/logs/level     — change runtime log level (JSON body)
 */
void log_stream_register_handlers(httpd_handle_t server);

/**
 * Push a typed JSON envelope frame down the active WebSocket connection.
 * Format: {"type": "<type>", "data": <data_obj>}
 *
 * Ownership: data_obj is transferred to this function.  It will be
 * embedded in the envelope and freed (via cJSON_Delete) before returning,
 * regardless of success or failure.  The caller must not reference
 * data_obj after the call returns.
 */
esp_err_t log_stream_ws_send_json(const char *type, cJSON *data_obj);

/**
 * Push a typed raw JSON string frame down the active WebSocket connection.
 * Format: {"type": "<type>", "data": <data_json_str>}
 */
esp_err_t log_stream_ws_send_json_raw(const char *type, const char *data_json_str);

/**
 * Request an immediate upload-progress push on the next forwarder cycle.
 * Called from the upload scheduler on backend state transitions.
 * Non-blocking — just sets a flag.
 */
void log_stream_request_upload_push(void);

/**
 * Request an immediate BLE-state push on the next forwarder cycle.
 * Called from the BLE state machine on pairing state changes.
 * Non-blocking — just sets a flag.
 */
void log_stream_request_ble_push(void);

/**
 * Request an immediate oximeter-state push on the next forwarder cycle.
 * Called from the oximeter drivers on state changes (pairing, sync, etc.).
 * Non-blocking — just sets a flag.
 */
void log_stream_request_ox_push(void);

#ifdef __cplusplus
}
#endif
