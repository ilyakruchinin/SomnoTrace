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

/* Covers the pieces of the SomnoBridge client that do not need BLE:
 *   - the 16-byte frame header (pack/parse round-trip, rejections),
 *   - the v2 block codecs (raw / raw-deflate / deflate+delta16),
 *   - the bridge-path → IMPORT/ mapping, including the drive-prefixed form
 *     the bridge canonicaliser demands,
 *   - stream response classification: the first GET_FILE reply may be a
 *     STREAM_BEGIN grant or an ok:false refusal (STALE/BUSY/therapy gates)
 *     that must be surfaced, never mistaken for stream admission,
 *   - the retry policy for those transient refusals.
 * The deflate fixtures are produced with zlib here, the same wire format the
 * firmware inflates through the ESP ROM.  Response trees are built with the
 * cJSON shim — its parser is a stub, but its constructors produce the same
 * trees the firmware sees after cJSON_Parse. */

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "cJSON.h"
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

    /* Trailing bytes after the deflate stream mean the block boundary
     * was spliced (a lost frame) — the stream must fail, not accept
     * a cleanly-ending prefix.  Previously this returned true.      */
    uint8_t junk[800];
    memcpy(junk, enc, (size_t)elen);
    junk[elen] = 0xAA;
    memset(out, 0, sizeof(out));
    CHECK(!sb_v2_block(1, junk, (uint32_t)elen + 1, out, sizeof(raw)),
          "trailing byte after deflate stream rejected");
    /* and a truncated stream that ends early must also fail */
    CHECK(!sb_v2_block(1, enc, (uint32_t)elen - 1, out, sizeof(raw)),
          "truncated deflate stream rejected");
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

/* Build a response tree shaped like the bridge's replies:
 *   {"op":<op>,"ok":true,"result":{"size_unit_shift":<shift>}}
 *   {"op":<op>,"ok":false,"error":{"code":<code>}}                    */
static cJSON *mk_rsp(const char *op, bool ok, const char *code, int shift)
{
    cJSON *root = cJSON_CreateObject();
    if (op) cJSON_AddStringToObject(root, "op", op);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (ok) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddNumberToObject(res, "size_unit_shift", shift);
        cJSON_AddItemToObject(root, "result", res);
    } else if (code) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "code", code);
        cJSON_AddItemToObject(root, "error", err);
    }
    return root;
}

static void test_rsp_classify(void)
{
    cJSON *res;
    char code[16];

    /* admission granted: ok + result object, unit shift extractable */
    cJSON *r = mk_rsp("STREAM_BEGIN", true, NULL, 12);
    CHECK(sb_rsp_classify(r, &res, code, sizeof(code)) == SB_RSP_OK,
          "STREAM_BEGIN ok classifies");
    CHECK(res && cJSON_IsObject(res), "result object returned");
    cJSON *us = cJSON_GetObjectItem(res, "size_unit_shift");
    CHECK(cJSON_IsNumber(us) && us->valuedouble == 12, "unit shift visible");
    cJSON_Delete(r);

    /* refusals carry the real error code — the phantom-stall fix relies
     * on these reaching s_last_error instead of a stall timeout       */
    const char *errs[] = { "STALE", "BUSY", "THERAPY_UNKNOWN",
                           "THERAPY_ACTIVE", "SIZE_MISMATCH" };
    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        code[0] = '\0';
        r = mk_rsp("GET_FILE", false, errs[i], 0);
        CHECK(sb_rsp_classify(r, &res, code, sizeof(code)) == SB_RSP_ERROR,
              "refusal classifies as error");
        CHECK(res == NULL, "refusal yields no result");
        CHECK_STR(code, errs[i], "error code preserved");
        cJSON_Delete(r);
    }

    /* a long code must still be copied bounded + NUL-terminated */
    char tiny[8];
    r = mk_rsp("GET_FILE", false, "THERAPY_UNKNOWN_LONG", 0);
    CHECK(sb_rsp_classify(r, NULL, tiny, sizeof(tiny)) == SB_RSP_ERROR,
          "long code still classifies");
    CHECK(strlen(tiny) < sizeof(tiny), "code copy is bounded");
    CHECK(tiny[sizeof(tiny) - 1] == '\0', "code copy is terminated");
    cJSON_Delete(r);

    /* malformed shapes are BAD, never OK or ERROR */
    CHECK(sb_rsp_classify(NULL, &res, code, sizeof(code)) == SB_RSP_BAD,
          "NULL response is malformed");
    r = mk_rsp("GET_FILE", false, NULL, 0);        /* ok:false, no error */
    CHECK(sb_rsp_classify(r, &res, code, sizeof(code)) == SB_RSP_BAD,
          "refusal without code is malformed");
    cJSON_Delete(r);
    r = cJSON_CreateObject();                      /* empty object */
    CHECK(sb_rsp_classify(r, &res, code, sizeof(code)) == SB_RSP_BAD,
          "empty response is malformed");
    cJSON_Delete(r);
    r = cJSON_CreateObject();                      /* ok:true, no result */
    cJSON_AddBoolToObject(r, "ok", true);
    CHECK(sb_rsp_classify(r, &res, code, sizeof(code)) == SB_RSP_BAD,
          "ok without result is malformed");
    cJSON_Delete(r);
    r = cJSON_CreateObject();                      /* ok:true, result≠object */
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "result", "nope");
    CHECK(sb_rsp_classify(r, &res, code, sizeof(code)) == SB_RSP_BAD,
          "non-object result is malformed");
    cJSON_Delete(r);
}

