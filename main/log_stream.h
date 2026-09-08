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
void log_stream_init(void);

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
