/*
 * SomnoTrace - BreathingRoom upload backend (shared upload_http plumbing)
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

/* Breathing Room API (https://breathingroom.io/api/openapi.json), verified
 * live 2026-10-14 with a personal access token and real card files:
 *
 *   POST /api/imports/upload   multipart `files` part per file, basename
 *                              only. Accepts STR.edf, Identification.json
 *                              and YYYYMMDD_HHMMSS_{BRP,PLD,SA2,EVE,CSL}.edf
 *                              — anything else is 400. Optional vendor=
 *                              resmed text part. 201 {created,skipped,
 *                              deferred,sessions[]}: skipped = already
 *                              there (success), deferred = over the 1000-
 *                              session parse queue, NOT stored → resend.
 *                              Free at every tier — never 402.
 *   POST /api/oximetry/upload  same `files` layout; ≤25 files, ≤8 MB each.
 *                              200 {"results":[{filename,status,id?…}]}
 *                              per file: imported | replaced | rejected
 *                              (+reason). Replaced returns the same id —
 *                              re-sending a day is a no-op. 402 feature
 *                              tracker on free accounts (Premium only).
 *   DELETE /api/oximetry/{id}  owner-only; ids returned by the upload.
 *   POST /api/sleep/upload     same `files` layout, one `<unit_id>_stages.sst`
 *                              per file (unit id = ring recording id or
 *                              as11_<session start>), optional `timeZone`
 *                              text part. 200 {"results":[…]} imported |
 *                              replaced | rejected (+reason). A known unit id
 *                              answers `replaced` with the same id, so a
 *                              re-send is safe. 402 feature tracker on free
 *                              accounts (Premium only), 256 KB per file.
 *
 * Auth is a static "slp_…" bearer PAT (Settings → Security → Access
 * tokens). Read-only tokens answer 403 token_read_only; bad ones 401.
 *
 * Server-side session clustering (15 min) merges the two SomnoTrace
 * groups of one therapy session automatically, so signal groups carry
 * neighboring event files in the same request. An EVE/CSL-only group is
 * skipped locally; a BRP under 4096 bytes may be dropped server-side
 * without being counted — still a deliberate skip here.
 *
 * TIMESTAMPS: the ring files are sent byte-for-byte with their original
 * names. The API rejects a file whose 14-digit filename timestamp differs
 * from its first sample by over 60 s, and stores ring samples unshifted —
 * if a future change ever rewrites a vendor file (clock normalisation),
 * it must rewrite the filename in the SAME operation. */

#include "uploader.h"
#include "upload_index.h"
#include "upload_paths.h"
#include "upload_http.h"
#include "br_parse.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "upload_br";

#define BR_HOST          "breathingroom.io"
#define BR_IMPORTS_PATH  "/api/imports/upload"
#define BR_OX_PATH       "/api/oximetry/upload"
#define BR_SLEEP_PATH    "/api/sleep/upload"
#define BR_ACCEPT        "application/json"

#define BR_TIMEOUT_MS        30000
#define BR_TEST_TIMEOUT_MS   10000
#define BR_RESP_CAP          32768
#define BR_MAX_IMPORT_FILES 999

/* ── Backend state ────────────────────────────────────────────────────
 * One keep-alive TLS connection per pass, same shape as the Aerivue
 * backend. There is no per-day scope: every request is independent, so
 * day_begin/day_end are left NULL and the scheduler supplies OK. */
static esp_tls_t *s_tls;
static char s_key[80];            /* bearer PAT, loaded at session_begin */
static bool s_peer_close;
static bool s_ox_blocked;         /* 402 seen this pass — Premium gate   */
static bool s_stage_blocked;      /* 402 on the sleep route — Premium    */

/* HTTP status → scheduler semantics:
 *   401/403  bad/revoked or read-only token — permanent, surface it
 *   402      oximetry Premium gate — permanent for ox units (CPAP never
 *            returns it)
 *   400      unsupported file type / bad vendor — permanent (our bug)
 *   413      over the ~100 MB platform body cap — permanent
 *   429      rate limit — transient, the cooldown ladder paces it
 *   5xx/-1   transient */
