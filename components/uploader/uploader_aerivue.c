/*
 * SomnoTrace - Aerivue upload backend (shared upload_http plumbing)
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

/* Aerivue upload API (https://aerivue.app/docs/upload-api), verified live
 * 2026-10-09 with a real upload key and a real CPAP day + O2 ring recording:
 *
 *   POST /api/v1/imports                    -> 201 {"id": "<uuid>"}
 *   POST /api/v1/imports/{id}/files         -> multipart name/path/file/
 *                                              content_hash; one file per
 *                                              call; 201 {path,bytes,stored}
 *   POST /api/v1/imports/{id}/process       -> 202, async parse
 *   GET  /api/v1/imports/{id}               -> status receiving|importing|
 *                                              done|failed + result.cpap
 *                                              + result.oximetry[]
 *   GET  /api/v1/me                         -> {key:{label},account:{email}}
 *
 * Auth is a static "avu_..." bearer key scoped to upload + own imports +
 * /me.  Imports stay open for 48 h; sessions may be split across imports
 * and re-sending a file is a server-side no-op (sessionsSkipped), which is
 * what makes our transient-retry semantics safe without extra tracking.
 *
 * Layout mirrors the card: root files (STR.edf, Identification.json/.crc)
 * with path "", files under the SETTINGS directory with "/SETTINGS", sessions
 * with "/DATALOG/<day>", ring recordings "/OXYMETRY/<day>" named by the ring's
 * own timestamp (ref->source_name already carries it). */

#include "uploader.h"
#include "upload_paths.h"
#include "upload_http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "upload_aer";

#define AER_HOST         "aerivue.app"
#define AER_ME_PATH      "/api/v1/me"
#define AER_IMPORTS_PATH "/api/v1/imports"
#define AER_IMPORT_FMT   "/api/v1/imports/%s"
#define AER_FILES_FMT    "/api/v1/imports/%s/files"
#define AER_PROCESS_FMT  "/api/v1/imports/%s/process"

#define AER_TIMEOUT_MS   30000
#define AER_TEST_TIMEOUT_MS 10000
#define AER_HDR_CAP      8192
#define AER_RESP_CAP     32768

/* ── Backend state ────────────────────────────────────────────────────
 * One TLS connection spans the run; an import spans one day. Same shape
 * as the SleepHQ backend — see its interface note in uploader_sleephq.c. */
static esp_tls_t *s_tls;
static char s_import_id[48];        /* uuid, 36 chars */
static int  s_day_files;            /* files sent in the current import */
static char s_key[64];              /* upload key, loaded at session_begin */
/* Set when the peer told us (or showed us) it is done with this socket —
 * server-side keep-alive request/lifetime limits close long upload runs
 * mid-day. The next send goes through aer_ensure_conn() and gets a fresh
 * connection instead of dying on the dead socket. */
static bool s_peer_close;

/* HTTP status → scheduler semantics. Aerivue's non-2xx codes:
 *   401/403  bad or revoked key, unverified email — permanent, surface it
 *   400      malformed request, EXCEPT content_hash_mismatch which is a
 *            "resend the file" answer → transient
 *   404      the import closed or expired (48 h TTL) — transient; the next
 *            pass opens a fresh import and server-side dedup absorbs the
 *            resend
 *   413      account storage quota — permanent, surface it
 *   429      >5000 calls/h/key — transient, cooldown ladder handles it
 *   5xx      transient
 *   -1       transport failure — transient */
static upload_result_t aer_map_status(int status, const char *body)
{
    if (status >= 200 && status < 300) return UPLOAD_OK;
    if (status == 401 || status == 403) return UPLOAD_ERR_PERMANENT;
    if (status == 404) return UPLOAD_ERR_TRANSIENT;
    if (status == 413) return UPLOAD_ERR_PERMANENT;
    if (status == 400) {
        if (body && strstr(body, "content_hash_mismatch"))
            return UPLOAD_ERR_TRANSIENT;
        ESP_LOGW(TAG, "HTTP 400: %s", body ? body : "(no body)");
        return UPLOAD_ERR_PERMANENT;
    }
    return UPLOAD_ERR_TRANSIENT;
}

