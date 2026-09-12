/*
 * SomnoTrace - host tests for the advertising-data salvage parser
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
 *
 * ---------------------------------------------------------------------------
 * This parser runs only on payloads NimBLE has already rejected as malformed,
 * which is exactly the input an attacker in radio range controls and exactly the
 * input a normal day never produces. So the cases here are mostly the broken
 * ones, and the last test is a containment property over pseudo-random bytes:
 * whatever comes back must point inside the caller's buffer.
 */
#include "as11_adv.h"

#include <stdio.h>
#include <string.h>

static int n_pass, n_fail;

static void ok(const char *what, int cond)
{
    if (cond) { n_pass++; } else { n_fail++; printf("  FAIL %s\n", what); }
}

static void eq(const char *what, long got, long want)
{
    if (got == want) { n_pass++; }
    else { n_fail++; printf("  FAIL %s: got %ld, want %ld\n", what, got, want); }
}

int main(void)
{
    as11_adv_fields_t f;

    /* ---- a well-formed payload -------------------------------------------- */
    {
        const uint8_t adv[] = {
            0x03, 0x03, 0x56, 0xfd,                     /* UUID16 list: 0xfd56 */
            0x05, 0x09, 'A', 'S', '1', '1',             /* complete local name */
        };
        as11_adv_salvage(adv, (int)sizeof(adv), &f);
        eq("well-formed: fields", f.fields, 2);
        ok("well-formed: not truncated", !f.truncated);
        eq("well-formed: name_len", f.name_len, 4);
        ok("well-formed: name bytes", f.name && memcmp(f.name, "AS11", 4) == 0);
        eq("well-formed: one uuid", f.num_uuids16, 1);
        ok("well-formed: uuid bytes", f.uuids16 &&
           f.uuids16[0] == 0x56 && f.uuids16[1] == 0xfd);
    }

    /* ---- THE AS11's OWN PAYLOAD: a good name, then a field one byte short.
     * This is the case the whole function exists for. NimBLE discards the lot;
     * the name must survive. ------------------------------------------------ */
    {
        const uint8_t adv[] = {
            0x05, 0x09, 'A', 'S', '1', '1',   /* complete local name */
            0x05, 0x1b, 0x01, 0x02, 0x03,     /* claims 5, only 4 bytes follow */
        };
        as11_adv_salvage(adv, (int)sizeof(adv), &f);
        eq("truncated tail: name still salvaged", f.name_len, 4);
        ok("truncated tail: name bytes", f.name && memcmp(f.name, "AS11", 4) == 0);
        ok("truncated tail: flagged", f.truncated);
        eq("truncated tail: only the good field counted", f.fields, 1);
    }

    /* ---- ad_len == 0 TERMINATES THE WALK.
     * Without this check `off += 1 + ad_len` advances by one and the loop runs
     * to the end of the buffer one byte at a time — or forever, if the bound
     * were ever written differently. It is the single most important line in
     * the parser. ------------------------------------------------------------ */
    {
        const uint8_t adv[] = { 0x05, 0x09, 'A', 'S', '1', '1', 0x00, 0x00, 0x00 };
        as11_adv_salvage(adv, (int)sizeof(adv), &f);
        eq("zero length stops: name kept", f.name_len, 4);
        eq("zero length stops: one field", f.fields, 1);
        ok("zero length stops: flagged", f.truncated);
    }

    /* ---- a field claiming more than the payload holds --------------------- */
    {
        const uint8_t adv[] = { 0x20, 0x09, 'A', 'S' };   /* claims 32 bytes */
        as11_adv_salvage(adv, (int)sizeof(adv), &f);
        eq("overrun: nothing salvaged", f.fields, 0);
        ok("overrun: no name", f.name == NULL);
        ok("overrun: flagged", f.truncated);
    }

    /* ---- two name fields: the LAST wins, pinned deliberately --------------- */
    {
        const uint8_t adv[] = {
            0x05, 0x09, 'f', 'i', 'r', 's',
            0x05, 0x09, 'l', 'a', 's', 't',
        };
        as11_adv_salvage(adv, (int)sizeof(adv), &f);
        eq("two names: both walked", f.fields, 2);
        ok("two names: the last one wins", f.name && memcmp(f.name, "last", 4) == 0);
    }

    /* ---- a name field with no data at all ---------------------------------- */
    {
        const uint8_t adv[] = { 0x01, 0x09, 0x03, 0x03, 0x56, 0xfd };
        as11_adv_salvage(adv, (int)sizeof(adv), &f);
        eq("empty name: len 0", f.name_len, 0);
        eq("empty name: uuid still found", f.num_uuids16, 1);
        ok("empty name: walk continued", f.fields == 2);
    }

    /* ---- an odd number of UUID bytes drops the trailing half --------------- */
    {
        const uint8_t adv[] = { 0x06, 0x03, 0x56, 0xfd, 0x11, 0x22, 0x33 };
        as11_adv_salvage(adv, (int)sizeof(adv), &f);
        eq("odd uuid bytes: 5 bytes -> 2 uuids", f.num_uuids16, 2);
    }

    /* ---- A PAYLOAD ENDING EXACTLY ON A BOUNDARY IS NOT TRUNCATED.
     * Found by the mutation harness: flipping the loop bound `off + 1 < len` to
     * `<=` leaves every salvaged field identical and only changes whether the
     * walk sets `truncated` on a payload whose last field ends one byte from the
     * end. Nothing asserted that, so the mutant lived. ---------------------- */
    {
        const uint8_t exact[] = { 0x05, 0x09, 'A', 'S', '1', '1' };
        as11_adv_salvage(exact, (int)sizeof(exact), &f);
        ok("exact fit: not truncated", !f.truncated);
        eq("exact fit: one field", f.fields, 1);

        /* One stray byte after the last field: off + 1 == len, so the walk
         * exits on its loop condition and never inspects it. The shipped
         * parser calls that clean, not truncated — pinned in that direction
         * because it is the ORIGINAL that leaves the flag false and the `<=`
         * mutant that sets it. */
        const uint8_t trailing[] = { 0x05, 0x09, 'A', 'S', '1', '1', 0x07 };
        as11_adv_salvage(trailing, (int)sizeof(trailing), &f);
        ok("one trailing byte: still clean, the stray byte is never read",
           !f.truncated);
        eq("one trailing byte: name still kept", f.name_len, 4);
        eq("one trailing byte: one field", f.fields, 1);
    }

    /* ---- AN UNRELATED AD TYPE IS NOT A UUID LIST.
     * Also found by the harness: turning the second UUID comparison into `!=`
     * makes the `||` true for almost every type, so a Flags or Tx-Power field
     * would be handed back as service UUIDs. No test used a third type, so the
     * mutant survived. ------------------------------------------------------ */
    {
        const uint8_t other[] = {
            0x02, 0x01, 0x06,               /* Flags */
            0x02, 0x0a, 0xf4,               /* Tx Power Level */
            0x05, 0x09, 'A', 'S', '1', '1', /* name */
        };
        as11_adv_salvage(other, (int)sizeof(other), &f);
        eq("other AD types: three fields walked", f.fields, 3);
        eq("other AD types: no uuids claimed", f.num_uuids16, 0);
        ok("other AD types: no uuid pointer", f.uuids16 == NULL);
        eq("other AD types: the name still found", f.name_len, 4);
    }

    /* ---- degenerate inputs ------------------------------------------------- */
    {
        const uint8_t one[] = { 0x06 };
        as11_adv_salvage(one, 1, &f);
        eq("single byte: no fields", f.fields, 0);
        ok("single byte: no name", f.name == NULL);

        as11_adv_salvage(one, 0, &f);
        eq("zero length: no fields", f.fields, 0);

        as11_adv_salvage(NULL, 31, &f);
        ok("NULL data: no name", f.name == NULL);
        eq("NULL data: no fields", f.fields, 0);

        as11_adv_salvage(one, -5, &f);
        eq("negative length: no fields", f.fields, 0);
    }

    /* ---- CONTAINMENT OVER PSEUDO-RANDOM PAYLOADS.
     * The real risk here is not a wrong name, it is a read past the end of a
     * 31-byte buffer supplied by any device in range. Assert the invariant that
     * matters: everything handed back lies inside the input. Deterministic LCG
     * so a failure is reproducible from the seed. ---------------------------- */
    {
        uint32_t seed = 0x5eed1234u;
        int violations = 0;
        for (int iter = 0; iter < 20000; iter++) {
            uint8_t buf[31];
            seed = seed * 1664525u + 1013904223u;
            int len = (int)(seed >> 16) % (int)(sizeof(buf) + 1);   /* 0..31 */
            for (int i = 0; i < len; i++) {
                seed = seed * 1664525u + 1013904223u;
                buf[i] = (uint8_t)(seed >> 24);
            }
            as11_adv_salvage(buf, len, &f);

            if (f.name != NULL) {
                if (f.name < buf || f.name + f.name_len > buf + len || f.name_len < 0) {
                    printf("  FAIL name escaped the buffer at iter %d (len %d)\n", iter, len);
                    violations++;
                }
            }
            if (f.uuids16 != NULL) {
                if (f.uuids16 < buf ||
                    f.uuids16 + 2 * f.num_uuids16 > buf + len ||
                    f.num_uuids16 < 0) {
                    printf("  FAIL uuids escaped the buffer at iter %d (len %d)\n", iter, len);
                    violations++;
                }
            }
            if (f.fields < 0 || f.fields > len) {
                printf("  FAIL implausible field count %d at iter %d\n", f.fields, iter);
                violations++;
            }
            if (violations > 3) break;
        }
        ok("20000 random payloads stay inside the buffer", violations == 0);
    }

    printf("as11_adv_test: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
