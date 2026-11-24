/*
 * SomnoTrace - WebSocket client lifecycle regression tests
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

#include <assert.h>
#include <stdio.h>

int main(void)
{
    ws_clients_t clients = {0};
    char server1, server2;
    ws_client_t old, current, evicted, other;
    assert(ws_clients_add(&clients, &server1, 36, &old, &evicted));
    assert(evicted.fd == -1 && ws_clients_pause(&clients, &old, true));

    /* An old queued send and failed-receive cleanup must not affect a new
     * handshake even when the server handle and socket fd are both reused. */
    assert(ws_clients_add(&clients, &server1, 36, &current, &evicted));
    assert(current.generation > old.generation && !current.paused);
    assert(!ws_clients_matches(&clients, &old));
    assert(!ws_clients_remove(&clients, &old));
    assert(!ws_clients_pause(&clients, &old, true));
    assert(ws_clients_matches(&clients, &current));

    /* A header or payload receive failure removes only its own generation. */
    assert(ws_clients_remove(&clients, &current));
    assert(!ws_clients_matches(&clients, &current));
    assert(ws_clients_add(&clients, &server1, 36, &current, &evicted));
    assert(ws_clients_add(&clients, &server2, 36, &other, &evicted));
    assert(ws_clients_matches(&clients, &current) && ws_clients_matches(&clients, &other));

    ws_client_t c3, c4, c5;
    assert(ws_clients_add(&clients, &server1, 37, &c3, &evicted));
    assert(ws_clients_add(&clients, &server1, 38, &c4, &evicted));
    assert(ws_clients_add(&clients, &server1, 39, &c5, &evicted));
    assert(evicted.generation == current.generation);
    assert(!ws_clients_matches(&clients, &evicted));
    assert(ws_clients_matches(&clients, &c5));
    assert(!ws_clients_remove(&clients, &evicted));

    /* The allocator can reuse the server handle as well as its socket fd. */
    ws_clients_server_stopping(&clients, &server1);
    assert(!ws_clients_server_accepting(&clients, &server1));
    assert(ws_clients_server_accepting(&clients, &server2));
    assert(!ws_clients_add(&clients, &server1, 36, &current, &evicted));
    assert(ws_clients_remove(&clients, &c5));
    ws_clients_server_started(&clients, &server1);
    assert(ws_clients_server_accepting(&clients, &server1));
    assert(ws_clients_add(&clients, &server1, 39, &current, &evicted));
    assert(current.generation > c5.generation && !ws_clients_matches(&clients, &c5));

    clients.last_generation = UINT64_MAX;
    int before = clients.count;
    assert(!ws_clients_add(&clients, &server1, 40, &old, &evicted));
    assert(clients.count == before && ws_clients_matches(&clients, &current));
    puts("ws_clients_test: reuse, receive failure, queued work and eviction passed");
    return 0;
}