static int aer_request(esp_tls_t *tls, const char *method,
                       const char *path, const char *bearer,
                       bool *peer_close,
                       char **body_out, size_t *body_len)
{
    return uph_request(tls, AER_HOST, method, path, NULL, bearer,
                       "application/json", NULL, NULL,
                       AER_HDR_CAP + AER_RESP_CAP, AER_RESP_CAP,
                       body_out, body_len, peer_close);
}

/* GET /api/v1/me with the configured key. Validates the credential and
 * logs which account it belongs to — converting a revoked key into an
 * immediate permanent failure at session open instead of N failed file
 * calls. Returns a classified upload_result_t. */
static upload_result_t aer_check_key(esp_tls_t *tls, const char *key,
                                     char *msg, size_t msg_len)
{
    char *body = NULL;
    size_t blen = 0;
    int status = aer_request(tls, "GET", AER_ME_PATH, key, NULL, &body, &blen);
    if (status < 200 || status >= 300) {
        free(body);
        if (status == 401 || status == 403) {
            if (msg) snprintf(msg, msg_len,
                              "Aerivue rejected the upload key (HTTP %d)", status);
            return UPLOAD_ERR_PERMANENT;
        }
        if (msg) snprintf(msg, msg_len, "No answer from %s (HTTP %d)",
                          AER_HOST, status);
        return UPLOAD_ERR_TRANSIENT;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        if (msg) snprintf(msg, msg_len, "Unexpected reply from %s", AER_HOST);
        return UPLOAD_ERR_TRANSIENT;
    }

    /* Copy out of the cJSON tree BEFORE deleting it — valuestring pointers
     * dangle after cJSON_Delete (this was a real UAF: the UI showed a
     * garbled key label and email until the strings were copied first). */
    char label[48] = "";
    char email[80] = "";
    cJSON *jkey = cJSON_GetObjectItem(root, "key");
    cJSON *jl = jkey ? cJSON_GetObjectItem(jkey, "label") : NULL;
    if (cJSON_IsString(jl)) snprintf(label, sizeof(label), "%s", jl->valuestring);
    cJSON *acct = cJSON_GetObjectItem(root, "account");
    cJSON *je = acct ? cJSON_GetObjectItem(acct, "email") : NULL;
    if (cJSON_IsString(je)) snprintf(email, sizeof(email), "%s", je->valuestring);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "upload key accepted: label=\"%s\" account=%s", label, email);
    if (msg) {
        if (label[0] || email[0])
            snprintf(msg, msg_len, "Upload key '%s' accepted — account %s",
                     label[0] ? label : "(unlabeled)",
                     email[0] ? email : "(unknown)");
        else
            snprintf(msg, msg_len, "Upload key accepted");
    }
    return UPLOAD_OK;
}

static bool aer_is_configured(void)
{
    return uploader_is_aerivue_configured();
}

static upload_result_t aer_session_begin(void)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);
    if (!cfg.aer_key[0])
        return UPLOAD_NOT_CONFIGURED;

    s_tls = uph_connect(AER_HOST, AER_TIMEOUT_MS);
    if (!s_tls) return UPLOAD_ERR_TRANSIENT;

    upload_result_t r = aer_check_key(s_tls, cfg.aer_key, NULL, 0);
    if (r != UPLOAD_OK) {
        uph_disconnect(s_tls);
        s_tls = NULL;
        return r;
    }
    snprintf(s_key, sizeof(s_key), "%s", cfg.aer_key);
    s_peer_close = false;
    return UPLOAD_OK;
}

static void aer_session_end(void)
{
    if (!s_tls) return;
    uph_disconnect(s_tls);
    s_tls = NULL;
    s_peer_close = false;
    /* s_import_id intentionally survives a reconnect: the import is
     * server-side state with a 48 h TTL, and a day that lost its socket
     * mid-flight should continue filing into the same import rather than
     * orphaning it. day_begin/day_end own its lifecycle. */
}

/* Reconnect when the socket is dead or the peer asked us to go away.
 * Cheap: one TLS handshake plus a key re-check per reconnect, and only on
 * a path that would otherwise have failed. */
static bool aer_ensure_conn(void)
{
    if (s_tls && !s_peer_close) return true;
    ESP_LOGI(TAG, "reconnecting (peer closed or socket dead)");
    aer_session_end();
    return aer_session_begin() == UPLOAD_OK;
}

