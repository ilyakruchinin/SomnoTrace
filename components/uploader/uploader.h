/*
 * SomnoTrace - Session upload system for SMB and SleepHQ backends
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

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "upload_scan.h"
#include "upload_ox.h"

/* ── Upload backend interface ──────────────────────────────────────────
 *
 * Each backend (SMB, SleepHQ, future additions) implements this interface.
 * Only one backend runs at a time, to avoid memory contention between TLS
 * (SleepHQ) and SMB socket buffers.
 *
 * The interface is split into a connection scope, a day scope and a per-group
 * transfer so that tracking can be per group while the *transport* stays
 * batched: one connect per backend per run, and for SleepHQ one import per
 * day.  Per-group tracking must never become per-group connecting — that
 * would be slower than the day-level uploader it replaces.
 *
 * Call order driven by the scheduler:
 *
 *   session_begin()
 *     day_begin(day)                      for each day with pending groups
 *       put_group(day, group)             for each pending group only
 *       put_bundle(day, bundle, changed)  root files (see note below)
 *     day_end(day, any_uploaded)
 *   session_end()
 *
 * put_bundle() is offered once per day. The backend decides what to do with
 * it: SleepHQ must include the root files in every import for the sessions to
 * be interpretable, whereas SMB only needs to re-send them when `changed` is
 * true. Returning UPLOAD_OK for a deliberate skip is correct and expected. */

typedef enum {
    UPLOAD_OK = 0,          /* transferred, or deliberately skipped         */
    UPLOAD_CANCELLED,       /* recording admission requested; leave pending */
    UPLOAD_NOT_CONFIGURED,  /* backend has no valid config — skipped        */
    UPLOAD_ERR_TRANSIENT,   /* timeout / unreachable / 5xx — retry later    */
    UPLOAD_ERR_PERMANENT,   /* auth rejected / 4xx — retry, but surface it  */
} upload_result_t;

/* Kept as an alias so existing backend code reads naturally; any error maps
 * onto the transient ladder unless the backend is more specific. */
#define UPLOAD_FAILED  UPLOAD_ERR_TRANSIENT

