/*
 * SomnoTrace - Upload orchestration task and config management
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
#include "upload_index.h"
#include "upload_scan.h"
#include "upload_sched.h"
#include "upload_migrate.h"   /* DECOMMISSION AFTER v0.7.x */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "uploader";

#define NVS_NAMESPACE  "uploader"

/* ── Internal state ───────────────────────────────────────────────────
 *
 * Scheduling, retry and tracking state all live in upload_sched /
 * upload_index now; this file owns configuration, the backend registry and
 * the public API only. */

static uploader_config_t s_config;
static bool s_initialised = false;

#define MAX_BACKENDS UPLOAD_MAX_BACKENDS
static const upload_backend_t *s_backends[MAX_BACKENDS];
static int s_n_backends = 0;

/* ── Backend registration ───────────────────────────────────────────── */

void uploader_register_backend(const upload_backend_t *backend)
{
    if (!backend || s_n_backends >= MAX_BACKENDS) return;
    s_backends[s_n_backends++] = backend;
    ESP_LOGI(TAG, "registered backend: %s", backend->id);
}

/* External backend declarations */
extern const upload_backend_t smb_backend;
extern const upload_backend_t sleephq_backend;

/* Injected NVS-write executor (the app's internal-stack nvs_writer). */
static uploader_nvs_exec_fn_t s_nvs_exec = NULL;

/* ── Config load / save (NVS) ───────────────────────────────────────── */

/* NVS-read portion — runs on the injected executor's task (internal stack)
 * when one is set, so the read is serialized with all other NVS access and
 * is safe even when uploader_load_config() is called from a PSRAM-stacked
 * task.  The output struct is filled into a stack-local copy first, then
 * copied to the caller only after the NVS handle is closed. */
static esp_err_t do_uploader_load_config(void *arg)
{
    uploader_config_t *out = arg;
    uploader_config_t local;
    memset(&local, 0, sizeof(local));

    /* Defaults: all toggles enabled for backward compatibility */
    local.smb_enabled   = true;
    local.shq_enabled   = true;
    local.ftp_enabled   = true;
    local.ftp_anonymous = true;
    local.max_days      = UPLOAD_DEFAULT_MAX_DAYS;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    /* Boolean toggles — use u8 with backward-compatible defaults.
     * If key is missing (first boot after upgrade), default to enabled
     * for SMB/SHQ and FTP so existing setups keep working. */
    uint8_t u8val;
    local.smb_enabled   = (nvs_get_u8(h, "smb_en", &u8val) == ESP_OK) ? u8val : 1;
    local.shq_enabled   = (nvs_get_u8(h, "shq_en", &u8val) == ESP_OK) ? u8val : 1;
    local.ftp_enabled   = (nvs_get_u8(h, "ftp_en", &u8val) == ESP_OK) ? u8val : 1;
    local.ftp_anonymous = (nvs_get_u8(h, "ftp_anon", &u8val) == ESP_OK) ? u8val : 1;

    /* Upload window: how many of the newest days are ever considered. Caps a
     * manual "reset state" so it cannot kick off months of re-uploading. */
    int32_t i32val;
    local.max_days = (nvs_get_i32(h, "max_days", &i32val) == ESP_OK)
                     ? (int)i32val : UPLOAD_DEFAULT_MAX_DAYS;
    if (local.max_days < 1) local.max_days = 1;
    if (local.max_days > UPLOAD_MAX_DAYS_CAP) local.max_days = UPLOAD_MAX_DAYS_CAP;

    size_t len;
    len = sizeof(local.smb_host);
    nvs_get_str(h, "smb_host", local.smb_host, &len);
    len = sizeof(local.smb_share);
    nvs_get_str(h, "smb_share", local.smb_share, &len);
    len = sizeof(local.smb_user);
    nvs_get_str(h, "smb_user", local.smb_user, &len);
    len = sizeof(local.smb_pass);
    nvs_get_str(h, "smb_pass", local.smb_pass, &len);
    len = sizeof(local.smb_path);
    nvs_get_str(h, "smb_path", local.smb_path, &len);
    len = sizeof(local.shq_client_id);
    nvs_get_str(h, "shq_cid", local.shq_client_id, &len);
    len = sizeof(local.shq_client_secret);
    nvs_get_str(h, "shq_secret", local.shq_client_secret, &len);
    len = sizeof(local.ftp_user);
    nvs_get_str(h, "ftp_user", local.ftp_user, &len);
    len = sizeof(local.ftp_pass);
    nvs_get_str(h, "ftp_pass", local.ftp_pass, &len);

    nvs_close(h);
    *out = local;
    return ESP_OK;
}

