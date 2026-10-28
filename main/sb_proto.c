/*
 * SomnoTrace - SomnoBridge wire helpers (pure, host-testable)
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

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "sb_proto.h"

#ifdef SB_PROTO_HOST
#include <zlib.h>          /* host test: raw inflate, same wire format */
/* The on-card root, spelled out so the host test does not need sd_storage.h */
#ifndef SD_IMPORT_DIR
#define SD_IMPORT_DIR "/somnotrace/IMPORT"
#endif
#else
#include "miniz.h"         /* firmware: ROM tinfl                    */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sd_storage.h"
#endif

void sb_hdr_pack(uint8_t *p, uint8_t kind, uint16_t flags, uint32_t rid,
                 uint64_t off)
{
    p[0] = 1;
    p[1] = kind;
    p[2] = (uint8_t)(flags & 0xFF);
    p[3] = (uint8_t)(flags >> 8);
    p[4] = (uint8_t)rid;
    p[5] = (uint8_t)(rid >> 8);
    p[6] = (uint8_t)(rid >> 16);
    p[7] = (uint8_t)(rid >> 24);
    for (int i = 0; i < 8; i++) p[8 + i] = (uint8_t)(off >> (8 * i));
}

bool sb_hdr_parse(const uint8_t *p, size_t n, uint8_t *kind, uint16_t *flags,
                  uint32_t *rid, uint64_t *off)
{
    if (!p || n < SB_HDR_LEN || p[0] != 1) return false;
    if (kind)  *kind = p[1];
    if (flags) *flags = (uint16_t)(p[2] | (p[3] << 8));
    if (rid)   *rid = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    if (off) {
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | p[8 + i];
        *off = v;
    }
    return true;
}

void sb_delta16(uint8_t *buf, uint32_t len)
{
    if (!buf) return;
    for (uint32_t i = 2; i + 1 < len; i += 2) {
        uint16_t prev, cur;
        memcpy(&prev, buf + i - 2, 2);
        memcpy(&cur, buf + i, 2);
        cur = (uint16_t)(cur + prev);
        memcpy(buf + i, &cur, 2);
    }
}

static bool inflate_raw(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                        uint32_t dst_len)
{
#ifdef SB_PROTO_HOST
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) return false;
    zs.next_in = (Bytef *)src;
    zs.avail_in = src_len;
    zs.next_out = dst;
    zs.avail_out = dst_len;
    int rc = inflate(&zs, Z_FINISH);
    const uint32_t produced = dst_len - zs.avail_out;
    const uLong consumed = zs.total_in;
    inflateEnd(&zs);
    /* One block = exactly one deflate stream: it must END, produce the
     * declared raw length, and consume the whole encoded payload — a
     * stream that finishes early with trailing bytes means the input was
     * spliced, which the caller must treat as corruption, not success. */
    return rc == Z_STREAM_END && produced == dst_len && consumed == src_len;
#else
    /* tinfl_decompress_mem_to_mem() instantiates the ~11 KiB
     * tinfl_decompressor on the stack.  Keep one static context in PSRAM
     * and drive the low-level API instead (single-user: stream decode is
     * confined to the bridge task's receive pump).                     */
    static tinfl_decompressor *s_tinfl;
    if (!s_tinfl)
        s_tinfl = heap_caps_malloc(sizeof(tinfl_decompressor),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tinfl) return false;
    tinfl_init(s_tinfl);
    size_t in = src_len, out = dst_len;
    /* USING_NON_WRAPPING_OUTPUT_BUF: dst always holds the whole block —
     * without it tinfl requires a power-of-2 output buffer and returns
     * BAD_PARAM for the arbitrary raw_len sizes the codec emits.        */
    const tinfl_status st = tinfl_decompress(s_tinfl, src, &in, dst, dst,
                                             &out,
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    /* in must equal src_len: TINFL_STATUS_DONE with bytes left over is
     * an early-ended stream + trailing junk — a spliced input, i.e.
     * corruption the stream layer must fail on (see seq-gap handling). */
    if (st != TINFL_STATUS_DONE || out != dst_len || in != src_len)
        ESP_LOGW("sb_proto", "tinfl st=%d in=%u/%lu out=%u/%lu",
                 (int)st, (unsigned)in, (unsigned long)src_len,
                 (unsigned)out, (unsigned long)dst_len);
    return st == TINFL_STATUS_DONE && out == dst_len && in == src_len;
#endif
}

