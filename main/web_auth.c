/*
 * SomnoTrace - Optional password protection for the web interface
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

#include "web_auth.h"
#include "web_auth_core.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_writer.h"

static const char *TAG = "web_auth";

#define NVS_NS        "webauth"
#define NVS_KEY_PW    "pw"
#define NVS_KEY_SKEY  "skey"
#define PW_REC_VER    1

#define COOKIE_NAME   "st_session"
#define BODY_MAX      512
#define AUTH_HDR_MAX  300
#define MAX_ROUTES    128

/* Before this the RTC has not been set (no NTP since power-on), and the wall
 * clock cannot be trusted to expire a session: 2025-01-01T00:00:00Z. */
#define WALL_CLOCK_MIN  1735689600LL

/* NVS layout of the password record.  Fixed-size fields only. */
typedef struct {
    uint8_t  version;
    uint8_t  reserved[3];
    uint32_t iterations;
    uint8_t  salt[WA_SALT_LEN];
    uint8_t  hash[WA_HASH_LEN];
} stored_pw_t;

/* Exchange buffer for nvs_writer_run().  Static, so it is in internal RAM
 * and never on the (PSRAM) stack of the httpd worker. */
typedef enum { IO_LOAD, IO_SET, IO_SET_KEY, IO_CLEAR } io_op_t;
typedef struct {
    io_op_t     op;
    bool        have_pw, pw_valid, have_key;
    stored_pw_t rec;
    uint8_t     key[WA_KEY_LEN];
} nvs_io_t;
static nvs_io_t s_io;

static bool           s_inited;
static bool           s_enabled;      /* a password record exists in NVS */
static bool           s_rec_valid;    /* ...and is usable; false = fail closed */
static wa_pw_record_t s_rec;
static uint8_t        s_key[WA_KEY_LEN];
static uint8_t        s_boot_id[WA_BOOT_ID_LEN];
static wa_limiter_t   s_limiter;
static int64_t        s_retry_after_ms;

/* Basic-auth fast path: HMAC of the last password that verified, under a
 * per-boot random key.  Lets an API client that sends Basic on every request
 * skip PBKDF2 without keeping the password itself in RAM. */
static uint8_t s_cache_key[32];
static uint8_t s_cache_tag[32];
static bool    s_cache_valid;

/* Response headers must outlive httpd_resp_set_hdr() until the send. */
static char s_cookie_hdr[WA_TOKEN_HEX_LEN + 96];
static char s_retry_hdr[24];

typedef struct {
    esp_err_t (*handler)(httpd_req_t *req);
    void *user_ctx;
    bool  is_websocket;
} route_t;
static route_t s_routes[MAX_ROUTES];
static int     s_nroutes;

typedef enum { AUTH_OK, AUTH_MISSING, AUTH_BAD, AUTH_THROTTLED, AUTH_ORIGIN } auth_result_t;

/* ── NVS ───────────────────────────────────────────────────────────── */

/* Runs on the internal-stack nvs_writer task: works on a local copy and
 * touches s_io only before and after the flash operations. */
static esp_err_t do_nvs_io(void *arg)
{
    nvs_io_t io;
    memcpy(&io, arg, sizeof(io));
    nvs_handle_t h;
    esp_err_t err;

    if (io.op == IO_LOAD) {
        io.have_pw = io.pw_valid = io.have_key = false;
        err = nvs_open(NVS_NS, NVS_READONLY, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            memcpy(arg, &io, sizeof(io));
            return ESP_OK;
        }
        if (err != ESP_OK) return err;
        size_t sz = sizeof(io.rec);
        err = nvs_get_blob(h, NVS_KEY_PW, &io.rec, &sz);
        if (err == ESP_OK) {
            io.have_pw = true;
            io.pw_valid = (sz == sizeof(io.rec) && io.rec.version == PW_REC_VER);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            io.have_pw = true;          /* something is there: fail closed */
        }
        sz = sizeof(io.key);
        err = nvs_get_blob(h, NVS_KEY_SKEY, io.key, &sz);
        io.have_key = (err == ESP_OK && sz == sizeof(io.key));
        nvs_close(h);
        memcpy(arg, &io, sizeof(io));
        wa_wipe(&io, sizeof(io));
        return ESP_OK;
    }

    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        wa_wipe(&io, sizeof(io));
        return err;
    }
    if (io.op == IO_SET) {
        err = nvs_set_blob(h, NVS_KEY_PW, &io.rec, sizeof(io.rec));
        if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_SKEY, io.key, sizeof(io.key));
    } else if (io.op == IO_SET_KEY) {
        err = nvs_set_blob(h, NVS_KEY_SKEY, io.key, sizeof(io.key));
    } else {
        err = nvs_erase_all(h);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    wa_wipe(&io, sizeof(io));
    return err;
}

