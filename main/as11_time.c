/*
 * SomnoTrace - AS11 timezone and noon-day mapping
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

#define _POSIX_C_SOURCE 200809L

#include "as11_time.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "as11_time";

#define OFFSET_MIN_S      (-12 * 3600)
#define OFFSET_MAX_S      ( 14 * 3600)
#define OFFSET_STEP_S     (15 * 60)     /* every real-world offset is a
                                         * multiple of 15 minutes */
#define NOON_TOLERANCE_S  300           /* PeriodStart must be within 5 min
                                         * of a 15-min boundary to be
                                         * treated as a noon stamp */

static bool          s_have_offset = false;
static bool          s_offset_from_settings = false;
static as11_offset_t s_offset_s = 0;

/* ── Calendar helpers ─────────────────────────────────────────────────
 * Howard Hinnant's days-from-civil / civil-from-days algorithms (public
 * domain).  Used instead of timegm()/mktime() where a pure UTC conversion is
 * needed, so nothing depends on the process timezone. */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    const unsigned mm = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

/* Floor division that behaves correctly for negative epochs. */
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

static int64_t floor_mod(int64_t a, int64_t b)
{
    return a - floor_div(a, b) * b;
}

/* The ESP's own UTC offset at instant t, computed without relying on the
 * non-portable tm_gmtoff field. */
static as11_offset_t esp_utc_offset(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    int64_t as_utc = days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400
                     + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    return (as11_offset_t)(as_utc - (int64_t)t);
}

/* ── Offset state ─────────────────────────────────────────────────── */

void as11_time_set_offset(as11_offset_t offset_s, const char *src)
{
    if (offset_s < OFFSET_MIN_S || offset_s > OFFSET_MAX_S) return;

    if (!s_have_offset || s_offset_s != offset_s) {
        int mins = offset_s / 60;
        ESP_LOGI(TAG, "AS11 UTC offset %s%02d:%02d (from %s)",
                 mins < 0 ? "-" : "+", abs(mins) / 60, abs(mins) % 60,
                 src ? src : "?");
        (void)mins;
    }
    (void)src;
    s_offset_s = offset_s;
    s_have_offset = true;
}

bool as11_time_get_offset(as11_offset_t *out)
{
    if (!s_have_offset) return false;
    if (out) *out = s_offset_s;
    return true;
}

/* ── Offset discovery ─────────────────────────────────────────────── */

bool as11_time_offset_from_settings(const cJSON *settings, as11_offset_t *out)
{
    if (!settings) return false;

    /* Accept both the raw Get-RPC shape and the AS11 file shape. */
    const cJSON *root = cJSON_GetObjectItem(settings, "FlowGenerator");
    if (!root) root = settings;
    const cJSON *profiles = cJSON_GetObjectItem(root, "SettingProfiles");
    if (!profiles) return false;
    const cJSON *features = cJSON_GetObjectItem(profiles, "FeatureProfiles");
    if (!features) return false;
    const cJSON *tz = cJSON_GetObjectItem(features, "TimeZoneFeature");
    if (!tz) return false;
    const cJSON *off = cJSON_GetObjectItem(tz, "TimeZoneOffset");
    if (!off || !cJSON_IsString(off) || !off->valuestring) return false;

    /* Format is "±HH:MM". */
    const char *s = off->valuestring;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    int hh = 0, mm = 0;
    if (sscanf(s, "%d:%d", &hh, &mm) != 2) return false;
    if (hh < 0 || hh > 14 || mm < 0 || mm > 59) return false;

    as11_offset_t v = sign * (hh * 3600 + mm * 60);
    if (v < OFFSET_MIN_S || v > OFFSET_MAX_S) return false;

    s_offset_from_settings = true;
    as11_time_set_offset(v, "settings");
    if (out) *out = v;
    return true;
}

