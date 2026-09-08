/*
 * SomnoTrace - Unified SNT file format and validation helpers
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

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── .snt file format constants ─────────────────────────────────────── */

#define SNT_MAGIC          0x534E5442u  /* "SNTB" in little-endian */
#define SNT_VERSION        2            /* current SNT writer version */
#define SNT_MISSING_V1     (-1)         /* v1 missing-data sentinel */
#define SNT_MISSING_V2     INT16_MIN    /* v2 unambiguous missing-data sentinel */
#define SNT_MISSING        SNT_MISSING_V2

#define SNT_TIER_RAW       0            /* L0 raw sample stream */
#define SNT_TIER_MINMAX    1            /* L1 MinMax stream */

/* Authoritative 28-byte packed SNT header */
typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 0x534E5442 "SNTB"                  */
    uint8_t  version;          /* format version (1, 2)              */
    uint8_t  tier;             /* 0 = L0 raw, 1 = L1 MinMax          */
    uint8_t  n_channels;       /* channels per record                 */
    uint8_t  sample_bytes;     /* 2 (int16)                           */
    uint16_t sample_hz_x10;   /* rate × 10 (250 = 25 Hz)            */
    uint16_t reserved;
    int64_t  start_epoch_ms;  /* session start (NTP clock)           */
    uint32_t sample_count;    /* records written (updated each flush) */
    uint32_t reserved2;
} snt_header_t;              /* exactly 28 bytes (packed) */

#define SNT_HEADER_SIZE    sizeof(snt_header_t)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(snt_header_t) == 28, "snt_header_t must be exactly 28 bytes");
#endif

/* ── Inline validation and format helpers ──────────────────────────── */

static inline int16_t snt_missing_for(uint8_t version)
{
    return (version >= 2) ? SNT_MISSING_V2 : SNT_MISSING_V1;
}

static inline int16_t clamp_i16(int val, int16_t min_val, int16_t max_val)
{
    if (val > max_val) return max_val;
    if (val < min_val) return min_val;
    return (int16_t)val;
}

static inline bool is_channel_map_valid(const int *map, int n_signals, int snt_channels)
{
    if (!map) return (snt_channels == n_signals);
    for (int i = 0; i < n_signals; i++) {
        if (map[i] < 0 || map[i] >= snt_channels) return false;
    }
    return true;
}

static inline int snt_read_header(FILE *f, snt_header_t *hdr)
{
    if (!f || !hdr) return -1;
    if (fread(hdr, 1, sizeof(snt_header_t), f) != sizeof(snt_header_t)) {
        return -1;
    }
    if (hdr->magic != SNT_MAGIC) {
        return -1;
    }
    return 0;
}


#ifdef __cplusplus
}
#endif