bool sb_v2_block(uint8_t codec, const uint8_t *enc, uint32_t enc_len,
                 uint8_t *out, uint32_t raw_len)
{
    if (!out) return false;
    if (codec == 0) {
        if (enc_len != raw_len) return false;
        if (raw_len) memcpy(out, enc, raw_len);
        return true;
    }
    if (codec != 1 && codec != 2) return false;
    if (!inflate_raw(enc, enc_len, out, raw_len)) return false;
    if (codec == 2) sb_delta16(out, raw_len);
    return true;
}

bool sb_stage_path(const char *directory, const char *name, char *out,
                   size_t cap, char *day, size_t day_cap)
{
    if (!directory || !name || !name[0] || !out || cap == 0) return false;
    if (day && day_cap) day[0] = '\0';

    /* Strip an optional drive and leading separators, then normalise to a
     * single forward-slash path so one comparison serves every spelling. */
    const char *p = directory;
    if ((p[0] == 'A' || p[0] == 'a') && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;

    char norm[96];
    size_t n = 0;
    for (; *p && n + 1 < sizeof(norm); p++)
        norm[n++] = (*p == '\\') ? '/' : *p;
    while (n && norm[n - 1] == '/') n--;
    norm[n] = '\0';

    int w;
    if (n == 0) {                                   /* A:\ → IMPORT/<name> */
        w = snprintf(out, cap, "%s/%s", SD_IMPORT_DIR, name);
    } else if (strcmp(norm, "SETTINGS") == 0) {
        w = snprintf(out, cap, "%s/SETTINGS/%s", SD_IMPORT_DIR, name);
    } else if (strncmp(norm, "DATALOG/", 8) == 0 && strlen(norm + 8) == 8) {
        if (day && day_cap) snprintf(day, day_cap, "%s", norm + 8);
        w = snprintf(out, cap, "%s/DATALOG/%s/%s", SD_IMPORT_DIR, norm + 8, name);
    } else {
        return false;                               /* never guess a path */
    }
    return w > 0 && (size_t)w < cap;
}

sb_rsp_kind_t sb_rsp_classify(const struct cJSON *root, struct cJSON **result,
                              char *code, size_t code_cap)
{
    if (result) *result = NULL;
    if (code && code_cap) code[0] = '\0';
    if (!root) return SB_RSP_BAD;
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"))) {
        struct cJSON *res = cJSON_GetObjectItem(root, "result");
        if (!cJSON_IsObject(res)) return SB_RSP_BAD;
        if (result) *result = res;
        return SB_RSP_OK;
    }
    const char *ec = cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "error"), "code"));
    if (!ec) return SB_RSP_BAD;
    if (code && code_cap) snprintf(code, code_cap, "%s", ec);
    return SB_RSP_ERROR;
}

unsigned sb_stream_retry_ms(const char *code)
{
    if (!code) return 0;
    /* Bridge refusals: the wait mirrors the resource the bridge named. */
    if (!strcmp(code, "STALE")) return 12000;
    if (!strcmp(code, "BUSY"))  return 1500;
    if (!strcmp(code, "HTTP_ERROR") || !strcmp(code, "http_error"))
        return 1500;
    if (!strcmp(code, "source_unverified") ||
        !strcmp(code, "journal_error"))
        return 2000;
    /* Local transport terminations: the stream is already dead, so a fresh
     * whole-file attempt (§10.3 restart-at-0) is the recovery path.  A
     * short settle lets the bridge finish tearing the old fetch down. */
    static const char *const transport[] = {
        "seq gap", "v2 decode", "rx overflow", "rx drop", "rx flatten",
        "stream stalled", "no STREAM_END", "no STREAM_BEGIN",
        "link lost", "initial credit", "write", "decode overflow",
        "size mismatch", "wire mismatch", "sha256 mismatch",
        "stream not ok", "aborted",
    };
    for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++)
        if (!strcmp(code, transport[i])) return 1000;
    return 0;
}

sb_seq_kind_t sb_seq_class(uint16_t seq, uint16_t expected)
{
    /* back = how far seq sits BEHIND expected (mod 65536).  A frame
     * behind the consumer position is a re-send and is dropped; a
     * forward jump (back > half the space) means frames were lost.  The
     * exact 0x8000 midpoint is genuinely ambiguous and is pinned to DUP:
     * if it really was a gap, the next in-order frame terminates the
     * stream anyway. */
    if (seq == expected) return SB_SEQ_ACCEPT;
    return (uint16_t)(expected - seq) > (uint16_t)0x8000 ? SB_SEQ_GAP
                                                       : SB_SEQ_DUP;
}

bool sb_parse_u64_str(const char *s, uint64_t *out)
{
    if (!s || !s[0]) return false;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        const uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    if (out) *out = v;
    return true;
}

bool sb_hex_str_ok(const char *s, size_t n)
{
    if (!s || strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