bool as11_time_offset_from_period_start(int64_t period_start_ms,
                                        as11_offset_t *out)
{
    if (period_start_ms <= 0) return false;

    int64_t secs = floor_div(period_start_ms, 1000);
    int64_t tod = floor_mod(secs, 86400);          /* UTC seconds-of-day */
    int64_t raw = 43200 - tod;                      /* noon - time-of-day */

    /* +13:00 noon and -11:00 noon land on the same UTC time-of-day, so pick
     * whichever candidate sits closest to the anchor offset (stored settings
     * offset if known, otherwise the ESP's own offset). */
    as11_offset_t anchor = (s_have_offset && s_offset_from_settings)
                           ? s_offset_s
                           : esp_utc_offset((time_t)secs);
    int64_t best = raw;
    int64_t best_d = llabs(raw - anchor);
    for (int k = -1; k <= 1; k += 2) {
        int64_t cand = raw + (int64_t)k * 86400;
        int64_t dist = llabs(cand - anchor);
        if (dist < best_d) { best = cand; best_d = dist; }
    }
    if (best < OFFSET_MIN_S || best > OFFSET_MAX_S) return false;

    /* Snap to a 15-minute boundary and refuse anything that is not close to
     * one: that means PeriodStart was not a noon stamp and the whole
     * derivation is invalid. */
    int64_t snapped = ((best + (best >= 0 ? OFFSET_STEP_S / 2 : -OFFSET_STEP_S / 2))
                       / OFFSET_STEP_S) * OFFSET_STEP_S;
    if (llabs(best - snapped) > NOON_TOLERANCE_S) {
        ESP_LOGW(TAG, "PeriodStart %lld is not a noon stamp "
                 "(implied offset %lld s) — keeping previous offset",
                 (long long)period_start_ms, (long long)best);
        return false;
    }
    if (snapped < OFFSET_MIN_S || snapped > OFFSET_MAX_S) return false;

    if (!s_offset_from_settings) {
        as11_time_set_offset((as11_offset_t)snapped, "period_start");
    }
    if (out) *out = (as11_offset_t)snapped;
    return true;
}

/* ── Day labelling ────────────────────────────────────────────────── */

static void noon_day_with_offset(int64_t as11_epoch_ms, as11_offset_t off,
                                 char *out, size_t out_len)
{
    int64_t local = floor_div(as11_epoch_ms, 1000) + off;
    int64_t days = floor_div(local, 86400);
    int64_t tod = floor_mod(local, 86400);
    if (tod < 43200) days -= 1;          /* before noon → previous day */
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    snprintf(out, out_len, "%04d%02d%02d", y, m, d);
}

/* Fallback behaviour: noon-day in ESP local time.  Used only when the AS11
 * offset is unknown. */
static void noon_day_esp_local(int64_t epoch_ms, char *out, size_t out_len)
{
    time_t t = (time_t)floor_div(epoch_ms, 1000);
    noon_day_with_offset(epoch_ms, esp_utc_offset(t), out, out_len);
}

void as11_time_noon_day(int64_t as11_epoch_ms, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    as11_offset_t off;
    if (as11_time_get_offset(&off)) {
        noon_day_with_offset(as11_epoch_ms, off, out, out_len);
    } else {
        noon_day_esp_local(as11_epoch_ms, out, out_len);
    }
}

void as11_time_noon_day_for_period_start(int64_t period_start_ms,
                                         char *out, size_t out_len)
{
    if (!out || out_len == 0) return;

    /* Derive from this record so a timezone or DST change part-way through
     * the retained window still labels each record on its own terms. */
    as11_offset_t off;
    if (as11_time_offset_from_period_start(period_start_ms, &off)) {
        noon_day_with_offset(period_start_ms, off, out, out_len);
        return;
    }
    as11_time_noon_day(period_start_ms, out, out_len);
}

/* ── Label → time helpers ─────────────────────────────────────────── */

static bool parse_label(const char *day_label, int *y, int *m, int *d)
{
    if (!day_label || strlen(day_label) != 8) return false;
    for (int i = 0; i < 8; i++) {
        if (day_label[i] < '0' || day_label[i] > '9') return false;
    }
    *y = (day_label[0] - '0') * 1000 + (day_label[1] - '0') * 100 +
         (day_label[2] - '0') * 10 + (day_label[3] - '0');
    *m = (day_label[4] - '0') * 10 + (day_label[5] - '0');
    *d = (day_label[6] - '0') * 10 + (day_label[7] - '0');
    return (*m >= 1 && *m <= 12 && *d >= 1 && *d <= 31);
}

