/*
 * SomnoTrace - SleepHQ upload backend using raw esp_tls socket
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

#include "uploader.h"
#include "upload_paths.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "mbedtls/md5.h"


static const char *TAG = "upload_shq";

#define SHQ_HOST        "sleephq.com"
#define SHQ_PORT        443
#define SHQ_URL_BASE    "https://sleephq.com"
#define SHQ_TOKEN_PATH  "/oauth/token"
#define SHQ_ME_PATH     "/api/v1/me"
#define SHQ_IMPORTS_FMT "/api/v1/teams/%s/imports"
#define SHQ_IMPORT_FMT  "/api/v1/imports/%s"
#define SHQ_FILES_FMT   "/api/v1/imports/%s/files"
#define SHQ_PROCESS_FMT "/api/v1/imports/%s/process_files"

#define SHQ_TIMEOUT_MS  30000
#define SHQ_READ_BUF    1024
#define SHQ_RESP_CAP    4096

/* Token cache (token string in PSRAM; allocated on first auth). */
#define SHQ_TOKEN_MAX  512
static char *s_token;
static int64_t s_token_time_s = 0;
static int s_token_expires = 0;
static char s_team_id[32] = {0};

static bool shq_token_ready(void)
{
    if (s_token) return true;
    s_token = heap_caps_calloc(1, SHQ_TOKEN_MAX, MALLOC_CAP_SPIRAM);
    if (!s_token) {
        ESP_LOGE(TAG, "token buffer alloc failed");
        return false;
    }
    return true;
}

/* ── TLS socket layer ───────────────────────────────────────────────── */

