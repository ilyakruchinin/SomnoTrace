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

#include "bridge_indicator.h"

void bridge_indicator_set_paired(bridge_indicator_t *s, bool paired)
{
    if (!paired && (s->paired || s->connected)) {
        s->epoch++;
        s->connected = false;
        s->status_valid = false;
    }
    s->paired = paired;
}

void bridge_indicator_set_connected(bridge_indicator_t *s, bool connected)
{
    if (s->connected != connected) {
        s->epoch++;
        s->status_valid = false;
        s->connected = connected;
    }
    if (connected) s->paired = true;
}

void bridge_indicator_set_status(bridge_indicator_t *s, uint32_t epoch,
                                 bool valid, bool card_wifi, int64_t now_us)
{
    if (!s->connected || s->epoch != epoch) return;
    s->status_valid = valid;
    s->card_wifi = card_wifi;
    s->status_us = now_us;
}

bridge_indicator_view_t bridge_indicator_view(const bridge_indicator_t *s,
                                              int64_t now_us)
{
    if (!s->paired) return BRIDGE_INDICATOR_HIDDEN;
    if (!s->connected) return BRIDGE_INDICATOR_DISCONNECTED;
    if (!s->status_valid || now_us < s->status_us ||
        now_us - s->status_us > BRIDGE_INDICATOR_STALE_US)
        return BRIDGE_INDICATOR_UNKNOWN;
    return s->card_wifi ? BRIDGE_INDICATOR_CARD_WIFI
                        : BRIDGE_INDICATOR_CARD_OFFLINE;
}
