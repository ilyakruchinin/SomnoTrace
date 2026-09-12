/*
 * SomnoTrace - salvage parser for truncated BLE advertising data
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
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * WHY THIS EXISTS AT ALL. NimBLE's ble_hs_adv_parse_fields() rejects an entire
 * advertising payload if any AD structure in it is malformed, and the AS11's
 * ADV_IND carries a trailing field (type 0x1b) that is one byte short — so a
 * perfectly good Local Name earlier in the same payload is thrown away with it.
 * The device would never appear in a scan. This walks the structures by hand and
 * keeps the ones that are intact.
 *
 * WHY IT IS ITS OWN TRANSLATION UNIT. It runs ONLY on payloads NimBLE has
 * already rejected as malformed — that is, only on the input least likely to be
 * seen in normal use and most likely to have been shaped deliberately, since
 * anything within radio range can advertise anything. That is the code you least
 * want to be untestable, and inside gap_event() it was: reaching it needed a
 * radio, a scan, and a peer emitting a broken packet. Here it is a function over
 * a byte buffer.
 *
 * It does NOT validate. Salvaging a malformed payload is the whole job; the
 * contract is only that whatever comes back points inside the caller's buffer.
 */

typedef struct {
    /* NOT NUL-terminated, and not validated as UTF-8 — raw advertising bytes.
     * NULL when no Local Name field survived. */
    const uint8_t *name;
    int            name_len;

    /* Raw little-endian 16-bit UUIDs, two bytes each, `num_uuids16` of them.
     * Deliberately kept as bytes rather than a uint16_t array: the field starts
     * at an arbitrary offset in the payload, so it carries no alignment
     * guarantee, and handing back a typed pointer would invite an unaligned
     * load. The caller casts as it always has; keeping the cast at the call
     * site keeps it visible. */
    const uint8_t *uuids16;
    int            num_uuids16;

    /* How many AD structures were walked before the end or the first bad one. */
    int            fields;
    /* True when the walk stopped on a malformed length rather than reaching the
     * end cleanly. Not an error — it is the normal case for the AS11 — but it
     * distinguishes "nothing was there" from "we stopped early". */
    bool           truncated;
} as11_adv_fields_t;

/**
 * Walk `len` bytes of advertising data, keeping the Local Name (AD types 0x08,
 * 0x09) and the 16-bit Service UUID list (0x02, 0x03).
 *
 * Every pointer returned in `out` points INSIDE [data, data+len). A NULL `data`
 * or a non-positive `len` yields an all-zero result rather than a fault.
 * `out` must not be NULL.
 */
void as11_adv_salvage(const uint8_t *data, int len, as11_adv_fields_t *out);
