/*
 * SomnoTrace - Shared HTTPS/multipart plumbing for upload backends
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

/* Extracted verbatim from uploader_sleephq.c so import-style backends
 * (SleepHQ, Aerivue, BreathingRoom…) share one TLS/HTTP/multipart
 * implementation. Behaviour is intentionally identical to the code that
 * has been running against sleephq.com for months; the only changes are
 * that host, Accept header, bearer token, request path and hash ordering
 * arrived as parameters instead of file-local constants. */

#include "upload_http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md5.h"

static const char *TAG = "upload_http";

esp_tls_t *uph_connect(const char *host, int timeout_ms)
{
    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
    };

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return NULL;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://%s", host);
    if (esp_tls_conn_http_new_sync(url, &tls_cfg, tls) != 1) {
        ESP_LOGE(TAG, "TLS connect to %s failed", host);
        esp_tls_conn_destroy(tls);
        return NULL;
    }
    ESP_LOGI(TAG, "TLS connected to %s", host);
    return tls;
}

void uph_disconnect(esp_tls_t *tls)
{
    if (tls) esp_tls_conn_destroy(tls);
}

int uph_write_all(esp_tls_t *tls, const void *data, size_t len)
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
 * If body_out is non-NULL, the response body is stored in a PSRAM buffer
 * (caller must free).  If body_out is NULL, the body is drained and
 * discarded.
 *
 * Uses buffered reads (not 1-byte-at-a-time) for compatibility with mbedTLS. */