static int shq_tls_write_all(esp_tls_t *tls, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t total = 0;
    int write_calls = 0;
    while (total < len) {
        ssize_t w = esp_tls_conn_write(tls, p + total, len - total);
        write_calls++;
        if (w < 0) {
            ESP_LOGE(TAG, "TLS write error: %d (after %u/%u bytes, %d calls)",
                     (int)w, (unsigned)total, (unsigned)len, write_calls);
            return -1;
        }
        if (w == 0) {
            if (write_calls > 100) {
                ESP_LOGE(TAG, "TLS write stuck: 0 return after %d calls", write_calls);
                return -1;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        total += w;
    }
    return (int)total;
}

/* Read a complete HTTP response from the TLS socket.
 * Returns HTTP status code (>=100) or -1 on error.
 * If body_out is non-NULL, the response body is stored in a malloc'd buffer
 * (caller must free).  If body_out is NULL, the body is drained and discarded.
 *
 * Uses buffered reads (not 1-byte-at-a-time) for compatibility with mbedTLS. */
static int shq_http_read_response(esp_tls_t *tls, char **body_out, size_t *body_len)
{
    /* Buffer for entire response (headers + body) */
    size_t buf_cap = SHQ_RESP_CAP + 1024;
    char *buf = heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(buf_cap);
    if (!buf) return -1;

    size_t buf_len = 0;
    char *header_end = NULL;

    /* Phase 1: read until we find \r\n\r\n (end of headers) */
    while (!header_end) {
        if (buf_len >= buf_cap - 1) {
            ESP_LOGE(TAG, "response headers too large (%u)", (unsigned)buf_len);
            free(buf);
            return -1;
        }
        ssize_t n = esp_tls_conn_read(tls, buf + buf_len, buf_cap - buf_len - 1);
        if (n < 0) {
            ESP_LOGE(TAG, "TLS read error during headers: %d", (int)n);
            free(buf);
            return -1;
        }
        if (n == 0) {
            /* Connection closed before headers complete */
            ESP_LOGE(TAG, "connection closed during headers (got %u bytes)", (unsigned)buf_len);
            free(buf);
            return -1;
        }
        buf_len += n;
        buf[buf_len] = '\0';

        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) header_end += 4;
    }

    /* Parse status line */
    int status = -1;
    if (strncmp(buf, "HTTP/", 5) == 0) {
        char *sp = strchr(buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (status < 0) {
        ESP_LOGE(TAG, "no HTTP status in response");
        free(buf);
        return -1;
    }

    /* Parse headers by temporarily null-terminating each line */
    size_t content_length = 0;
    bool chunked = false;

    char *line = buf;
    char *body_start = header_end;
    while (line < body_start - 4) {
        char *eol = strstr(line, "\r\n");
        if (!eol || eol >= body_start - 4) break;
        *eol = '\0';

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *v = line + 15;
            while (*v == ' ') v++;
            content_length = (size_t)atoi(v);
        }
        if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            if (strstr(line, "chunked")) chunked = true;
        }

        *eol = '\r';
        line = eol + 2;
    }

    /* Calculate body data already in buffer */
    size_t body_in_buf = buf_len - (body_start - buf);

    /* Phase 2: read remaining body */
    if (content_length > 0 && body_in_buf < content_length) {
        size_t remaining = content_length - body_in_buf;
        while (remaining > 0) {
            if (buf_len >= buf_cap - 1) break;
            size_t to_read = remaining < (buf_cap - buf_len - 1) ?
                             remaining : (buf_cap - buf_len - 1);
            ssize_t n = esp_tls_conn_read(tls, buf + buf_len, to_read);
            if (n < 0) {
                ESP_LOGE(TAG, "TLS read error during body: %d", (int)n);
                free(buf);
                return -1;
            }
            if (n == 0) break;
            buf_len += n;
            remaining -= n;
        }
    } else if (chunked) {
        /* Read until we see 0\r\n\r\n or connection closes */
        while (1) {
            if (buf_len >= buf_cap - 1) break;
            ssize_t n = esp_tls_conn_read(tls, buf + buf_len, buf_cap - buf_len - 1);
            if (n < 0) { free(buf); return -1; }
            if (n == 0) break;
            buf_len += n;
            buf[buf_len] = '\0';
            if (strstr(body_start, "\r\n0\r\n\r\n")) break;
        }
    } else if (content_length == 0 && !chunked) {
        /* No Content-Length, no chunked — read until connection closes */
        while (1) {
            if (buf_len >= buf_cap - 1) break;
            ssize_t n = esp_tls_conn_read(tls, buf + buf_len, buf_cap - buf_len - 1);
            if (n <= 0) break;
            buf_len += n;
        }
    }

    /* De-chunk the body in-place if the response used chunked transfer encoding.
     * Without this, chunk size prefixes (e.g. "409\r\n") corrupt the JSON body
     * and cJSON_Parse misinterprets the hex chunk size as a JSON number. */
    size_t body_total = buf_len - (body_start - buf);
    if (chunked && body_total > 0) {
        size_t rd = 0;          /* read position in raw body  */
        size_t wr = 0;          /* write position (de-chunked) */
        while (rd < body_total) {
            /* Parse hex chunk size up to \r\n */
            size_t chunk_sz = 0;
            int hex_digits = 0;
            while (rd < body_total && body_start[rd] != '\r') {
                char c = body_start[rd];
                int val;
                if (c >= '0' && c <= '9') val = c - '0';
                else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
                else break;
                chunk_sz = chunk_sz * 16 + val;
                hex_digits++;
                rd++;
            }
            if (hex_digits == 0) break;
            /* Skip \r\n after chunk size */
            if (rd + 1 < body_total && body_start[rd] == '\r' && body_start[rd + 1] == '\n')
                rd += 2;
            else break;
            /* Copy chunk data (if it fits) */
            if (chunk_sz == 0) break;       /* terminal chunk */
            size_t copy = chunk_sz;
            if (rd + copy > body_total) copy = body_total - rd;
            if (wr + copy > buf_cap - (body_start - buf)) break;
            memmove(body_start + wr, body_start + rd, copy);
            wr += copy;
            rd += chunk_sz;
            /* Skip trailing \r\n after chunk data */
            if (rd + 1 < body_total && body_start[rd] == '\r' && body_start[rd + 1] == '\n')
                rd += 2;
        }
        body_total = wr;
        buf_len = (body_start - buf) + body_total;
    }

    /* Extract body if requested */
    if (body_out) {
        if (body_total > SHQ_RESP_CAP) body_total = SHQ_RESP_CAP;
        char *body = heap_caps_malloc(SHQ_RESP_CAP, MALLOC_CAP_SPIRAM);
        if (!body) body = malloc(SHQ_RESP_CAP);
        if (!body) { free(buf); return -1; }
        memcpy(body, body_start, body_total);
        if (body_total < SHQ_RESP_CAP) body[body_total] = '\0';
        *body_out = body;
        if (body_len) *body_len = body_total;
    }

    ESP_LOGI(TAG, "response: HTTP %d (%u bytes body)", status,
             (unsigned)body_total);

    free(buf);
    return status;
}

/* ── HTTP request helpers ───────────────────────────────────────────── */

/* Send a simple GET or POST request with optional body and read the response.
 * The TLS connection stays open — caller manages it.
 * If body_out is non-NULL, response body is returned (caller frees). */
static int shq_http_request(esp_tls_t *tls, const char *method,
                            const char *path, const char *query,
                            const char *auth_token,
                            const char *body, const char *content_type,
                            char **body_out, size_t *body_len)
{
    /* Build request line + headers */
    char req[2048];
    int pos = 0;

    if (query) {
        pos += snprintf(req + pos, sizeof(req) - pos, "%s %s?%s HTTP/1.1\r\n", method, path, query);
    } else {
        pos += snprintf(req + pos, sizeof(req) - pos, "%s %s HTTP/1.1\r\n", method, path);
    }
    pos += snprintf(req + pos, sizeof(req) - pos, "Host: %s\r\n", SHQ_HOST);
    pos += snprintf(req + pos, sizeof(req) - pos, "Accept: application/vnd.api+json\r\n");
    pos += snprintf(req + pos, sizeof(req) - pos, "Connection: keep-alive\r\n");

    if (auth_token) {
        pos += snprintf(req + pos, sizeof(req) - pos, "Authorization: Bearer %s\r\n", auth_token);
    }

    if (body && content_type) {
        pos += snprintf(req + pos, sizeof(req) - pos, "Content-Type: %s\r\n", content_type);
        pos += snprintf(req + pos, sizeof(req) - pos, "Content-Length: %d\r\n", (int)strlen(body));
    }

    pos += snprintf(req + pos, sizeof(req) - pos, "\r\n");

    if (body) {
        pos += snprintf(req + pos, sizeof(req) - pos, "%s", body);
    }

    if (pos >= (int)sizeof(req) - 1) {
        ESP_LOGE(TAG, "request header too long (%d bytes)", pos);
        return -1;
    }

    ESP_LOGI(TAG, "sending %s %s (%d bytes)", method, path, pos);

    if (shq_tls_write_all(tls, req, pos) < 0) {
        ESP_LOGE(TAG, "failed to send request");
        return -1;
    }

    int status = shq_http_read_response(tls, body_out, body_len);
    if (status < 0) {
        ESP_LOGE(TAG, "failed to read response");
        return -1;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, body_out && *body_out ? *body_out : "(no body)");
        return status;
    }

    return status;
}