static void test_retry_policy(void)
{
    /* transient refusals: bounded waits matching the reference client */
    CHECK(sb_stream_retry_ms("STALE") == 12000, "STALE waits out the rescan");
    CHECK(sb_stream_retry_ms("BUSY") == 1500, "BUSY waits out teardown");
    CHECK(sb_stream_retry_ms("HTTP_ERROR") == 1500, "card-side HTTP retry");
    CHECK(sb_stream_retry_ms("http_error") == 1500, "END http_error retry");
    CHECK(sb_stream_retry_ms("source_unverified") == 2000, "post-verify race");
    CHECK(sb_stream_retry_ms("journal_error") == 2000, "journal flush retry");
    /* local transport terminations: restart-at-0 after a short settle —
     * the stream is already dead, so retrying is the recovery path     */
    const char *transport[] = {
        "seq gap", "v2 decode", "rx overflow", "rx drop", "rx flatten",
        "stream stalled", "no STREAM_END", "no STREAM_BEGIN",
        "link lost", "initial credit", "write", "decode overflow",
        "size mismatch", "wire mismatch", "sha256 mismatch",
        "stream not ok", "aborted",
    };
    for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++)
        CHECK(sb_stream_retry_ms(transport[i]) == 1000,
              "transport failure retries");
    /* everything else fails the attempt outright */
    CHECK(sb_stream_retry_ms("THERAPY_UNKNOWN") == 0, "therapy gate not retried");
    CHECK(sb_stream_retry_ms("THERAPY_ACTIVE") == 0, "active therapy not retried");
    CHECK(sb_stream_retry_ms("SIZE_MISMATCH") == 0, "source change not retried");
    CHECK(sb_stream_retry_ms("bad STREAM_BEGIN") == 0, "malformed not retried");
    CHECK(sb_stream_retry_ms("bad STREAM_END") == 0, "malformed END not retried");
    CHECK(sb_stream_retry_ms("expected size mismatch") == 0,
          "listing disagreement not retried");
    CHECK(sb_stream_retry_ms("truncated block") == 0, "residual not retried");
    CHECK(sb_stream_retry_ms("missing sha256") == 0, "missing field not retried");
    CHECK(sb_stream_retry_ms("") == 0, "empty code not retried");
    CHECK(sb_stream_retry_ms(NULL) == 0, "NULL code not retried");
    /* exact-match only: prefixes/truncations must not retry */
    CHECK(sb_stream_retry_ms("STAL") == 0, "truncated STALE not retried");
    CHECK(sb_stream_retry_ms("STALE ") == 0, "padded STALE not retried");
    CHECK(sb_stream_retry_ms("seq") == 0, "prefix of seq gap not retried");
}

/* Compact-frame sequence check, modulo 65536: duplicates behind the
 * expected position must be dropped, forward jumps are lost frames. */
