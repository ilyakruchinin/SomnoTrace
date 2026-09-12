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
#include "as11_adv.h"

#include <string.h>

/* AD types, Core Specification Supplement Part A §1. */
#define AD_TYPE_UUID16_INCOMPLETE   0x02
#define AD_TYPE_UUID16_COMPLETE     0x03
#define AD_TYPE_NAME_SHORTENED      0x08
#define AD_TYPE_NAME_COMPLETE       0x09

void as11_adv_salvage(const uint8_t *data, int len, as11_adv_fields_t *out)
{
    memset(out, 0, sizeof(*out));
    if (data == NULL || len <= 0) {
        return;
    }

    /* An AD structure is: length byte, type byte, then length-1 bytes of data.
     * The length byte counts the TYPE as well as the data, which is the detail
     * every hand-written parser of this format gets wrong once. */
    for (int off = 0; off + 1 < len; ) {
        uint8_t ad_len = data[off];

        /* TWO WAYS TO STOP, AND BOTH MATTER.
         *
         * ad_len == 0 is the padding an advertiser uses to fill the rest of the
         * payload — but it is also the value that makes `off += 1 + ad_len`
         * advance by one forever if the type is never inspected. Stopping here
         * is what makes this loop terminate on every input.
         *
         * off + 1 + ad_len > len is the truncated field: the structure claims
         * more bytes than the payload holds. This is the AS11's own trailing
         * 0x1b field, and it is why we are in this function rather than using
         * NimBLE's parser. Reading it would run off the end of the buffer. */
        if (ad_len == 0 || off + 1 + ad_len > len) {
            out->truncated = true;
            break;
        }

        uint8_t ad_type = data[off + 1];
        const uint8_t *ad_data = data + off + 2;
        int ad_data_len = ad_len - 1;     /* never negative: ad_len 0 stopped above */

        if (ad_type == AD_TYPE_NAME_COMPLETE || ad_type == AD_TYPE_NAME_SHORTENED) {
            /* A LATER name replaces an earlier one. The spec does not permit two
             * Local Name structures in one payload, so either choice is a choice
             * about malformed input; this preserves the behaviour that shipped,
             * and the test pins it so a change is deliberate rather than
             * incidental. */
            out->name = ad_data;
            out->name_len = ad_data_len;
        } else if (ad_type == AD_TYPE_UUID16_COMPLETE ||
                   ad_type == AD_TYPE_UUID16_INCOMPLETE) {
            /* An odd byte count means a trailing half UUID; integer division
             * drops it rather than reading one byte past the field. */
            out->uuids16 = ad_data;
            out->num_uuids16 = ad_data_len / 2;
        }

        out->fields++;
        off += 1 + ad_len;
    }
}