/* aer_request on the session connection, with reconnect-and-retry: a
 * transport -1 means the request may never have reached the server, and a
 * "Connection: close" flag means the NEXT one would not, so both end up
 * on a fresh socket. Requests are idempotent (server dedups files by
 * content_hash; creating a duplicate import is absorbed by sessionsSkipped),
 * which is what makes resend safe here. */
static int aer_req(const char *method, const char *path,
                   char **body_out, size_t *body_len)
{
    if (!aer_ensure_conn()) return -1;
    bool pc = false;
    int status = aer_request(s_tls, method, path, s_key, &pc,
                             body_out, body_len);
    if (pc) s_peer_close = true;
    if (status < 0) {
        s_peer_close = true;
        if (!aer_ensure_conn()) return -1;
        pc = false;
        status = aer_request(s_tls, method, path, s_key, &pc,
                             body_out, body_len);
        if (pc) s_peer_close = true;
    }
    return status;
}

/* POST /api/v1/imports -> parse the new import's uuid into s_import_id. */
static upload_result_t aer_open_import(void)
{
    s_import_id[0] = '\0';
    char *body = NULL;
    size_t blen = 0;
    int status = aer_req("POST", AER_IMPORTS_PATH, &body, &blen);
    if (status >= 200 && status < 300 && body) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            cJSON *id = cJSON_GetObjectItem(root, "id");
            if (!id) {
                cJSON *imp = cJSON_GetObjectItem(root, "import");
                if (imp) id = cJSON_GetObjectItem(imp, "id");
            }
            if (cJSON_IsString(id))
                snprintf(s_import_id, sizeof(s_import_id), "%s", id->valuestring);
            cJSON_Delete(root);
        }
    }
    free(body);
    if (s_import_id[0]) return UPLOAD_OK;
    return aer_map_status(status, NULL);
}

static upload_result_t aer_day_begin(const char *day)
{
    if (!s_tls) {
        if (aer_session_begin() != UPLOAD_OK)
            return UPLOAD_ERR_TRANSIENT;
    }
    s_import_id[0] = '\0';
    s_day_files = 0;

    upload_result_t r = aer_open_import();
    if (r != UPLOAD_OK) {
        /* TLS connection may have been closed by the remote server. Try
         * reconnecting once before giving the day a transient failure. */
        ESP_LOGW(TAG, "import creation failed for day %s, reconnecting...", day);
        aer_session_end();
        if (aer_session_begin() != UPLOAD_OK ||
            aer_open_import() != UPLOAD_OK) {
            ESP_LOGE(TAG, "import creation failed for day %s", day);
            return UPLOAD_ERR_TRANSIENT;
        }
    }
    ESP_LOGI(TAG, "day %s -> import %s", day, s_import_id);
    return UPLOAD_OK;
}

/* One file POST to the open import; classifies the status and, when the
 * body is captured, notes a `stored:false` (file type the server chose to
 * ignore — still a success from our side). */
static upload_result_t aer_send_file(const char *local_path,
                                     const char *remote_subpath,
                                     const char *filename,
                                     uph_md5_mode_t md5_mode)
{
    char path[256];
    snprintf(path, sizeof(path), AER_FILES_FMT, s_import_id);

    if (!aer_ensure_conn()) return UPLOAD_ERR_TRANSIENT;

    char *body = NULL;
    bool pc = false;
    int status = uph_put_file(s_tls, AER_HOST, path, s_key,
                              "application/json",
                              local_path, remote_subpath, filename,
                              md5_mode, &body, 4096, &pc);
    if (pc) s_peer_close = true;
    if (status < 0) {
        /* Dead socket: the request may never have reached the server, and
         * a file resend is a server-side no-op (content_hash dedup) —
         * reconnect and try once more on a fresh connection. */
        free(body);
        body = NULL;
        s_peer_close = true;
        if (!aer_ensure_conn()) return UPLOAD_ERR_TRANSIENT;
        pc = false;
        status = uph_put_file(s_tls, AER_HOST, path, s_key,
                              "application/json",
                              local_path, remote_subpath, filename,
                              md5_mode, &body, 4096, &pc);
        if (pc) s_peer_close = true;
    }
    upload_result_t r = aer_map_status(status, body);
    if (r == UPLOAD_OK && body && strstr(body, "\"stored\":false"))
        ESP_LOGI(TAG, "  %s accepted but not stored (unrecognised type)", filename);
    free(body);
    return r;
}