static void test_seq_class(void)
{
    CHECK(sb_seq_class(0, 0) == SB_SEQ_ACCEPT, "first frame accepts");
    CHECK(sb_seq_class(7, 7) == SB_SEQ_ACCEPT, "in-order frame accepts");
    CHECK(sb_seq_class(6, 7) == SB_SEQ_DUP, "one-behind is a duplicate");
    CHECK(sb_seq_class(0, 7) == SB_SEQ_DUP, "re-send from far behind");
    CHECK(sb_seq_class(8, 7) == SB_SEQ_GAP, "one-ahead means a lost frame");
    CHECK(sb_seq_class(700, 7) == SB_SEQ_GAP, "large forward jump is a gap");
    /* wrap-around: the counter crosses 65535→0 continuously, and a frame
     * that stayed behind the wrap is still a duplicate, not a gap       */
    CHECK(sb_seq_class(0, 0) == SB_SEQ_ACCEPT, "wrapped position accepts");
    CHECK(sb_seq_class(65535, 0) == SB_SEQ_DUP, "pre-wrap frame is dup");
    CHECK(sb_seq_class(1, 0) == SB_SEQ_GAP, "post-wrap forward jump is gap");
    CHECK(sb_seq_class(65534, 65535) == SB_SEQ_DUP, "one before wrap dup");
    /* boundary: forward distances below half the space are gaps; the
     * exact midpoint is pinned to DUP (a real gap terminates on the next
     * in-order frame anyway, so the ambiguous edge must not)           */
    CHECK(sb_seq_class(32772, 5) == SB_SEQ_GAP,
          "forward distance 32767 is a gap");
    CHECK(sb_seq_class(32773, 5) == SB_SEQ_DUP,
          "midpoint 0x8000 is pinned to dup");
}

/* Strict u64 decimal parse — the whole string must be digits. */
static void test_parse_u64(void)
{
    uint64_t v = 0;
    CHECK(sb_parse_u64_str("0", &v) && v == 0, "zero parses");
    CHECK(sb_parse_u64_str("12345", &v) && v == 12345, "plain parses");
    CHECK(sb_parse_u64_str("18446744073709551615", &v) &&
          v == 18446744073709551615ULL, "u64 max parses");
    CHECK(!sb_parse_u64_str("18446744073709551616", &v),
          "u64 overflow rejected");
    CHECK(!sb_parse_u64_str("99999999999999999999", &v),
          "long overflow rejected");
    CHECK(!sb_parse_u64_str("12x", &v), "trailing junk rejected");
    CHECK(!sb_parse_u64_str("x12", &v), "leading junk rejected");
    CHECK(!sb_parse_u64_str("12 3", &v), "embedded space rejected");
    CHECK(!sb_parse_u64_str("", &v), "empty rejected");
    CHECK(!sb_parse_u64_str("-1", &v), "sign rejected");
    CHECK(!sb_parse_u64_str(NULL, &v), "NULL rejected");
    CHECK(sb_parse_u64_str("12", NULL), "NULL out is validate-only");
}

static void test_hex_str_ok(void)
{
    const char *h64 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CHECK(sb_hex_str_ok(h64, 64), "64-hex sha accepted");
    CHECK(sb_hex_str_ok(h64, 63) == false, "short sha rejected");
    CHECK(sb_hex_str_ok(h64, 65) == false, "long sha rejected");
    char bad[65];
    memcpy(bad, h64, 64);
    bad[63] = 'g';
    bad[64] = '\0';
    CHECK(!sb_hex_str_ok(bad, 64), "non-hex char rejected");
    memcpy(bad, h64, 64);
    bad[10] = 'F';
    CHECK(sb_hex_str_ok(bad, 64), "uppercase hex accepted");
    CHECK(!sb_hex_str_ok(NULL, 64), "NULL rejected");
    CHECK(sb_hex_str_ok("", 0), "empty with n=0 vacuously true");
}

int main(void)
{
    test_header();
    test_delta16();
    test_codecs();
    test_paths();
    test_rsp_classify();
    test_retry_policy();
    test_seq_class();
    test_parse_u64();
    test_hex_str_ok();
    printf("sb_ble_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