static upload_result_t br_map_status(int status)
{
    if (status >= 200 && status < 300) return UPLOAD_OK;
    if (status == 401 || status == 403 || status == 402 ||
        status == 400 || status == 413) return UPLOAD_ERR_PERMANENT;
    return UPLOAD_ERR_TRANSIENT;
}

static bool br_is_configured(void)
{
    return uploader_is_breathingroom_configured();
}

static upload_result_t br_session_begin(void)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);
    if (!cfg.br_key[0]) return UPLOAD_NOT_CONFIGURED;

    s_tls = uph_connect(BR_HOST, BR_TIMEOUT_MS);
    if (!s_tls) return UPLOAD_ERR_TRANSIENT;
    snprintf(s_key, sizeof(s_key), "%s", cfg.br_key);
    s_peer_close = false;
    s_ox_blocked = false;
    s_stage_blocked = false;

    /* Validate write access up front with a zero-file probe on the CPAP
     * route: 400 "No files provided" means the token authenticated and may
     * write; 401/403 convert a bad token into one permanent failure at
     * session open instead of N failed transfers. The imports route is
     * free at every tier, so a free account still passes here — the
     * Premium gate only fires on the first put_oximetry (402 latch). */
    bool pc = false;
    char *body = NULL;
    int status = uph_put_files(s_tls, BR_HOST, BR_IMPORTS_PATH, s_key,
                               BR_ACCEPT, NULL, 0, NULL, NULL,
                               &body, 4096, &pc);
    free(body);
    if (status == 400) {
        /* The probe worked; if the server already closed its side, flag it
         * so the first real request goes through br_ensure_conn(). */
        s_peer_close = pc;
        return UPLOAD_OK;   /* expected: no files */
    }
    uph_disconnect(s_tls);
    s_tls = NULL;
    if (status == 401 || status == 403) {
        ESP_LOGW(TAG, "token rejected (HTTP %d)", status);
        return UPLOAD_ERR_PERMANENT;
    }
    return UPLOAD_ERR_TRANSIENT;
}

static void br_session_end(void)
{
    if (!s_tls) return;
    uph_disconnect(s_tls);
    s_tls = NULL;
    s_peer_close = false;
}

static bool br_ensure_conn(void)
{
    if (s_tls && !s_peer_close) return true;
    ESP_LOGI(TAG, "reconnecting (peer closed or socket dead)");
    br_session_end();
    return br_session_begin() == UPLOAD_OK;
}

/* Send local_paths[] as one `files`-per-file request, reconnecting and
 * retrying once on a dead socket. Both routes dedupe server-side (skipped
 * sessions / replaced recordings), so a resend is safe. `vendor` is sent
 * only on the CPAP route — the oximetry route takes no text fields.
 * Response body is captured for the caller to parse (caller frees). */
static int br_send_files(const char *const *local_paths, int n_paths,
                         const char *const *filenames,
                         const char *req_path, const char *extra_field,
                         const char *extra_value, char **body_out)
{
    *body_out = NULL;
    if (!br_ensure_conn()) return -1;
    bool pc = false;
    int status = uph_put_files_named(s_tls, BR_HOST, req_path, s_key, BR_ACCEPT,
                                     local_paths, n_paths, filenames, extra_field, extra_value,
                               body_out, BR_RESP_CAP, &pc);
    if (pc) s_peer_close = true;
    if (status < 0) {
        free(*body_out);
        *body_out = NULL;
        s_peer_close = true;
        if (!br_ensure_conn()) return -1;
        pc = false;
        status = uph_put_files_named(s_tls, BR_HOST, req_path, s_key, BR_ACCEPT,
                                     local_paths, n_paths, filenames, extra_field, extra_value,
                               body_out, BR_RESP_CAP, &pc);
        if (pc) s_peer_close = true;
    }
    return status;
}

/* SomnoTrace splits one therapy session into two groups a few seconds
 * apart: {EVE,CSL} stamped at TherapyStart and {BRP,PLD,SA2} at MaskOn.
 * BreathingRoom forms sessions only from signal files — a request with no
 * BRP/PLD/SA2 answers 400 "No valid session files found" (verified live,
 * the docs' "stub dropped" only applies inside requests that DO form a
 * session). So an events-only group is never sent alone; its files ride
 * inside the sibling signal group's request, where the server's 15-minute
 * clustering attaches them to the same session. */

