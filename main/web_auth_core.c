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

#include "web_auth_core.h"

#include <string.h>
#include <strings.h>

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#define TOKEN_FLAG_WALL  0x01
#define TOKEN_FLAG_BOOT  0x02
#define TOKEN_BODY_LEN   (1 + 8 + WA_NONCE_LEN)

bool wa_ct_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a;
    const volatile uint8_t *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}

void wa_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

int wa_pbkdf2_sha256(const uint8_t *pw, size_t pw_len,
                     const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t out[WA_HASH_LEN])
{
    if (iterations == 0) return -1;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    uint8_t u[WA_HASH_LEN], t[WA_HASH_LEN];
    static const uint8_t block_index[4] = { 0, 0, 0, 1 };   /* one output block */
    int rc = mbedtls_md_setup(&ctx, info, 1);
    if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, pw, pw_len);
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, salt, salt_len);
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, block_index, sizeof(block_index));
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, u);
    if (rc == 0) memcpy(t, u, sizeof(t));
    for (uint32_t i = 1; rc == 0 && i < iterations; i++) {
        rc = mbedtls_md_hmac_reset(&ctx);
        if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, u, sizeof(u));
        if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, u);
        for (size_t k = 0; k < sizeof(t); k++) t[k] ^= u[k];
    }
    if (rc == 0) memcpy(out, t, WA_HASH_LEN);
    mbedtls_md_free(&ctx);
    wa_wipe(u, sizeof(u));
    wa_wipe(t, sizeof(t));
    return rc == 0 ? 0 : -1;
}

