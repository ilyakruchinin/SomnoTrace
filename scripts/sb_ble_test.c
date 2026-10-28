/*
 * SomnoTrace - host test for the SomnoBridge wire helpers (sb_proto.c)
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

/* Covers the three pieces of the SomnoBridge client that do not need BLE:
 *   - the 16-byte frame header (pack/parse round-trip, rejections),
 *   - the v2 block codecs (raw / raw-deflate / deflate+delta16),
 *   - the bridge-path → IMPORT/ mapping, including the drive-prefixed form
 *     the bridge canonicaliser demands.
 * The deflate fixtures are produced with zlib here, the same wire format the
 * firmware inflates through the ESP ROM. */

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "sb_proto.h"

static int checks, fails;

#define CHECK(cond, msg) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAILED: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

#define CHECK_STR(got, want, msg) do { \
        checks++; \
        if (strcmp((got), (want)) != 0) { \
            fails++; printf("FAILED: %s (line %d)\n  got:  %s\n  want: %s\n", \
                            msg, __LINE__, (got), (want)); \
        } \
    } while (0)

/* Raw deflate (no zlib wrapper) — what codec 1/2 carry on the wire. */
static size_t raw_deflate(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;
    zs.next_in = (Bytef *)src;
    zs.avail_in = (uInt)n;
    zs.next_out = dst;
    zs.avail_out = (uInt)cap;
    int rc = deflate(&zs, Z_FINISH);
    size_t produced = cap - zs.avail_out;
    deflateEnd(&zs);
    return (rc == Z_STREAM_END) ? produced : 0;
}

static void test_header(void)
{
    uint8_t hdr[SB_HDR_LEN];
    uint8_t kind;
    uint16_t flags;
    uint32_t rid;
    uint64_t off;

    sb_hdr_pack(hdr, SB_KIND_JSON_RSP, SB_FLAG_START | SB_FLAG_END,
                0xDEADBEEF, 0x0102030405060708ULL);
    CHECK(hdr[0] == 1, "header version is 1");
    CHECK(hdr[1] == SB_KIND_JSON_RSP, "header kind");
    CHECK(sb_hdr_parse(hdr, sizeof(hdr), &kind, &flags, &rid, &off),
          "header parses");
    CHECK(kind == SB_KIND_JSON_RSP, "kind round-trips");
    CHECK(flags == (SB_FLAG_START | SB_FLAG_END), "flags round-trip");
    CHECK(rid == 0xDEADBEEF, "request id round-trips");
    CHECK(off == 0x0102030405060708ULL, "offset round-trips");

    /* every kind and a stream offset */
    for (uint8_t k = SB_KIND_JSON_REQ; k <= SB_KIND_STREAM2; k++) {
        sb_hdr_pack(hdr, k, SB_FLAG_END, 7, 123456789ULL);
        CHECK(sb_hdr_parse(hdr, sizeof(hdr), &kind, &flags, &rid, &off) &&
              kind == k && flags == SB_FLAG_END && rid == 7 &&
              off == 123456789ULL, "kind/offset round-trip in the loop");
    }

    /* rejections */
    CHECK(!sb_hdr_parse(hdr, SB_HDR_LEN - 1, &kind, &flags, &rid, &off),
          "short buffer rejected");
    hdr[0] = 2;
    CHECK(!sb_hdr_parse(hdr, sizeof(hdr), &kind, &flags, &rid, &off),
          "wrong version rejected");
    CHECK(!sb_hdr_parse(NULL, sizeof(hdr), &kind, &flags, &rid, &off),
          "NULL rejected");
}

static void test_delta16(void)
{
    /* deltas {1, 5, 1, 127} → prefix sums {1, 6, 7, 134} */
    uint8_t buf[8] = { 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x7f, 0x00 };
    sb_delta16(buf, sizeof(buf));
    const uint16_t want[4] = { 1, 6, 7, 134 };
    for (int i = 0; i < 4; i++) {
        uint16_t got;
        memcpy(&got, buf + i * 2, 2);
        CHECK(got == want[i], "delta16 prefix sum");
    }
    /* odd length must not touch the trailing byte */
    uint8_t odd[5] = { 0x02, 0x00, 0x03, 0x00, 0xAA };
    sb_delta16(odd, sizeof(odd));
    CHECK(odd[4] == 0xAA, "delta16 leaves a trailing odd byte alone");
    sb_delta16(NULL, 4);      /* must not crash */
    CHECK(1, "delta16 NULL is safe");
}