/* True when `name` is an EVE/CSL EDF whose HHMMSS sits inside the server's
 * 15-minute clustering window of `prefix_sec` (seconds-since-midnight).
 * The filename date is deliberately ignored — a noon-day folder holds one
 * night, stamped <day> 12:00–23:59 or <day+1> 00:00–11:59, so a wrap at
 * midnight is handled by folding the difference into ±12h. */
static bool br_sibling_event(const char *name, uint32_t prefix_sec)
{
    if (!name || strlen(name) != 23 || name[8] != '_' || name[15] != '_' ||
        (strcmp(name + 16, "EVE.edf") != 0 && strcmp(name + 16, "CSL.edf") != 0))
        return false;
    for (int i = 0; i < 15; i++)
        if (i != 8 && (name[i] < '0' || name[i] > '9')) return false;
    int h = (name[9] - '0') * 10 + name[10] - '0';
    int m = (name[11] - '0') * 10 + name[12] - '0';
    int s = (name[13] - '0') * 10 + name[14] - '0';
    if (h > 23 || m > 59 || s > 59 || prefix_sec >= 86400) return false;
    int sec = h * 3600 + m * 60 + s;
    int diff = sec - (int)prefix_sec;
    if (diff > 43200) diff -= 86400;
    else if (diff < -43200) diff += 86400;
    return diff >= -900 && diff <= 900;
}

static bool br_extra_event(const char *name, const upload_group_ref_t *g)
{
    if (!br_sibling_event(name, g->prefix_sec)) return false;
    for (int i = 0; i < g->n_files; i++)
        if (strcmp(name, g->files[i]) == 0) return false;
    return true;
}

