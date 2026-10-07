/*
 * SomnoTrace - Web interface password: hashing, session tokens, parsing
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

/*
 * The parts of the web password feature that need no ESP-IDF: PBKDF2 password
 * hashing, HMAC-signed session tokens, Basic-auth / Origin parsing and the
 * login rate limiter.  Depends only on mbedTLS, so scripts/web_auth_test.c
 * runs it on the host against the system libmbedcrypto.  web_auth.c is the
 * httpd/NVS glue around it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WA_SALT_LEN        16
#define WA_HASH_LEN        32
#define WA_KEY_LEN         32
#define WA_BOOT_ID_LEN     16
#define WA_NONCE_LEN       16

/* PBKDF2-HMAC-SHA256 work factor for new passwords.  Stored per record, so it
 * can be raised later without invalidating existing passwords.  Online
 * guessing is bounded by the rate limiter; this only slows an offline attack
 * on a dumped flash image. */
#define WA_PBKDF2_ITERATIONS  10000u
#define WA_PBKDF2_MAX_ITER    1000000u

#define WA_PW_MIN_LEN      8
#define WA_PW_MAX_LEN      64

/* Session lifetime.  Logout and every password change rotate the signing key,
 * which ends all sessions early. */
#define WA_SESSION_MAX_S   (30LL * 24 * 3600)

/* token = hex( flags[1] | expiry[8, big endian] | nonce[16] | HMAC-SHA256[32] ) */
#define WA_TOKEN_RAW_LEN   (1 + 8 + WA_NONCE_LEN + 32)
#define WA_TOKEN_HEX_LEN   (2 * WA_TOKEN_RAW_LEN)

/* Login throttling: WA_FREE_ATTEMPTS wrong passwords cost nothing beyond the
 * hash; each further one doubles the wait from 1 s up to WA_MAX_BLOCK_MS. */
#define WA_FREE_ATTEMPTS   5
#define WA_MAX_BLOCK_MS    (5LL * 60 * 1000)

typedef struct {
    uint32_t iterations;
    uint8_t  salt[WA_SALT_LEN];
    uint8_t  hash[WA_HASH_LEN];
} wa_pw_record_t;

typedef struct {
    uint32_t failures;
    int64_t  blocked_until_ms;
} wa_limiter_t;

/* Constant-time comparison: run time depends only on n. */
bool wa_ct_equal(const void *a, const void *b, size_t n);

/* Overwrite memory in a way the compiler may not elide. */
void wa_wipe(void *p, size_t n);

/* PBKDF2-HMAC-SHA256 with a 32-byte output.  Returns 0 on success. */
int wa_pbkdf2_sha256(const uint8_t *pw, size_t pw_len,
                     const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t out[WA_HASH_LEN]);

/* True if pw satisfies the length policy (bytes, not characters) and has no
 * NUL or ASCII control characters. */
bool wa_pw_acceptable(const char *pw, size_t len);

/* Fill rec->hash for pw using rec->salt and rec->iterations. 0 on success. */
int wa_pw_hash(wa_pw_record_t *rec, const char *pw, size_t len);

/* True if pw matches rec.  A record with an out-of-range iteration count
 * never matches (fail closed). */
bool wa_pw_verify(const wa_pw_record_t *rec, const char *pw, size_t len);

/* Mint a session token into out (needs WA_TOKEN_HEX_LEN + 1 bytes).
 * wall: expiry is a Unix time; otherwise it is seconds of uptime and the
 * token is additionally bound to boot_id, so it dies with this boot.
 * Returns 0 on success. */
int wa_token_mint(const uint8_t key[WA_KEY_LEN], const uint8_t boot_id[WA_BOOT_ID_LEN],
                  bool wall, int64_t expiry, const uint8_t nonce[WA_NONCE_LEN],
                  char *out, size_t out_cap);

/* Verify a token.  wall_now < 0 means "the wall clock is not trustworthy",
 * which rejects every wall-clock token.  boot_now_s is uptime in seconds. */
bool wa_token_verify(const uint8_t key[WA_KEY_LEN], const uint8_t boot_id[WA_BOOT_ID_LEN],
                     const char *token, int64_t wall_now, int64_t boot_now_s);

/* Extract the password from an "Authorization: Basic ..." value.  The user
 * name is ignored.  Returns false on any malformed input. */
bool wa_basic_password(const char *header, char *pw, size_t pw_cap, size_t *pw_len);

/* True if an Origin header value names the same host[:port] as the Host
 * header.  "null" and anything unparsable are a mismatch. */
bool wa_origin_matches_host(const char *origin, const char *host);

/* Milliseconds the caller must still wait before another password attempt. */
int64_t wa_limiter_wait_ms(const wa_limiter_t *l, int64_t now_ms);
void wa_limiter_fail(wa_limiter_t *l, int64_t now_ms);
void wa_limiter_reset(wa_limiter_t *l);
