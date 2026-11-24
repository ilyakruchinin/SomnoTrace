/*
 * SomnoTrace - WebSocket client lifecycle tracking
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

#define WS_CLIENT_LIMIT 4

typedef struct {
    void *server;
    int fd;
    uint64_t generation;
    bool paused;
} ws_client_t;

/* The caller serializes access. Generation is never reset during a boot,
 * including when an HTTP server is stopped and recreated. */
typedef struct {
    ws_client_t clients[WS_CLIENT_LIMIT];
    int count;
    uint64_t last_generation;
    void *stopping_server;
} ws_clients_t;

bool ws_clients_add(ws_clients_t *clients, void *server, int fd,
                    ws_client_t *added, ws_client_t *evicted);
bool ws_clients_matches(const ws_clients_t *clients, const ws_client_t *client);
bool ws_clients_remove(ws_clients_t *clients, const ws_client_t *client);
bool ws_clients_pause(ws_clients_t *clients, const ws_client_t *client, bool paused);
void ws_clients_server_stopping(ws_clients_t *clients, void *server);
void ws_clients_server_started(ws_clients_t *clients, void *server);
bool ws_clients_server_accepting(const ws_clients_t *clients, void *server);