static upload_result_t br_put_group(const char *day, const upload_group_ref_t *g)
{
    if (!s_tls || !day || !g || g->n_files <= 0 ||
        g->n_files > UPLOAD_GROUP_MAX_FILES) return UPLOAD_ERR_TRANSIENT;

    /* Events-only group: nothing here can form a session. Its files are
     * carried by the sibling signal group below — so this unit's real job
     * is just bookkeeping. Mark it done in every case:
     *  - sibling still pending  → it will carry these files when it goes;
     *  - sibling permanently failed → without a session there is nothing
     *    to attach events to, so they are unimportable anyway;
     *  - sibling already uploaded → it may already have carried these
     *    files, including events that sort later than the signal group;
     *    completion alone is not evidence of missing event delivery;
     *  - no sibling at all → events-only fragment, equally unimportable. */
    if (!(g->kinds & (UGK_BRP | UGK_PLD | UGK_SA2))) {
        upload_day_t *d = upload_index_day((uint32_t)strtoul(day, NULL, 10),
                                           false);
        int slot = upload_index_backend_slot("breathingroom");
        bool sibling_pending = false, sibling_done = false;
        if (d) {
            for (int i = 0; i < d->n_groups; i++) {
                upload_group_t *s = &d->groups[i];
                if (s->prefix_sec == g->prefix_sec ||
                    !(s->kinds & (UGK_BRP | UGK_PLD | UGK_SA2)))
                    continue;
                int diff = (int)s->prefix_sec - (int)g->prefix_sec;
                if (diff > 43200) diff -= 86400;
                else if (diff < -43200) diff += 86400;
                if (diff < -900 || diff > 900) continue;
                if (slot >= 0 && s->be[slot].status == UG_OK)
                    sibling_done = true;
                else
                    sibling_pending = true;
            }
        }
        if (sibling_done)
            ESP_LOGI(TAG, "  %s: sibling session already processed; "
                     "event delivery cannot be inferred from group state",
                     g->prefix);
        else
            ESP_LOGI(TAG, "  %s: no signal files — %s", g->prefix,
                     sibling_pending ? "rides a sibling group"
                                     : "no sibling session");
        return UPLOAD_OK;
    }

    /* Sibling event files from the same noon-day folder ride inside this
     * request — their own group was just marked done above, so this is the
     * only place they are uploaded. Count them first, then allocate the
     * exact size needed: a fragmented night can place several EVE/CSL
     * pairs inside the window, and silently truncating would drop events
     * while still marking their group done. Heap, not stack — the buffer
     * scales with folder contents. */
    char day_dir[160];
    snprintf(day_dir, sizeof(day_dir), "%s/%s", SD_SDCARD_DATALOG, day);
    int n_ev = 0;
    DIR *dir = opendir(day_dir);
    if (!dir) return UPLOAD_ERR_TRANSIENT;
    struct dirent *de;
    errno = 0;
    while ((de = readdir(dir)) != NULL) {
        if (br_extra_event(de->d_name, g)) n_ev++;
        if (g->n_files + n_ev > BR_MAX_IMPORT_FILES) break;
        errno = 0;
    }
    bool scan_failed = errno != 0 || g->n_files + n_ev > BR_MAX_IMPORT_FILES;
    if (closedir(dir) != 0) scan_failed = true;
    if (scan_failed) return UPLOAD_ERR_TRANSIENT;

    int n_total = g->n_files + n_ev;
    const char **paths = malloc((size_t)n_total * sizeof(char *));
    char (*buf)[128] = malloc((size_t)n_total * sizeof(*buf));
    if (!paths || !buf) {
        free(paths);
        free(buf);
        return UPLOAD_ERR_TRANSIENT;
    }

    int n = 0;
    for (int i = 0; i < g->n_files; i++) {
        snprintf(buf[n], sizeof(buf[n]), "%s/%s/%s",
                 SD_SDCARD_DATALOG, day, g->files[i]);
        paths[n] = buf[n];
        n++;
    }
    dir = opendir(day_dir);
    scan_failed = !dir;
    if (dir) {
        errno = 0;
        while ((de = readdir(dir)) != NULL) {
            if (br_extra_event(de->d_name, g)) {
                if (n >= n_total) {
                    scan_failed = true;
                    break;
                }
                int len = snprintf(buf[n], sizeof(buf[n]), "%s/%s",
                                   day_dir, de->d_name);
                if (len < 0 || len >= (int)sizeof(buf[n])) {
                    scan_failed = true;
                    break;
                }
                paths[n] = buf[n];
                n++;
            }
            errno = 0;
        }
        if (errno != 0) scan_failed = true;
        if (closedir(dir) != 0) scan_failed = true;
    }
    if (scan_failed || n != n_total) {
        free(paths);
        free(buf);
        return UPLOAD_ERR_TRANSIENT;
    }

    char *body = NULL;
    int status = br_send_files(paths, n, NULL, BR_IMPORTS_PATH,
                               "vendor", "resmed", &body);
    free(paths);
    free(buf);
    upload_result_t r = br_map_status(status);
    if (r == UPLOAD_OK) {
        br_import_result_t res;
        if (body && br_parse_import(body, &res)) {
            ESP_LOGI(TAG, "  %s: created %d, skipped %d, deferred %d",
                     g->prefix, res.created, res.skipped, res.deferred);
            /* Deferred sessions are NOT stored — they have to go out again
             * on a later pass, so the group stays pending. */
            if (res.deferred > 0) r = UPLOAD_ERR_TRANSIENT;
        } else {
            ESP_LOGW(TAG, "  %s: unparseable upload response", g->prefix);
            r = UPLOAD_ERR_TRANSIENT;
        }
    } else if (status == 400 && body &&
               strstr(body, "No valid session files")) {
        /* Terminal state for this group — its signal file is a stub
         * (BRP under 4096 bytes / unreadable header) that can never become
         * importable. Retrying only burns cooldowns to park it anyway. */
        ESP_LOGI(TAG, "  %s: server reports no importable session — done",
                 g->prefix);
        r = UPLOAD_OK;
    } else if (body) {
        ESP_LOGW(TAG, "  group %s failed (HTTP %d): %.200s",
                 g->prefix, status, body);
    }
    free(body);
    return r;
}

/* The remote accepts exactly STR.edf + Identification.json (+ session
 * EDFs). Identification.crc and the SETTINGS members of the bundle
 * would each draw a 400, so only the two accepted names are sent. */
