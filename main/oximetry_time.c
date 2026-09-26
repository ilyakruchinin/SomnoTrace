/*
 * SomnoTrace - Civil time from oximeter recording names
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

#include "oximetry_time.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

static int days_in_month(int year, int mon)
{
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return mon == 2 && leap ? 29 : dim[mon - 1];
}

int64_t oximetry_civil_epoch_ms(int year, int mon, int day, int hour, int min, int sec)
{
    if (year < 2015 || year > 2099 || mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
        return 0;
    /* The round-trip below cannot catch an impossible date: mktime() normalises
     * tm IN PLACE (30 Feb -> 2 Mar) before check is compared against it.  So the
     * calendar is checked here, against the fields as given. */
    if (day > days_in_month(year, mon))
        return 0;
    struct tm tm = {0};
    tm.tm_year = year - 1900; tm.tm_mon = mon - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec; tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return 0;
    struct tm check;
    if (!localtime_r(&t, &check) || check.tm_year != tm.tm_year || check.tm_mon != tm.tm_mon ||
        check.tm_mday != tm.tm_mday || check.tm_hour != tm.tm_hour ||
        check.tm_min != tm.tm_min || check.tm_sec != tm.tm_sec)
        return 0;
    return (int64_t)t * 1000;
}

int64_t oximetry_filename_epoch_ms(const char *name)
{
    if (!name || strlen(name) < 14) return 0;
    for (int i = 0; i < 14; i++) if (name[i] < '0' || name[i] > '9') return 0;
    int year, mon, day, hour, min, sec;
    if (sscanf(name, "%4d%2d%2d%2d%2d%2d", &year, &mon, &day,
               &hour, &min, &sec) != 6) return 0;
    return oximetry_civil_epoch_ms(year, mon, day, hour, min, sec);
}
