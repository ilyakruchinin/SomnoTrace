/*
 * SomnoTrace - Host unit tests for civil time from oximeter recording names
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
#include <stdlib.h>
#include <time.h>

#include "oximetry_time.h"

/* Deliberately NOT assert(): a suite built with -DNDEBUG would elide every
 * assertion and still exit 0, which is a green run that checked nothing. */
static int g_failures;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));          \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* The parser resolves LOCAL time, so each test pins the zone it means. */
static void use_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

/* Expected values were computed independently (Python zoneinfo), not by
 * running this code, so a parser that agrees with itself cannot pass. */
static void test_known_answers(void)
{
    use_tz("UTC0");
    CHECK(oximetry_filename_epoch_ms("20260923031114") == 1790133074000LL, "UTC, a real ring name");
    CHECK(oximetry_filename_epoch_ms("20150101000000") == 1420070400000LL, "the first accepted instant");
    CHECK(oximetry_filename_epoch_ms("20991231235959") == 4102444799000LL, "the last accepted instant");
    CHECK(oximetry_filename_epoch_ms("20280229120000") == 1835438400000LL, "29 Feb of a leap year");

    use_tz("EST5EDT,M3.2.0,M11.1.0");
    CHECK(oximetry_filename_epoch_ms("20260923031114") == 1790147474000LL, "New York, summer (UTC-4)");
    CHECK(oximetry_filename_epoch_ms("20260123031114") == 1769155874000LL, "New York, winter (UTC-5)");
}

/* A ring whose clock was never set names its files from whatever epoch it
 * booted with.  Before this change the OxyII driver accepted those and filed
 * the night under that day. */
static void test_unset_clock_names_are_refused(void)
{
    use_tz("UTC0");
    CHECK(oximetry_filename_epoch_ms("20000101000000") == 0, "2000-01-01");
    CHECK(oximetry_filename_epoch_ms("19700101000000") == 0, "1970-01-01");
    CHECK(oximetry_filename_epoch_ms("20141231235959") == 0, "one second before the range");
    CHECK(oximetry_filename_epoch_ms("21000101000000") == 0, "one second after the range");
}

/* mktime() normalises these into a different real date.  The round-trip in
 * civil_epoch_ms() could not catch that, because mktime() rewrites the struct
 * it is compared against. */
static void test_impossible_dates_are_refused(void)
{
    use_tz("UTC0");
    CHECK(oximetry_filename_epoch_ms("20260230120000") == 0, "30 Feb");
    CHECK(oximetry_filename_epoch_ms("20270229120000") == 0, "29 Feb, not a leap year");
    CHECK(oximetry_filename_epoch_ms("21000229120000") == 0, "29 Feb 2100 (range and century rule)");
    CHECK(oximetry_filename_epoch_ms("20260431120000") == 0, "31 Apr");
    CHECK(oximetry_civil_epoch_ms(2026, 2, 30, 12, 0, 0) == 0, "30 Feb via the header path");
    CHECK(oximetry_civil_epoch_ms(2024, 2, 29, 12, 0, 0) != 0, "29 Feb 2024 via the header path");
}

static void test_field_ranges(void)
{
    use_tz("UTC0");
    CHECK(oximetry_filename_epoch_ms("20261301000000") == 0, "month 13");
    CHECK(oximetry_filename_epoch_ms("20260001000000") == 0, "month 0");
    CHECK(oximetry_filename_epoch_ms("20260100000000") == 0, "day 0");
    CHECK(oximetry_filename_epoch_ms("20260132000000") == 0, "day 32");
    CHECK(oximetry_filename_epoch_ms("20260101240000") == 0, "hour 24");
    CHECK(oximetry_filename_epoch_ms("20260101006000") == 0, "minute 60");
    CHECK(oximetry_filename_epoch_ms("20260101000060") == 0, "second 60");
    CHECK(oximetry_filename_epoch_ms("20261231235959") != 0, "every field at its maximum");
}

static void test_malformed_names(void)
{
    use_tz("UTC0");
    CHECK(oximetry_filename_epoch_ms(NULL) == 0, "NULL");
    CHECK(oximetry_filename_epoch_ms("") == 0, "empty");
    CHECK(oximetry_filename_epoch_ms("2026092303111") == 0, "13 digits");
    CHECK(oximetry_filename_epoch_ms("2026O923031114") == 0, "a letter O among the digits");
    CHECK(oximetry_filename_epoch_ms("20260923031114.vld") == 1790133074000LL, "a suffix after 14 digits");
}

/* Anti-vacuity: the seconds field reaches the result, one-for-one. */
static void test_resolution(void)
{
    use_tz("UTC0");
    int64_t a = oximetry_filename_epoch_ms("20260923031114");
    int64_t b = oximetry_filename_epoch_ms("20260923031115");
    CHECK(a != 0 && b - a == 1000, "names one second apart are 1000 ms apart");
}

int main(void)
{
    test_known_answers();
    test_unset_clock_names_are_refused();
    test_impossible_dates_are_refused();
    test_field_ranges();
    test_malformed_names();
    test_resolution();

    if (g_failures) {
        printf("oximetry_time tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("oximetry_time tests passed\n");
    return 0;
}
