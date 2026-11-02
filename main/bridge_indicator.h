/*
 * SomnoTrace - SomnoBridge LCD indicator state
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
#include <stdint.h>

#define BRIDGE_INDICATOR_STALE_US (90LL * 1000000)

typedef enum {
    BRIDGE_INDICATOR_HIDDEN,
    BRIDGE_INDICATOR_DISCONNECTED,
    BRIDGE_INDICATOR_UNKNOWN,
    BRIDGE_INDICATOR_CARD_OFFLINE,
    BRIDGE_INDICATOR_CARD_WIFI,
} bridge_indicator_view_t;

/* The caller serializes access to this state. The epoch rejects replies from
 * a previous BLE connection, even if NimBLE reuses the connection handle. */
typedef struct {
    bool paired;
    bool connected;
    bool status_valid;
    bool card_wifi;
    uint32_t epoch;
    int64_t status_us;
} bridge_indicator_t;

void bridge_indicator_set_paired(bridge_indicator_t *s, bool paired);
void bridge_indicator_set_connected(bridge_indicator_t *s, bool connected);
void bridge_indicator_set_status(bridge_indicator_t *s, uint32_t epoch,
                                 bool valid, bool card_wifi, int64_t now_us);
bridge_indicator_view_t bridge_indicator_view(const bridge_indicator_t *s,
                                              int64_t now_us);