/* ── Authentication ─────────────────────────────────────────────────── */

/* One token request.  On success the token is copied into `token` and its
 * lifetime into *expires_s.  On failure `err` (may be NULL) receives a
 * one-line reason worded for the web UI. */
static esp_err_t shq_request_token(esp_tls_t *tls, const uploader_config_t *cfg,
                                   char *token, size_t token_cap, int *expires_s,
                                   char *err, size_t err_len)
{
    char body[512];
    snprintf(body, sizeof(body),
             "grant_type=password&client_id=%s&client_secret=%s&scope=read+write",
             cfg->shq_client_id, cfg->shq_client_secret);

    char *resp_body = NULL;
    size_t resp_len = 0;
    int status = shq_http_request(tls, "POST", SHQ_TOKEN_PATH, NULL, NULL,
                                  body, "application/x-www-form-urlencoded",
                                  &resp_body, &resp_len);
    if (status < 200) {
        ESP_LOGE(TAG, "auth request failed (status=%d)", status);
        if (err) snprintf(err, err_len, "No answer from %s", SHQ_HOST);
        free(resp_body);
        return ESP_FAIL;
    }
    if (status >= 300) {
        /* 401 is what a wrong or revoked Client ID / Secret produces. */
        if (err) snprintf(err, err_len, "SleepHQ rejected the API key (HTTP %d): "
                          "check Client ID / Secret and that the account has API access",
                          status);
        free(resp_body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);

    if (!root) {
        ESP_LOGE(TAG, "auth: failed to parse JSON");
        if (err) snprintf(err, err_len, "Unexpected reply from %s", SHQ_HOST);
        return ESP_FAIL;
    }

    cJSON *tok = cJSON_GetObjectItem(root, "access_token");
    cJSON *expires = cJSON_GetObjectItem(root, "expires_in");

    if (!tok || !cJSON_IsString(tok)) {
        ESP_LOGE(TAG, "auth: no access_token in response");
        if (err) snprintf(err, err_len, "SleepHQ did not issue a token");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(token, tok->valuestring, token_cap);
    *expires_s = (expires && cJSON_IsNumber(expires)) ? expires->valueint : 7200;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t shq_authenticate(esp_tls_t *tls, const uploader_config_t *cfg)
{
    if (s_token && s_token[0] && s_token_expires > 0) {
        int64_t now_s = time(NULL);
        int elapsed = (int)(now_s - s_token_time_s);
        if (elapsed < s_token_expires - 60) {
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "authenticating with SleepHQ...");

    if (!shq_token_ready()) return ESP_ERR_NO_MEM;

    int expires_s = 0;
    esp_err_t rc = shq_request_token(tls, cfg, s_token, SHQ_TOKEN_MAX, &expires_s,
                                     NULL, 0);
    if (rc != ESP_OK) return rc;
    s_token_expires = expires_s;
    s_token_time_s = time(NULL);

    ESP_LOGI(TAG, "authenticated, token expires in %d s", s_token_expires);
    return ESP_OK;
}

/* ── Team discovery ─────────────────────────────────────────────────── */

static esp_err_t shq_discover_team(esp_tls_t *tls)
{
    if (s_team_id[0]) return ESP_OK;

    ESP_LOGI(TAG, "discovering team ID...");

    char *resp_body = NULL;
    size_t resp_len = 0;
    int status = shq_http_request(tls, "GET", SHQ_ME_PATH, NULL, s_token,
                                  NULL, NULL, &resp_body, &resp_len);
    if (status < 200) {
        free(resp_body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);

    if (!root) return ESP_FAIL;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON *attrs = cJSON_GetObjectItem(data, "attributes");
        cJSON *team = NULL;
        if (attrs) team = cJSON_GetObjectItem(attrs, "current_team_id");
        if (!team) team = cJSON_GetObjectItem(data, "current_team_id");
        if (team) {
            if (cJSON_IsNumber(team))
                snprintf(s_team_id, sizeof(s_team_id), "%d", team->valueint);
            else if (cJSON_IsString(team))
                strlcpy(s_team_id, team->valuestring, sizeof(s_team_id));
        }
    }

    cJSON_Delete(root);

    if (!s_team_id[0]) {
        ESP_LOGE(TAG, "team discovery: no current_team_id found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "team ID: %s", s_team_id);
    return ESP_OK;
}

/* ── Create import session ──────────────────────────────────────────── */

static esp_err_t shq_create_import(esp_tls_t *tls, char *out_import_id, size_t id_len, bool o2)
{
    ESP_LOGI(TAG, "creating import session...");

    char path[256];
    snprintf(path, sizeof(path), SHQ_IMPORTS_FMT, s_team_id);

    char *resp_body = NULL;
    size_t resp_len = 0;
    int status = shq_http_request(tls, "POST", path, o2 ? "o2=true" : NULL, s_token,
                                  NULL, NULL, &resp_body, &resp_len);
    if (status < 200) {
        free(resp_body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);

    if (!root) return ESP_FAIL;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON *attrs = cJSON_GetObjectItem(data, "attributes");
        cJSON *id = NULL;
        if (attrs) id = cJSON_GetObjectItem(attrs, "id");
        if (!id) id = cJSON_GetObjectItem(data, "id");
        if (id) {
            if (cJSON_IsNumber(id))
                snprintf(out_import_id, id_len, "%d", id->valueint);
            else if (cJSON_IsString(id))
                strlcpy(out_import_id, id->valuestring, id_len);
        }
    }

    cJSON_Delete(root);

    if (!out_import_id[0]) {
        ESP_LOGE(TAG, "create import: no import ID in response");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "import ID: %s", out_import_id);
    return ESP_OK;
}

/* ── Process import (finalize) ──────────────────────────────────────── */

static esp_err_t shq_process_import(esp_tls_t *tls, const char *import_id)
{
    ESP_LOGI(TAG, "processing import %s...", import_id);

    char path[256];
    snprintf(path, sizeof(path), SHQ_PROCESS_FMT, import_id);

    int status = shq_http_request(tls, "POST", path, NULL, s_token,
                                  NULL, NULL, NULL, NULL);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "process import HTTP %d", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "import %s processed", import_id);
    return ESP_OK;
}

static esp_err_t shq_wait_import(esp_tls_t *tls, const char *import_id)
{
    char path[256];
    snprintf(path, sizeof(path), SHQ_IMPORT_FMT, import_id);
    for (int attempt = 0; attempt < 30; attempt++) {
        char *body = NULL; size_t body_len = 0;
        int status = shq_http_request(tls, "GET", path, NULL, s_token,
                                       NULL, NULL, &body, &body_len);
        if (status < 200 || status >= 300) { free(body); return ESP_FAIL; }
        cJSON *root = cJSON_Parse(body); free(body);
        if (!root) return ESP_FAIL;
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *attrs = data ? cJSON_GetObjectItem(data, "attributes") : NULL;
        cJSON *st = attrs ? cJSON_GetObjectItem(attrs, "status") : NULL;
        const char *name = cJSON_IsString(st) ? st->valuestring : "";
        bool complete = strcmp(name, "complete") == 0 || strcmp(name, "completed") == 0;
        bool failed = strcmp(name, "failed") == 0 || strcmp(name, "error") == 0;
        cJSON_Delete(root);
        if (complete) return ESP_OK;
        if (failed) return ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return ESP_ERR_TIMEOUT;
}

/* ── Multipart file upload (streaming, on-the-fly MD5) ────────────────
 *
 * Streams the file directly from SD card to the TLS socket in a single
 * pass.  MD5 is computed on-the-fly as each chunk is read and sent.
 * The content_hash is sent in the multipart footer after the file data.
 * No PSRAM buffering needed — only one chunk buffer is allocated. */

static upload_result_t shq_upload_file(esp_tls_t *tls,
                                       const char *import_id,
                                       const char *local_path,
                                       const char *remote_subpath,
                                       const char *filename,
                                       bool filename_first)
{
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "  cannot open %s", local_path);
        return UPLOAD_FAILED;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Build multipart boundary */
    char boundary[48];
    snprintf(boundary, sizeof(boundary), "----ESP32%08X", (unsigned)esp_random());

    /* Calculate sizes of multipart parts (no heap alloc for dummy calc) */
    char part1[512];
    size_t part1_len = snprintf(part1, sizeof(part1),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
        "%s\r\n",
        boundary, filename);

    char part2[512];
    size_t part2_len = snprintf(part2, sizeof(part2),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
        "%s\r\n",
        boundary, remote_subpath);

    char part3[512];
    size_t part3_len = snprintf(part3, sizeof(part3),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        boundary, filename);

    /* Footer: content_hash (32 hex chars) + closing boundary */
    char footer_hdr[256];
    size_t footer_hdr_len = snprintf(footer_hdr, sizeof(footer_hdr),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"content_hash\"\r\n\r\n",
        boundary);

    char closing[64];
    size_t closing_len = snprintf(closing, sizeof(closing),
        "\r\n--%s--\r\n",
        boundary);

    /* Total multipart body length = parts + file + footer_header + 32 (md5 hex) + closing */
    size_t total_body_len = part1_len + part2_len + part3_len + file_size
                           + footer_hdr_len + 32 + closing_len;

    /* Build HTTP request headers */
    char path[256];
    snprintf(path, sizeof(path), SHQ_FILES_FMT, import_id);

    char req_hdr[1024];
    int hdr_pos = snprintf(req_hdr, sizeof(req_hdr),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Accept: application/vnd.api+json\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %u\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path, SHQ_HOST, s_token, boundary, (unsigned)total_body_len);

    if (hdr_pos <= 0 || hdr_pos >= (int)sizeof(req_hdr)) {
        ESP_LOGE(TAG, "  request header too long");
        fclose(f);
        return UPLOAD_FAILED;
    }

    /* Send HTTP headers */
    if (shq_tls_write_all(tls, req_hdr, hdr_pos) < 0) {
        ESP_LOGE(TAG, "  failed to send HTTP headers for %s", filename);
        fclose(f);
        return UPLOAD_FAILED;
    }

    /* Send multipart preamble parts */
    if (shq_tls_write_all(tls, part1, part1_len) < 0 ||
        shq_tls_write_all(tls, part2, part2_len) < 0 ||
        shq_tls_write_all(tls, part3, part3_len) < 0) {
        ESP_LOGE(TAG, "  failed to send multipart preamble for %s", filename);
        fclose(f);
        return UPLOAD_FAILED;
    }

    /* Stream file data with on-the-fly MD5 */
    uint8_t *chunk = heap_caps_malloc(SHQ_READ_BUF, MALLOC_CAP_SPIRAM);
    if (!chunk) chunk = malloc(SHQ_READ_BUF);
    if (!chunk) {
        ESP_LOGE(TAG, "  cannot alloc chunk buffer for %s", filename);
        fclose(f);
        return UPLOAD_FAILED;
    }

    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    /* O2 imports use filename + content; preserve the CPAP contract unless
     * the caller explicitly selects the O2 profile. */
    if (filename_first)
        mbedtls_md5_update(&md5, (const unsigned char *)filename, strlen(filename));

    size_t total_sent = 0;
    while (total_sent < file_size) {
        size_t to_read = SHQ_READ_BUF;
        if (to_read > file_size - total_sent)
            to_read = file_size - total_sent;

        size_t nread = fread(chunk, 1, to_read, f);
        if (nread == 0) {
            ESP_LOGE(TAG, "  short read for %s at offset %u", filename, (unsigned)total_sent);
            free(chunk);
            fclose(f);
            mbedtls_md5_free(&md5);
            return UPLOAD_FAILED;
        }

        /* Feed to MD5 */
        mbedtls_md5_update(&md5, chunk, nread);

        /* Write to TLS socket */
        if (shq_tls_write_all(tls, chunk, nread) < 0) {
            ESP_LOGE(TAG, "  TLS write failed for %s at offset %u", filename, (unsigned)total_sent);
            free(chunk);
            fclose(f);
            mbedtls_md5_free(&md5);
            return UPLOAD_FAILED;
        }

        total_sent += nread;

        /* Yield to scheduler */
        if (total_sent % (SHQ_READ_BUF * 4) == 0)
            taskYIELD();
    }

    free(chunk);
    fclose(f);

    /* Finalize the profile-specific MD5 ordering. */
    if (!filename_first)
        mbedtls_md5_update(&md5, (const unsigned char *)filename, strlen(filename));
    unsigned char md5_raw[16];
    mbedtls_md5_finish(&md5, md5_raw);
    mbedtls_md5_free(&md5);

    char md5_hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(md5_hex + i * 2, 3, "%02x", md5_raw[i]);
    md5_hex[32] = '\0';

    /* Send footer: header + md5 hex + closing boundary */
    if (shq_tls_write_all(tls, footer_hdr, footer_hdr_len) < 0 ||
        shq_tls_write_all(tls, md5_hex, 32) < 0 ||
        shq_tls_write_all(tls, closing, closing_len) < 0) {
        ESP_LOGE(TAG, "  failed to send multipart footer for %s", filename);
        return UPLOAD_FAILED;
    }

    /* Read response (drain body, we only need status) */
    int status = shq_http_read_response(tls, NULL, NULL);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "  file upload HTTP %d for %s", status, filename);
        return UPLOAD_FAILED;
    }

    ESP_LOGI(TAG, "  uploaded %s (%u bytes, hash=%s)", filename,
             (unsigned)file_size, md5_hex);
    return UPLOAD_OK;
}

/* ── Upload all files for a session ─────────────────────────────────── */

/* ── Backend interface ──────────────────────────────────────────────
 *
 * One TLS connection (and one OAuth token) spans the whole run; an *import*
 * spans one day.  A day's import therefore contains only that day's pending
 * groups plus the root bundle, which is exactly what SleepHQ needs to
 * interpret them — partial-day imports are normal and expected.
 *
 * The root bundle is sent for every import, not just when it changed: without
 * STR.edf the sessions in that import cannot be interpreted. */

static esp_tls_t *s_tls;                  /* live for the whole run */
static char s_import_id[32];
static int  s_day_files;                  /* files sent in the current import */

static bool shq_is_configured(void)
{
    return uploader_is_sleephq_configured();
}

static upload_result_t shq_session_begin(void)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);
    if (!cfg.shq_client_id[0] || !cfg.shq_client_secret[0])
        return UPLOAD_NOT_CONFIGURED;

    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = SHQ_TIMEOUT_MS,
    };

    s_tls = esp_tls_init();
    if (!s_tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return UPLOAD_ERR_TRANSIENT;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://%s", SHQ_HOST);
    if (esp_tls_conn_http_new_sync(url, &tls_cfg, s_tls) != 1) {
        ESP_LOGE(TAG, "TLS connect to %s failed", SHQ_HOST);
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
        return UPLOAD_ERR_TRANSIENT;
    }
    ESP_LOGI(TAG, "TLS connected to %s", SHQ_HOST);

    if (shq_authenticate(s_tls, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "authentication failed");
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
        /* Credentials the server refuses will not start working on retry, so
         * report it as permanent and let the UI say so. */
        return UPLOAD_ERR_PERMANENT;
    }
    if (shq_discover_team(s_tls) != ESP_OK) {
        ESP_LOGE(TAG, "team discovery failed");
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
        return UPLOAD_ERR_TRANSIENT;
    }
    return UPLOAD_OK;
}

static void shq_session_end(void)
{
    if (!s_tls) return;
    esp_tls_conn_destroy(s_tls);
    s_tls = NULL;
    s_import_id[0] = '\0';
}

/* ── "Test connection" (web UI) ───────────────────────────────────────
 *
 * TLS connect plus one token request with the saved Client ID / Secret, on
 * a private connection so it can run from the httpd task while the
 * scheduler is idle.  The token is discarded rather than cached: the cache
 * belongs to the scheduler task and a probe must not race it.  No import is
 * opened, so nothing appears in the user's SleepHQ history. */
#define SHQ_TEST_TIMEOUT_MS 10000

static bool shq_test(char *msg, size_t msg_len)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);
    if (!cfg.shq_client_id[0] || !cfg.shq_client_secret[0]) {
        snprintf(msg, msg_len, "Client ID and Client Secret are required");
        return false;
    }

    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = SHQ_TEST_TIMEOUT_MS,
    };
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        snprintf(msg, msg_len, "Out of memory");
        return false;
    }
    if (esp_tls_conn_http_new_sync(SHQ_URL_BASE, &tls_cfg, tls) != 1) {
        snprintf(msg, msg_len, "Cannot reach %s: check the internet connection", SHQ_HOST);
        esp_tls_conn_destroy(tls);
        return false;
    }

    char *token = heap_caps_malloc(SHQ_TOKEN_MAX, MALLOC_CAP_SPIRAM);
    if (!token) token = malloc(SHQ_TOKEN_MAX);
    if (!token) {
        snprintf(msg, msg_len, "Out of memory");
        esp_tls_conn_destroy(tls);
        return false;
    }

    int expires_s = 0;
    char err[128];
    esp_err_t rc = shq_request_token(tls, &cfg, token, SHQ_TOKEN_MAX, &expires_s,
                                     err, sizeof(err));
    esp_tls_conn_destroy(tls);
    memset(token, 0, SHQ_TOKEN_MAX);
    free(token);

    if (rc != ESP_OK) {
        snprintf(msg, msg_len, "%s", err);
        return false;
    }
    snprintf(msg, msg_len, "Signed in to SleepHQ, API key accepted (token valid for %d min)",
             expires_s / 60);
    return true;
}

