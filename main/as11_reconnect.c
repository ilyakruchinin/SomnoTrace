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
#include "as11_reconnect.h"

int as11_reconnect_delay_s(int attempt)
{
    if (attempt <= 1) {
        return 0;
    }
    if (attempt <= AS11_RECONNECT_FAST_ATTEMPTS) {
        /* 4 s then 6 s. The comment this replaced said "2-4s backoff" while the
         * code computed attempt * 2, i.e. 4 and 6 — the description had drifted
         * from the arithmetic. The arithmetic is kept; the description now
         * matches it, and the test pins the values so they cannot drift apart
         * again silently. */
        return attempt * 2;
    }
    return AS11_RECONNECT_SLOW_DELAY_S;
}
