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

/* The parts of the SomnoBridge client that are pure logic: the 16-byte frame
 * header, the v2 block codecs, and the bridge-path → IMPORT/ mapping.  Kept
 * out of sb_ble.c so scripts/sb_ble_test.c can exercise them on the host
 * (sb_ble.c itself needs NimBLE and cannot be linked there). */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SB_HDR_LEN 16

#define SB_KIND_JSON_REQ  1
#define SB_KIND_JSON_RSP  2
#define SB_KIND_FILE      3
#define SB_KIND_ACK       4
#define SB_KIND_ABORT     5
#define SB_KIND_TIME_REQ  6
#define SB_KIND_TIME_RSP  7
#define SB_KIND_TIME_CMT  8
#define SB_KIND_EVIDENCE  9
#define SB_KIND_STREAM2   10

#define SB_FLAG_START 0x0001
#define SB_FLAG_END   0x0002

/* Little-endian frame header: ver(1) kind(1) flags(2) request_id(4) offset(8). */
void sb_hdr_pack(uint8_t *p, uint8_t kind, uint16_t flags, uint32_t rid,
                 uint64_t off);
bool sb_hdr_parse(const uint8_t *p, size_t n, uint8_t *kind, uint16_t *flags,
                  uint32_t *rid, uint64_t *off);

/* Inverse uint16 prefix-sum, in place (codec 2's second stage). */
void sb_delta16(uint8_t *buf, uint32_t len);

/* Decode one v2 block (codec 0 raw, 1 raw deflate, 2 deflate + delta16).
 * Returns true when exactly raw_len bytes were produced. */
bool sb_v2_block(uint8_t codec, const uint8_t *enc, uint32_t enc_len,
                 uint8_t *out, uint32_t raw_len);

/* Bridge directory → staging target under IMPORT/.
 *   "A:\DATALOG\20261023" → IMPORT/DATALOG/20261023/<name>, day="20261023"
 *   "A:\SETTINGS"         → IMPORT/SETTINGS/<name>,          day=""
 *   "A:\"                 → IMPORT/<name>,                   day=""
 * Accepts a missing drive prefix and either separator, because the reference
 * client and the spec's examples disagree on the spelling.  Returns false on
 * anything else (never guesses a path). */
bool sb_stage_path(const char *directory, const char *name, char *out,
                   size_t cap, char *day, size_t day_cap);

struct cJSON;

/* Stream-op response classification (STREAM_BEGIN / STREAM_END shapes).
 * SB_RSP_OK    — ok:true with a result object; *result receives it
 * SB_RSP_ERROR — a protocol rejection; error.code copied (bounded) to `code`
 * SB_RSP_BAD   — anything else: malformed or an unexpected shape          */
typedef enum {
    SB_RSP_OK = 0,
    SB_RSP_ERROR,
    SB_RSP_BAD
} sb_rsp_kind_t;
sb_rsp_kind_t sb_rsp_classify(const struct cJSON *root, struct cJSON **result,
                              char *code, size_t code_cap);

/* How long to wait before retrying a transient stream error: STALE means
 * the bridge queued a day rescan (~10 s); BUSY means the previous stream
 * is still tearing down.  Local transport terminations (sequence gap,
 * decode failure, dead air, link loss) restart the whole file after a
 * short settle.  0 for anything not retryable. */
unsigned sb_stream_retry_ms(const char *code);

/* Compact-frame sequence check, modulo 65536.  Frames behind the expected
 * counter by >= 0x8000 (a backward distance) are duplicates of already
 * received frames — the sender never reuses seq — and must be dropped.
 * A forward jump means frames were lost; the splice that would follow is
 * unrecoverable, so the stream must be terminated rather than continued. */
typedef enum {
    SB_SEQ_ACCEPT = 0,
    SB_SEQ_DUP,
    SB_SEQ_GAP
} sb_seq_kind_t;
sb_seq_kind_t sb_seq_class(uint16_t seq, uint16_t expected);

/* Strict unsigned-64 decimal parse: the whole string must be 1-20 digits
 * and fit — partial parses and trailing junk are rejected. */
bool sb_parse_u64_str(const char *s, uint64_t *out);

/* True when s is exactly n hex digits (either case). */
bool sb_hex_str_ok(const char *s, size_t n);
