/*
 * SomnoTrace - Wellue/Viatom VLD3 binary decoder
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

#include "oximetry_vld3.h"

#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool leap_year(uint16_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static bool valid_date(uint16_t year, uint8_t month, uint8_t day)
{
    static const uint8_t days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (year < 2015 || year > 2099 || month < 1 || month > 12 || day < 1)
        return false;
    uint8_t max_day = days[month - 1] + (month == 2 && leap_year(year));
    return day <= max_day;
}

bool ox_vld3_parse_header(const uint8_t *data, size_t len, size_t source_size,
                          ox_vld3_header_t *out)
{
    if (!data || !out || len < OX_VLD3_HEADER_LEN ||
        source_size < OX_VLD3_HEADER_LEN + OX_VLD3_RECORD_LEN)
        return false;
    size_t body_size = source_size - OX_VLD3_HEADER_LEN;
    if (body_size % OX_VLD3_RECORD_LEN != 0)
        return false;

    ox_vld3_header_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.version = data[0];
    parsed.mode = data[1];
    parsed.year = le16(data + 2);
    parsed.month = data[4];
    parsed.day = data[5];
    parsed.hour = data[6];
    parsed.minute = data[7];
    parsed.second = data[8];
    parsed.declared_size = le32(data + 9);
    parsed.duration_seconds = le16(data + 13);
    parsed.asleep_seconds = le16(data + 15);
    parsed.sample_count = body_size / OX_VLD3_RECORD_LEN;
    parsed.datetime_valid = valid_date(parsed.year, parsed.month, parsed.day) &&
                            parsed.hour <= 23 && parsed.minute <= 59 && parsed.second <= 59;
    parsed.declared_size_matches = parsed.declared_size == source_size;

    if (parsed.version != OX_VLD3_VERSION || parsed.mode > 1 ||
        parsed.sample_count == 0 || parsed.duration_seconds == 0)
        return false;

    uint64_t period_num = (uint64_t)parsed.duration_seconds * 1000000ULL;
    if (period_num % parsed.sample_count != 0)
        return false;
    uint64_t period_us = period_num / parsed.sample_count;
    if (period_us != 2000000ULL && period_us != 4000000ULL)
        return false;
    parsed.period_us = (uint32_t)period_us;
    *out = parsed;
    return true;
}

bool ox_vld3_parse_record(const uint8_t data[OX_VLD3_RECORD_LEN],
                          ox_vld3_record_t *out)
{
    if (!data || !out)
        return false;
    out->spo2 = data[0];
    out->pulse = le16(data + 1);
    out->acceleration = data[3];
    out->reserved = data[4];
    return true;
}
