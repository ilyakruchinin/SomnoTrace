/*
 * SomnoTrace - Wellue/Viatom VLD3 binary decoder interface
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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OX_VLD3_HEADER_LEN 40u
#define OX_VLD3_RECORD_LEN 5u
#define OX_VLD3_VERSION 3u

typedef struct {
    uint8_t version;
    uint8_t mode;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint32_t declared_size;
    uint16_t duration_seconds;
    uint16_t asleep_seconds;
    uint32_t sample_count;
    uint32_t period_us;
    bool datetime_valid;
    bool declared_size_matches;
} ox_vld3_header_t;

typedef struct {
    uint8_t spo2;
    uint16_t pulse;
    uint8_t acceleration;
    uint8_t reserved;
} ox_vld3_record_t;

bool ox_vld3_parse_header(const uint8_t *data, size_t len, size_t source_size,
                          ox_vld3_header_t *out);
bool ox_vld3_parse_record(const uint8_t data[OX_VLD3_RECORD_LEN],
                          ox_vld3_record_t *out);
