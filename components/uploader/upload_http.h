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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_tls.h"
#include "uploader.h"   /* upload_result_t */

/* ── Shared minimal HTTPS client for upload backends ──────────────────
 *
 * Extracted verbatim from uploader_sleephq.c so that import-style backends
 * (SleepHQ, Aerivue, BreathingRoom…) share one TLS/HTTP/multipart
 * implementation instead of each carrying a private copy.
 *
 * Conventions preserved from the SleepHQ original:
 *  - raw esp_tls with the cert bundle, one keep-alive connection per pass
 *  - response buffer lives strictly in PSRAM (hdr_cap + resp_cap)
 *  - chunked transfer-encoding is de-chunked before the body is returned
 *  - file bodies stream SD → TLS in 1 KiB chunks, never buffered whole
 *
 * Backends own their own URLs, auth tokens and request paths; this layer
 * knows nothing about any service's API shape. */

/* Response sizing: headers can be several KB behind a CDN (CSP, Report-To),
 * bodies are usually small JSON. Callers pass their own caps. */
#define UPH_READ_CHUNK   1024

/* Content-hash mode for the trailing content_hash multipart field.
 * Aerivue accepts all three; SleepHQ requires FILE_NAME for CPAP files and
 * NAME_FILE for O2. */
typedef enum {
    UPH_MD5_FILE_ONLY = 0,   /* md5(file bytes)                              */
    UPH_MD5_FILE_NAME,       /* md5(file bytes + filename)                   */
    UPH_MD5_NAME_FILE,       /* md5(filename + file bytes)                   */
} uph_md5_mode_t;

/* Open a keep-alive TLS connection to host:443. NULL on failure. */
esp_tls_t *uph_connect(const char *host, int timeout_ms);

/* Close and free a connection from uph_connect(). Tolerates NULL. */
void uph_disconnect(esp_tls_t *tls);

/* Write all of `len` bytes, retrying short writes. -1 on error. */
int uph_write_all(esp_tls_t *tls, const void *data, size_t len);

/* Read a complete HTTP response.
 * buf_cap:  total scratch buffer (headers + body), allocated in PSRAM
 * body_cap: cap on the extracted body when body_out is non-NULL
 * Returns HTTP status (>=100) or -1 on transport/protocol error.
 * If body_out is non-NULL the de-chunked body is returned in a PSRAM buffer
 * the caller must free(); NULL drains and discards.
 *
 * peer_close (may be NULL) is set true when the connection is no longer
 * usable for another request: the server sent "Connection: close", the
 * peer closed/reset the socket, or a protocol error desynced the stream.
 * Callers that pool a keep-alive connection MUST reconnect before their
 * next request when this fires — servers routinely cap requests per
 * connection (nginx keepalive_requests / LB lifetime limits), which is
 * exactly what kills a long upload run mid-day otherwise. */
int uph_read_response(esp_tls_t *tls, size_t buf_cap, size_t body_cap,
                      char **body_out, size_t *body_len, bool *peer_close);

/* Send a simple GET/POST with optional string body; read the response.
 * bearer/accept/body/content_type may be NULL. Connection stays open
 * unless the server says otherwise (peer_close — see above).
 * Returns HTTP status or -1. */
int uph_request(esp_tls_t *tls, const char *host, const char *method,
                const char *path, const char *query,
                const char *bearer, const char *accept,
                const char *body, const char *content_type,
                size_t buf_cap, size_t body_cap,
                char **body_out, size_t *body_len, bool *peer_close);

/* Stream one local file to `req_path` as a multipart/form-data request with
 * the field layout both SleepHQ and Aerivue accept:
 *
 *   name=filename  path=remote_subpath  file=<file bytes>  content_hash=<md5>
 *
 * Streams the file in UPH_READ_CHUNK pieces, computing the MD5 on the fly
 * (md5_mode chooses the ordering). Returns the HTTP status or -1.
 * resp_body_out (may be NULL) captures the response body up to resp_cap —
 * Aerivue reports a per-file `stored` flag worth inspecting.
 *
 * BreathingRoom will need a multi-file variant (repeated `files` parts,
 * basenames only) — add uph_put_files() alongside this rather than making
 * this signature serve both. */
int uph_put_file(esp_tls_t *tls, const char *host, const char *req_path,
                 const char *bearer, const char *accept,
                 const char *local_path, const char *remote_subpath,
                 const char *filename, uph_md5_mode_t md5_mode,
                 char **resp_body_out, size_t resp_cap, bool *peer_close);