typedef struct {
    const char *id;         /* "smb" | "sleephq" — stable tracking key      */
    const char *label;      /* "NAS (SMB)" — shown in the UI                */

    /* May this backend be contacted when the ONLY thing that changed is the
     * root bundle, with no new session groups to send?
     *
     * True for a plain file tree like SMB: copying five files over is
     * harmless. False for SleepHQ, where every visit creates an import, and
     * an import holding root files but no session data is just clutter in the
     * user's history. A false here means the bundle waits and rides along
     * with the next session upload instead — at worst a day of staleness,
     * since a genuinely changed STR.edf with no new sessions only happens
     * when the AS11 revises an earlier day's summary. */
    bool bundle_only_ok;

    /* Check if this backend has valid configuration (config keys in NVS). */
    bool (*is_configured)(void);

    /* Resolve names before taking any SD lease. No card access here. */
    upload_result_t (*prepare)(void);

    /* Open the connection: TCP/SMB session or TLS + auth. */
    upload_result_t (*session_begin)(void);

    /* Enter a day: create the remote folder (SMB) or open an import (SHQ). */
    upload_result_t (*day_begin)(const char *day);

    /* Transfer every file of one group. All-or-nothing: return an error
     * unless every file in the group was accepted. */
    upload_result_t (*put_group)(const char *day, const upload_group_ref_t *g);

    /* Oximetry transport scope. If omitted, the normal day callbacks are
     * reused (SMB); SleepHQ uses a distinct O2 import scope. */
    upload_result_t (*ox_day_begin)(const char *day);
    upload_result_t (*put_oximetry)(const upload_ox_ref_t *ref);
    upload_result_t (*ox_day_end)(const char *day, bool any_uploaded);

    /* Offer the root bundle for this day (see note above). */
    upload_result_t (*put_bundle)(const char *day,
                                  const upload_bundle_ref_t *b, bool changed);

    /* Leave a day: finalise the import (SHQ) or nothing (SMB).
     * any_uploaded is false when the day had no pending groups left. */
    upload_result_t (*day_end)(const char *day, bool any_uploaded);

    /* Close the connection. Always called if session_begin() succeeded. */
    void (*session_end)(void);

    /* Optional "Test connection" probe for the web UI: connect and
     * authenticate with the saved configuration, then disconnect, without
     * transferring anything.  Writes a one-line outcome for the user into
     * msg and returns true when the backend is usable.  Must use its own
     * connection object, never the one session_begin() owns — it runs on
     * the httpd task, not the scheduler. */
    bool (*test)(char *msg, size_t msg_len);
} upload_backend_t;

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    /* SMB server */
    bool smb_enabled;        /* toggle: include SMB in upload cycle         */
    char smb_host[64];       /* server IP or hostname                      */
    char smb_share[64];      /* share name (e.g. "cpap")                   */
    char smb_user[64];       /* username (empty = guest)                   */
    char smb_pass[64];       /* password (empty = guest)                   */
    char smb_path[128];      /* remote path within share (e.g. "/SomnoTrace") */

    /* SleepHQ */
    bool shq_enabled;        /* toggle: include SleepHQ in upload cycle     */
    char shq_client_id[128];      /* API key (Client UID)                */
    char shq_client_secret[128];  /* Client Secret                       */

    /* Built-in FTP server */
    bool ftp_enabled;        /* toggle: start FTP server at boot            */
    bool ftp_anonymous;      /* true = anonymous, false = user/pass auth    */
    char ftp_user[32];       /* FTP username (when not anonymous)           */
    char ftp_pass[32];       /* FTP password (when not anonymous)           */

    /* Upload window in days (newest first).  Bounds the periodic scan, the
     * progress denominators, and above all a manual "reset upload state" so
     * one click cannot start re-uploading a year of history.
     * Default UPLOAD_DEFAULT_MAX_DAYS (30), hard cap UPLOAD_MAX_DAYS_CAP. */
    int  max_days;
} uploader_config_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialise the uploader subsystem.
 * - Mounts LittleFS on the "storage" partition
 * - Loads upload state and config from NVS
 * - Starts the persistent upload task
 * Call once at boot after nvs_flash_init() and sd_storage_init(). */
esp_err_t uploader_init(void);

/* Durable export invalidation handoff. Register before uploader_init().
 * The app owns persisted work and stable generation tokens (not RAM queue
 * entries). next returns one pending day/token without consuming it; ack
 * retires only that exact token and returns failure if it cannot do so.
 * Both callbacks run on the scheduler outside its storage lease and must
 * use bounded, nonblocking storage admission of their own. They must not
 * perform network work or wait for this scheduler. The scheduler takes a
 * separate zero-wait lease to delete old index state, releases it, then ack's.
 * A reset at any boundary repeats invalidation safely before acknowledgement. */
#define UPLOADER_INVALIDATION_TOKEN_CAP 128
typedef bool (*uploader_invalidation_next_fn_t)(uint32_t *day, char *token,
                                               size_t token_cap);
typedef esp_err_t (*uploader_invalidation_ack_fn_t)(uint32_t day, const char *token);
void uploader_set_invalidation_hooks(uploader_invalidation_next_fn_t next,
                                      uploader_invalidation_ack_fn_t ack);

/* ── Event triggers ───────────────────────────────────────────────────
 * All are safe to call from any task; they only nudge the scheduler.
 * Persist invalidation work before posting. A full queue or restart is
 * repaired by periodic polling of the durable handoff, not by queue replay. */

/* An export finished for this noon-day. Rescan after servicing the persisted
 * invalidation token, so changed samples under existing filenames are offered
 * again as well as new groups. Delivery is intentionally at least once. */
void uploader_on_export_complete(const char *day_folder);

