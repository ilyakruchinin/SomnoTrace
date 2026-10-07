/*
 * SomnoTrace - host tests for the web interface password core
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
 *
 * ---------------------------------------------------------------------------
 * Everything here is a security boundary, so most cases are the hostile ones:
 * a token with any single character changed, a token replayed after its
 * expiry or into another boot, an Origin that only looks like ours.  PBKDF2 is
 * checked against the published PBKDF2-HMAC-SHA256 vectors, not against
 * itself.
 */
#include "web_auth_core.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/base64.h"

static int n_pass, n_fail;

static void ok(const char *what, int cond)
{
    if (cond) { n_pass++; } else { n_fail++; printf("  FAIL %s\n", what); }
}

static void eq(const char *what, long long got, long long want)
{
    if (got == want) { n_pass++; }
    else { n_fail++; printf("  FAIL %s: got %lld, want %lld\n", what, got, want); }
}

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static void test_pbkdf2_vectors(void)
{
    static const struct { const char *p, *s; uint32_t c; const char *want; } v[] = {
        { "password", "salt", 1,
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b" },
        { "password", "salt", 2,
          "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43" },
        { "password", "salt", 4096,
          "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a" },
        { "passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096,
          "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1" },
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint8_t out[WA_HASH_LEN];
        char got[2 * WA_HASH_LEN + 1];
        int rc = wa_pbkdf2_sha256((const uint8_t *)v[i].p, strlen(v[i].p),
                                  (const uint8_t *)v[i].s, strlen(v[i].s), v[i].c, out);
        hex(out, sizeof(out), got);
        char what[64];
        snprintf(what, sizeof(what), "pbkdf2 vector %zu (c=%u)", i, v[i].c);
        ok(what, rc == 0 && strcmp(got, v[i].want) == 0);
    }
    uint8_t out[WA_HASH_LEN];
    ok("pbkdf2 rejects zero iterations",
       wa_pbkdf2_sha256((const uint8_t *)"x", 1, (const uint8_t *)"s", 1, 0, out) != 0);
}

static void test_password_records(void)
{
    wa_pw_record_t rec = { .iterations = 1000 };
    memcpy(rec.salt, "0123456789abcdef", WA_SALT_LEN);
    const char *pw = "correct horse";
    ok("hash", wa_pw_hash(&rec, pw, strlen(pw)) == 0);
    ok("verify right password", wa_pw_verify(&rec, pw, strlen(pw)));
    ok("verify wrong password", !wa_pw_verify(&rec, "correct hors3", 13));
    ok("verify prefix of password", !wa_pw_verify(&rec, pw, strlen(pw) - 1));
    ok("verify empty password", !wa_pw_verify(&rec, "", 0));

    /* the salt is part of the hash: same password, other salt, other hash */
    wa_pw_record_t rec2 = rec;
    rec2.salt[0] ^= 1;
    ok("hash with other salt", wa_pw_hash(&rec2, pw, strlen(pw)) == 0);
    ok("salt changes the hash", memcmp(rec.hash, rec2.hash, WA_HASH_LEN) != 0);

    /* a damaged record must never verify, not even the right password */
    wa_pw_record_t bad = rec;
    bad.iterations = 0;
    ok("zero-iteration record fails closed", !wa_pw_verify(&bad, pw, strlen(pw)));
    bad.iterations = WA_PBKDF2_MAX_ITER + 1;
    ok("absurd-iteration record fails closed", !wa_pw_verify(&bad, pw, strlen(pw)));
    bad = rec;
    bad.hash[31] ^= 0x80;
    ok("flipped hash bit fails", !wa_pw_verify(&bad, pw, strlen(pw)));
}

static void test_password_policy(void)
{
    char buf[WA_PW_MAX_LEN + 2];
    memset(buf, 'a', sizeof(buf));
    ok("7 bytes rejected", !wa_pw_acceptable(buf, WA_PW_MIN_LEN - 1));
    ok("8 bytes accepted", wa_pw_acceptable(buf, WA_PW_MIN_LEN));
    ok("64 bytes accepted", wa_pw_acceptable(buf, WA_PW_MAX_LEN));
    ok("65 bytes rejected", !wa_pw_acceptable(buf, WA_PW_MAX_LEN + 1));
    ok("NUL rejected", !wa_pw_acceptable("abcd\0efgh", 9));
    ok("newline rejected", !wa_pw_acceptable("abcd\nefgh", 9));
    ok("DEL rejected", !wa_pw_acceptable("abcd\x7f" "efgh", 9));
    ok("UTF-8 accepted", wa_pw_acceptable("p\xc3\xa4sswort!", 10));
    ok("spaces accepted", wa_pw_acceptable("a b c d e", 9));
}

static const uint8_t KEY[WA_KEY_LEN] = "k0k1k2k3k4k5k6k7k8k9kakbkckdkek";
static const uint8_t BOOT[WA_BOOT_ID_LEN] = "boot-id-0123456";
static const uint8_t NONCE[WA_NONCE_LEN] = "nonce-0123456789";

static void test_tokens(void)
{
    const int64_t now = 1790000000;   /* 2026 */
    char tok[WA_TOKEN_HEX_LEN + 1];

    ok("mint wall", wa_token_mint(KEY, BOOT, true, now + 3600, NONCE, tok, sizeof(tok)) == 0);
    eq("token length", (long long)strlen(tok), WA_TOKEN_HEX_LEN);
    ok("wall token valid now", wa_token_verify(KEY, BOOT, tok, now, 5));
    ok("wall token valid 1 s before expiry", wa_token_verify(KEY, BOOT, tok, now + 3599, 5));
    ok("wall token dead at expiry", !wa_token_verify(KEY, BOOT, tok, now + 3600, 5));
    ok("wall token rejected without a trusted clock", !wa_token_verify(KEY, BOOT, tok, -1, 5));
    uint8_t other_boot[WA_BOOT_ID_LEN];
    memcpy(other_boot, BOOT, sizeof(other_boot));
    other_boot[0] ^= 1;
    ok("wall token survives a reboot", wa_token_verify(KEY, other_boot, tok, now, 5));
    uint8_t other_key[WA_KEY_LEN];
    memcpy(other_key, KEY, sizeof(other_key));
    other_key[31] ^= 1;
    ok("rotated key kills the token", !wa_token_verify(other_key, BOOT, tok, now, 5));

    /* every single-character change must be caught */
    int survivors = 0;
    for (int i = 0; i < WA_TOKEN_HEX_LEN; i++) {
        char t2[sizeof(tok)];
        memcpy(t2, tok, sizeof(tok));
        t2[i] = (t2[i] == '0') ? '1' : '0';
        if (wa_token_verify(KEY, BOOT, t2, now, 5)) survivors++;
    }
    eq("single-char tampering never verifies", survivors, 0);

    /* turning a wall token into a boot token (flag byte) must not verify */
    {
        char t2[sizeof(tok)];
        memcpy(t2, tok, sizeof(tok));
        t2[0] = '0'; t2[1] = '2';
        ok("flag swap rejected", !wa_token_verify(KEY, BOOT, t2, now, 5));
    }

    char btok[WA_TOKEN_HEX_LEN + 1];
    ok("mint boot", wa_token_mint(KEY, BOOT, false, 100 + 600, NONCE, btok, sizeof(btok)) == 0);
    ok("boot token valid in its boot", wa_token_verify(KEY, BOOT, btok, -1, 100));
    ok("boot token valid with a wall clock too", wa_token_verify(KEY, BOOT, btok, now, 100));
    ok("boot token dead in the next boot", !wa_token_verify(KEY, other_boot, btok, -1, 100));
    ok("boot token dead at expiry", !wa_token_verify(KEY, BOOT, btok, -1, 700));

    char far[WA_TOKEN_HEX_LEN + 1];
    wa_token_mint(KEY, BOOT, true, now + WA_SESSION_MAX_S + 1, NONCE, far, sizeof(far));
    ok("expiry beyond the maximum lifetime rejected", !wa_token_verify(KEY, BOOT, far, now, 5));
    wa_token_mint(KEY, BOOT, true, now + WA_SESSION_MAX_S, NONCE, far, sizeof(far));
    ok("expiry at the maximum lifetime accepted", wa_token_verify(KEY, BOOT, far, now, 5));

    ok("short token", !wa_token_verify(KEY, BOOT, "abcd", now, 5));
    char longer[WA_TOKEN_HEX_LEN + 3];
    snprintf(longer, sizeof(longer), "%s00", tok);
    ok("token with trailing bytes", !wa_token_verify(KEY, BOOT, longer, now, 5));
    char nonhex[sizeof(tok)];
    memcpy(nonhex, tok, sizeof(tok));
    nonhex[10] = 'g';
    ok("non-hex token", !wa_token_verify(KEY, BOOT, nonhex, now, 5));
    ok("empty token", !wa_token_verify(KEY, BOOT, "", now, 5));
    ok("mint into a short buffer fails",
       wa_token_mint(KEY, BOOT, true, now + 1, NONCE, tok, WA_TOKEN_HEX_LEN) != 0);

    char t3[WA_TOKEN_HEX_LEN + 1];
    uint8_t n2[WA_NONCE_LEN];
    memcpy(n2, NONCE, sizeof(n2));
    n2[0] ^= 1;
    wa_token_mint(KEY, BOOT, true, now + 3600, n2, t3, sizeof(t3));
    ok("different nonce, different token", strcmp(t3, tok) != 0);
}

static void test_basic(void)
{
    char pw[WA_PW_MAX_LEN + 1];
    size_t n = 0;

    /* RFC 7617 section 2 example */
    ok("RFC 7617 example", wa_basic_password("Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==", pw, sizeof(pw), &n));
    ok("RFC 7617 password", n == 11 && strcmp(pw, "open sesame") == 0);
    ok("scheme is case-insensitive", wa_basic_password("basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==", pw, sizeof(pw), &n));
    ok("extra whitespace", wa_basic_password("Basic   QWxhZGRpbjpvcGVuIHNlc2FtZQ==  ", pw, sizeof(pw), &n)
                           && strcmp(pw, "open sesame") == 0);

    char hdr[400];
    unsigned char b64[300];
    size_t olen;
    #define ENC(s) (mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char *)(s), strlen(s)), \
                    snprintf(hdr, sizeof(hdr), "Basic %s", b64), hdr)

    ok("colon in password kept", wa_basic_password(ENC("admin:a:b:c"), pw, sizeof(pw), &n) &&
                                 strcmp(pw, "a:b:c") == 0);
    ok("empty user", wa_basic_password(ENC(":secret12"), pw, sizeof(pw), &n) && strcmp(pw, "secret12") == 0);
    ok("empty password parses", wa_basic_password(ENC("admin:"), pw, sizeof(pw), &n) && n == 0);
    ok("no colon", !wa_basic_password(ENC("adminsecret"), pw, sizeof(pw), &n));
    ok("NUL inside password", !wa_basic_password("Basic YWRtaW46YQBi", pw, sizeof(pw), &n));

    char big[WA_PW_MAX_LEN + 8];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    big[0] = ':';
    ok("password longer than the buffer", !wa_basic_password(ENC(big), pw, sizeof(pw), &n));

    ok("Bearer scheme", !wa_basic_password("Bearer QWxhZGRpbjpvcGVuIHNlc2FtZQ==", pw, sizeof(pw), &n));
    ok("scheme without separator", !wa_basic_password("BasicQWxhZGRpbjpvcGVuIHNlc2FtZQ==", pw, sizeof(pw), &n));
    ok("scheme only", !wa_basic_password("Basic ", pw, sizeof(pw), &n));
    ok("bad base64", !wa_basic_password("Basic !!!!", pw, sizeof(pw), &n));
    char huge[2048] = "Basic ";
    memset(huge + 6, 'Q', sizeof(huge) - 7);
    huge[sizeof(huge) - 1] = '\0';
    ok("oversized header", !wa_basic_password(huge, pw, sizeof(pw), &n));
    #undef ENC
}