static upload_result_t br_put_bundle(const char *day,
                                     const upload_bundle_ref_t *b,
                                     bool changed)
{
    (void)day;
    (void)changed;   /* a changed bundle must go out — see uploader.h */
    if (!s_tls || !b) return UPLOAD_ERR_TRANSIENT;

    const char *paths[2];
    int n = 0;
    for (int i = 0; i < b->n_files && n < 2; i++) {
        if (strcmp(b->names[i], "STR.edf") == 0 ||
            strcmp(b->names[i], "Identification.json") == 0)
            paths[n++] = b->paths[i];
    }
    if (n == 0) {
        ESP_LOGW(TAG, "  bundle has no BreathingRoom-accepted files");
        return UPLOAD_OK;   /* nothing to send — not an error */
    }

    char *body = NULL;
    int status = br_send_files(paths, n, NULL, BR_IMPORTS_PATH,
                               "vendor", "resmed", &body);
    upload_result_t r = br_map_status(status);
    if (r == UPLOAD_OK) {
        br_import_result_t res;
        if (body && br_parse_import(body, &res) && res.deferred == 0) {
            ESP_LOGI(TAG, "  root bundle: %d file(s)", n);
        } else {
            r = UPLOAD_ERR_TRANSIENT;
        }
    } else if (body) {
        ESP_LOGW(TAG, "  bundle HTTP %d: %.200s", status, body);
    }
    free(body);
    return r;
}

/* Oximetry: the ORIGINAL vendor file(s) under source/ (source.bin /
 * source.vld — never the canonical vitals.snt). One ref carries one
 * recording; the API's 25-file cap is comfortably met. The returned
 * recording id is persisted into ox state via upload_ox_mark so a
 * future DELETE can find it; the scheduler's own mark (NULL remote)
 * preserves it. */
static upload_result_t br_put_oximetry(const upload_ox_ref_t *ref)
{
    if (!s_tls || !ref) return UPLOAD_ERR_TRANSIENT;
    if (s_ox_blocked) return UPLOAD_ERR_PERMANENT;

    const char *paths[UPLOAD_OX_MAX_FILES];
    const char *filenames[UPLOAD_OX_MAX_FILES];
    char names[UPLOAD_OX_MAX_FILES][128];
    int n = 0;
    if (ref->n_files < 0 || ref->n_files > UPLOAD_OX_MAX_FILES)
        return UPLOAD_ERR_TRANSIENT;
    for (int i = 0; i < ref->n_files; i++) {
        const char *rel = ref->relative_paths[i];
        if (strcmp(rel, "source/source.bin") != 0 &&
            strcmp(rel, "source/source.vld") != 0) continue;
        if (!ref->source_name[0]) return UPLOAD_ERR_TRANSIENT;
        const char *ext = strrchr(rel, '.');
        const char *name_ext = strrchr(ref->source_name, '.');
        bool has_ext = name_ext &&
                       (strcasecmp(name_ext, ".bin") == 0 || strcasecmp(name_ext, ".vld") == 0);
        int len = snprintf(names[n], sizeof(names[n]), "%s%s", ref->source_name,
                           has_ext ? "" : ext);
        if (len < 0 || len >= (int)sizeof(names[n])) return UPLOAD_ERR_TRANSIENT;
        for (int j = 0; j < n; j++)
            if (strcmp(names[j], names[n]) == 0) return UPLOAD_ERR_TRANSIENT;
        filenames[n] = names[n];
        paths[n++] = ref->local_paths[i];
    }
    if (n == 0) {
        ESP_LOGW(TAG, "  oximetry %s: no source file in ref",
                 ref->recording_id);
        return UPLOAD_ERR_UNIT_REJECTED;
    }

    char *body = NULL;
    int status = br_send_files(paths, n, filenames, BR_OX_PATH, NULL, NULL, &body);
    if (status == 402) {
        /* Premium gate: log once per pass, then every remaining ox unit
         * fails fast without burning a request. */
        s_ox_blocked = true;
        ESP_LOGW(TAG, "oximetry needs a BreathingRoom Premium account "
                      "(402) — skipping O2 uploads for this pass");
        free(body);
        return UPLOAD_ERR_PERMANENT;
    }
    upload_result_t r = br_map_status(status);
    if (r == UPLOAD_OK) {
        br_ox_result_t res;
        if (!body || !br_parse_oximetry_files(body, &res, filenames, n) ||
            res.n_results != n) {
            ESP_LOGW(TAG, "  oximetry %s: unparseable response",
                     ref->recording_id);
            free(body);
            return UPLOAD_ERR_TRANSIENT;
        }
        if (res.n_rejected > 0) {
            ESP_LOGW(TAG, "  oximetry %s rejected: %s",
                     ref->recording_id,
                     res.first_reason[0] ? res.first_reason : "?");
            free(body);
            /* "storage error" is server-side — worth retrying; genuine
             * rejections (parse, timestamp mismatch, oversize) are not. */
            if (res.first_reason[0] &&
                strstr(res.first_reason, "storage error"))
                return UPLOAD_ERR_TRANSIENT;
            return UPLOAD_ERR_UNIT_REJECTED;
        }
        ESP_LOGI(TAG, "  oximetry %s: %d file(s) imported (id %s)",
                 ref->recording_id, res.n_imported,
                 res.first_id[0] ? res.first_id : "-");
        if (res.first_id[0]) {
            /* Scheduler marks this unit right after we return; it passes
             * NULL remote_id, which leaves whatever we store here intact. */
            upload_ox_mark(ref, upload_index_backend_slot("breathingroom"),
                           UG_OK, res.first_id);
        }
        free(body);
        return UPLOAD_OK;
    }
    if (body) ESP_LOGW(TAG, "  oximetry HTTP %d: %.200s", status, body);
    free(body);
    return r;
}

