/*
 * SomnoTrace - Read-only SDCARD HTTP export API
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

#include "sdcard_api.h"
#include "sdcard_api_core.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "http_query.h"
#include "httpd_defer.h"
#include "sd_storage.h"
#include "session_graph.h"
#include "web_auth.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "sdcard_api";

#define SDCARD_CHUNK_CAP   2048
#define SDCARD_INLINE_MAX  2048
#define SDCARD_LEASE_WAIT  2000

static esp_err_t send_json_err(httpd_req_t *req, const char *status, const char *code)
{
    char body[96];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

static int read_qparam(httpd_req_t *req, const char *key, char *out, size_t out_cap)
{
    char raw[512];
    if (!http_query_read_encoded(req, key, raw, sizeof(raw))) {
        return 0; /* absent or too long */
    }
    return http_query_decode(out, raw, (int)out_cap) > 0 ? 1 : -1;
}

typedef struct {
    httpd_req_t *req;
    char        *buf;
    size_t       pos;
    size_t       cap;
    char         esc[SDCARD_API_MAX_RELPATH * 2];
    bool         first;
} files_stream_ctx_t;

static esp_err_t flush_chunk(files_stream_ctx_t *s)
{
    if (s->pos == 0) return ESP_OK;
    esp_err_t err = httpd_resp_send_chunk(s->req, s->buf, (ssize_t)s->pos);
    s->pos = 0;
    return err;
}

static esp_err_t append_bytes(files_stream_ctx_t *s, const char *data, size_t len)
{
    if (len == 0) return ESP_OK;
    if (s->pos + len > s->cap) {
        esp_err_t err = flush_chunk(s);
        if (err != ESP_OK) return err;
    }
    if (len > s->cap) {
        return httpd_resp_send_chunk(s->req, data, (ssize_t)len);
    }
    memcpy(s->buf + s->pos, data, len);
    s->pos += len;
    return ESP_OK;
}

static int stream_file_cb(const char *relpath, uint64_t size, int64_t mtime, void *arg)
{
    files_stream_ctx_t *s = (files_stream_ctx_t *)arg;
    int elen = sdcard_api_json_escape(relpath, s->esc, sizeof(s->esc));
    if (elen < 0) return -ENAMETOOLONG;

    char entry[SDCARD_API_MAX_RELPATH * 2 + 80];
    int n = snprintf(entry, sizeof(entry),
                     "%s{\"path\":\"%s\",\"size\":%llu,\"mtime\":%lld}",
                     s->first ? "" : ",",
                     s->esc,
                     (unsigned long long)size,
                     (long long)mtime);
    if (n < 0 || (size_t)n >= sizeof(entry)) return -ENAMETOOLONG;
    s->first = false;

    return append_bytes(s, entry, (size_t)n) == ESP_OK ? 0 : -EIO;
}