static void test_origin(void)
{
    ok("same host", wa_origin_matches_host("http://somnotrace.local", "somnotrace.local"));
    ok("case-insensitive", wa_origin_matches_host("http://SomnoTrace.local", "somnotrace.LOCAL"));
    ok("same ip:port", wa_origin_matches_host("http://192.168.1.5:8080", "192.168.1.5:8080"));
    ok("https origin", wa_origin_matches_host("https://somnotrace.local", "somnotrace.local"));
    ok("port differs", !wa_origin_matches_host("http://192.168.1.5:8080", "192.168.1.5"));
    ok("other site", !wa_origin_matches_host("http://evil.example", "somnotrace.local"));
    ok("our name as a subdomain of theirs",
       !wa_origin_matches_host("http://somnotrace.local.evil.example", "somnotrace.local"));
    ok("prefix of our name", !wa_origin_matches_host("http://somno", "somnotrace.local"));
    ok("opaque origin", !wa_origin_matches_host("null", "somnotrace.local"));
    ok("origin with a path", !wa_origin_matches_host("http://somnotrace.local/x", "somnotrace.local"));
    ok("other scheme", !wa_origin_matches_host("file://somnotrace.local", "somnotrace.local"));
    ok("empty host header", !wa_origin_matches_host("http://somnotrace.local", ""));
    ok("missing", !wa_origin_matches_host(NULL, "somnotrace.local"));
}

