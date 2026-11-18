/*
 * SomnoTrace - Deferred execution of slow HTTP handlers
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

/* Handler body signature — identical to a normal URI handler, but it runs
 * on the deferred worker instead of the httpd task. */
typedef esp_err_t (*httpd_defer_fn_t)(httpd_req_t *req);

/* Must be called once at startup (before registering URI handlers) to start
 * the deferred-handler worker task. */
void httpd_defer_init(void);

/* Dispatch a request to a handler that blocks on slow work (SD/FatFS I/O,
 * BLE RPC round-trips, long scans).
 *
 * esp_http_server runs every URI handler serially on a single task, so a
 * handler that stalls for seconds blocks all other requests — including
 * fast in-memory ones — and sockets pile up until lru_purge resets them.
 * This hands the request to a persistent worker via
 * httpd_req_async_handler_begin() and returns immediately, so the httpd
 * task stays responsive.
 *
 * `fn` is invoked on the worker with the async request and may use all
 * httpd_req_* / httpd_resp_* calls as usual. Deferred jobs are serialized
 * (one worker, FIFO) which matches FatFS volume-lock serialisation anyway.
 * On queue overflow the client gets a 503 rather than a dropped socket.
 *
 * Must be called from a URI handler on the httpd task. Always returns
 * ESP_OK unless the request could not be converted to async at all. */
esp_err_t httpd_defer_dispatch(httpd_req_t *req, httpd_defer_fn_t fn);