static esp_err_t nvs_io(io_op_t op)
{
    s_io.op = op;
    esp_err_t err = nvs_writer_run(do_nvs_io, &s_io);
    if (op != IO_LOAD) wa_wipe(&s_io, sizeof(s_io));
    return err;
}

/* ── helpers ───────────────────────────────────────────────────────── */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t wall_now(void)
{
    time_t t = time(NULL);
    return (int64_t)t >= WALL_CLOCK_MIN ? (int64_t)t : -1;
}

static void peer_ip(httpd_req_t *req, char *out, size_t cap)
{
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    out[0] = '\0';
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        strlcpy(out, "?", cap);
        return;
    }
    if (addr.sin6_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, out, cap);
    } else {
        inet_ntop(AF_INET6, &addr.sin6_addr, out, cap);
    }
}

static void cache_tag(const char *pw, size_t len, uint8_t out[32])
{
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    s_cache_key, sizeof(s_cache_key), (const uint8_t *)pw, len, out);
}

static void cache_forget(void)
{
    s_cache_valid = false;
    wa_wipe(s_cache_tag, sizeof(s_cache_tag));
}

/* Verify a password against the stored record, applying the rate limiter. */
static auth_result_t check_password(httpd_req_t *req, const char *pw, size_t len)
{
    if (s_cache_valid) {
        uint8_t tag[32];
        cache_tag(pw, len, tag);
        bool hit = wa_ct_equal(tag, s_cache_tag, sizeof(tag));
        wa_wipe(tag, sizeof(tag));
        if (hit) return AUTH_OK;
    }
    int64_t wait = wa_limiter_wait_ms(&s_limiter, now_ms());
    if (wait > 0) {
        s_retry_after_ms = wait;
        return AUTH_THROTTLED;
    }
    if (s_rec_valid && wa_pw_verify(&s_rec, pw, len)) {
        if (s_limiter.failures) ESP_LOGI(TAG, "password accepted after %u failed attempt(s)",
                                         (unsigned)s_limiter.failures);
        wa_limiter_reset(&s_limiter);
        cache_tag(pw, len, s_cache_tag);
        s_cache_valid = true;
        return AUTH_OK;
    }
    wa_limiter_fail(&s_limiter, now_ms());
    char ip[48];
    peer_ip(req, ip, sizeof(ip));
    ESP_LOGW(TAG, "wrong password from %s (%u consecutive)", ip, (unsigned)s_limiter.failures);
    return AUTH_BAD;
}

static bool session_valid(httpd_req_t *req)
{
    char tok[WA_TOKEN_HEX_LEN + 8];
    size_t len = sizeof(tok);
    if (httpd_req_get_cookie_val(req, COOKIE_NAME, tok, &len) != ESP_OK) return false;
    return wa_token_verify(s_key, s_boot_id, tok, wall_now(), esp_timer_get_time() / 1000000);
}

/* CSRF defence in depth behind SameSite=Strict: a browser request that
 * carries an Origin must come from the page the device itself served. */