int64_t as11_time_local_noon_epoch(const char *day_label)
{
    int y, m, d;
    if (!parse_label(day_label, &y, &m, &d)) return 0;

    struct tm tm = {
        .tm_year = y - 1900,
        .tm_mon = m - 1,
        .tm_mday = d,
        .tm_hour = 12,
        .tm_min = 0,
        .tm_sec = 0,
        .tm_isdst = -1,
    };
    time_t t = mktime(&tm);
    return (t == (time_t)-1) ? 0 : (int64_t)t;
}

int64_t as11_time_noon_period_end_ms(int64_t as11_epoch_ms)
{
    char day_label[16];
    as11_time_noon_day(as11_epoch_ms, day_label, sizeof(day_label));
    int y, m, d;
    if (!parse_label(day_label, &y, &m, &d)) return 0;

    struct tm tm = {
        .tm_year = y - 1900,
        .tm_mon = m - 1,
        .tm_mday = d + 1,        /* Period ends at next day's noon */
        .tm_hour = 12,
        .tm_min = 0,
        .tm_sec = 0,
        .tm_isdst = -1,
    };
    time_t t = mktime(&tm);
    return (t == (time_t)-1) ? 0 : ((int64_t)t * 1000);
}

int as11_time_day_number(const char *day_label)
{
    int y, m, d;
    if (!parse_label(day_label, &y, &m, &d)) return -1;
    return (int)days_from_civil(y, m, d);
}

int64_t as11_time_parse_iso8601_ms(const char *iso_str)
{
    if (!iso_str) return -1;
    struct tm ev_tm = {0};
    int ms = 0;
    int n = sscanf(iso_str, "%d-%d-%dT%d:%d:%d.%dZ",
                   &ev_tm.tm_year, &ev_tm.tm_mon, &ev_tm.tm_mday,
                   &ev_tm.tm_hour, &ev_tm.tm_min, &ev_tm.tm_sec, &ms);
    if (n < 6) {
        n = sscanf(iso_str, "%d-%d-%dT%d:%d:%dZ",
                   &ev_tm.tm_year, &ev_tm.tm_mon, &ev_tm.tm_mday,
                   &ev_tm.tm_hour, &ev_tm.tm_min, &ev_tm.tm_sec);
        if (n < 6) return -1;
        ms = 0;
    }
    int64_t days = days_from_civil(ev_tm.tm_year, ev_tm.tm_mon, ev_tm.tm_mday);
    int64_t secs = days * 86400 + ev_tm.tm_hour * 3600 +
                   ev_tm.tm_min * 60 + ev_tm.tm_sec;
    return secs * 1000 + ms;
}

void as11_time_format_edf_datetime(int64_t epoch_ms,
                                   char *date_out, int date_len,
                                   char *time_out, int time_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    if (date_out && date_len > 0) {
        snprintf(date_out, date_len, "%02d.%02d.%02d",
                 tm.tm_mday % 100, (tm.tm_mon + 1) % 100, (tm.tm_year % 100 + 100) % 100);
    }
    if (time_out && time_len > 0) {
        snprintf(time_out, time_len, "%02d.%02d.%02d",
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

void as11_time_format_recording_id(char *out, size_t out_len,
                                   int64_t epoch_ms,
                                   const char *srn, const char *mid, const char *vid)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);

    static const char *month_names[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    snprintf(out, out_len,
             "Startdate %02d-%s-%04d X X X SRN=%s MID=%s VID=%s",
             tm.tm_mday, month_names[tm.tm_mon % 12],
             tm.tm_year + 1900, srn ? srn : "", mid ? mid : "", vid ? vid : "");
}

void as11_time_format_session_prefix(int64_t epoch_ms, char *out, size_t out_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, out_len, "%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}