static upload_result_t shq_day_begin(const char *day)
{
    if (!s_tls) {
        if (shq_session_begin() != UPLOAD_OK)
            return UPLOAD_ERR_TRANSIENT;
    }
    s_import_id[0] = '\0';
    s_day_files = 0;

    if (shq_create_import(s_tls, s_import_id, sizeof(s_import_id), false) != ESP_OK) {
        /* TLS connection may have been closed by remote server. Try reconnecting once. */
        ESP_LOGW(TAG, "import creation failed on existing TLS connection, reconnecting...");
        shq_session_end();
        if (shq_session_begin() != UPLOAD_OK ||
            shq_create_import(s_tls, s_import_id, sizeof(s_import_id), false) != ESP_OK) {
            ESP_LOGE(TAG, "import creation failed for day %s", day);
            return UPLOAD_ERR_TRANSIENT;
        }
    }
    ESP_LOGI(TAG, "day %s -> import %s", day, s_import_id);
    return UPLOAD_OK;
}

static upload_result_t shq_ox_day_begin(const char *day)
{
    if (!s_tls) {
        if (shq_session_begin() != UPLOAD_OK)
            return UPLOAD_ERR_TRANSIENT;
    }
    s_import_id[0] = '\0';
    s_day_files = 0;
    if (shq_create_import(s_tls, s_import_id, sizeof(s_import_id), true) != ESP_OK) {
        /* TLS connection may have been closed by remote server. Try reconnecting once. */
        ESP_LOGW(TAG, "O2 import creation failed on existing TLS connection, reconnecting...");
        shq_session_end();
        if (shq_session_begin() != UPLOAD_OK ||
            shq_create_import(s_tls, s_import_id, sizeof(s_import_id), true) != ESP_OK) {
            ESP_LOGE(TAG, "O2 import creation failed for day %s", day);
            return UPLOAD_ERR_TRANSIENT;
        }
    }
    ESP_LOGI(TAG, "O2 day %s -> import %s", day, s_import_id);
    return UPLOAD_OK;
}