bool wa_pw_acceptable(const char *pw, size_t len)
{
    if (!pw || len < WA_PW_MIN_LEN || len > WA_PW_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pw[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

int wa_pw_hash(wa_pw_record_t *rec, const char *pw, size_t len)
{
    if (!rec || !pw || rec->iterations == 0 || rec->iterations > WA_PBKDF2_MAX_ITER) return -1;
    return wa_pbkdf2_sha256((const uint8_t *)pw, len, rec->salt, sizeof(rec->salt),
                            rec->iterations, rec->hash);
}

bool wa_pw_verify(const wa_pw_record_t *rec, const char *pw, size_t len)
{
    if (!rec || !pw || len > WA_PW_MAX_LEN) return false;
    if (rec->iterations == 0 || rec->iterations > WA_PBKDF2_MAX_ITER) return false;
    uint8_t h[WA_HASH_LEN];
    if (wa_pbkdf2_sha256((const uint8_t *)pw, len, rec->salt, sizeof(rec->salt),
                         rec->iterations, h) != 0) {
        return false;
    }
    bool ok = wa_ct_equal(h, rec->hash, sizeof(h));
    wa_wipe(h, sizeof(h));
    return ok;
}

static int token_mac(const uint8_t key[WA_KEY_LEN], const uint8_t boot_id[WA_BOOT_ID_LEN],
                     const uint8_t body[TOKEN_BODY_LEN], uint8_t mac[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 1);
    if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, key, WA_KEY_LEN);
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, body, TOKEN_BODY_LEN);
    /* A boot-bound token signs the boot id too, so it cannot outlive the boot. */
    if (rc == 0 && body[0] == TOKEN_FLAG_BOOT) rc = mbedtls_md_hmac_update(&ctx, boot_id, WA_BOOT_ID_LEN);
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    return rc == 0 ? 0 : -1;
}

static const char HEX[] = "0123456789abcdef";

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int wa_token_mint(const uint8_t key[WA_KEY_LEN], const uint8_t boot_id[WA_BOOT_ID_LEN],
                  bool wall, int64_t expiry, const uint8_t nonce[WA_NONCE_LEN],
                  char *out, size_t out_cap)
{
    if (!key || !boot_id || !nonce || !out || out_cap < WA_TOKEN_HEX_LEN + 1) return -1;
    uint8_t raw[WA_TOKEN_RAW_LEN];
    raw[0] = wall ? TOKEN_FLAG_WALL : TOKEN_FLAG_BOOT;
    uint64_t e = (uint64_t)expiry;
    for (int i = 0; i < 8; i++) raw[1 + i] = (uint8_t)(e >> (56 - 8 * i));
    memcpy(raw + 9, nonce, WA_NONCE_LEN);
    if (token_mac(key, boot_id, raw, raw + TOKEN_BODY_LEN) != 0) return -1;
    for (size_t i = 0; i < sizeof(raw); i++) {
        out[2 * i]     = HEX[raw[i] >> 4];
        out[2 * i + 1] = HEX[raw[i] & 0x0f];
    }
    out[WA_TOKEN_HEX_LEN] = '\0';
    return 0;
}

bool wa_token_verify(const uint8_t key[WA_KEY_LEN], const uint8_t boot_id[WA_BOOT_ID_LEN],
                     const char *token, int64_t wall_now, int64_t boot_now_s)
{
    if (!key || !boot_id || !token) return false;
    if (strnlen(token, WA_TOKEN_HEX_LEN + 1) != WA_TOKEN_HEX_LEN) return false;
    uint8_t raw[WA_TOKEN_RAW_LEN];
    for (size_t i = 0; i < sizeof(raw); i++) {
        int hi = hexval(token[2 * i]), lo = hexval(token[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        raw[i] = (uint8_t)((hi << 4) | lo);
    }
    uint8_t mac[32];
    if (token_mac(key, boot_id, raw, mac) != 0) return false;
    if (!wa_ct_equal(mac, raw + TOKEN_BODY_LEN, sizeof(mac))) return false;

    uint64_t e = 0;
    for (int i = 0; i < 8; i++) e = (e << 8) | raw[1 + i];
    int64_t expiry = (int64_t)e;
    int64_t now;
    if (raw[0] == TOKEN_FLAG_WALL) {
        if (wall_now < 0) return false;
        now = wall_now;
    } else if (raw[0] == TOKEN_FLAG_BOOT) {
        now = boot_now_s;
    } else {
        return false;
    }
    return now < expiry && expiry - now <= WA_SESSION_MAX_S;
}

bool wa_basic_password(const char *header, char *pw, size_t pw_cap, size_t *pw_len)
{
    if (!header || !pw || pw_cap == 0 || !pw_len) return false;
    if (strncasecmp(header, "Basic", 5) != 0 || (header[5] != ' ' && header[5] != '\t')) return false;
    const char *b64 = header + 5;
    while (*b64 == ' ' || *b64 == '\t') b64++;
    size_t n = strlen(b64);
    while (n > 0 && (b64[n - 1] == ' ' || b64[n - 1] == '\t')) n--;

    /* user ":" password; the user name is not checked, only bounded */
    uint8_t dec[192];
    size_t dlen = 0;
    if (n == 0 || n > 4 * (sizeof(dec) / 3)) return false;
    if (mbedtls_base64_decode(dec, sizeof(dec), &dlen, (const unsigned char *)b64, n) != 0) {
        wa_wipe(dec, sizeof(dec));
        return false;
    }
    const uint8_t *colon = memchr(dec, ':', dlen);
    bool ok = false;
    if (colon) {
        size_t off = (size_t)(colon - dec) + 1;
        size_t plen = dlen - off;
        if (plen < pw_cap && !memchr(dec + off, '\0', plen)) {
            memcpy(pw, dec + off, plen);
            pw[plen] = '\0';
            *pw_len = plen;
            ok = true;
        }
    }
    wa_wipe(dec, sizeof(dec));
    return ok;
}

bool wa_origin_matches_host(const char *origin, const char *host)
{
    if (!origin || !host || !*host) return false;
    const char *auth;
    if (strncasecmp(origin, "http://", 7) == 0)       auth = origin + 7;
    else if (strncasecmp(origin, "https://", 8) == 0) auth = origin + 8;
    else return false;
    size_t alen = strcspn(auth, "/");
    if (auth[alen] != '\0' || alen == 0) return false;   /* an origin has no path */
    return strlen(host) == alen && strncasecmp(auth, host, alen) == 0;
}

int64_t wa_limiter_wait_ms(const wa_limiter_t *l, int64_t now_ms)
{
    if (!l || l->blocked_until_ms <= now_ms) return 0;
    return l->blocked_until_ms - now_ms;
}

void wa_limiter_fail(wa_limiter_t *l, int64_t now_ms)
{
    if (!l) return;
    if (l->failures < UINT32_MAX) l->failures++;
    if (l->failures <= WA_FREE_ATTEMPTS) return;
    uint32_t shift = l->failures - WA_FREE_ATTEMPTS - 1;
    int64_t block = shift >= 20 ? WA_MAX_BLOCK_MS : (1000LL << shift);
    if (block > WA_MAX_BLOCK_MS) block = WA_MAX_BLOCK_MS;
    l->blocked_until_ms = now_ms + block;
}

void wa_limiter_reset(wa_limiter_t *l)
{
    if (l) memset(l, 0, sizeof(*l));
}