static void test_codecs(void)
{
    /* A night's worth of plausible stage bytes: 30 s epochs, 4 stages. */
    uint8_t raw[600];
    for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (uint8_t)(i % 4);

    /* codec 0: raw passthrough */
    uint8_t out[600];
    CHECK(sb_v2_block(0, raw, sizeof(raw), out, sizeof(raw)),
          "codec 0 accepts raw");
    CHECK(memcmp(out, raw, sizeof(raw)) == 0, "codec 0 copies verbatim");
    CHECK(!sb_v2_block(0, raw, sizeof(raw) - 1, out, sizeof(raw)),
          "codec 0 rejects a length mismatch");

    /* codec 1: raw deflate */
    uint8_t enc[700];
    size_t elen = raw_deflate(raw, sizeof(raw), enc, sizeof(enc));
    CHECK(elen > 0 && elen < sizeof(raw), "fixture compresses");
    memset(out, 0, sizeof(out));
    CHECK(sb_v2_block(1, enc, (uint32_t)elen, out, sizeof(raw)),
          "codec 1 inflates");
    CHECK(memcmp(out, raw, sizeof(raw)) == 0, "codec 1 output matches");

    /* codec 2: delta16 first, then deflate */
    uint8_t delta[600];
    memcpy(delta, raw, sizeof(raw));
    for (int i = (int)sizeof(raw) - 2; i > 0; i -= 2) {
        uint16_t cur, prev;
        memcpy(&cur, delta + i, 2);
        memcpy(&prev, delta + i - 2, 2);
        cur = (uint16_t)(cur - prev);
        memcpy(delta + i, &cur, 2);
    }
    elen = raw_deflate(delta, sizeof(delta), enc, sizeof(enc));
    CHECK(elen > 0, "delta fixture compresses");
    memset(out, 0, sizeof(out));
    CHECK(sb_v2_block(2, enc, (uint32_t)elen, out, sizeof(raw)),
          "codec 2 inflates");
    CHECK(memcmp(out, raw, sizeof(raw)) == 0, "codec 2 restores the prefix sums");

    /* unknown codec and a corrupt body */
    CHECK(!sb_v2_block(3, enc, (uint32_t)elen, out, sizeof(raw)),
          "unknown codec rejected");
    CHECK(!sb_v2_block(1, enc, (uint32_t)elen, out, sizeof(raw) - 1),
          "wrong raw_len rejected");
}

static void test_paths(void)
{
    char out[200], day[16];

    CHECK(sb_stage_path("A:\\DATALOG\\20261023", "x_BRP.edf", out, sizeof(out),
                        day, sizeof(day)), "canonical DATALOG path");
    CHECK_STR(out, "/somnotrace/IMPORT/DATALOG/20261023/x_BRP.edf", "DATALOG target");
    CHECK_STR(day, "20261023", "day extracted");

    /* spellings the reference client and the spec disagree on */
    CHECK(sb_stage_path("\\DATALOG\\20261023", "a.edf", out, sizeof(out), day,
                        sizeof(day)), "backslash, no drive");
    CHECK_STR(day, "20261023", "day extracted without drive");
    CHECK(sb_stage_path("DATALOG/20261023", "a.edf", out, sizeof(out), day,
                        sizeof(day)), "forward slashes, no drive");
    CHECK_STR(day, "20261023", "day extracted with forward slashes");

    CHECK(sb_stage_path("A:\\", "STR.edf", out, sizeof(out), day, sizeof(day)),
          "card root");
    CHECK_STR(out, "/somnotrace/IMPORT/STR.edf", "root target");
    CHECK_STR(day, "", "root has no day");
    CHECK(sb_stage_path("/", "Identification.json", out, sizeof(out), day,
                        sizeof(day)), "bare root");
    CHECK_STR(out, "/somnotrace/IMPORT/Identification.json", "bare root target");

    CHECK(sb_stage_path("A:\\SETTINGS", "CurrentSettings.json", out, sizeof(out),
                        day, sizeof(day)), "SETTINGS path");
    CHECK_STR(out, "/somnotrace/IMPORT/SETTINGS/CurrentSettings.json",
              "SETTINGS target");
    CHECK_STR(day, "", "SETTINGS has no day");

    /* anything else is refused rather than guessed */
    CHECK(!sb_stage_path("A:\\DATALOG", "x.edf", out, sizeof(out), day, sizeof(day)),
          "DATALOG without a day rejected");
    CHECK(!sb_stage_path("A:\\DATALOG\\20261023\\sub", "x.edf", out, sizeof(out),
                         day, sizeof(day)), "nested DATALOG rejected");
    CHECK(!sb_stage_path("A:\\OXIMETRY", "x.edf", out, sizeof(out), day, sizeof(day)),
          "unknown directory rejected");
    CHECK(!sb_stage_path(NULL, "x.edf", out, sizeof(out), day, sizeof(day)),
          "NULL directory rejected");
    CHECK(!sb_stage_path("A:\\", "", out, sizeof(out), day, sizeof(day)),
          "empty name rejected");
    CHECK(!sb_stage_path("A:\\DATALOG\\20261023", "x.edf", out, 8, day, sizeof(day)),
          "short output buffer rejected");
}

int main(void)
{
    test_header();
    test_delta16();
    test_codecs();
    test_paths();
    printf("sb_ble_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
