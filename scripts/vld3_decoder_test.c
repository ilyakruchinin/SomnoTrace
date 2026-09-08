/*
 * SomnoTrace - Synthetic host tests for the Wellue/Viatom VLD3 decoder
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

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void le16(uint8_t *p, uint16_t value)
{
    p[0] = value & 0xff;
    p[1] = value >> 8;
}

static void le32(uint8_t *p, uint32_t value)
{
    p[0] = value & 0xff;
    p[1] = (value >> 8) & 0xff;
    p[2] = (value >> 16) & 0xff;
    p[3] = value >> 24;
}

static void make_header(uint8_t header[OX_VLD3_HEADER_LEN], uint8_t mode,
                        uint16_t year, uint8_t month, uint8_t day,
                        uint32_t samples, uint16_t duration)
{
    memset(header, 0, OX_VLD3_HEADER_LEN);
    header[0] = OX_VLD3_VERSION;
    header[1] = mode;
    le16(header + 2, year);
    header[4] = month;
    header[5] = day;
    header[6] = 22;
    header[7] = 24;
    header[8] = 42;
    le32(header + 9, OX_VLD3_HEADER_LEN + samples * OX_VLD3_RECORD_LEN);
    le16(header + 13, duration);
}

int main(void)
{
    uint8_t header[OX_VLD3_HEADER_LEN];
    ox_vld3_header_t parsed;
    make_header(header, 0, 2026, 8, 29, 5485, 21940);
    le16(header + 15, 19800);
    assert(ox_vld3_parse_header(header, sizeof(header), 27465, &parsed));
    assert(parsed.duration_seconds == 21940 && parsed.asleep_seconds == 19800);
    assert(parsed.version == 3 && parsed.mode == 0 && parsed.year == 2026);
    assert(parsed.month == 8 && parsed.day == 29 && parsed.hour == 22);
    assert(parsed.minute == 24 && parsed.second == 42);
    assert(parsed.sample_count == 5485 && parsed.period_us == 4000000);
    assert(parsed.datetime_valid && parsed.declared_size_matches);

    make_header(header, 1, 2024, 2, 29, 120, 240);
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed));
    assert(parsed.mode == 1 && parsed.period_us == 2000000);
    assert(parsed.datetime_valid);                  /* Feb 29 in a leap year is a real day */
    le32(header + 9, 1234);
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed));
    assert(!parsed.declared_size_matches);

    header[5] = 30;
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed));
    assert(!parsed.datetime_valid);
    make_header(header, 0, 2023, 2, 29, 120, 240);
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed));
    assert(!parsed.datetime_valid);                 /* ...and not in a common year */

    /* Bounds of the date and time-of-day checks, each side of every edge. */
    make_header(header, 0, 2015, 1, 1, 120, 240);
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed) && parsed.datetime_valid);
    make_header(header, 0, 2014, 12, 31, 120, 240);
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed) && !parsed.datetime_valid);
    make_header(header, 0, 2099, 12, 31, 120, 240);
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed) && parsed.datetime_valid);
    make_header(header, 0, 2100, 1, 1, 120, 240);
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed) && !parsed.datetime_valid);
    make_header(header, 0, 2026, 8, 31, 120, 240);
    header[6] = 23; header[7] = 59; header[8] = 59;
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed) && parsed.datetime_valid);
    header[6] = 24;
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed) && !parsed.datetime_valid);
    header[6] = 23; header[7] = 60;
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed) && !parsed.datetime_valid);
    header[7] = 59; header[8] = 60;
    assert(ox_vld3_parse_header(header, sizeof(header), 640, &parsed) && !parsed.datetime_valid);

    make_header(header, 0, 2026, 1, 1, 120, 361);
    assert(!ox_vld3_parse_header(header, sizeof(header), 640, &parsed));
    make_header(header, 0, 2026, 1, 1, 120, 480);
    assert(!ox_vld3_parse_header(header, sizeof(header), 639, &parsed));

    assert(!ox_vld3_parse_header(NULL, sizeof(header), 640, &parsed));
    assert(!ox_vld3_parse_header(header, sizeof(header), 640, NULL));

    uint8_t record[OX_VLD3_RECORD_LEN] = { 97, 0x2c, 0x01, 7, 0x40 };
    ox_vld3_record_t sample;
    assert(!ox_vld3_parse_record(NULL, &sample));
    assert(!ox_vld3_parse_record(record, NULL));
    assert(ox_vld3_parse_record(record, &sample));
    assert(sample.spo2 == 97 && sample.pulse == 300);
    assert(sample.acceleration == 7 && sample.reserved == 0x40);

    record[0] = 0xff;
    record[1] = 0xff;
    record[2] = 0xff;
    assert(ox_vld3_parse_record(record, &sample));
    assert(sample.spo2 == 0xff && sample.pulse == 0xffff);
    puts("vld3 decoder tests passed");
    return 0;
}
