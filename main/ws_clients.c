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

#include "ws_clients.h"

#include <stddef.h>
#include <string.h>

static int client_index(const ws_clients_t *clients, const ws_client_t *client)
{
    for (int i = 0; i < clients->count; i++) {
        const ws_client_t *c = &clients->clients[i];
        if (c->server == client->server && c->fd == client->fd &&
            c->generation == client->generation) return i;
    }
    return -1;
}

static void remove_index(ws_clients_t *clients, int index)
{
    for (int i = index; i + 1 < clients->count; i++)
        clients->clients[i] = clients->clients[i + 1];
    clients->count--;
}

bool ws_clients_add(ws_clients_t *clients, void *server, int fd,
                    ws_client_t *added, ws_client_t *evicted)
{
    if (!clients || !server || fd < 0 || !added || !evicted ||
        !ws_clients_server_accepting(clients, server) ||
        clients->last_generation == UINT64_MAX) return false;
    memset(evicted, 0, sizeof(*evicted));
    evicted->fd = -1;
    for (int i = 0; i < clients->count; i++) {
        if (clients->clients[i].server == server && clients->clients[i].fd == fd) {
            /* A reused fd is not safe to send an eviction frame to. */
            remove_index(clients, i);
            break;
        }
    }
    if (clients->count == WS_CLIENT_LIMIT) {
        *evicted = clients->clients[0];
        remove_index(clients, 0);
    }
    *added = (ws_client_t){ .server = server, .fd = fd,
                            .generation = ++clients->last_generation,
                            .paused = false };
    clients->clients[clients->count++] = *added;
    return true;
}

bool ws_clients_matches(const ws_clients_t *clients, const ws_client_t *client)
{
    return clients && client && client_index(clients, client) >= 0;
}

bool ws_clients_remove(ws_clients_t *clients, const ws_client_t *client)
{
    if (!clients || !client) return false;
    int i = client_index(clients, client);
    if (i < 0) return false;
    remove_index(clients, i);
    return true;
}

bool ws_clients_pause(ws_clients_t *clients, const ws_client_t *client, bool paused)
{
    if (!clients || !client) return false;
    int i = client_index(clients, client);
    if (i < 0) return false;
    clients->clients[i].paused = paused;
    return true;
}

void ws_clients_server_stopping(ws_clients_t *clients, void *server)
{
    if (clients && server) clients->stopping_server = server;
}

void ws_clients_server_started(ws_clients_t *clients, void *server)
{
    if (clients && clients->stopping_server == server) clients->stopping_server = NULL;
}

bool ws_clients_server_accepting(const ws_clients_t *clients, void *server)
{
    return clients && server && clients->stopping_server != server;
}