static upload_result_t aer_put_group(const char *day, const upload_group_ref_t *g)
{
    if (!s_tls || !s_import_id[0] || !g) return UPLOAD_ERR_TRANSIENT;

    char remote_subpath[64];
    snprintf(remote_subpath, sizeof(remote_subpath), "/DATALOG/%s", day);

    for (int i = 0; i < g->n_files; i++) {
        char local[512];
        snprintf(local, sizeof(local), "%s/%s/%s", SD_SDCARD_DATALOG, day,
                 g->files[i]);
        upload_result_t r = aer_send_file(local, remote_subpath,
                                          g->files[i], UPH_MD5_FILE_NAME);
        if (r != UPLOAD_OK) {
            ESP_LOGW(TAG, "  failed to upload %s", g->files[i]);
            return r;
        }
        s_day_files++;
    }
    ESP_LOGI(TAG, "  group %s: %d file(s)", g->prefix, g->n_files);
    return UPLOAD_OK;
}

static upload_result_t aer_put_bundle(const char *day,
                                      const upload_bundle_ref_t *b, bool changed)
{
    (void)day;
    (void)changed;   /* root files belong in every import — see uploader.h */
    if (!s_tls || !s_import_id[0] || !b) return UPLOAD_ERR_TRANSIENT;

    for (int i = 0; i < b->n_files; i++) {
        const char *subpath = b->in_settings[i] ? "/SETTINGS" : "";
        upload_result_t r = aer_send_file(b->paths[i], subpath,
                                          b->names[i], UPH_MD5_FILE_NAME);
        if (r != UPLOAD_OK) {
            ESP_LOGW(TAG, "  failed to upload %s", b->names[i]);
            return r;
        }
        s_day_files++;
    }
    ESP_LOGI(TAG, "  root bundle: %d file(s)", b->n_files);
    return UPLOAD_OK;
}

/* Oximetry days ride the same import lifecycle: one import per day, the
 * recording named by the ring's own timestamp (its start second), which is
 * what the server keys records on. */
static upload_result_t aer_put_oximetry(const upload_ox_ref_t *ref)
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
        upload_result_t r = aer_send_file(ref->local_paths[i], subpath,
                                          filename, UPH_MD5_NAME_FILE);
        if (r != UPLOAD_OK) return r;
        s_day_files++;
        return UPLOAD_OK;
    }
    return UPLOAD_ERR_PERMANENT;
}

