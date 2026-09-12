/*
 * SomnoTrace - AS11 reconnect backoff policy
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
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin".
 */
#pragma once

/*
 * HOW LONG TO WAIT BEFORE THE NEXT RECONNECT ATTEMPT, and nothing else.
 *
 * This is deliberately a separate translation unit with NO ESP-IDF dependency.
 * The policy used to live inline in as11_ble.c's reconnect_task, interleaved
 * with vTaskDelay() and vTaskDelete(), which meant it could not be exercised
 * without a radio and a scheduler — so it never was. It has been changed three
 * times regardless: an F1 retry limit for unresponsive O2 Ring firmware, a
 * clarification of the slow-retry timing, and a report about the reconnect
 * retries themselves. A number that keeps being adjusted and can never be
 * checked is worth eight lines and a test.
 *
 * Retrying against a peer that does not answer is the one BLE failure mode no
 * amount of host-stack quality removes: a peripheral out of range, asleep, or
 * wedged is indistinguishable from one that is about to reply. The only
 * defences are a bounded fast phase and a slow phase that costs little, which
 * is exactly what this encodes.
 */

/* Attempts in the fast phase. The AS11 needs a few seconds to restart
 * advertising after a transient drop, so the first few retries are close
 * together and cheap. */
#define AS11_RECONNECT_FAST_ATTEMPTS   3

/* The slow phase runs unbounded on purpose: the AS11 may simply be switched
 * off at boot and turned on hours later, and without this the device would
 * never reconnect until someone power-cycled it. With the 30 s connect
 * timeout this is roughly a 90 s cycle, which is negligible on mains power. */
#define AS11_RECONNECT_SLOW_DELAY_S   60

/**
 * Seconds to wait BEFORE making attempt number `attempt`.
 *
 * `attempt` is 1-based and counts across BOTH phases, so attempt 4 is the
 * first slow retry. Attempt 1 is immediate.
 *
 *   1 -> 0      the first try, no delay
 *   2 -> 4      fast phase
 *   3 -> 6      fast phase
 *   4+ -> 60    slow phase, forever
 *
 * Non-positive input returns 0 rather than a negative delay: a caller that
 * has lost count should retry immediately, not compute a nonsense sleep.
 */
int as11_reconnect_delay_s(int attempt);