static bool origin_ok(httpd_req_t *req)
{
    char origin[128], host[128];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
        /* absent: same-origin GET, or not a browser.  Truncated: reject. */
        return httpd_req_get_hdr_value_len(req, "Origin") == 0;
    }
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    return wa_origin_matches_host(origin, host);
}

static auth_result_t authenticate(httpd_req_t *req)
{
    if (!s_enabled) return AUTH_OK;

    if (session_valid(req)) {
        bool safe = req->method == HTTP_GET || req->method == HTTP_HEAD;
        return (safe && !httpd_req_get_hdr_value_len(req, "Upgrade")) || origin_ok(req)
               ? AUTH_OK : AUTH_ORIGIN;
    }

    size_t hlen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hlen == 0) return AUTH_MISSING;
    if (hlen > AUTH_HDR_MAX) return AUTH_BAD;
    char hdr[AUTH_HDR_MAX + 1];
    char pw[WA_PW_MAX_LEN + 1];
    size_t pwlen = 0;
    auth_result_t r = AUTH_BAD;
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        wa_basic_password(hdr, pw, sizeof(pw), &pwlen)) {
        r = check_password(req, pw, pwlen);
    }
    wa_wipe(hdr, sizeof(hdr));
    wa_wipe(pw, sizeof(pw));
    return r;
}

static esp_err_t send_json_status(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_denied(httpd_req_t *req, auth_result_t r)
{
    /* No WWW-Authenticate: it would make browsers pop their own dialog and
     * then attach the cached credentials to every request, CSRF included. */
    if (r == AUTH_THROTTLED) {
        snprintf(s_retry_hdr, sizeof(s_retry_hdr), "%lld", (long long)(s_retry_after_ms + 999) / 1000);
        httpd_resp_set_hdr(req, "Retry-After", s_retry_hdr);
        send_json_status(req, "429 Too Many Requests", "{\"error\":\"throttled\"}");
    } else if (r == AUTH_ORIGIN) {
        send_json_status(req, "403 Forbidden", "{\"error\":\"cross_origin\"}");
    } else {
        send_json_status(req, "401 Unauthorized", "{\"error\":\"auth_required\"}");
    }
    /* Do not let an unauthenticated client make us drain a large body
     * (firmware upload): failing the request closes the connection. */
    return req->content_len > BODY_MAX ? ESP_FAIL : ESP_OK;
}

static void set_session_cookie(httpd_req_t *req)
{
    uint8_t nonce[WA_NONCE_LEN];
    char tok[WA_TOKEN_HEX_LEN + 1];
    esp_fill_random(nonce, sizeof(nonce));
    int64_t wall = wall_now();
    int64_t exp = (wall >= 0 ? wall : esp_timer_get_time() / 1000000) + WA_SESSION_MAX_S;
    if (wa_token_mint(s_key, s_boot_id, wall >= 0, exp, nonce, tok, sizeof(tok)) != 0) return;
    snprintf(s_cookie_hdr, sizeof(s_cookie_hdr),
             COOKIE_NAME "=%s; Path=/; Max-Age=%lld; HttpOnly; SameSite=Strict",
             tok, (long long)WA_SESSION_MAX_S);
    httpd_resp_set_hdr(req, "Set-Cookie", s_cookie_hdr);
}

static void clear_session_cookie(httpd_req_t *req)
{
    strlcpy(s_cookie_hdr, COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict",
            sizeof(s_cookie_hdr));
    httpd_resp_set_hdr(req, "Set-Cookie", s_cookie_hdr);
}

/* Drop every other connection, so nothing opened under the old password
 * (a log WebSocket, a keep-alive socket) outlives a change. */
static void close_other_sessions(httpd_req_t *req)
{
    int fds[24];
    size_t n = sizeof(fds) / sizeof(fds[0]);
    if (httpd_get_client_list(req->handle, &n, fds) != ESP_OK) return;
    int own = httpd_req_to_sockfd(req);
    for (size_t i = 0; i < n; i++) {
        if (fds[i] != own) httpd_sess_trigger_close(req->handle, fds[i]);
    }
}

/* Read a small JSON body.  Caller wipes and frees with free_body(). */
static cJSON *read_json_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || req->content_len >= cap) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return NULL;
        got += r;
    }
    buf[got] = '\0';
    return cJSON_Parse(buf);
}