esp_err_t uploader_load_config(uploader_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));

    /* Defaults: all toggles enabled for backward compatibility */
    cfg->smb_enabled   = true;
    cfg->shq_enabled   = true;
    cfg->ftp_enabled   = true;
    cfg->ftp_anonymous = true;
    cfg->max_days      = UPLOAD_DEFAULT_MAX_DAYS;

    /* Use the injected NVS executor (nvs_writer_run) if available so the
     * read is serialized with all other NVS access.  Fall back to direct
     * access only before the executor is set (early boot, internal stack). */
    esp_err_t err = s_nvs_exec ? s_nvs_exec(do_uploader_load_config, cfg)
                               : do_uploader_load_config(cfg);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no uploader config in NVS — using defaults");
    }
    return err;
}

/* Injected storage-lease hooks (the app's sd_storage arbitration). */
static uploader_lease_acquire_fn_t s_lease_acquire = NULL;
static uploader_lease_release_fn_t s_lease_release = NULL;

/* Injected "progress changed" notifier (the app's WebSocket push). */
static uploader_progress_notify_fn_t s_progress_notify = NULL;

void uploader_set_lease_fns(uploader_lease_acquire_fn_t acquire,
                            uploader_lease_release_fn_t release)
{
    s_lease_acquire = acquire;
    s_lease_release = release;
}

void uploader_set_progress_notify_fn(uploader_progress_notify_fn_t fn)
{
    s_progress_notify = fn;
}

/* Called by the scheduler on every backend state transition.  Must stay
 * cheap and allocation-free: it runs on the scheduler task and the hook is
 * expected only to set a flag for another task to act on. */
void uploader_notify_progress_changed(void)
{
    if (s_progress_notify) s_progress_notify();
}

void uploader_set_nvs_executor(uploader_nvs_exec_fn_t exec)
{
    s_nvs_exec = exec;
}

/* NVS-write portion — runs on the injected executor's task (internal stack)
 * when one is set, so it is safe even when uploader_save_config() is called
 * from the PSRAM-stacked httpd worker. */
