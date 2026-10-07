/*
 * SomnoTrace - Optional password protection for the web interface
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

/*
 * Web interface password (spec 0014).  Off by default.  When a password is
 * set, every route registered with web_auth_register() requires either the
 * session cookie issued by POST /api/auth/login or an
 * "Authorization: Basic" header carrying the password (for Home Assistant,
 * curl and other API clients).
 *
 * Default-deny: all routes go through web_auth_register(); only the static
 * app shell and the login endpoints use web_auth_register_public().  Nothing
 * outside web_auth.c calls httpd_register_uri_handler() (scripts/lint.sh).
 *
 * All functions must be called from the httpd worker task, or before the
 * server starts; state is unsynchronised by design (single worker).
 */

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Load the password record from NVS.  Call after nvs_writer_init() and
 * before httpd_start().  Idempotent. */
esp_err_t web_auth_init(void);

/* Forget the routes of a stopped server.  Call before registering the routes
 * of a new server instance. */
void web_auth_begin_routes(void);

/* Register a route that requires authentication when a password is set. */
esp_err_t web_auth_register(httpd_handle_t server, const httpd_uri_t *uri);

/* Register a route that never requires authentication.  Only for content
 * that is safe to show anyone on the network (the app shell, static assets,
 * captive-portal probes, login). */
esp_err_t web_auth_register_public(httpd_handle_t server, const httpd_uri_t *uri);

/* Register /api/auth/{status,login,logout,password}. */
void web_auth_register_handlers(httpd_handle_t server);

bool web_auth_enabled(void);
