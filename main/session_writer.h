/*
 * SomnoTrace - Session data writer for SD card storage
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

/* Opaque session handle. */
typedef struct session_writer session_writer_t;

/* Initialise the session writer subsystem. Creates the flush task.
 * Call once at boot after sd_storage_init(). */
esp_err_t session_writer_init(void);

/* Start a new therapy session.
 * Creates noon-day folder under .somnotrace/sessions/streams/YYYYMMDD/ and opens
 * YYYYMMDD_HHMMSS_*.snt files with prefix-based naming.
 * Returns a handle or NULL on failure. */
session_writer_t *session_writer_start(void);

/* Stop the current therapy session.
 * Performs final flush, writes session.json, closes all files.
 * Safe to call with NULL handle. */
esp_err_t session_writer_stop(session_writer_t *s);

/* Ingest a JSON-RPC notification from the AS11.
 * The session writer parses the method name and params to detect
 * therapy data (waveforms, vitals, events) and writes to .snt files.
 * Called from the BLE notification handler.
 * If no session is active, therapy-start notifications will auto-start one. */
void session_writer_on_notification(session_writer_t *s, const cJSON *msg);

/* Fast-path processor for StreamData notifications using raw JSON.
 * Bypasses cJSON tree building for the high-frequency StreamData path.
 * Handles flow display push, active-flow detection, and sample routing. */
void session_writer_on_stream_data_raw(const char *json, int len);

/* Returns true if a session is currently active. */
bool session_writer_is_active(const session_writer_t *s);

/* Get the current active session handle (or NULL). */
session_writer_t *session_writer_get_active(void);

/* Set the AS11 device address and client ID for session metadata. */
void session_writer_set_device_info(const char *addr, const char *client_id);

/* Crash recovery: scan for interrupted sessions and finalise them.
 *
 * Repairs raw files, writes a final "interrupted" manifest, and durably
 * queues each affected noon-day for an automatic export rebuild.  The
 * rebuild itself is deliberately NOT run here: this executes before BLE
 * starts, and a long rebuild would delay reconnect. */
void session_writer_recover(void);

/* Allow the idle post worker to start draining days queued by recovery.
 * Call once at boot after BLE initialisation has been started, so a rebuild
 * can never delay reconnect or the first StreamData of a live session. */
void session_writer_enable_deferred_export(void);

/* Days still awaiting an automatic export rebuild, as a JSON array of
 * {day, attempts, needs_attention}.  Caller frees. */
esp_err_t session_writer_pending_export_json(char **out_json);

/* Check whether an _SNC ValueChange notification has been received since
 * the last call.  Returns true and stores the new value in *out_value
 * (if non-NULL), then clears the flag.  Used by post_therapy to detect
 * when the AS11 has updated its Summary spool without polling. */
bool session_writer_snc_changed(int64_t *out_value);