/* ── "Test connection" (web UI) ───────────────────────────────────────
 * A zero-file POST on the OXIMETRY route is the most informative probe:
 *   400 "No files provided"  token authenticates and may write
 *   401                      invalid / revoked token
 *   403                      read-only token
 *   402                      valid token on a free account — CPAP would
 *                            still upload, oximetry needs Premium
 * Own connection object — runs on the httpd task, never NVS. */
static bool br_test(const uploader_config_t *cfgp, char *msg, size_t msg_len)
{
    uploader_config_t cfg = *cfgp;   /* by value — never NVS */
    if (!cfg.br_key[0]) {
        snprintf(msg, msg_len, "Access token is required");
        return false;
    }
    if (strncmp(cfg.br_key, "slp_", 4) != 0) {
        snprintf(msg, msg_len,
                 "Access tokens start with \"slp_\" — check you pasted the whole token");
        return false;
    }

    esp_tls_t *tls = uph_connect(BR_HOST, BR_TEST_TIMEOUT_MS);
    if (!tls) {
        snprintf(msg, msg_len, "Cannot reach %s: check the internet connection",
                 BR_HOST);
        return false;
    }

    bool pc = false;
    char *body = NULL;
    int status = uph_put_files(tls, BR_HOST, BR_OX_PATH, cfg.br_key,
                               BR_ACCEPT, NULL, 0, NULL, NULL,
                               &body, 4096, &pc);
    uph_disconnect(tls);
    ESP_LOGI(TAG, "breathingroom: connection test finished with %u bytes "
             "of stack headroom", (unsigned)uxTaskGetStackHighWaterMark(NULL));

    bool ok = false;
    switch (status) {
    case 400:   /* "No files provided" — token accepted and writable */
        snprintf(msg, msg_len, "Token accepted");
        ok = true;
        break;
    case 401:
        snprintf(msg, msg_len, "Token rejected (HTTP 401)");
        break;
    case 403:
        snprintf(msg, msg_len, "Read-only token — cannot upload (HTTP 403)");
        break;
    case 402:
        snprintf(msg, msg_len, "Token accepted — oximetry needs a "
                 "BreathingRoom Premium account (CPAP still uploads)");
        ok = true;   /* the backend is usable: CPAP is free at every tier */
        break;
    case -1:
        snprintf(msg, msg_len, "No response from %s", BR_HOST);
        break;
    default:
        snprintf(msg, msg_len, "Unexpected HTTP %d from %s", status, BR_HOST);
        break;
    }
    free(body);
    return ok;
}

/* SomnoStage sleep stages: one `<unit_id>_stages.sst` per request, sent
 * byte-for-byte.  The file carries UTC only; the server derives the local
 * offset from the unit id unless epoch 0 sits >5 min away from the id's local
 * stamp, in which case we send the device's IANA zone (an unknown zone name
 * is a 400 for the whole request, so it is only sent when it looks valid). */
