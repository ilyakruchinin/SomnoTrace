/*
 * SomnoTrace - host tests for the AS11 reconnect backoff policy
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
 *
 * ---------------------------------------------------------------------------
 * These pin the reconnect timing, which until now could not be checked without
 * a radio: the policy lived inline in reconnect_task between vTaskDelay() and
 * vTaskDelete(). Every assertion here is a number a mutant can change, and the
 * totals are asserted as well as the individual steps — an off-by-one in the
 * phase boundary keeps every single delay correct while moving the moment the
 * device gives up trying quickly.
 */
#include "as11_reconnect.h"

#include <assert.h>
#include <stdio.h>

static int n_pass, n_fail;

static void check(const char *what, long got, long want)
{
    if (got == want) {
        n_pass++;
    } else {
        n_fail++;
        printf("  FAIL %s: got %ld, want %ld\n", what, got, want);
    }
}

int main(void)
{
    /* The schedule, attempt by attempt. */
    check("attempt 1 is immediate",        as11_reconnect_delay_s(1), 0);
    check("attempt 2 waits 4s",            as11_reconnect_delay_s(2), 4);
    check("attempt 3 waits 6s",            as11_reconnect_delay_s(3), 6);
    check("attempt 4 is the first slow",   as11_reconnect_delay_s(4), 60);
    check("attempt 5 stays slow",          as11_reconnect_delay_s(5), 60);
    check("attempt 1000 stays slow",       as11_reconnect_delay_s(1000), 60);

    /* THE PHASE BOUNDARY, pinned from both sides. Off-by-one here leaves every
     * individual delay correct while changing how long the device stays in the
     * cheap phase — the kind of change that looks harmless in review. */
    check("last fast attempt is FAST_ATTEMPTS",
          as11_reconnect_delay_s(AS11_RECONNECT_FAST_ATTEMPTS),
          AS11_RECONNECT_FAST_ATTEMPTS * 2);
    check("the attempt after it is slow",
          as11_reconnect_delay_s(AS11_RECONNECT_FAST_ATTEMPTS + 1),
          AS11_RECONNECT_SLOW_DELAY_S);

    /* Defensive: a caller that has lost count must not sleep a negative or
     * enormous time. Retry now instead. */
    check("attempt 0 is immediate",        as11_reconnect_delay_s(0), 0);
    check("negative attempt is immediate", as11_reconnect_delay_s(-7), 0);

    /* Properties that hold whatever the numbers become. */
    int prev = -1;
    for (int a = 1; a <= 200; a++) {
        int d = as11_reconnect_delay_s(a);
        if (d < 0) {
            n_fail++;
            printf("  FAIL attempt %d returned a negative delay %d\n", a, d);
            break;
        }
        if (d < prev) {
            n_fail++;
            printf("  FAIL delay decreased at attempt %d: %d after %d\n", a, d, prev);
            break;
        }
        if (d > AS11_RECONNECT_SLOW_DELAY_S) {
            n_fail++;
            printf("  FAIL attempt %d waits %d, longer than the slow phase %d\n",
                   a, d, AS11_RECONNECT_SLOW_DELAY_S);
            break;
        }
        prev = d;
    }
    n_pass++;   /* the loop above completed without tripping */

    /* THE NUMBER A USER WOULD FEEL: total wall time before the first slow
     * retry. 0 + 4 + 6 = 10 s of fast attempts, then a 60 s wait, so the
     * fourth attempt starts 70 s after the first. Asserted as a sum so that a
     * mutant changing any one step is caught even if it keeps the others. */
    long total = 0;
    for (int a = 1; a <= AS11_RECONNECT_FAST_ATTEMPTS + 1; a++) {
        total += as11_reconnect_delay_s(a);
    }
    check("70s from the first attempt to the first slow retry", total, 70);

    printf("as11_reconnect_test: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