/* This day's exported files were replaced (rebuild-day / recreate-edfs).
 * Nudge the durable handoff; the event itself never authorizes deletion or
 * acknowledgement. The persisted day token causes every group to be offered. */
void uploader_on_day_invalidated(const char *day_folder);

/* Ask for an immediate reconciliation of the card against the tracking state
 * (the periodic scan otherwise runs every 10 minutes). */
void uploader_request_scan(void);

/* Register a backend. Called during uploader_init() for built-in backends. */
void uploader_register_backend(const upload_backend_t *backend);

/* Load / save upload configuration from / to NVS. */
esp_err_t uploader_load_config(uploader_config_t *cfg);
esp_err_t uploader_save_config(const uploader_config_t *cfg);

/* Optional NVS-write executor injection.
 *
 * uploader_save_config() is reached from the httpd worker, which runs on a
 * PSRAM stack — a task with a PSRAM stack cannot itself perform a flash write.
 * The app injects an executor (its internal-stack nvs_writer) here; when set,
 * uploader_save_config() runs its NVS write on that task instead of inline.
 * If never set, the write runs inline (safe when the caller has an internal
 * stack). The uploader component does not depend on the app, hence injection. */
typedef esp_err_t (*uploader_nvs_task_fn_t)(void *arg);
typedef esp_err_t (*uploader_nvs_exec_fn_t)(uploader_nvs_task_fn_t fn, void *arg);
void uploader_set_nvs_executor(uploader_nvs_exec_fn_t exec);

/* Nonblocking recording-intent predicate, injected by the app. */
typedef bool (*uploader_cancel_fn_t)(void);
void uploader_set_cancel_fn(uploader_cancel_fn_t fn);
bool uploader_should_cancel(void);
/* Resolve to a numeric IPv4 address, only before storage admission. */
bool uploader_resolve_host(const char *host, char *out, size_t out_size);

/* Storage-lease hooks (injected for the same reason as the NVS executor:
 * this component does not depend on the app).
 *
 * The uploader reads a day folder that a rebuild may be replacing, so it
 * takes a lease for the duration of each day it uploads.  acquire() returns
 * false if the lease is unavailable, in which case the day is left pending
 * and retried later rather than read mid-replacement. */
typedef bool (*uploader_lease_acquire_fn_t)(uint32_t timeout_ms);
typedef void (*uploader_lease_release_fn_t)(void);
void uploader_set_lease_fns(uploader_lease_acquire_fn_t acquire,
                            uploader_lease_release_fn_t release);

/* Optional "upload progress changed" hook (injected for the same reason as
 * the lease and NVS hooks: this component does not depend on the app).
 *
 * Invoked on every backend state transition (idle/uploading/cooldown/
 * disabled) so the web UI can be pushed an update immediately instead of
 * waiting for the next periodic poll.  It is called from the scheduler
 * task, so the implementation must be cheap and non-blocking — set a flag
 * and let another task do the work. */
typedef void (*uploader_progress_notify_fn_t)(void);
void uploader_set_progress_notify_fn(uploader_progress_notify_fn_t fn);

/* Check if specific backends are configured and enabled. */
bool uploader_is_smb_configured(void);
bool uploader_is_sleephq_configured(void);
bool uploader_is_smb_enabled(void);
bool uploader_is_sleephq_enabled(void);

/* Check if FTP server is enabled in config. */
bool uploader_is_ftp_enabled(void);

/* Fixed-size upload progress for memory-constrained callers such as the
 * bedside display.  The snapshot owns all of its strings and the getter does
 * not allocate.  Entries include every registered backend, including ones
 * that are disabled or do not yet have complete configuration. */
#define UPLOADER_PROGRESS_MAX_BACKENDS  4
#define UPLOADER_PROGRESS_ID_LEN        12
#define UPLOADER_PROGRESS_LABEL_LEN     32
#define UPLOADER_PROGRESS_DAY_LEN       12
#define UPLOADER_PROGRESS_ERROR_LEN     72
#define UPLOADER_PROGRESS_STATUS_LEN    64

