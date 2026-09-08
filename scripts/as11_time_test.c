/*
 * SomnoTrace - Host unit tests for AS11 timezone and noon-day mapping
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

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "as11_time.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;

static void set_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

static void expect_str(const char *what, const char *got, const char *want)
{
    bool ok = strcmp(got, want) == 0;
    printf("  %-54s got=%-10s want=%-10s %s\n", what, got, want, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static void expect_int(const char *what, long long got, long long want)
{
    bool ok = (got == want);
    printf("  %-54s got=%-10lld want=%-10lld %s\n", what, got, want, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

/* Reset the cached offset between scenarios by forcing a fresh derivation. */
static void reset_offset(void)
{
    as11_time_set_offset(0, "reset");
}

static void check_period_end(const char *what, int y, int m, int d, int h, int min, const char *want_dt)
{
    struct tm tm = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = h, .tm_min = min, .tm_isdst = -1 };
    time_t t = mktime(&tm);
    int64_t end_ms = as11_time_noon_period_end_ms((int64_t)t * 1000);
    time_t end_t = (time_t)(end_ms / 1000);
    struct tm end_tm;
    localtime_r(&end_t, &end_tm);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             end_tm.tm_year + 1900, end_tm.tm_mon + 1, end_tm.tm_mday,
             end_tm.tm_hour, end_tm.tm_min);
    expect_str(what, buf, want_dt);
}

/* Feed one TimeZoneOffset string through the settings parser, in the FlowGenerator shape
 * Scenario 4 builds by hand. A successful parse also makes the value the anchor for every
 * later noon-stamp derivation, which is what Scenario 11 relies on. */
static bool parse_tz_setting(const char *value, as11_offset_t *out)
{
    cJSON n_off = { .child = NULL, .next = NULL, .string = "TimeZoneOffset", .valuestring = (char *)value, .type = cJSON_String };
    cJSON n_tz = { .child = &n_off, .next = NULL, .string = "TimeZoneFeature", .valuestring = NULL, .type = 0 };
    cJSON n_fp = { .child = &n_tz, .next = NULL, .string = "FeatureProfiles", .valuestring = NULL, .type = 0 };
    cJSON n_sp = { .child = &n_fp, .next = NULL, .string = "SettingProfiles", .valuestring = NULL, .type = 0 };
    cJSON n_fg = { .child = &n_sp, .next = NULL, .string = "FlowGenerator", .valuestring = NULL, .type = 0 };
    return as11_time_offset_from_settings(&n_fg, out);
}