static upload_result_t br_put_stage(const upload_stage_ref_t *ref)
{
    if (!s_tls || !ref) return UPLOAD_ERR_TRANSIENT;
    if (!upload_stage_ref_has_fmt(ref, UPLOAD_STAGE_FMT_SST))
        return UPLOAD_ERR_UNIT_REJECTED;
    if (s_stage_blocked) return UPLOAD_ERR_PERMANENT;

    char filename[UPLOAD_STAGE_ID_LEN + 16];
    upload_stage_sst_filename(ref, filename, sizeof(filename));
    if (!filename[0] || !ref->sst_path[0]) return UPLOAD_ERR_UNIT_REJECTED;

    const char *tz_field = NULL, *tz_value = NULL;
    char tz[40];
    if (upload_stage_needs_timezone(ref)) {
        uploader_tz_name(tz, sizeof(tz));
        if (strchr(tz, '/')) {
            tz_field = "timeZone";
            tz_value = tz;
            ESP_LOGI(TAG, "  stages %s: epoch 0 is not derivable from the "
                     "unit id — sending timeZone %s", ref->unit_id, tz);
        }
    }

    const char *paths[1] = { ref->sst_path };
    const char *names[1] = { filename };
    char *body = NULL;
    int status = br_send_files(paths, 1, names, BR_SLEEP_PATH,
                               tz_field, tz_value, &body);
    if (status == 402) {
        s_stage_blocked = true;
        ESP_LOGW(TAG, "sleep stages need a BreathingRoom Premium account "
                      "(402) — skipping stage uploads for this pass");
        free(body);
        return UPLOAD_ERR_PERMANENT;
    }
    upload_result_t r = br_map_status(status);
    if (r == UPLOAD_OK) {
        br_ox_result_t res;
        if (!body || !br_parse_stage_files(body, &res, names, 1) ||
            res.n_results != 1) {
            ESP_LOGW(TAG, "  stages %s: unparseable response", ref->unit_id);
            free(body);
            return UPLOAD_ERR_TRANSIENT;
        }
        if (res.n_rejected > 0) {
            ESP_LOGW(TAG, "  stages %s rejected: %s", ref->unit_id,
                     res.first_reason[0] ? res.first_reason : "?");
            free(body);
            /* Only a storage error is worth retrying; every other reason is
             * permanent for that file (bad name, unsupported version, out of
             * range, partial run, unknown source type, …). */
            if (res.first_reason[0] && strstr(res.first_reason, "storage error"))
                return UPLOAD_ERR_TRANSIENT;
            return UPLOAD_ERR_UNIT_REJECTED;
        }
        ESP_LOGI(TAG, "  stages %s: uploaded (id %s)", ref->unit_id,
                 res.first_id[0] ? res.first_id : "-");
        if (res.first_id[0]) {
            /* Scheduler marks this unit right after we return; it passes
             * NULL remote_id, which leaves the id stored here intact. */
            upload_stage_mark(ref, upload_index_backend_slot("breathingroom"),
                              UG_OK, res.first_id);
        }
        free(body);
        return UPLOAD_OK;
    }
    if (body) ESP_LOGW(TAG, "  stages HTTP %d: %.200s", status, body);
    free(body);
    return r;
}

const upload_backend_t breathingroom_backend = {
    .id = "breathingroom",
    .label = "BreathingRoom",
    .bundle_only_ok = true,    /* settings-only upload is meaningful */
    .supports_ox = true,
    .supports_stage = true,
    .stage_fmt = UPLOAD_STAGE_FMT_SST,
    .is_configured = br_is_configured,
    .session_begin = br_session_begin,
    .day_begin = NULL,          /* requests are independent — no day scope */
    .put_group = br_put_group,
    .ox_day_begin = NULL,
    .put_oximetry = br_put_oximetry,
    .ox_day_end = NULL,
    .stage_day_begin = NULL,    /* one request per unit — no day scope */
    .put_stage = br_put_stage,
    .stage_day_end = NULL,
    .put_bundle = br_put_bundle,
    .day_end = NULL,
    .session_end = br_session_end,
    .test = br_test,
};