static esp_err_t do_uploader_save_config(void *arg)
{
    const uploader_config_t *cfg = (const uploader_config_t *)arg;
    uploader_config_t local = *cfg;
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    nvs_set_u8(h, "smb_en", local.smb_enabled ? 1 : 0);
    nvs_set_u8(h, "shq_en", local.shq_enabled ? 1 : 0);
    nvs_set_u8(h, "ftp_en", local.ftp_enabled ? 1 : 0);
    nvs_set_u8(h, "ftp_anon", local.ftp_anonymous ? 1 : 0);
    nvs_set_i32(h, "max_days", local.max_days);
    nvs_set_str(h, "smb_host", local.smb_host);
    nvs_set_str(h, "smb_share", local.smb_share);
    nvs_set_str(h, "smb_user", local.smb_user);
    nvs_set_str(h, "smb_pass", local.smb_pass);
    nvs_set_str(h, "smb_path", local.smb_path);
    nvs_set_str(h, "shq_cid", local.shq_client_id);
    nvs_set_str(h, "shq_secret", local.shq_client_secret);
    nvs_set_str(h, "ftp_user", local.ftp_user);
    nvs_set_str(h, "ftp_pass", local.ftp_pass);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t uploader_save_config(const uploader_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Delegate the flash write to the injected executor (internal-stack
     * nvs_writer) so a caller on a PSRAM stack (httpd) is safe. Runs inline
     * if no executor was injected (caller then has an internal stack). */
    esp_err_t ret = s_nvs_exec ? s_nvs_exec(do_uploader_save_config, (void *)cfg)
                               : do_uploader_save_config((void *)cfg);
    if (ret == ESP_OK) {
        /* Update in-memory copy */
        memcpy(&s_config, cfg, sizeof(s_config));
        ESP_LOGI(TAG, "config saved to NVS");
    }
    return ret;
}

bool uploader_is_smb_configured(void)
{
    return s_config.smb_enabled &&
           s_config.smb_host[0] != '\0' && s_config.smb_share[0] != '\0';
}

bool uploader_is_sleephq_configured(void)
{
    return s_config.shq_enabled &&
           s_config.shq_client_id[0] != '\0' && s_config.shq_client_secret[0] != '\0';
}

bool uploader_is_smb_enabled(void)
{
    return s_config.smb_enabled;
}

bool uploader_is_sleephq_enabled(void)
{
    return s_config.shq_enabled;
}

bool uploader_is_ftp_enabled(void)
{
    return s_config.ftp_enabled;
}

/* ── Config JSON for web UI ─────────────────────────────────────────── */

esp_err_t uploader_get_config_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();

    cJSON *smb = cJSON_CreateObject();
    cJSON_AddBoolToObject(smb, "enabled", s_config.smb_enabled);
    cJSON_AddStringToObject(smb, "host", s_config.smb_host);
    cJSON_AddStringToObject(smb, "share", s_config.smb_share);
    cJSON_AddStringToObject(smb, "user", s_config.smb_user);
    cJSON_AddStringToObject(smb, "path", s_config.smb_path);
    /* Mask password */
    cJSON_AddStringToObject(smb, "pass", s_config.smb_pass[0] ? "***" : "");
    cJSON_AddBoolToObject(smb, "configured", uploader_is_smb_configured());
    cJSON_AddItemToObject(root, "smb", smb);

    cJSON *shq = cJSON_CreateObject();
    cJSON_AddBoolToObject(shq, "enabled", s_config.shq_enabled);
    cJSON_AddStringToObject(shq, "client_id", s_config.shq_client_id);
    /* Mask secret */
    cJSON_AddStringToObject(shq, "client_secret", s_config.shq_client_secret[0] ? "***" : "");
    cJSON_AddBoolToObject(shq, "configured", uploader_is_sleephq_configured());
    cJSON_AddItemToObject(root, "sleephq", shq);

    cJSON_AddNumberToObject(root, "max_days", uploader_max_days());
    cJSON_AddNumberToObject(root, "max_days_cap", UPLOAD_MAX_DAYS_CAP);

    cJSON *ftp = cJSON_CreateObject();
    cJSON_AddBoolToObject(ftp, "enabled", s_config.ftp_enabled);
    cJSON_AddBoolToObject(ftp, "anonymous", s_config.ftp_anonymous);
    cJSON_AddStringToObject(ftp, "user", s_config.ftp_user);
    /* Mask password */
    cJSON_AddStringToObject(ftp, "pass", s_config.ftp_pass[0] ? "***" : "");
    cJSON_AddItemToObject(root, "ftp", ftp);

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t uploader_save_config_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "failed to parse config JSON");
        return ESP_ERR_INVALID_STATE;
    }

    uploader_config_t cfg;
    memcpy(&cfg, &s_config, sizeof(cfg));  /* start from current */

    cJSON *smb = cJSON_GetObjectItem(root, "smb");
    if (smb) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(smb, "enabled")) && cJSON_IsBool(v))
            cfg.smb_enabled = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(smb, "host")) && cJSON_IsString(v))
            strlcpy(cfg.smb_host, v->valuestring, sizeof(cfg.smb_host));
        if ((v = cJSON_GetObjectItem(smb, "share")) && cJSON_IsString(v))
            strlcpy(cfg.smb_share, v->valuestring, sizeof(cfg.smb_share));
        if ((v = cJSON_GetObjectItem(smb, "user")) && cJSON_IsString(v))
            strlcpy(cfg.smb_user, v->valuestring, sizeof(cfg.smb_user));
        if ((v = cJSON_GetObjectItem(smb, "path")) && cJSON_IsString(v))
            strlcpy(cfg.smb_path, v->valuestring, sizeof(cfg.smb_path));
        /* Only update password if not the mask string */
        if ((v = cJSON_GetObjectItem(smb, "pass")) && cJSON_IsString(v)) {
            if (strcmp(v->valuestring, "***") != 0)
                strlcpy(cfg.smb_pass, v->valuestring, sizeof(cfg.smb_pass));
        }
    }

    cJSON *shq = cJSON_GetObjectItem(root, "sleephq");
    if (shq) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(shq, "enabled")) && cJSON_IsBool(v))
            cfg.shq_enabled = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(shq, "client_id")) && cJSON_IsString(v))
            strlcpy(cfg.shq_client_id, v->valuestring, sizeof(cfg.shq_client_id));
        if ((v = cJSON_GetObjectItem(shq, "client_secret")) && cJSON_IsString(v)) {
            if (strcmp(v->valuestring, "***") != 0)
                strlcpy(cfg.shq_client_secret, v->valuestring, sizeof(cfg.shq_client_secret));
        }
    }

    cJSON *md = cJSON_GetObjectItem(root, "max_days");
    if (md && cJSON_IsNumber(md)) {
        int v = md->valueint;
        if (v < 1) v = 1;
        if (v > UPLOAD_MAX_DAYS_CAP) v = UPLOAD_MAX_DAYS_CAP;
        cfg.max_days = v;
    }

    cJSON *ftp = cJSON_GetObjectItem(root, "ftp");
    if (ftp) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(ftp, "enabled")) && cJSON_IsBool(v))
            cfg.ftp_enabled = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(ftp, "anonymous")) && cJSON_IsBool(v))
            cfg.ftp_anonymous = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(ftp, "user")) && cJSON_IsString(v))
            strlcpy(cfg.ftp_user, v->valuestring, sizeof(cfg.ftp_user));
        if ((v = cJSON_GetObjectItem(ftp, "pass")) && cJSON_IsString(v)) {
            if (strcmp(v->valuestring, "***") != 0)
                strlcpy(cfg.ftp_pass, v->valuestring, sizeof(cfg.ftp_pass));
        }
    }

    cJSON_Delete(root);

    esp_err_t ret = uploader_save_config(&cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "config updated via web UI");
    }
    return ret;
}