static esp_err_t sdcard_files_impl(httpd_req_t *req)
{
    if (!sd_storage_is_ready()) {
        return send_json_err(req, "503 Service Unavailable", "sd_not_ready");
    }

    char sub_dir[SDCARD_API_MAX_RELPATH] = {0};
    char raw_dir[512];
    if (http_query_read_encoded(req, "dir", raw_dir, sizeof(raw_dir))) {
        if (http_query_decode(sub_dir, raw_dir, (int)sizeof(sub_dir)) <= 0 ||
            !sdcard_api_relpath_valid(sub_dir)) {
            return send_json_err(req, "400 Bad Request", "bad_path");
        }
    }

    if (!sd_storage_lease_acquire(SD_LEASE_UPLOAD, SDCARD_LEASE_WAIT)) {
        return send_json_err(req, "503 Service Unavailable", "sd_busy");
    }

    char check_path[256];
    if (sub_dir[0] != '\0') {
        if (!sdcard_api_build_path(SD_SDCARD_DIR, sub_dir, check_path, sizeof(check_path))) {
            sd_storage_lease_release(SD_LEASE_UPLOAD);
            return send_json_err(req, "400 Bad Request", "bad_path");
        }
    } else {
        strlcpy(check_path, SD_SDCARD_DIR, sizeof(check_path));
    }

    struct stat st;
    if (stat(check_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        if (sub_dir[0] == '\0') {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_sendstr(req, "[]");
        }
        return send_json_err(req, "404 Not Found", "not_found");
    }

    files_stream_ctx_t *ctx = heap_caps_calloc(1, sizeof(*ctx), MALLOC_CAP_SPIRAM);
    if (!ctx) ctx = calloc(1, sizeof(*ctx));
    char *chunk = heap_caps_malloc(SDCARD_CHUNK_CAP, MALLOC_CAP_SPIRAM);
    if (!chunk) chunk = malloc(SDCARD_CHUNK_CAP);
    if (!ctx || !chunk) {
        free(ctx);
        free(chunk);
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ctx->req = req;
    ctx->buf = chunk;
    ctx->cap = SDCARD_CHUNK_CAP;
    ctx->first = true;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t ret = ESP_OK;
    if (append_bytes(ctx, "[", 1) != ESP_OK) {
        ret = ESP_FAIL;
    } else {
        int wrc = sdcard_api_walk(SD_SDCARD_DIR, sub_dir[0] ? sub_dir : NULL, stream_file_cb, ctx);
        if (wrc != 0) {
            ESP_LOGW(TAG, "sdcard_api_walk failed: %d", wrc);
            ret = ESP_FAIL;
        } else if (append_bytes(ctx, "]", 1) != ESP_OK ||
                   flush_chunk(ctx) != ESP_OK ||
                   httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
            ret = ESP_FAIL;
        }
    }

    free(chunk);
    free(ctx);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return ret;
}

static esp_err_t sdcard_files_handler(httpd_req_t *req)
{
    return httpd_defer_dispatch(req, sdcard_files_impl);
}

static esp_err_t sdcard_file_handler(httpd_req_t *req)
{
    if (!sd_storage_is_ready()) {
        return send_json_err(req, "503 Service Unavailable", "sd_not_ready");
    }

    char relpath[SDCARD_API_MAX_RELPATH];
    if (read_qparam(req, "path", relpath, sizeof(relpath)) <= 0 ||
        !sdcard_api_relpath_valid(relpath)) {
        return send_json_err(req, "400 Bad Request", "bad_path");
    }

    char full_path[256];
    if (!sdcard_api_build_path(SD_SDCARD_DIR, relpath, full_path, sizeof(full_path))) {
        return send_json_err(req, "400 Bad Request", "bad_path");
    }

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_json_err(req, "404 Not Found", "not_found");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (req->method == HTTP_HEAD) {
        char cl[24];
        snprintf(cl, sizeof(cl), "%ld", (long)st.st_size);
        httpd_resp_set_hdr(req, "Content-Length", cl);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    if (st.st_size <= SDCARD_INLINE_MAX) {
        if (st.st_size == 0) {
            httpd_resp_set_hdr(req, "Content-Length", "0");
            return httpd_resp_send(req, "", 0);
        }
        char *buf = malloc((size_t)st.st_size);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        FILE *f = fopen(full_path, "rb");
        if (!f) {
            free(buf);
            return send_json_err(req, "404 Not Found", "not_found");
        }
        size_t got = fread(buf, 1, (size_t)st.st_size, f);
        fclose(f);
        char cl[24];
        snprintf(cl, sizeof(cl), "%ld", (long)got);
        httpd_resp_set_hdr(req, "Content-Length", cl);
        esp_err_t serr = httpd_resp_send(req, buf, (ssize_t)got);
        free(buf);
        return serr;
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    return session_graph_enqueue_file(req, full_path, 0, (long)st.st_size);
}

void sdcard_api_register_handlers(httpd_handle_t server)
{
    httpd_uri_t files = {
        .uri = "/api/sdcard/files", .method = HTTP_GET,
        .handler = sdcard_files_handler,
    };
    httpd_uri_t file = {
        .uri = "/api/sdcard/file", .method = HTTP_GET,
        .handler = sdcard_file_handler,
    };
    httpd_uri_t file_head = {
        .uri = "/api/sdcard/file", .method = HTTP_HEAD,
        .handler = sdcard_file_handler,
    };
    web_auth_register(server, &files);
    web_auth_register(server, &file);
    web_auth_register(server, &file_head);
}