static void test_limiter(void)
{
    wa_limiter_t l = { 0 };
    int64_t t = 1000000;
    for (int i = 0; i < WA_FREE_ATTEMPTS; i++) {
        wa_limiter_fail(&l, t);
        eq("free attempts do not block", wa_limiter_wait_ms(&l, t), 0);
    }
    wa_limiter_fail(&l, t);
    eq("first paid failure: 1 s", wa_limiter_wait_ms(&l, t), 1000);
    eq("wait counts down", wa_limiter_wait_ms(&l, t + 400), 600);
    eq("wait over", wa_limiter_wait_ms(&l, t + 1000), 0);
    wa_limiter_fail(&l, t);
    eq("second paid failure: 2 s", wa_limiter_wait_ms(&l, t), 2000);
    for (int i = 0; i < 100; i++) wa_limiter_fail(&l, t);
    eq("block is capped", wa_limiter_wait_ms(&l, t), WA_MAX_BLOCK_MS);
    l.failures = UINT32_MAX;
    wa_limiter_fail(&l, t);
    eq("failure counter saturates", l.failures, UINT32_MAX);
    eq("still capped after saturation", wa_limiter_wait_ms(&l, t), WA_MAX_BLOCK_MS);
    wa_limiter_reset(&l);
    eq("reset clears the block", wa_limiter_wait_ms(&l, t), 0);
    wa_limiter_fail(&l, t);
    eq("reset restores the free attempts", wa_limiter_wait_ms(&l, t), 0);
}

int main(void)
{
    test_pbkdf2_vectors();
    test_password_records();
    test_password_policy();
    test_tokens();
    test_basic();
    test_origin();
    test_limiter();
    printf("web_auth_test: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