/* cJSON frees strings without clearing them; clear the secrets first. */
static void free_body(cJSON *root, char *buf, size_t cap)
{
    if (root) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, root) {
            if (cJSON_IsString(it) && it->valuestring) wa_wipe(it->valuestring, strlen(it->valuestring));
        }
        cJSON_Delete(root);
    }
    wa_wipe(buf, cap);
}

static const char *json_str(cJSON *root, const char *name, size_t *len)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(it) || !it->valuestring) return NULL;
    *len = strlen(it->valuestring);
    return it->valuestring;
}

/* ── gate ──────────────────────────────────────────────────────────── */

static esp_err_t gate(httpd_req_t *req)
{
    const route_t *r = (const route_t *)req->user_ctx;
    req->user_ctx = r->user_ctx;

    /* A WebSocket data frame arrives on a socket whose handshake already
     * passed this gate; every password change closes all other sockets. */
    if (r->is_websocket && req->method != HTTP_GET) return r->handler(req);

    auth_result_t a = authenticate(req);
    if (a == AUTH_OK) return r->handler(req);

    /* httpd has already answered a WebSocket handshake before calling us;
     * the only safe refusal left is to drop the connection. */
    if (r->is_websocket) return ESP_FAIL;
    return send_denied(req, a);
}

void web_auth_begin_routes(void)
{
    s_nroutes = 0;
}

esp_err_t web_auth_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    if (s_nroutes >= MAX_ROUTES) {
        ESP_LOGE(TAG, "route table full, '%s' NOT registered", uri->uri);
        return ESP_ERR_NO_MEM;
    }
    route_t *r = &s_routes[s_nroutes];
    r->handler = uri->handler;
    r->user_ctx = uri->user_ctx;
    r->is_websocket = uri->is_websocket;

    httpd_uri_t wrapped = *uri;
    wrapped.handler = gate;
    wrapped.user_ctx = r;
    esp_err_t err = httpd_register_uri_handler(server, &wrapped);
    if (err == ESP_OK) {
        s_nroutes++;
    } else {
        ESP_LOGE(TAG, "failed to register URI '%s' (method %d): %s",
                 uri->uri, (int)uri->method, esp_err_to_name(err));
    }
    return err;
}

esp_err_t web_auth_register_public(httpd_handle_t server, const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register URI '%s' (method %d): %s",
                 uri->uri, (int)uri->method, esp_err_to_name(err));
    }
    return err;
}

/* ── /api/auth endpoints ──────────────────────────────────────────────────── */