/* ── Progress / status for the web UI ───────────────────────────────── */

/* Both of these are reachable from the httpd worker and from the WebSocket
 * forwarder task, either of which can run before uploader_init() has
 * finished — the forwarder starts as soon as a browser attaches, which at
 * boot is well before the uploader's index load completes.  Reading the
 * scheduler's state in that window previously took an uninitialised mutex
 * and aborted, so both entry points are gated on full initialisation. */
esp_err_t uploader_get_progress_json(char **out_json)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;
    return upload_sched_progress_json(out_json);
}

void uploader_get_summary(int *out_pending, const char **out_worst)
{
    if (!s_initialised) {
        if (out_pending) *out_pending = 0;
        if (out_worst) *out_worst = "idle";
        return;
    }
    upload_sched_summary(out_pending, out_worst);
}

esp_err_t uploader_get_day_state_json(const char *day, char **out_json)
{
    if (!day || !out_json) return ESP_ERR_INVALID_ARG;
    return upload_index_day_to_json((uint32_t)strtoul(day, NULL, 10), out_json);
}

/* ── Reset state ─────────────────────────────────────────────────────── */

esp_err_t uploader_reset_state(void)
{
    /* Clearing and rescanning happens on the scheduler task so it cannot race
     * an upload in progress. */
    upload_sched_request_reset();
    return ESP_OK;
}

/* ── Hooks used by upload_sched (same component, no app dependency) ──── */

int uploader_max_days(void)
{
    int d = s_config.max_days;
    if (d <= 0) d = UPLOAD_DEFAULT_MAX_DAYS;
    if (d > UPLOAD_MAX_DAYS_CAP) d = UPLOAD_MAX_DAYS_CAP;
    return d;
}

