/*
 * SomnoTrace - Session graph data HTTP endpoint
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
#include "esp_http_server.h"

/* Must be called once at startup (before registering HTTP handlers) to
 * initialise the binary semaphore that serialises large-file transfers. */
void session_graph_init(void);

/* GET /api/sessions?date=YYYYMMDD
 * Returns JSON array of sessions for the given noon-day folder. */
esp_err_t sessions_list_handler(httpd_req_t *req);

/* GET /api/session/graph?session=ID&channel=flow|leak
 *   &points=800           (overview — server decimates)
 *   &start_ms=X&end_ms=Y  (zoom — raw L0 for time range) */
esp_err_t session_graph_handler(httpd_req_t *req);

/* GET /api/session/file?session=ID&date=YYYYMMDD&type=brp|brp_mm|pld|sa2|events
 * Streams the raw .snt file as application/octet-stream. */
esp_err_t session_file_handler(httpd_req_t *req);

/* GET /api/days
 * Returns JSON array of noon-days that have session data, e.g.
 * [{"day":"20260719","sessions":2}, ...] sorted ascending. */
esp_err_t days_list_handler(httpd_req_t *req);

/* GET /api/summary?date=YYYYMMDD
 * Returns the day's summary metrics (AHI/indices, usage, leak/pressure/EPAP/
 * resp-rate percentiles) parsed from the AS11 Summary spool. 404 if none. */
esp_err_t summary_handler(httpd_req_t *req);

/* GET /api/session/settings?session=ID&date=YYYYMMDD
 * Returns therapy mode + pressure/EPR settings parsed from the session's
 * settings.json, for the pressure-panel rendering (CPAP flat lines). */
esp_err_t session_settings_handler(httpd_req_t *req);