static esp_err_t auth_status_handler(httpd_req_t *req)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"enabled\":%s,\"authenticated\":%s}",
             s_enabled ? "true" : "false",
             (!s_enabled || session_valid(req)) ? "true" : "false");
    return send_json_status(req, "200 OK", json);
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    if (!origin_ok(req)) return send_denied(req, AUTH_ORIGIN);
    if (!s_enabled) return send_json_status(req, "200 OK", "{\"ok\":true}");

    char buf[BODY_MAX];
    cJSON *root = read_json_body(req, buf, sizeof(buf));
    size_t len = 0;
    const char *pw = root ? json_str(root, "password", &len) : NULL;
    if (!pw || len > WA_PW_MAX_LEN) {
        free_body(root, buf, sizeof(buf));
        return send_json_status(req, "400 Bad Request", "{\"error\":\"bad_request\"}");
    }
    auth_result_t r = check_password(req, pw, len);
    free_body(root, buf, sizeof(buf));

    if (r == AUTH_THROTTLED) return send_denied(req, r);
    if (r != AUTH_OK) return send_json_status(req, "401 Unauthorized", "{\"error\":\"wrong_password\"}");

    char ip[48];
    peer_ip(req, ip, sizeof(ip));
    ESP_LOGI(TAG, "login from %s", ip);
    set_session_cookie(req);
    return send_json_status(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t rotate_key(void)
{
    uint8_t key[WA_KEY_LEN];
    esp_fill_random(key, sizeof(key));
    memcpy(s_io.key, key, sizeof(key));
    esp_err_t err = nvs_io(IO_SET_KEY);
    if (err == ESP_OK) memcpy(s_key, key, sizeof(key));
    wa_wipe(key, sizeof(key));
    return err;
}

static esp_err_t auth_logout_handler(httpd_req_t *req)
{
    /* Ends every session, not just this one: tokens are stateless, so the
     * only way to revoke one is to change the key that signs them all. */
    if (s_enabled && rotate_key() != ESP_OK) {
        return send_json_status(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
    }
    clear_session_cookie(req);
    return send_json_status(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t set_password(const char *pw, size_t len)
{
    wa_pw_record_t rec = { .iterations = WA_PBKDF2_ITERATIONS };
    uint8_t key[WA_KEY_LEN];
    esp_fill_random(rec.salt, sizeof(rec.salt));
    esp_fill_random(key, sizeof(key));

    int64_t t0 = esp_timer_get_time();
    if (wa_pw_hash(&rec, pw, len) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "PBKDF2 (%u iterations) took %lld ms", (unsigned)rec.iterations,
             (long long)(esp_timer_get_time() - t0) / 1000);

    memset(&s_io.rec, 0, sizeof(s_io.rec));
    s_io.rec.version = PW_REC_VER;
    s_io.rec.iterations = rec.iterations;
    memcpy(s_io.rec.salt, rec.salt, sizeof(rec.salt));
    memcpy(s_io.rec.hash, rec.hash, sizeof(rec.hash));
    memcpy(s_io.key, key, sizeof(key));
    esp_err_t err = nvs_io(IO_SET);
    if (err == ESP_OK) {
        s_rec = rec;
        memcpy(s_key, key, sizeof(key));
        s_enabled = s_rec_valid = true;
    }
    wa_wipe(&rec, sizeof(rec));
    wa_wipe(key, sizeof(key));
    return err;
}

static esp_err_t auth_password_handler(httpd_req_t *req)
{
    char buf[BODY_MAX];
    cJSON *root = read_json_body(req, buf, sizeof(buf));
    size_t cur_len = 0, new_len = 0;
    const char *cur = root ? json_str(root, "current", &cur_len) : NULL;
    const char *npw = root ? json_str(root, "new", &new_len) : NULL;
    if (!npw) {
        free_body(root, buf, sizeof(buf));
        return send_json_status(req, "400 Bad Request", "{\"error\":\"bad_request\"}");
    }

    /* A valid session is not enough to change the password: the current one
     * must be re-entered, so a borrowed browser cannot lock the owner out. */
    if (s_enabled) {
        auth_result_t r = (cur && cur_len <= WA_PW_MAX_LEN)
                          ? check_password(req, cur, cur_len) : AUTH_BAD;
        if (r == AUTH_THROTTLED) {
            free_body(root, buf, sizeof(buf));
            return send_denied(req, r);
        }
        if (r != AUTH_OK) {
            free_body(root, buf, sizeof(buf));
            /* 403, not 401: the session is fine, only the re-entry is wrong. */
            return send_json_status(req, "403 Forbidden", "{\"error\":\"wrong_password\"}");
        }
    }

    char ip[48];
    peer_ip(req, ip, sizeof(ip));

    if (new_len == 0) {
        free_body(root, buf, sizeof(buf));
        if (!s_enabled) return send_json_status(req, "200 OK", "{\"ok\":true,\"enabled\":false}");
        if (nvs_io(IO_CLEAR) != ESP_OK) {
            return send_json_status(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
        }
        s_enabled = s_rec_valid = false;
        wa_wipe(&s_rec, sizeof(s_rec));
        wa_wipe(s_key, sizeof(s_key));
        cache_forget();
        wa_limiter_reset(&s_limiter);
        ESP_LOGW(TAG, "web password removed by %s", ip);
        close_other_sessions(req);
        clear_session_cookie(req);
        return send_json_status(req, "200 OK", "{\"ok\":true,\"enabled\":false}");
    }

    if (!wa_pw_acceptable(npw, new_len)) {
        free_body(root, buf, sizeof(buf));
        return send_json_status(req, "400 Bad Request", "{\"error\":\"weak_password\"}");
    }
    bool was_enabled = s_enabled;
    esp_err_t err = set_password(npw, new_len);
    free_body(root, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing the web password failed: %s", esp_err_to_name(err));
        return send_json_status(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
    }
    cache_forget();
    wa_limiter_reset(&s_limiter);
    ESP_LOGW(TAG, "web password %s by %s", was_enabled ? "changed" : "enabled", ip);
    close_other_sessions(req);
    set_session_cookie(req);
    return send_json_status(req, "200 OK", "{\"ok\":true,\"enabled\":true}");
}

void web_auth_register_handlers(httpd_handle_t server)
{
    httpd_uri_t status = { .uri = "/api/auth/status", .method = HTTP_GET, .handler = auth_status_handler };
    httpd_uri_t login = { .uri = "/api/auth/login", .method = HTTP_POST, .handler = auth_login_handler };
    httpd_uri_t logout = { .uri = "/api/auth/logout", .method = HTTP_POST, .handler = auth_logout_handler };
    httpd_uri_t password = { .uri = "/api/auth/password", .method = HTTP_POST, .handler = auth_password_handler };
    web_auth_register_public(server, &status);
    web_auth_register_public(server, &login);
    web_auth_register(server, &logout);
    web_auth_register(server, &password);
}

/* ── init ──────────────────────────────────────────────────────────── */

esp_err_t web_auth_init(void)
{
    if (s_inited) return ESP_OK;

    /* Per-boot secrets; the RF is up by the time the server starts, so
     * esp_fill_random() draws on the hardware entropy source. */
    esp_fill_random(s_boot_id, sizeof(s_boot_id));
    esp_fill_random(s_cache_key, sizeof(s_cache_key));

    esp_err_t err = nvs_io(IO_LOAD);
    if (err != ESP_OK) {
        /* Cannot tell whether a password is set: fail closed. */
        ESP_LOGE(TAG, "reading the web password failed (%s): web interface locked",
                 esp_err_to_name(err));
        s_enabled = true;
        s_rec_valid = false;
        esp_fill_random(s_key, sizeof(s_key));
        s_inited = true;
        return err;
    }

    s_enabled = s_io.have_pw;
    s_rec_valid = s_io.have_pw && s_io.pw_valid;
    if (s_rec_valid) {
        s_rec.iterations = s_io.rec.iterations;
        memcpy(s_rec.salt, s_io.rec.salt, sizeof(s_rec.salt));
        memcpy(s_rec.hash, s_io.rec.hash, sizeof(s_rec.hash));
    }
    bool have_key = s_io.have_key;
    if (have_key) memcpy(s_key, s_io.key, sizeof(s_key));
    wa_wipe(&s_io, sizeof(s_io));

    if (s_enabled && !have_key && rotate_key() != ESP_OK) {
        esp_fill_random(s_key, sizeof(s_key));   /* sessions last this boot only */
    }
    if (s_enabled && !s_rec_valid) {
        ESP_LOGE(TAG, "web password record is damaged: web interface locked");
    }
    ESP_LOGI(TAG, "web password %s", s_enabled ? "enabled" : "not set (web interface open)");
    s_inited = true;
    return ESP_OK;
}

bool web_auth_enabled(void)
{
    return s_enabled;
}