int uph_read_response(esp_tls_t *tls, size_t buf_cap, size_t body_cap,
                      char **body_out, size_t *body_len, bool *peer_close)
{
    bool dead = false;
#define UPH_FAIL() do { dead = true; goto fail; } while (0)
    if (peer_close) *peer_close = false;

    /* Buffer for entire response (headers + body), allocated strictly in PSRAM */
    char *buf = heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "failed to allocate %u bytes in PSRAM for response buffer", (unsigned)buf_cap);
        return -1;
    }

    size_t buf_len = 0;
    char *header_end = NULL;

    /* Phase 1: read until we find \r\n\r\n (end of headers) */
    while (!header_end) {
        if (buf_len >= buf_cap - 1) {
            ESP_LOGE(TAG, "response headers too large (%u)", (unsigned)buf_len);
            UPH_FAIL();
        }
        ssize_t n = esp_tls_conn_read(tls, buf + buf_len, buf_cap - buf_len - 1);
        if (n < 0) {
            ESP_LOGE(TAG, "TLS read error during headers: %d", (int)n);
            UPH_FAIL();
        }
        if (n == 0) {
            /* Connection closed before headers complete — the peer is done
             * with this socket (keep-alive limit/idle timeout). Report it. */
            ESP_LOGE(TAG, "connection closed during headers (got %u bytes)", (unsigned)buf_len);
            UPH_FAIL();
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
        UPH_FAIL();
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
        if (strncasecmp(line, "Connection:", 11) == 0) {
            if (strcasestr(line + 11, "close")) dead = true;
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
            if (buf_len >= buf_cap - 1) {
                ESP_LOGE(TAG, "response exceeded buffer capacity (%u bytes, content_length=%u)",
                         (unsigned)buf_cap, (unsigned)content_length);
                UPH_FAIL();
            }
            size_t to_read = remaining < (buf_cap - buf_len - 1) ?
                             remaining : (buf_cap - buf_len - 1);
            ssize_t n = esp_tls_conn_read(tls, buf + buf_len, to_read);
            if (n < 0) {
                ESP_LOGE(TAG, "TLS read error during body: %d", (int)n);
                UPH_FAIL();
            }
            if (n == 0) {
                ESP_LOGE(TAG, "connection closed before full body read (%u bytes remaining of %u)",
                         (unsigned)remaining, (unsigned)content_length);
                UPH_FAIL();
            }
            buf_len += n;
            remaining -= n;
        }
    } else if (chunked) {
        /* Read until we see \r\n0\r\n\r\n or connection closes */
        while (1) {
            if (buf_len >= buf_cap - 1) {
                ESP_LOGE(TAG, "chunked response exceeded buffer capacity (%u bytes)", (unsigned)buf_cap);
                UPH_FAIL();
            }
            ssize_t n = esp_tls_conn_read(tls, buf + buf_len, buf_cap - buf_len - 1);
            if (n < 0) { UPH_FAIL(); }
            if (n == 0) break;
            buf_len += n;
            buf[buf_len] = '\0';
            if (strstr(body_start, "\r\n0\r\n\r\n")) break;
        }
    } else if (content_length == 0 && !chunked) {
        /* No Content-Length, no chunked — read until connection closes */
        while (1) {
            if (buf_len >= buf_cap - 1) {
                ESP_LOGW(TAG, "connection-closed response reached buffer capacity (%u bytes)", (unsigned)buf_cap);
                break;
            }
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
        if (body_total > body_cap) {
            ESP_LOGW(TAG, "response body truncated to cap (%u > %u bytes)",
                     (unsigned)body_total, (unsigned)body_cap);
            body_total = body_cap;
        }
        char *body = heap_caps_malloc(body_total + 1, MALLOC_CAP_SPIRAM);
        if (!body) {
            ESP_LOGE(TAG, "failed to allocate %u bytes in PSRAM for response body",
                     (unsigned)(body_total + 1));
            UPH_FAIL();
        }
        memcpy(body, body_start, body_total);
        body[body_total] = '\0';
        *body_out = body;
        if (body_len) *body_len = body_total;
    }

    ESP_LOGI(TAG, "response: HTTP %d (%u bytes body)", status,
             (unsigned)body_total);

    free(buf);
    if (peer_close) *peer_close = dead;
    return status;

fail:
    free(buf);
    if (peer_close) *peer_close = true;
    return -1;
#undef UPH_FAIL
}

/* Send a simple GET or POST request with optional body and read the response.
 * The TLS connection stays open — caller manages it.
 * If body_out is non-NULL, response body is returned (caller frees).
 * Returns the HTTP status, or -1 on transport/protocol failure. Non-2xx
 * statuses are logged but still returned so the backend can classify them. */
int uph_request(esp_tls_t *tls, const char *host, const char *method,
                const char *path, const char *query,
                const char *bearer, const char *accept,
                const char *body, const char *content_type,
                size_t buf_cap, size_t body_cap,
                char **body_out, size_t *body_len, bool *peer_close)
{
    /* Build request line + headers */
    char req[2048];
    int pos = 0;

    if (query) {
        pos += snprintf(req + pos, sizeof(req) - pos, "%s %s?%s HTTP/1.1\r\n", method, path, query);
    } else {
        pos += snprintf(req + pos, sizeof(req) - pos, "%s %s HTTP/1.1\r\n", method, path);
    }
    pos += snprintf(req + pos, sizeof(req) - pos, "Host: %s\r\n", host);
    pos += snprintf(req + pos, sizeof(req) - pos, "Accept: %s\r\n",
                    accept ? accept : "application/json");
    pos += snprintf(req + pos, sizeof(req) - pos, "Connection: keep-alive\r\n");

    if (bearer) {
        pos += snprintf(req + pos, sizeof(req) - pos, "Authorization: Bearer %s\r\n", bearer);
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

    if (uph_write_all(tls, req, pos) < 0) {
        ESP_LOGE(TAG, "failed to send request");
        if (peer_close) *peer_close = true;
        return -1;
    }

    int status = uph_read_response(tls, buf_cap, body_cap, body_out, body_len,
                                   peer_close);
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

/* ── Multipart file upload (streaming, on-the-fly MD5) ────────────────
 *
 * Streams the file directly from SD card to the TLS socket in a single
 * pass.  MD5 is computed on-the-fly as each chunk is read and sent.
 * The content_hash is sent in the multipart footer after the file data.
 * No PSRAM buffering needed — only one chunk buffer is allocated.
 *
 * Multipart field layout (name/path/file/content_hash) is what both
 * SleepHQ and Aerivue accept. */
int uph_put_file(esp_tls_t *tls, const char *host, const char *req_path,
                 const char *bearer, const char *accept,
                 const char *local_path, const char *remote_subpath,
                 const char *filename, uph_md5_mode_t md5_mode,
                 char **resp_body_out, size_t resp_cap, bool *peer_close)
{
    if (resp_body_out) *resp_body_out = NULL;
    if (peer_close) *peer_close = false;

    FILE *f = fopen(local_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "  cannot open %s", local_path);
        return -1;
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
    char req_hdr[1024];
    int hdr_pos = snprintf(req_hdr, sizeof(req_hdr),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Accept: %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %u\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        req_path, host, bearer ? bearer : "",
        accept ? accept : "application/json",
        boundary, (unsigned)total_body_len);

    if (hdr_pos <= 0 || hdr_pos >= (int)sizeof(req_hdr)) {
        ESP_LOGE(TAG, "  request header too long");
        fclose(f);
        return -1;
    }

    /* Send HTTP headers */
    if (uph_write_all(tls, req_hdr, hdr_pos) < 0) {
        ESP_LOGE(TAG, "  failed to send HTTP headers for %s", filename);
        fclose(f);
        if (peer_close) *peer_close = true;
        return -1;
    }

    /* Send multipart preamble parts */
    if (uph_write_all(tls, part1, part1_len) < 0 ||
        uph_write_all(tls, part2, part2_len) < 0 ||
        uph_write_all(tls, part3, part3_len) < 0) {
        ESP_LOGE(TAG, "  failed to send multipart preamble for %s", filename);
        fclose(f);
        if (peer_close) *peer_close = true;
        return -1;
    }

    /* Stream file data with on-the-fly MD5 */
    uint8_t *chunk = heap_caps_malloc(UPH_READ_CHUNK, MALLOC_CAP_SPIRAM);
    if (!chunk) chunk = malloc(UPH_READ_CHUNK);
    if (!chunk) {
        ESP_LOGE(TAG, "  cannot alloc chunk buffer for %s", filename);
        fclose(f);
        return -1;
    }

    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    /* O2-style imports use filename + content; the CPAP contract is
     * content + filename. Aerivue accepts either ordering (or the bare
     * file hash). */
    if (md5_mode == UPH_MD5_NAME_FILE)
        mbedtls_md5_update(&md5, (const unsigned char *)filename, strlen(filename));

    size_t total_sent = 0;
    while (total_sent < file_size) {
        size_t to_read = UPH_READ_CHUNK;
        if (to_read > file_size - total_sent)
            to_read = file_size - total_sent;

        size_t nread = fread(chunk, 1, to_read, f);
        if (nread == 0) {
            ESP_LOGE(TAG, "  short read for %s at offset %u", filename, (unsigned)total_sent);
            free(chunk);
            fclose(f);
            mbedtls_md5_free(&md5);
            /* Request was partially sent — socket is desynced */
            if (peer_close) *peer_close = true;
            return -1;
        }

        /* Feed to MD5 */
        mbedtls_md5_update(&md5, chunk, nread);

        /* Write to TLS socket */
        if (uph_write_all(tls, chunk, nread) < 0) {
            ESP_LOGE(TAG, "  TLS write failed for %s at offset %u", filename, (unsigned)total_sent);
            free(chunk);
            fclose(f);
            mbedtls_md5_free(&md5);
            if (peer_close) *peer_close = true;
            return -1;
        }

        total_sent += nread;

        /* Yield to scheduler */
        if (total_sent % (UPH_READ_CHUNK * 4) == 0)
            taskYIELD();
    }

    free(chunk);
    fclose(f);

    /* Finalize the profile-specific MD5 ordering. */
    if (md5_mode == UPH_MD5_FILE_NAME)
        mbedtls_md5_update(&md5, (const unsigned char *)filename, strlen(filename));
    unsigned char md5_raw[16];
    mbedtls_md5_finish(&md5, md5_raw);
    mbedtls_md5_free(&md5);

    char md5_hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(md5_hex + i * 2, 3, "%02x", md5_raw[i]);
    md5_hex[32] = '\0';

    /* Send footer: header + md5 hex + closing boundary */
    if (uph_write_all(tls, footer_hdr, footer_hdr_len) < 0 ||
        uph_write_all(tls, md5_hex, 32) < 0 ||
        uph_write_all(tls, closing, closing_len) < 0) {
        ESP_LOGE(TAG, "  failed to send multipart footer for %s", filename);
        if (peer_close) *peer_close = true;
        return -1;
    }

    /* Read response — drained unless the caller asked for the body */
    size_t body_cap = resp_cap > 0 ? resp_cap : 4096;
    int status = uph_read_response(tls, 8192 + body_cap, body_cap,
                                   resp_body_out, NULL, peer_close);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "  file upload HTTP %d for %s", status, filename);
        return status;
    }

    ESP_LOGI(TAG, "  uploaded %s (%u bytes, hash=%s)", filename,
             (unsigned)file_size, md5_hex);
    return status;
}