int main(void)
{
    printf("\n=== Scenario 1: Issue #75 Vector (AS11 -05:00 vs ESP MST7MDT) ===\n");
    set_tz("MST7MDT,M3.2.0,M11.1.0");
    reset_offset();

    const int64_t PS_AUG7 = 1786122000000LL;
    as11_offset_t off = 0;
    bool derived = as11_time_offset_from_period_start(PS_AUG7, &off);
    expect_int("offset derived from PeriodStart (s)", derived ? off : -999999, -18000);

    char day[16];
    as11_time_noon_day_for_period_start(PS_AUG7, day, sizeof(day));
    expect_str("day label for the Aug 7 summary record", day, "20260807");
    as11_time_noon_day_for_period_start(PS_AUG7 - 86400000LL, day, sizeof(day));
    expect_str("previous day's record", day, "20260806");
    as11_time_noon_day_for_period_start(PS_AUG7 + 86400000LL, day, sizeof(day));
    expect_str("next day's record", day, "20260808");

    expect_int("day number for 20260807 (STR Date)",
               as11_time_day_number("20260807"), 20672);
    int64_t noon = as11_time_local_noon_epoch("20260807");
    expect_int("ESP-local noon epoch for 20260807", noon, 1786125600LL);

    printf("\n=== Scenario 2: Control (AS11 and ESP both +10:00 AEST) ===\n");
    set_tz("AEST-10AEDT,M10.1.0,M4.1.0/3");
    reset_offset();
    const int64_t PS_MEL = 1782698400000LL;
    derived = as11_time_offset_from_period_start(PS_MEL, &off);
    expect_int("offset derived (s)", derived ? off : -999999, 36000);
    as11_time_noon_day_for_period_start(PS_MEL, day, sizeof(day));
    expect_str("day label", day, "20260629");

    printf("\n=== Scenario 3: +13:00 NZDT / Pacific Disambiguation ===\n");
    set_tz("NZST-12NZDT,M9.5.0,M4.1.0/3");
    reset_offset();
    struct tm t = {0};
    t.tm_year = 126; t.tm_mon = 0; t.tm_mday = 14; t.tm_hour = 23;
    int64_t ps_nz = (int64_t)timegm(&t) * 1000;
    derived = as11_time_offset_from_period_start(ps_nz, &off);
    expect_int("offset derived (s)", derived ? off : -999999, 46800);
    as11_time_noon_day_for_period_start(ps_nz, day, sizeof(day));
    expect_str("day label", day, "20260115");

    printf("\n=== Scenario 4: Settings Authoritative Preservation (+13:00) ===\n");
    reset_offset();
    set_tz("UTC0");
    cJSON node_off = { .child = NULL, .next = NULL, .string = "TimeZoneOffset", .valuestring = "+13:00", .type = cJSON_String };
    cJSON node_tz = { .child = &node_off, .next = NULL, .string = "TimeZoneFeature", .valuestring = NULL, .type = 0 };
    cJSON node_fp = { .child = &node_tz, .next = NULL, .string = "FeatureProfiles", .valuestring = NULL, .type = 0 };
    cJSON node_sp = { .child = &node_fp, .next = NULL, .string = "SettingProfiles", .valuestring = NULL, .type = 0 };
    cJSON node_fg = { .child = &node_sp, .next = NULL, .string = "FlowGenerator", .valuestring = NULL, .type = 0 };
    bool s_ok = as11_time_offset_from_settings(&node_fg, &off);
    expect_int("settings offset parsed", s_ok ? off : -999999, 46800);
    derived = as11_time_offset_from_period_start(ps_nz, &off);
    expect_int("period_start respects settings offset anchor", derived ? off : -999999, 46800);

    printf("\n=== Scenario 5: 23-Hour Spring-Forward Transition Fallback ===\n");
    set_tz("EST5EDT,M3.2.0,M11.1.0");
    reset_offset();
    as11_time_noon_day(1772992800LL * 1000LL, day, sizeof(day));
    expect_str("Mar 8 14:00 EDT (afternoon session)", day, "20260308");
    as11_time_noon_day(1773030600LL * 1000LL, day, sizeof(day));
    expect_str("Mar 9 00:30 EDT (post-midnight on 23h transition night)", day, "20260308");
    as11_time_noon_day(1773073800LL * 1000LL, day, sizeof(day));
    expect_str("Mar 9 12:30 EDT (next noon-day)", day, "20260309");

    /* Europe/Prague (spring forward on 2026-03-29) */
    set_tz("CET-1CEST,M3.5.0,M10.5.0/3");
    reset_offset();
    struct tm prague_tm = { .tm_year = 126, .tm_mon = 2, .tm_mday = 30, .tm_hour = 0, .tm_min = 30, .tm_isdst = -1 };
    time_t t_prague = mktime(&prague_tm);
    as11_time_noon_day((int64_t)t_prague * 1000, day, sizeof(day));
    expect_str("Prague Mar 30 00:30 (post-midnight on 23h night)", day, "20260329");
    prague_tm.tm_min = 59;
    t_prague = mktime(&prague_tm);
    as11_time_noon_day((int64_t)t_prague * 1000, day, sizeof(day));
    expect_str("Prague Mar 30 00:59", day, "20260329");
    prague_tm.tm_hour = 1; prague_tm.tm_min = 0;
    t_prague = mktime(&prague_tm);
    as11_time_noon_day((int64_t)t_prague * 1000, day, sizeof(day));
    expect_str("Prague Mar 30 01:00", day, "20260329");

    /* Australia/Sydney (spring forward on 2026-10-04) */
    set_tz("AEST-10AEDT,M10.1.0,M4.1.0/3");
    reset_offset();
    struct tm syd_tm = { .tm_year = 126, .tm_mon = 9, .tm_mday = 5, .tm_hour = 0, .tm_min = 30, .tm_isdst = -1 };
    time_t t_syd = mktime(&syd_tm);
    as11_time_noon_day((int64_t)t_syd * 1000, day, sizeof(day));
    expect_str("Sydney Oct 5 00:30 (post-midnight on 23h night)", day, "20261004");

    printf("\n=== Scenario 6: Non-Noon PeriodStart Rejection ===\n");
    reset_offset();
    derived = as11_time_offset_from_period_start(PS_AUG7 + 7 * 60 * 1000, &off);
    expect_int("derivation refused (7 min past noon)", derived ? 1 : 0, 0);

    printf("\n=== Scenario 7: Session Timestamps -> AS11 Noon-Day ===\n");
    reset_offset();
    as11_time_set_offset(-18000, "test");
    as11_time_noon_day(1786138713027LL, day, sizeof(day));
    expect_str("evening session belongs to its own day", day, "20260807");
    as11_time_noon_day(1786197600000LL, day, sizeof(day));
    expect_str("post-midnight session belongs to previous day", day, "20260807");

    printf("\n=== Scenario 8: Noon Period End Boundary Normalization ===\n");
    set_tz("CET-1CEST,M3.5.0,M10.5.0/3");
    reset_offset();
    check_period_end("Sat Mar 28 23:00 (evening before spring-forward)", 2026, 3, 28, 23, 0, "2026-03-29 12:00");
    check_period_end("Sun Mar 29 05:00 (morning of transition, before noon)", 2026, 3, 29, 5, 0, "2026-03-29 12:00");
    check_period_end("Sun Mar 29 14:00 (afternoon of transition)", 2026, 3, 29, 14, 0, "2026-03-30 12:00");
    check_period_end("Mar 31 23:00 (month rollover)", 2026, 3, 31, 23, 0, "2026-04-01 12:00");
    check_period_end("Dec 31 23:00 (year rollover)", 2026, 12, 31, 23, 0, "2027-01-01 12:00");
    check_period_end("Feb 28 23:00 (leap year rollover)", 2024, 2, 28, 23, 0, "2024-02-29 12:00");
    check_period_end("Oct 24 23:00 (evening before fall-back)", 2026, 10, 24, 23, 0, "2026-10-25 12:00");

    printf("\n=== Scenario 9: Civil-Date Arithmetic at the February Boundary ===\n");
    /* days_from_civil() is Hinnant's algorithm: the year is shifted so it starts on
     * March 1, and the shift only fires for January and February. Every scenario above
     * reaches it with a March-or-later date, so a wrong shift (m < 2, m >= 2) would
     * pass the whole suite. The event-timestamp parser is the only caller that sees
     * arbitrary dates and had no host test. Expected values are from an independent
     * calendar (Python datetime), not from the function under test. */
    expect_int("ISO event on leap day 2024-02-29T12:00:00.000Z",
               as11_time_parse_iso8601_ms("2024-02-29T12:00:00.000Z"), 1709208000000LL);
    expect_int("ISO event last ms of Feb 28 (leap year)",
               as11_time_parse_iso8601_ms("2024-02-28T23:59:59.999Z"), 1709164799999LL);
    expect_int("ISO event last second of Feb 28 (non-leap, no ms)",
               as11_time_parse_iso8601_ms("2023-02-28T23:59:59Z"), 1677628799000LL);
    expect_int("ISO event first second of Mar 1 (non-leap, no ms)",
               as11_time_parse_iso8601_ms("2023-03-01T00:00:00Z"), 1677628800000LL);
    expect_int("ISO event malformed returns -1",
               as11_time_parse_iso8601_ms("2024-02-29"), -1);
    expect_int("day number for 20240229 (leap day)",
               as11_time_day_number("20240229"), 19782);
    expect_int("day number for 20240301 (day after leap day)",
               as11_time_day_number("20240301"), 19783);
    /* 2100 is the first non-leap century year in the era that began in 2000; the
     * doe/36524 term is inert for every date before it. */
    expect_int("ISO event on 2100-03-01 (century non-leap boundary)",
               as11_time_parse_iso8601_ms("2100-03-01T00:00:00Z"), 4107542400000LL);
    expect_int("day number for 21000301",
               as11_time_day_number("21000301"), 47541);
    /* The reverse direction, civil_from_days(), is reached only through the noon-day
     * label. Pin the offset so the label is a pure function of the epoch. */
    as11_time_set_offset(0, "test");
    as11_time_noon_day(4107589200000LL, day, sizeof(day));   /* 2100-03-01T13:00Z */
    expect_str("noon-day label on 2100-03-01 (century boundary)", day, "21000301");
    as11_time_noon_day(4107502800000LL, day, sizeof(day));   /* 2100-02-28T13:00Z */
    expect_str("noon-day label on 2100-02-28 (2100 is not a leap year)", day, "21000228");
    reset_offset();

    printf("\n=== Scenario 10: Settings Offset Parser Boundaries ===\n");
    reset_offset();
    /* Every earlier settings case is a whole hour in the interior of [-12:00, +14:00], so
     * hh*3600 + mm*60 was never told apart from hh*3600 - mm*60, and neither bound was
     * ever shown to be inclusive. */
    expect_int("+05:30 (half-hour offset)", parse_tz_setting("+05:30", &off) ? off : -999999, 19800);
    expect_int("-03:30 (negative half-hour)", parse_tz_setting("-03:30", &off) ? off : -999999, -12600);
    expect_int("+00:00 (hour zero is a valid hour)", parse_tz_setting("+00:00", &off) ? off : -999999, 0);
    expect_int("-12:00 (lower bound is inclusive)", parse_tz_setting("-12:00", &off) ? off : -999999, -43200);
    expect_int("+14:00 (upper bound is inclusive)", parse_tz_setting("+14:00", &off) ? off : -999999, 50400);
    expect_int("-12:15 refused (below lower bound)", parse_tz_setting("-12:15", &off) ? 1 : 0, 0);
    expect_int("+14:15 refused (above upper bound)", parse_tz_setting("+14:15", &off) ? 1 : 0, 0);
    expect_int("+15:00 refused (hour out of range)", parse_tz_setting("+15:00", &off) ? 1 : 0, 0);
    /* The sign is optional: an unsigned value reads as positive, and its first digit must
     * not be swallowed as though it were a sign character. */
    expect_int("10:00 (unsigned reads as +10:00)", parse_tz_setting("10:00", &off) ? off : -999999, 36000);

    /* as11_time_set_offset() applies the same bounds, inclusive, and ignores the rest. */
    as11_time_set_offset(-43200, "test");
    expect_int("set_offset accepts -12:00", as11_time_get_offset(&off) ? off : -999999, -43200);
    as11_time_set_offset(-43201, "test");
    expect_int("set_offset ignores 1 s below -12:00", as11_time_get_offset(&off) ? off : -999999, -43200);
    as11_time_set_offset(50400, "test");
    expect_int("set_offset accepts +14:00", as11_time_get_offset(&off) ? off : -999999, 50400);
    as11_time_set_offset(50401, "test");
    expect_int("set_offset ignores 1 s above +14:00", as11_time_get_offset(&off) ? off : -999999, 50400);

    printf("\n=== Scenario 11: Noon-Stamp Derivation Edges ===\n");
    /* PS_MEL is 2026-06-29T02:00:00Z, local noon at +10:00. Anchor +10:00 through the
     * settings parser so the derivation is compared against a known value, not the host TZ. */
    parse_tz_setting("+10:00", &off);
    expect_int("PeriodStart 0 refused", as11_time_offset_from_period_start(0, &off) ? 1 : 0, 0);
    expect_int("noon + 5:00 accepted (at tolerance)",
               as11_time_offset_from_period_start(PS_MEL + 300 * 1000, &off) ? off : -999999, 36000);
    expect_int("noon - 5:00 accepted (at tolerance)",
               as11_time_offset_from_period_start(PS_MEL - 300 * 1000, &off) ? off : -999999, 36000);
    expect_int("noon + 5:01 refused",
               as11_time_offset_from_period_start(PS_MEL + 301 * 1000, &off) ? 1 : 0, 0);

    /* 00:00Z is local noon at +12:00 and at -12:00 alike; the anchor decides, and -12:00 is
     * the lower bound itself. */
    const int64_t PS_MIDNIGHT_Z = 1782691200000LL;   /* 2026-06-29T00:00:00Z */
    parse_tz_setting("-10:00", &off);
    expect_int("00:00Z, anchor -10:00 -> -12:00 (the bound itself)",
               as11_time_offset_from_period_start(PS_MIDNIGHT_Z, &off) ? off : -999999, -43200);
    parse_tz_setting("+10:00", &off);
    expect_int("00:00Z, anchor +10:00 -> +12:00",
               as11_time_offset_from_period_start(PS_MIDNIGHT_Z, &off) ? off : -999999, 43200);
    parse_tz_setting("+00:00", &off);
    expect_int("00:00Z, anchor 00:00 -> +12:00 (tie keeps the raw candidate)",
               as11_time_offset_from_period_start(PS_MIDNIGHT_Z, &off) ? off : -999999, 43200);
    reset_offset();

    printf("\n=== Scenario 12: Header Formatters ===\n");
    /* The three formatters that write EDF header fields and the session prefix are
     * reached by no host test: the edf suites build headers through their own
     * fixtures. All three go through localtime_r(), so a fixed non-UTC zone is
     * pinned and the expected strings come from Python datetime in that zone; in
     * UTC a gmtime_r() swap would pass unnoticed. EST5 has no DST rule, so the
     * result does not depend on the date's position in the year. */
    set_tz("EST5");
    char edf_date[9], edf_time[9], rec_id[128], prefix[24];

    /* Scenario 7's evening session: 2026-08-07T21:38:33.027Z = 16:38:33 EST. */
    as11_time_format_edf_datetime(1786138713027LL, edf_date, sizeof(edf_date),
                                  edf_time, sizeof(edf_time));
    expect_str("EDF startdate dd.mm.yy (local)", edf_date, "07.08.26");
    expect_str("EDF starttime hh.mm.ss (local, ms dropped)", edf_time, "16.38.33");
    as11_time_format_session_prefix(1786138713027LL, prefix, sizeof(prefix));
    expect_str("session prefix yyyymmdd_hhmmss (local)", prefix, "20260807_163833");
    as11_time_format_recording_id(rec_id, sizeof(rec_id), 1786138713027LL,
                                  "23241234567", "AS11", "ResMed");
    expect_str("recording id", rec_id,
               "Startdate 07-AUG-2026 X X X SRN=23241234567 MID=AS11 VID=ResMed");

    /* Last month of the year: the month-name table has twelve entries and DEC is
     * the one an off-by-one index runs past. 2026-12-31T23:30:00Z = 18:30 EST. */
    as11_time_format_edf_datetime(1798759800000LL, edf_date, sizeof(edf_date),
                                  edf_time, sizeof(edf_time));
    expect_str("EDF startdate on Dec 31", edf_date, "31.12.26");
    expect_str("EDF starttime 18:30:00", edf_time, "18.30.00");
    as11_time_format_recording_id(rec_id, sizeof(rec_id), 1798759800000LL, "1", "2", "3");
    expect_str("recording id in DEC", rec_id, "Startdate 31-DEC-2026 X X X SRN=1 MID=2 VID=3");

    /* Two-digit year: 2009 must print "09", not "9", and the century wrap in 2100
     * must print "00". The 2100 epoch is 00:00Z, which the -5 h zone moves back to
     * Feb 28, so the date and the year both come from the local conversion. */
    as11_time_format_edf_datetime(1231156800000LL, edf_date, sizeof(edf_date),
                                  edf_time, sizeof(edf_time));
    expect_str("EDF startdate in 2009 keeps the leading zero", edf_date, "05.01.09");
    as11_time_format_session_prefix(1231156800000LL, prefix, sizeof(prefix));
    expect_str("session prefix in JAN 2009", prefix, "20090105_070000");
    as11_time_format_edf_datetime(4107542400000LL, edf_date, sizeof(edf_date),
                                  edf_time, sizeof(edf_time));
    expect_str("EDF startdate 2100-03-01T00:00Z is 28.02.00 local", edf_date, "28.02.00");
    as11_time_format_recording_id(rec_id, sizeof(rec_id), 4107542400000LL, "s", "m", "v");
    expect_str("recording id in 2100 prints the four-digit year", rec_id,
               "Startdate 28-FEB-2100 X X X SRN=s MID=m VID=v");

    /* NULL identifiers print as empty fields rather than "(null)" or a crash; a
     * NULL date or time output is skipped rather than written. */
    as11_time_format_recording_id(rec_id, sizeof(rec_id), 1786138713027LL, NULL, NULL, NULL);
    expect_str("recording id with NULL identifiers", rec_id,
               "Startdate 07-AUG-2026 X X X SRN= MID= VID=");
    strcpy(edf_time, "keep");
    as11_time_format_edf_datetime(1786138713027LL, edf_date, sizeof(edf_date), NULL, 0);
    expect_str("date written when time_out is NULL", edf_date, "07.08.26");
    as11_time_format_edf_datetime(1786138713027LL, NULL, 0, edf_time, 0);
    expect_str("time_len 0 leaves time_out untouched", edf_time, "keep");
    reset_offset();
    printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