static upload_result_t aer_day_end(const char *day, bool any_uploaded)
{
    if (!s_tls || !s_import_id[0]) return UPLOAD_ERR_TRANSIENT;

    /* An import with no files is an empty record — leave it unprocessed.
     * It expires on its own (48 h TTL) server-side. */
    if (!any_uploaded && s_day_files == 0) {
        ESP_LOGI(TAG, "day %s: nothing sent, import left unprocessed", day);
        s_import_id[0] = '\0';
        return UPLOAD_OK;
    }

    char path[256];
    snprintf(path, sizeof(path), AER_PROCESS_FMT, s_import_id);

    int status = aer_req("POST", path, NULL, NULL);
    upload_result_t r = aer_map_status(status, NULL);
    if (r != UPLOAD_OK) {
        ESP_LOGW(TAG, "process import %s failed (HTTP %d)", s_import_id, status);
        s_import_id[0] = '\0';
        return r;
    }

    /* Poll the import until done/failed (~60 s budget, same as SleepHQ). */
    snprintf(path, sizeof(path), AER_IMPORT_FMT, s_import_id);
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < 30; attempt++) {
        char *body = NULL;
        size_t blen = 0;
        status = aer_req("GET", path, &body, &blen);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "import %s: status query HTTP %d (attempt %d/30)",
                     s_import_id, status, attempt + 1);
            free(body);
            if (aer_map_status(status, NULL) == UPLOAD_ERR_PERMANENT) {
                err = ESP_FAIL;
                break;
            }
            if (attempt + 1 >= 30) break;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) {
            if (attempt + 1 >= 30) break;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        cJSON *st = cJSON_GetObjectItem(root, "status");
        const char *name = cJSON_IsString(st) ? st->valuestring : "";
        bool done = strcmp(name, "done") == 0;
        bool failed = strcmp(name, "failed") == 0;
        if (done) {
            cJSON *res = cJSON_GetObjectItem(root, "result");
            cJSON *cpap = res ? cJSON_GetObjectItem(res, "cpap") : NULL;
            if (cpap) {
                cJSON *si = cJSON_GetObjectItem(cpap, "sessionsImported");
                cJSON *ss = cJSON_GetObjectItem(cpap, "sessionsSkipped");
                cJSON *ni = cJSON_GetObjectItem(cpap, "nightsImported");
                ESP_LOGI(TAG, "import %s done: sessions +%d skipped %d, nights +%d",
                         s_import_id,
                         cJSON_IsNumber(si) ? si->valueint : -1,
                         cJSON_IsNumber(ss) ? ss->valueint : -1,
                         cJSON_IsNumber(ni) ? ni->valueint : -1);
            }
            cJSON *ox = res ? cJSON_GetObjectItem(res, "oximetry") : NULL;
            if (cJSON_IsArray(ox)) {
                cJSON *rec;
                cJSON_ArrayForEach(rec, ox) {
                    cJSON *rn = cJSON_GetObjectItem(rec, "name");
                    cJSON *rs = cJSON_GetObjectItem(rec, "status");
                    cJSON *rr = cJSON_GetObjectItem(rec, "readings");
                    ESP_LOGI(TAG, "  oximetry %s: %s (%d readings)",
                             cJSON_IsString(rn) ? rn->valuestring : "?",
                             cJSON_IsString(rs) ? rs->valuestring : "?",
                             cJSON_IsNumber(rr) ? rr->valueint : 0);
                }
            }
            err = ESP_OK;
        } else if (failed) {
            ESP_LOGW(TAG, "import %s: remote status \"failed\"", s_import_id);
            err = ESP_FAIL;
        }
        cJSON_Delete(root);
        if (err != ESP_ERR_TIMEOUT) break;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    s_import_id[0] = '\0';
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "import processing failed for day %s, resetting TLS connection", day);
        aer_session_end();
        return UPLOAD_ERR_TRANSIENT;
    }
    ESP_LOGI(TAG, "day %s uploaded (%d file(s))", day, s_day_files);
    return UPLOAD_OK;
}

/* ── "Test connection" (web UI) ───────────────────────────────────────
 * TLS connect plus one GET /api/v1/me with the form's key, on a private
 * connection so it can run from the httpd task while the scheduler is
 * idle. No import is opened — nothing appears in the user's history. */
static bool aer_test(const uploader_config_t *cfgp, char *msg, size_t msg_len)
{
    uploader_config_t cfg = *cfgp;   /* by value — never NVS */
    if (!cfg.aer_key[0]) {
        snprintf(msg, msg_len, "Upload key is required");
        return false;
    }
    if (strncmp(cfg.aer_key, "avu_", 4) != 0) {
        snprintf(msg, msg_len, "Upload keys start with \"avu_\" — check you pasted the whole key");
        return false;
    }

    esp_tls_t *tls = uph_connect(AER_HOST, AER_TEST_TIMEOUT_MS);
    if (!tls) {
        snprintf(msg, msg_len, "Cannot reach %s: check the internet connection", AER_HOST);
        return false;
    }

    upload_result_t r = aer_check_key(tls, cfg.aer_key, msg, msg_len);
    uph_disconnect(tls);

    ESP_LOGI(TAG, "aerivue: connection test finished with %u bytes of stack headroom",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    if (r == UPLOAD_OK) return true;
    if (msg[0] == '\0')
        snprintf(msg, msg_len, "Key check failed — see device log");
    return false;
}

const upload_backend_t aerivue_backend = {
    .id = "aerivue",
    .label = "Aerivue",
    .bundle_only_ok = false,    /* an import with no sessions is clutter */
    .supports_ox = true,
    .is_configured = aer_is_configured,
    .session_begin = aer_session_begin,
    .day_begin = aer_day_begin,
    .put_group = aer_put_group,
    .ox_day_begin = aer_day_begin,
    .put_oximetry = aer_put_oximetry,
    .ox_day_end = aer_day_end,
    .put_bundle = aer_put_bundle,
    .day_end = aer_day_end,
    .session_end = aer_session_end,
    .test = aer_test,
};