int uploader_enabled_backends(const upload_backend_t **out, int max_out)
{
    int n = 0;
    for (int i = 0; i < s_n_backends && n < max_out; i++) {
        if (s_backends[i]) out[n++] = s_backends[i];
    }
    return n;
}

bool uploader_lease_take(uint32_t timeout_ms)
{
    return s_lease_acquire ? s_lease_acquire(timeout_ms) : true;
}

void uploader_lease_give(void)
{
    if (s_lease_release) s_lease_release();
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t uploader_init(void)
{
    if (s_initialised) return ESP_OK;

    uploader_load_config(&s_config);

    esp_err_t ret = upload_index_init();
    if (ret != ESP_OK) return ret;

    /* Backends must be registered before the index loads, so that their slots
     * are assigned in a stable order and the state files can be attributed. */
    uploader_register_backend(&smb_backend);
    uploader_register_backend(&sleephq_backend);
    for (int i = 0; i < s_n_backends; i++) {
        upload_index_backend_slot(s_backends[i]->id);
    }

    upload_index_load(uploader_max_days());

    /* DECOMMISSION AFTER v0.7.x — carries day-level state across the switch to
     * per-group tracking so an already-synced device does not re-upload
     * everything once.  See upload_migrate.h. */
    upload_migrate_legacy_state();

    ret = upload_sched_init();
    if (ret != ESP_OK) return ret;

    s_initialised = true;
    ESP_LOGI(TAG, "uploader initialised (window %d days)", uploader_max_days());
    return ESP_OK;
}

/* ── Event triggers ─────────────────────────────────────────────────── */

static uint32_t day_to_num(const char *day_folder)
{
    if (!day_folder || strlen(day_folder) != 8) return 0;
    for (int i = 0; i < 8; i++) {
        if (day_folder[i] < '0' || day_folder[i] > '9') return 0;
    }
    return (uint32_t)strtoul(day_folder, NULL, 10);
}

void uploader_on_export_complete(const char *day_folder)
{
    if (!s_initialised) return;
    uint32_t day = day_to_num(day_folder);
    if (!day) {
        ESP_LOGW(TAG, "ignoring export notification for invalid day '%s'",
                 day_folder ? day_folder : "(null)");
        return;
    }
    ESP_LOGI(TAG, "export complete: %s", day_folder);
    upload_sched_notify_export(day);
}

void uploader_on_day_invalidated(const char *day_folder)
{
    if (!s_initialised) return;
    uint32_t day = day_to_num(day_folder);
    if (!day) return;
    ESP_LOGI(TAG, "day %s invalidated", day_folder);
    upload_sched_notify_invalidate(day);
}

void uploader_request_scan(void)
{
    if (!s_initialised) return;
    upload_sched_request_scan();
}

/* ── "Test connection" (web UI) ─────────────────────────────────────── */

esp_err_t uploader_test_connection(const char *backend_id, bool *out_ok,
                                   char *msg, size_t msg_len)
{
    if (!backend_id || !out_ok || !msg || msg_len == 0) return ESP_ERR_INVALID_ARG;
    *out_ok = false;

    const upload_backend_t *be = NULL;
    for (int i = 0; i < s_n_backends; i++) {
        if (s_backends[i] && strcmp(s_backends[i]->id, backend_id) == 0) {
            be = s_backends[i];
            break;
        }
    }
    if (!be || !be->test) {
        snprintf(msg, msg_len, "Unknown backend");
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_initialised) {
        snprintf(msg, msg_len, "Uploader is still starting up, try again in a moment");
        return ESP_ERR_INVALID_STATE;
    }
    /* Only one transport at a time: TLS and SMB buffers contend for memory
     * (see the backend interface note), and a run may be mid-transfer. */
    if (upload_sched_uploading()) {
        snprintf(msg, msg_len, "An upload is in progress, try again when it has finished");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "%s: connection test requested", be->id);
    *out_ok = be->test(msg, msg_len);
    ESP_LOGI(TAG, "%s: connection test %s: %s", be->id,
             *out_ok ? "passed" : "failed", msg);
    return ESP_OK;
}
