/*
 * SomnoTrace - HTTP endpoints for canonical oximetry recordings
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3, or (at your option) any later version.
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

#include "oximetry_http.h"
#include "oximetry_canonical.h"
#include "upload_ox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"

#include "esp_log.h"

char *ox_store_conversion_diagnostics_json(void);

static const char *TAG = "ox_http";

static bool query(httpd_req_t *req, const char *key, char *out, size_t out_size)
{
    char buf[512];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) return false;
    return httpd_query_key_value(buf, key, out, out_size) == ESP_OK;
}

static esp_err_t json_send(httpd_req_t *req, char *json)
{
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return e;
}

static esp_err_t oximetry_recordings_handler(httpd_req_t *req)
{
    char date_str[16];
    if (query(req, "date", date_str, sizeof(date_str)) && strlen(date_str) == 8) {
        cJSON *arr = NULL;
        if (oximetry_canonical_list_ready_for_day(date_str, &arr) == ESP_OK && arr) {
            char *s = cJSON_PrintUnformatted(arr);
            cJSON_Delete(arr);
            return json_send(req, s);
        }
        return json_send(req, strdup("[]"));
    }
    return json_send(req, oximetry_canonical_list_json());
}

static esp_err_t oximetry_days_handler(httpd_req_t *req)
{
    (void)req;
    cJSON *arr = NULL;
    if (oximetry_canonical_list_days(&arr) == ESP_OK && arr) {
        char *s = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        return json_send(req, s);
    }
    return json_send(req, strdup("[]"));
}

static esp_err_t oximetry_recording_handler(httpd_req_t *req)
{
    char id[OXIMETRY_CANONICAL_MAX_COMPONENT];
    if (!query(req, "id", id, sizeof(id))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
        return ESP_FAIL;
    }
    return json_send(req, oximetry_canonical_manifest_json(id));
}

static esp_err_t oximetry_uploads_handler(httpd_req_t *req)
{
    (void)req;
    return json_send(req, upload_ox_status_json());
}

static esp_err_t oximetry_diagnostics_handler(httpd_req_t *req)
{
    (void)req;
    return json_send(req, ox_store_conversion_diagnostics_json());
}

static esp_err_t oximetry_file_handler(httpd_req_t *req)
{
    char id[OXIMETRY_CANONICAL_MAX_COMPONENT];
    char track[24];
    if (!query(req, "id", id, sizeof(id)) || !query(req, "track", track, sizeof(track))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id or track");
        return ESP_FAIL;
    }
    char path[OXIMETRY_CANONICAL_MAX_PATH];
    if (oximetry_canonical_resolve_track(id, track, path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "track not found");
        return ESP_FAIL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "track not found");
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, strcmp(track, "events") == 0 ? "application/jsonl" :
                        "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    long start = 0, end = size - 1;
    char range[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK &&
        strncmp(range, "bytes=", 6) == 0) {
        char *dash = strchr(range + 6, '-');
        if (dash) {
            *dash = '\0';
            start = strtol(range + 6, NULL, 10);
            if (*(dash + 1)) end = strtol(dash + 1, NULL, 10);
            if (start < 0) start = 0;
            if (end >= size) end = size - 1;
            if (start > end || start >= size) {
                fclose(f); httpd_resp_set_status(req, "416 Range Not Satisfiable");
                return httpd_resp_send(req, NULL, 0);
            }
            char content_range[64];
            snprintf(content_range, sizeof(content_range), "bytes %ld-%ld/%ld", start, end, size);
            httpd_resp_set_hdr(req, "Content-Range", content_range);
            httpd_resp_set_status(req, "206 Partial Content");
        }
    }
    if (req->method == HTTP_HEAD) {
        char len[24]; snprintf(len, sizeof(len), "%ld", end - start + 1);
        httpd_resp_set_hdr(req, "Content-Length", len);
        fclose(f); return httpd_resp_send(req, NULL, 0);
    }
    if (fseek(f, start, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }
    long remaining = end - start + 1;
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(4096);
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_err_t e = ESP_OK;
    while (remaining > 0) {
        size_t want = remaining > 4096 ? 4096 : (size_t)remaining;
        size_t n = fread(buf, 1, want, f);
        if (n == 0) break;
        remaining -= (long)n;
        e = httpd_resp_send_chunk(req, (const char *)buf, n);
        if (e != ESP_OK) break;
    }
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    fclose(f);
    if (e != ESP_OK) ESP_LOGW(TAG, "track send failed: %s", esp_err_to_name(e));
    return e;
}

void oximetry_http_register_handlers(httpd_handle_t server)
{
    httpd_uri_t list = {
        .uri = "/api/oximetry/recordings", .method = HTTP_GET,
        .handler = oximetry_recordings_handler,
    };
    httpd_uri_t recording = {
        .uri = "/api/oximetry/recording", .method = HTTP_GET,
        .handler = oximetry_recording_handler,
    };
    httpd_uri_t uploads = {
        .uri = "/api/oximetry/uploads", .method = HTTP_GET,
        .handler = oximetry_uploads_handler,
    };
    httpd_uri_t diagnostics = {
        .uri = "/api/oximetry/diagnostics", .method = HTTP_GET,
        .handler = oximetry_diagnostics_handler,
    };
    httpd_uri_t file = {
        .uri = "/api/oximetry/file", .method = HTTP_GET,
        .handler = oximetry_file_handler,
    };
    httpd_uri_t file_head = {
        .uri = "/api/oximetry/file", .method = HTTP_HEAD,
        .handler = oximetry_file_handler,
    };
    httpd_uri_t days = {
        .uri = "/api/oximetry/days", .method = HTTP_GET,
        .handler = oximetry_days_handler,
    };
    httpd_register_uri_handler(server, &list);
    httpd_register_uri_handler(server, &days);
    httpd_register_uri_handler(server, &recording);
    httpd_register_uri_handler(server, &uploads);
    httpd_register_uri_handler(server, &diagnostics);
    httpd_register_uri_handler(server, &file);
    httpd_register_uri_handler(server, &file_head);
}