static upload_result_t shq_put_oximetry(const upload_ox_ref_t *ref)
{
    if (!s_tls || !s_import_id[0] || !ref) return UPLOAD_ERR_TRANSIENT;
    for (int i = 0; i < ref->n_files; i++) {
        const char *rel = ref->relative_paths[i];
        if (strcmp(rel, "source/source.bin") != 0 &&
            strcmp(rel, "source/source.vld") != 0) continue;
        char subpath[96];
        snprintf(subpath, sizeof(subpath), "/OXYMETRY/%s", ref->day);
        char filename[128];
        snprintf(filename, sizeof(filename), "%s",
                 ref->source_name[0] ? ref->source_name : "oximetry");
        if (shq_upload_file(s_tls, s_import_id, ref->local_paths[i], subpath,
                            filename, true) != UPLOAD_OK)
            return UPLOAD_ERR_TRANSIENT;
        s_day_files++;
        return UPLOAD_OK;
    }
    return UPLOAD_ERR_PERMANENT;
}

static upload_result_t shq_put_group(const char *day, const upload_group_ref_t *g)
{
    if (!s_tls || !s_import_id[0] || !g) return UPLOAD_ERR_TRANSIENT;

    char remote_subpath[64];
    snprintf(remote_subpath, sizeof(remote_subpath), "/DATALOG/%s", day);

    for (int i = 0; i < g->n_files; i++) {
        char local[512];
        snprintf(local, sizeof(local), "%s/%s/%s", SD_SDCARD_DATALOG, day,
                 g->files[i]);

        if (shq_upload_file(s_tls, s_import_id, local, remote_subpath,
                            g->files[i], false) != UPLOAD_OK) {
            ESP_LOGW(TAG, "  failed to upload %s", g->files[i]);
            return UPLOAD_ERR_TRANSIENT;
        }
        s_day_files++;
    }
    ESP_LOGI(TAG, "  group %s: %d file(s)", g->prefix, g->n_files);
    return UPLOAD_OK;
}

