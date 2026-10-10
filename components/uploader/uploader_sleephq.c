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
#include "upload_http.h"

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
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"


static const char *TAG = "upload_shq";

#define SHQ_HOST        "sleephq.com"
#define SHQ_TOKEN_PATH  "/oauth/token"
#define SHQ_ME_PATH     "/api/v1/me"
#define SHQ_IMPORTS_FMT "/api/v1/teams/%s/imports"
#define SHQ_IMPORT_FMT  "/api/v1/imports/%s"
#define SHQ_FILES_FMT   "/api/v1/imports/%s/files"
#define SHQ_PROCESS_FMT "/api/v1/imports/%s/process_files"

#define SHQ_TIMEOUT_MS  30000
#define SHQ_HDR_CAP     8192    /* Cloudflare + SleepHQ headers (CSP, Report-To, etc. are ~3.2 KB) */
#define SHQ_RESP_CAP    32768   /* Max response body in PSRAM (supports 700+ files per import) */

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

/* ── HTTP request helper ──────────────────────────────────────────────
 * All socket/TLS/multipart machinery lives in upload_http.c — see the
 * header there for the shared contract. These wrappers pin the SleepHQ
 * host, Accept type and buffer caps the shared layer was extracted from. */
static int shq_http_request(esp_tls_t *tls, const char *method,
                            const char *path, const char *query,
                            const char *auth_token,
                            const char *body, const char *content_type,
                            char **body_out, size_t *body_len)
{
    return uph_request(tls, SHQ_HOST, method, path, query, auth_token,
                       "application/vnd.api+json", body, content_type,
                       SHQ_HDR_CAP + SHQ_RESP_CAP, SHQ_RESP_CAP,
                       body_out, body_len, NULL);
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
        /* Every failure below used to return the same bare ESP_FAIL, and the caller
         * reports all of them as "import processing failed for day <d>" -- which reads
         * as "SleepHQ rejected the import" whichever one actually happened. Three
         * different causes wearing one message is why #151 could run for weeks without
         * the cause being identifiable from a log. Say which.
         *
         * Measured on the log attached to #151: the failing GET returns HTTP 200 with a
         * 1971-byte body, well under the read cap, so the body is NOT truncated and the
         * failure is decided in the lines below -- but which line is not recoverable
         * from the log as it stands. */
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "import %s: status query returned HTTP %d (attempt %d/30)",
                     import_id, status, attempt + 1);
            free(body);
            if (attempt + 1 >= 30) return ESP_FAIL;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        cJSON *root = cJSON_Parse(body);
        if (!root) {
            /* A body we cannot parse is OUR problem, not SleepHQ's verdict. The length
             * and the head of it separate the cases: a truncated body shows a length at
             * the read cap, a de-chunking fault shows hex chunk prefixes in the text. */
            ESP_LOGW(TAG, "import %s: unparseable status body (%u bytes, attempt %d/30): %.120s",
                     import_id, (unsigned)body_len, attempt + 1, body ? body : "(null)");
            free(body);
            if (attempt + 1 >= 30) return ESP_FAIL;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        free(body);
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *attrs = data ? cJSON_GetObjectItem(data, "attributes") : NULL;
        cJSON *st = attrs ? cJSON_GetObjectItem(attrs, "status") : NULL;
        const char *name = cJSON_IsString(st) ? st->valuestring : "";
        bool complete = strcmp(name, "complete") == 0 || strcmp(name, "completed") == 0;
        bool failed = strcmp(name, "failed") == 0 || strcmp(name, "error") == 0;
        if (failed || (!name[0] && attempt == 0)) {
            /* The remote verdict, verbatim. An absent status means the reply was not the
             * shape we expect, which is a different bug from a rejected import -- warn on
             * the first poll only, since that case still retries and would otherwise log
             * thirty times. Behaviour is unchanged by this commit; only what it says is. */
            ESP_LOGW(TAG, "import %s: remote status \"%s\" after %d poll(s)",
                     import_id, name[0] ? name : "(absent)", attempt + 1);
        }
        cJSON_Delete(root);
        if (complete) return ESP_OK;
        if (failed) return ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "import %s: still not complete after 30 polls (~60 s)", import_id);
    return ESP_ERR_TIMEOUT;
}

/* ── Multipart file upload ─────────────────────────────────────────────
 * Shared streaming implementation in upload_http.c. SleepHQ requires
 * md5(content + filename) for CPAP files and md5(filename + content) for
 * O2 — selected by filename_first. */
static upload_result_t shq_upload_file(esp_tls_t *tls,
                                       const char *import_id,
                                       const char *local_path,
                                       const char *remote_subpath,
                                       const char *filename,
                                       bool filename_first)
{
    char path[256];
    snprintf(path, sizeof(path), SHQ_FILES_FMT, import_id);

    int status = uph_put_file(tls, SHQ_HOST, path, s_token,
                              "application/vnd.api+json",
                              local_path, remote_subpath, filename,
                              filename_first ? UPH_MD5_NAME_FILE
                                             : UPH_MD5_FILE_NAME,
                              NULL, 0, NULL);
    return (status >= 200 && status < 300) ? UPLOAD_OK : UPLOAD_FAILED;
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

    s_tls = uph_connect(SHQ_HOST, SHQ_TIMEOUT_MS);
    if (!s_tls) return UPLOAD_ERR_TRANSIENT;

    if (shq_authenticate(s_tls, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "authentication failed");
        uph_disconnect(s_tls);
        s_tls = NULL;
        /* Credentials the server refuses will not start working on retry, so
         * report it as permanent and let the UI say so. */
        return UPLOAD_ERR_PERMANENT;
    }
    if (shq_discover_team(s_tls) != ESP_OK) {
        ESP_LOGE(TAG, "team discovery failed");
        uph_disconnect(s_tls);
        s_tls = NULL;
        return UPLOAD_ERR_TRANSIENT;
    }
    return UPLOAD_OK;
}

static void shq_session_end(void)
{
    if (!s_tls) return;
    uph_disconnect(s_tls);
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

static bool shq_test(const uploader_config_t *cfgp, char *msg, size_t msg_len)
{
    uploader_config_t cfg = *cfgp;   /* by value — see smb_test */
    if (!cfg.shq_client_id[0] || !cfg.shq_client_secret[0]) {
        snprintf(msg, msg_len, "Client ID and Client Secret are required");
        return false;
    }

    esp_tls_t *tls = uph_connect(SHQ_HOST, SHQ_TEST_TIMEOUT_MS);
    if (!tls) {
        snprintf(msg, msg_len, "Cannot reach %s: check the internet connection", SHQ_HOST);
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
    uph_disconnect(tls);
    memset(token, 0, SHQ_TOKEN_MAX);
    free(token);

    /* #214.3 -- what the TLS handshake actually cost on this task's stack.
     * Measured rather than estimated: the httpd worker gets 12 KB in PSRAM and
     * a handshake is expected to want 6.5-8.5 KB, so the margin is real but
     * thin, and it is worth a log line on the one path that exercises it.
     *
     * ESP-IDF returns this value in BYTES.  Vanilla FreeRTOS returns WORDS, and
     * scaling by sizeof(StackType_t) here -- the reflex when porting the idiom
     * -- would report four times the true headroom, which is the direction that
     * hides a problem rather than raising one. */
    ESP_LOGI(TAG, "sleephq: connection test finished with %u bytes of stack headroom",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

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
    .supports_ox = true,
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
