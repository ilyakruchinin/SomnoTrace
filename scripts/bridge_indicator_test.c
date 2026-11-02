/*
 * SomnoTrace - host tests for SomnoBridge LCD indicator state
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

#include <stdio.h>

#include "bridge_indicator.h"

static int checks, failures;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAILED: %s (line %d)\n", msg, __LINE__); } \
} while (0)

#define US(s) ((int64_t)(s) * 1000000)

int main(void)
{
    bridge_indicator_t s = {0};
    CHECK(bridge_indicator_view(&s, 0) == BRIDGE_INDICATOR_HIDDEN,
          "no bridge means no icon");

    bridge_indicator_set_paired(&s, true);
    CHECK(bridge_indicator_view(&s, US(1)) == BRIDGE_INDICATOR_DISCONNECTED,
          "saved bond is not a live connection");
    bridge_indicator_set_status(&s, s.epoch, true, true, US(1));
    CHECK(bridge_indicator_view(&s, US(1)) == BRIDGE_INDICATOR_DISCONNECTED,
          "a status cannot make a disconnected bridge look connected");

    bridge_indicator_set_connected(&s, true);
    uint32_t first = s.epoch;
    CHECK(bridge_indicator_view(&s, US(2)) == BRIDGE_INDICATOR_UNKNOWN,
          "new secure link waits for card status");
    bridge_indicator_set_status(&s, first, true, false, US(2));
    CHECK(bridge_indicator_view(&s, US(2)) == BRIDGE_INDICATOR_CARD_OFFLINE,
          "card Wi-Fi disconnected");
    bridge_indicator_set_status(&s, first, true, true, US(3));
    CHECK(bridge_indicator_view(&s, US(3)) == BRIDGE_INDICATOR_CARD_WIFI,
          "card Wi-Fi joined");
    bridge_indicator_set_connected(&s, true);
    CHECK(s.epoch == first &&
          bridge_indicator_view(&s, US(4)) == BRIDGE_INDICATOR_CARD_WIFI,
          "duplicate connected notification preserves current status");

    CHECK(bridge_indicator_view(&s, US(93)) == BRIDGE_INDICATOR_CARD_WIFI,
          "status remains fresh at 90 seconds");
    CHECK(bridge_indicator_view(&s, US(93) + 1) == BRIDGE_INDICATOR_UNKNOWN,
          "old status ages to unknown even during a long sync");
    CHECK(bridge_indicator_view(&s, US(2)) == BRIDGE_INDICATOR_UNKNOWN,
          "a timestamp in the future is not fresh");
    bridge_indicator_set_status(&s, first, false, false, US(94));
    CHECK(bridge_indicator_view(&s, US(94)) == BRIDGE_INDICATOR_UNKNOWN,
          "failed or malformed status is unknown, not card offline");

    bridge_indicator_set_status(&s, first, true, true, US(95));
    bridge_indicator_set_connected(&s, false);
    CHECK(bridge_indicator_view(&s, US(96)) == BRIDGE_INDICATOR_DISCONNECTED,
          "disconnect invalidates the card status immediately");
    bridge_indicator_set_status(&s, first, true, true, US(96));
    CHECK(bridge_indicator_view(&s, US(96)) == BRIDGE_INDICATOR_DISCONNECTED,
          "late reply cannot revive a disconnected link");
    bridge_indicator_set_connected(&s, true);
    uint32_t second = s.epoch;
    CHECK(second != first &&
          bridge_indicator_view(&s, US(97)) == BRIDGE_INDICATOR_UNKNOWN,
          "reconnection starts with unknown card status");
    bridge_indicator_set_status(&s, first, true, true, US(97));
    CHECK(bridge_indicator_view(&s, US(97)) == BRIDGE_INDICATOR_UNKNOWN,
          "old connection's reply cannot affect new connection");
    bridge_indicator_set_status(&s, second, true, false, US(98));
    CHECK(bridge_indicator_view(&s, US(98)) == BRIDGE_INDICATOR_CARD_OFFLINE,
          "new connection accepts its own status");
    bridge_indicator_set_status(&s, first, false, false, US(99));
    CHECK(bridge_indicator_view(&s, US(99)) == BRIDGE_INDICATOR_CARD_OFFLINE,
          "old failed poll cannot erase new connection status");

    bridge_indicator_set_paired(&s, false);
    CHECK(bridge_indicator_view(&s, US(100)) == BRIDGE_INDICATOR_HIDDEN,
          "forget hides both icons");
    bridge_indicator_set_status(&s, second, true, true, US(100));
    CHECK(bridge_indicator_view(&s, US(100)) == BRIDGE_INDICATOR_HIDDEN,
          "late reply after forget cannot restore icons");
    bridge_indicator_set_paired(&s, true);
    CHECK(bridge_indicator_view(&s, US(101)) == BRIDGE_INDICATOR_DISCONNECTED,
          "new bond does not reuse old connection or status");

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