static upload_result_t shq_put_bundle(const char *day,
                                     const upload_bundle_ref_t *b, bool changed)
{
    (void)day;
    (void)changed;   /* always required inside an import — see header note */
    if (!s_tls || !s_import_id[0] || !b) return UPLOAD_ERR_TRANSIENT;

    for (int i = 0; i < b->n_files; i++) {
        const char *subpath = b->in_settings[i] ? "/SETTINGS" : "";
        if (shq_upload_file(s_tls, s_import_id, b->paths[i], subpath,
                            b->names[i], false) != UPLOAD_OK) {
            ESP_LOGW(TAG, "  failed to upload %s", b->names[i]);
            return UPLOAD_ERR_TRANSIENT;
        }
        s_day_files++;
    }
    ESP_LOGI(TAG, "  root bundle: %d file(s)", b->n_files);
    return UPLOAD_OK;
}

static upload_result_t shq_day_end(const char *day, bool any_uploaded)
{
    if (!s_tls || !s_import_id[0]) return UPLOAD_ERR_TRANSIENT;

    /* An import with no files would leave an empty record in the user's
     * SleepHQ history; skip processing it. */
    if (!any_uploaded && s_day_files == 0) {
        ESP_LOGI(TAG, "day %s: nothing sent, import left unprocessed", day);
        s_import_id[0] = '\0';
        return UPLOAD_OK;
    }

    esp_err_t err = shq_process_import(s_tls, s_import_id);
    if (err == ESP_OK) err = shq_wait_import(s_tls, s_import_id);
    s_import_id[0] = '\0';
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "import processing failed for day %s, resetting TLS connection", day);
        shq_session_end();
        return UPLOAD_ERR_TRANSIENT;
    }
    ESP_LOGI(TAG, "day %s uploaded (%d file(s))", day, s_day_files);
    return UPLOAD_OK;
}

const upload_backend_t sleephq_backend = {
    .id = "sleephq",
    .label = "SleepHQ Cloud",
    .bundle_only_ok = false,    /* would create an import with no sessions */
    .is_configured = shq_is_configured,
    .session_begin = shq_session_begin,
    .day_begin = shq_day_begin,
    .put_group = shq_put_group,
    .ox_day_begin = shq_ox_day_begin,
    .put_oximetry = shq_put_oximetry,
    .ox_day_end = shq_day_end,
    .put_bundle = shq_put_bundle,
    .day_end = shq_day_end,
    .session_end = shq_session_end,
    .test = shq_test,
};