typedef enum {
    UPLOADER_BACKEND_DISABLED = 0,
    UPLOADER_BACKEND_IDLE,
    UPLOADER_BACKEND_UPLOADING,
    UPLOADER_BACKEND_COOLDOWN,
} uploader_backend_state_t;

typedef struct {
    char id[UPLOADER_PROGRESS_ID_LEN];
    char label[UPLOADER_PROGRESS_LABEL_LEN];
    bool configured;
    uploader_backend_state_t state;
    int days_done;
    int days_total;

    bool current_valid;
    char current_day[UPLOADER_PROGRESS_DAY_LEN];
    int current_unit;
    int current_units;

    bool last_success_valid;
    uint32_t last_success_epoch_s;

    bool retry_valid;
    uint32_t retry_in_s;
    bool error_valid;
    bool error_permanent;
    char error[UPLOADER_PROGRESS_ERROR_LEN];
} uploader_backend_progress_t;

typedef struct {
    char status[UPLOADER_PROGRESS_STATUS_LEN];
    bool scanning;
    uint32_t next_scan_s;
    int max_days;
    size_t backend_count;
    uploader_backend_progress_t backends[UPLOADER_PROGRESS_MAX_BACKENDS];
} uploader_progress_snapshot_t;

/* Populate a caller-owned, bounded RAM snapshot without heap allocation or
 * SD/index traversal. Returns ESP_ERR_INVALID_STATE before uploader_init()
 * has completed. */
esp_err_t uploader_get_progress_snapshot(uploader_progress_snapshot_t *out);

/* Compact upload progress for the web UI: one entry per backend with its
 * state, days done/total and, while uploading, the current day and unit.
 * Bounded in size regardless of how much history exists. Progress is
 * serialized from the scheduler's RAM snapshot rather than the mutable index.
 * Returns ESP_ERR_INVALID_STATE before uploader_init() has completed.
 * Caller must free() the returned string. */
esp_err_t uploader_get_progress_json(char **out_json);

/* Cached one-line summary for /api/status (header badge): how many units are
 * still outstanding across configured backends, and the worst backend state
 * ("idle" | "uploading" | "cooldown"). This is a bounded RAM-only snapshot;
 * callers never scan or mutate the SD card. */
void uploader_get_summary(int *out_pending, const char **out_worst);

/* Debug: the parsed tracking state for one day ("YYYYMMDD").
 * Caller must free() the returned string. */
esp_err_t uploader_get_day_state_json(const char *day, char **out_json);

/* Get upload config as a JSON string (for web UI).
 * Passwords/secrets are masked. Caller must free() the returned string. */
esp_err_t uploader_get_config_json(char **out_json);

/* Save upload config from a JSON string (from web UI POST body).
 * Parses the JSON and stores values in NVS. */
esp_err_t uploader_save_config_json(const char *json_str);

/* Clear all upload tracking state, then rescan and re-upload — bounded by
 * config.max_days, so this cannot start an unbounded re-upload.
 * Asynchronous: the work happens on the scheduler task. */
esp_err_t uploader_reset_state(void);

/* "Test connection" for the web UI: probe one backend ("smb" | "sleephq")
 * with the configuration currently saved in NVS, without uploading anything.
 * msg receives a one-line outcome for the user in every case.
 *   ESP_OK                 the probe ran; *out_ok says whether it passed
 *   ESP_ERR_INVALID_STATE  an upload is in progress (one transport at a time,
 *                          see the backend interface note) or the uploader
 *                          has not finished initialising
 *   ESP_ERR_NOT_FOUND      unknown backend id
 * Blocks the caller for up to the backend's probe timeout (about 10 s). */
esp_err_t uploader_test_connection(const char *backend_id, bool *out_ok,
                                   char *msg, size_t msg_len);
