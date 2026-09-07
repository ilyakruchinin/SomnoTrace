/*
 * SomnoTrace - Upload scheduler (triggers, per-backend state machine)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "upload_sched.h"
#include "upload_index.h"
#include "upload_scan.h"
#include "upload_ox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "up_sched";

/* ── Tunables ─────────────────────────────────────────────────────── */

#define SCAN_INTERVAL_MS      600000   /* 10 min self-healing scan          */
#define FIRST_SCAN_DELAY_MS    60000   /* let Wi-Fi/NTP settle after boot   */
#define FAILS_BEFORE_SWITCH        2   /* then move to the next backend     */
#define LEASE_WAIT_MS           5000

/* Per-backend cooldown ladder, minutes. Reset on any success. */
static const int COOLDOWN_MIN[] = { 1, 5, 15, 30, 60 };
#define N_COOLDOWN (int)(sizeof(COOLDOWN_MIN) / sizeof(COOLDOWN_MIN[0]))

/* The backends run on this task: shq_http_request() alone puts a 2 KB
 * request buffer on the stack. The per-stop task this replaced used 12288, so
 * keep that proven figure rather than discovering the limit in the field. */
#define SCHED_TASK_STACK     12288
#define SCHED_QUEUE_LEN          8

_Static_assert(UPLOADER_PROGRESS_MAX_BACKENDS >= UPLOAD_MAX_BACKENDS,
               "public progress snapshot is too small for scheduler slots");

/* ── Backend runtime ──────────────────────────────────────────────── */

typedef enum {
    SB_DISABLED = 0,   /* not configured                                   */
    SB_IDLE,           /* configured, nothing to do                        */
    SB_UPLOADING,
    SB_COOLDOWN,       /* waiting out a failure                            */
} sb_state_t;

typedef struct {
    const upload_backend_t *be;
    int      slot;
    sb_state_t state;
    int      cooldown_idx;
    int64_t  retry_at_us;      /* monotonic                                */
    uint32_t last_ok_s;        /* epoch seconds of last successful unit     */
    bool     last_err_permanent;
    char     err[72];
    /* live progress, valid while SB_UPLOADING */
    char     cur_day[12];
    int      cur_unit;
    int      n_units;
    /* Aggregate upload-index progress is computed only by the scheduler and
     * copied under s_lock.  API callers must not walk upload_index's mutable
     * s_days array while a scan/reset is replacing it. */
    int      days_done;
    int      days_total;
} backend_rt_t;

static backend_rt_t s_rt[UPLOAD_MAX_BACKENDS];
static int s_n_rt = 0;

/* ── Module state ─────────────────────────────────────────────────── */

typedef enum { EV_EXPORT = 0, EV_INVALIDATE, EV_SCAN, EV_RESET } ev_type_t;

typedef struct {
    uint8_t  type;
    uint32_t day;
} sched_ev_t;

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;
static SemaphoreHandle_t s_lock;      /* guards s_rt + s_status for the API */

static upload_sched_busy_fn_t s_busy_fn;

static int64_t s_next_scan_us;
static bool    s_scanning;
static int s_progress_max_days;
static int s_summary_pending;
static char    s_status[64] = "Starting up";

/* ── Helpers ──────────────────────────────────────────────────────── */

static int64_t now_us(void) { return esp_timer_get_time(); }
static uint32_t now_s(void) { return (uint32_t)time(NULL); }

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    xSemaphoreGive(s_lock);
    va_end(ap);
}

static void set_summary_pending(int pending)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_summary_pending = pending;
    xSemaphoreGive(s_lock);
}

/* Single choke point for backend state changes so the UI can be pushed an
 * update the moment anything transitions, rather than up to one poll period
 * later.  Only real transitions notify — re-assigning the same state is
 * common in the scan loop and must not generate traffic. */
static void set_be_state(backend_rt_t *r, sb_state_t st)
{
    bool changed = false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (r->state != st) {
        r->state = st;
        changed = true;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    if (changed) uploader_notify_progress_changed();
}

static backend_rt_t *rt_for(const upload_backend_t *be)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n_rt; i++) {
        if (s_rt[i].be == be) {
            backend_rt_t *r = &s_rt[i];
            if (s_lock) xSemaphoreGive(s_lock);
            return r;
        }
    }
    if (s_n_rt >= UPLOAD_MAX_BACKENDS) {
        if (s_lock) xSemaphoreGive(s_lock);
        return NULL;
    }

    /* Slot lookup may inspect persistent upload-index state.  Do it without
     * holding the runtime snapshot lock, then re-check before publishing the
     * new slot in case this function ever gains a second caller. */
    if (s_lock) xSemaphoreGive(s_lock);
    int slot = upload_index_backend_slot(be->id);

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n_rt; i++) {
        if (s_rt[i].be == be) {
            backend_rt_t *r = &s_rt[i];
            if (s_lock) xSemaphoreGive(s_lock);
            return r;
        }
    }
    if (s_n_rt >= UPLOAD_MAX_BACKENDS) {
        if (s_lock) xSemaphoreGive(s_lock);
        return NULL;
    }
    backend_rt_t *r = &s_rt[s_n_rt++];
    memset(r, 0, sizeof(*r));
    r->be = be;
    r->slot = slot;
    r->state = SB_IDLE;
    if (s_lock) xSemaphoreGive(s_lock);
    return r;
}

static void cooldown_enter(backend_rt_t *r, const char *why, bool permanent)
{
    int mins = COOLDOWN_MIN[r->cooldown_idx];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (r->cooldown_idx < N_COOLDOWN - 1) r->cooldown_idx++;
    r->retry_at_us = now_us() + (int64_t)mins * 60 * 1000000LL;
    r->last_err_permanent = permanent;
    if (why && why != r->err)
        strlcpy(r->err, why, sizeof(r->err));
    xSemaphoreGive(s_lock);
    set_be_state(r, SB_COOLDOWN);
    ESP_LOGW(TAG, "%s: cooldown %d min (%s)", r->be->id, mins,
             why ? why : "error");
}

static void cooldown_reset(backend_rt_t *r)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    r->cooldown_idx = 0;
    r->retry_at_us = 0;
    r->err[0] = '\0';
    r->last_err_permanent = false;
    xSemaphoreGive(s_lock);
}

static void set_be_error(backend_rt_t *r, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(r->err, error ? error : "", sizeof(r->err));
    xSemaphoreGive(s_lock);
}

static void set_next_scan(int64_t at_us)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_next_scan_us = at_us;
    xSemaphoreGive(s_lock);
}

static void set_scanning(bool scanning)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scanning = scanning;
    xSemaphoreGive(s_lock);
}

/* upload_index is scheduler-owned after boot, but progress snapshots are read
 * concurrently by the display, HTTP server, and WebSocket forwarder.  Keep
 * the public path off the index entirely: the scheduler computes these small
 * aggregates after each operation that can change the index, then publishes
 * them under the same lock as the rest of the runtime snapshot. */
static void refresh_index_progress_cache(void)
{
    int slots[UPLOAD_MAX_BACKENDS] = {0};
    int done[UPLOAD_MAX_BACKENDS] = {0};
    int total[UPLOAD_MAX_BACKENDS] = {0};
    int n_runtime;
    int max_days = uploader_max_days();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    n_runtime = s_n_rt < UPLOAD_MAX_BACKENDS ? s_n_rt : UPLOAD_MAX_BACKENDS;
    for (int i = 0; i < n_runtime; ++i) slots[i] = s_rt[i].slot;
    xSemaphoreGive(s_lock);

    for (int i = 0; i < n_runtime; ++i) {
        upload_index_backend_progress(slots[i], max_days, &done[i], &total[i]);
    }

    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_progress_max_days != max_days) changed = true;
    s_progress_max_days = max_days;
    for (int i = 0; i < n_runtime; ++i) {
        if (s_rt[i].days_done != done[i] || s_rt[i].days_total != total[i])
            changed = true;
        s_rt[i].days_done = done[i];
        s_rt[i].days_total = total[i];
    }
    xSemaphoreGive(s_lock);

    if (changed) uploader_notify_progress_changed();
}

static void ox_mark_day_failed(upload_ox_ref_t *refs, int n_refs, int slot,
                               const char *day)
{
    for (int i = 0; i < n_refs; i++) {
        if (strcmp(refs[i].day, day) != 0) continue;
        if (upload_ox_status(&refs[i], slot) == UG_OK)
            upload_ox_mark(&refs[i], slot, UG_FAILED, NULL);
    }
}

/* ── One backend pass ─────────────────────────────────────────────────
 * Returns true if the backend did any work (so the caller can log/report). */

static bool run_backend(backend_rt_t *r, int max_days)
{
    const upload_backend_t *be = r->be;

    /* Which days still have pending groups for this backend? Newest first,
     * which is the order the index already keeps. */
    /* static: only the scheduler task ever runs this, and 366 entries have no
     * business on the stack alongside the backends' own buffers. */
    static uint32_t days[UPLOAD_MAX_DAYS_CAP];
    int n_days = 0;
    int n_units = 0;
    int n_index = upload_index_day_count();
    for (int i = 0; i < n_index && i < max_days; i++) {
        upload_day_t *d = upload_index_day_at(i);
        if (!d) continue;
        int pend = 0;
        for (int g = 0; g < d->n_groups; g++) {
            if (d->groups[g].be[r->slot].status != UG_OK) pend++;
        }
        if (pend > 0) {
            days[n_days++] = d->day;
            n_units += pend;
        }
    }

    /* Bundle changes on every export (STR.edf is cumulative), so a changed
     * bundle alone is reason enough to connect for SMB. */
    upload_bundle_ref_t bundle;
    bool have_bundle = upload_scan_bundle(&bundle);
    bool bundle_changed = have_bundle &&
                          (upload_index_bundle_ok_fp(r->slot) != bundle.fp);
    upload_ox_ref_t *ox_refs = heap_caps_malloc(sizeof(upload_ox_ref_t) * UPLOAD_OX_MAX_UNITS, MALLOC_CAP_SPIRAM);
    if (!ox_refs) { set_be_state(r, SB_IDLE); return false; }
    int n_ox = upload_ox_reconcile(ox_refs, UPLOAD_OX_MAX_UNITS, max_days);
    int ox_pending = upload_ox_pending(ox_refs, n_ox, r->slot);

    if (n_days == 0 && !bundle_changed && ox_pending == 0) {
        free(ox_refs);
        set_be_state(r, SB_IDLE);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        r->cur_day[0] = '\0';
        xSemaphoreGive(s_lock);
        return false;
    }
    if (!have_bundle && (n_days > 0 || bundle_changed)) {
        /* CPAP session uploads still require their root bundle. Oximetry
         * packages are self-contained and may upload before any EDF exists. */
        free(ox_refs);
        set_be_state(r, SB_IDLE);
        return false;
    }

    /* Nothing but the bundle changed.  Backends that can take it cheaply get
     * it now; the rest wait for the next session upload to carry it, so a
     * short session that produced no EDFs does not cause a pointless visit. */
    bool bundle_only_run = false;
    if (n_days == 0 && ox_pending == 0) {
        if (!be->bundle_only_ok) {
            ESP_LOGI(TAG, "%s: only root files changed — deferring to the "
                     "next session upload", be->id);
            free(ox_refs);
            set_be_state(r, SB_IDLE);
            return false;
        }

        /* The attachment day must come from the INDEX, not from the card.
         * Taking the newest DATALOG folder picked up day folders that hold no
         * groups (an aborted short session used to leave one behind), and the
         * loop below then found nothing to do — so the run connected, sent
         * nothing, and never recorded the bundle as uploaded, which made it
         * repeat on every trigger.  The index only ever contains days that
         * really have groups. */
        uint32_t attach = 0;
        int n_idx = upload_index_day_count();
        for (int i = 0; i < n_idx && i < max_days; i++) {   /* newest first */
            upload_day_t *d = upload_index_day_at(i);
            if (d && d->n_groups > 0) { attach = d->day; break; }
        }
        if (attach == 0) {
            ESP_LOGI(TAG, "%s: root files changed but no exported day to attach "
                     "them to — nothing to do", be->id);
            free(ox_refs);
            set_be_state(r, SB_IDLE);
            return false;
        }
        days[n_days++] = attach;
        bundle_only_run = true;
    }

    set_be_state(r, SB_UPLOADING);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    r->n_units = n_units + ox_pending;
    r->cur_unit = 0;
    xSemaphoreGive(s_lock);

    /* Say plainly which of the two kinds of run this is.  The old message
     * reported "1 day(s), 0 unit(s) pending" for a root-files-only run, which
     * read like a contradiction. */
    if (bundle_only_run) {
        ESP_LOGI(TAG, "%s: root files changed, attaching to day %08u",
                 be->id, (unsigned)days[0]);
    } else {
        ESP_LOGI(TAG, "%s: %d day(s), %d unit(s) pending%s", be->id, n_days,
                 n_units, bundle_changed ? ", root files changed too" : "");
    }

    upload_result_t res = be->session_begin ? be->session_begin() : UPLOAD_OK;
    if (res != UPLOAD_OK) {
        cooldown_enter(r, res == UPLOAD_ERR_PERMANENT ? "auth/config rejected"
                                                      : "cannot connect",
                       res == UPLOAD_ERR_PERMANENT);
        if (be->session_end) be->session_end();
        free(ox_refs);
        return true;
    }

    int fails = 0;
    bool any_ok = false;
    bool deferred_busy = false;
    /* Recorded here rather than inside the day loop: a `continue` in that loop
     * used to skip the fingerprint update, leaving the bundle permanently
     * "changed" and the backend reconnecting forever. */
    bool bundle_committed = false;

    upload_group_ref_t *refs = heap_caps_calloc(UPLOAD_MAX_GROUPS_PER_DAY, sizeof(*refs), MALLOC_CAP_SPIRAM);
    if (!refs) refs = calloc(UPLOAD_MAX_GROUPS_PER_DAY, sizeof(*refs));
    if (!refs) {
        if (be->session_end) be->session_end();
        free(ox_refs);
        return false;
    }

    for (int di = 0; di < n_days && fails < FAILS_BEFORE_SWITCH; di++) {
        char daystr[12];
        snprintf(daystr, sizeof(daystr), "%08u", (unsigned)days[di]);
        /* Read by the progress endpoint on the httpd task. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(r->cur_day, daystr, sizeof(r->cur_day));
        xSemaphoreGive(s_lock);

        upload_day_t *d = upload_index_day(days[di], false);
        if (!d) {
            /* days[] is built from the index, so this means the day vanished
             * between building the list and getting here. */
            ESP_LOGW(TAG, "%s: day %s no longer tracked, skipping", be->id, daystr);
            continue;
        }

        /* Storage lease is scoped to reading the day's EDF files on the SD card.
         * Taking it per-day avoids blocking exports during long multi-day backlog runs. */
        if (!uploader_lease_take(LEASE_WAIT_MS)) {
            ESP_LOGI(TAG, "%s: storage busy, deferring day %s", be->id, daystr);
            deferred_busy = true;
            continue;
        }

        int n_refs = upload_scan_day_groups(daystr, refs, UPLOAD_MAX_GROUPS_PER_DAY);
        if (n_refs == 0) {
            /* Files disappeared since the last scan; the next scan will drop
             * the day from the index. */
            ESP_LOGW(TAG, "%s: day %s has no EDF files on the card, skipping",
                     be->id, daystr);
            uploader_lease_give();
            continue;
        }

        res = be->day_begin ? be->day_begin(daystr) : UPLOAD_OK;
        if (res != UPLOAD_OK) {
            cooldown_enter(r, "cannot open remote day", res == UPLOAD_ERR_PERMANENT);
            fails = FAILS_BEFORE_SWITCH;
            uploader_lease_give();
            break;
        }

        bool day_any = false;
        for (int gi = 0; gi < n_refs && fails < FAILS_BEFORE_SWITCH; gi++) {
            upload_group_t *g = upload_index_group(d, refs[gi].prefix_sec, false);
            if (!g || g->be[r->slot].status == UG_OK) continue;

            res = be->put_group(daystr, &refs[gi]);
            g->be[r->slot].attempts++;
            g->be[r->slot].last_try_s = now_s();

            if (res == UPLOAD_OK) {
                /* Only now is the unit durable-good: every file landed. */
                g->be[r->slot].status = UG_OK;
                day_any = true;
                any_ok = true;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                r->cur_unit++;
                xSemaphoreGive(s_lock);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                r->last_ok_s = now_s();
                xSemaphoreGive(s_lock);
            } else {
                g->be[r->slot].status = UG_FAILED;
                fails++;
                ESP_LOGW(TAG, "%s: group %s failed (%d/%d)", be->id,
                         refs[gi].prefix, fails, FAILS_BEFORE_SWITCH);
                if (res == UPLOAD_ERR_PERMANENT) {
                    cooldown_enter(r, "rejected by server", true);
                    fails = FAILS_BEFORE_SWITCH;
                }
            }
            d->dirty = true;
        }

        /* Offer the bundle whenever this day sent something (SleepHQ needs it
         * inside the import) or when it changed (SMB). */
        bool bundle_ok = true;
        bool bundle_pushed = false;
        if (day_any || bundle_changed) {
            res = be->put_bundle ? be->put_bundle(daystr, &bundle, bundle_changed)
                                 : UPLOAD_OK;
            bundle_pushed = true;
            if (res != UPLOAD_OK) {
                bundle_ok = false;
                fails++;
                ESP_LOGW(TAG, "%s: bundle failed for %s", be->id, daystr);
                set_be_error(r, "root files failed");
            }
        }

        /* All EDF files on SD card for this day have been read and sent.
         * Release the storage lease before day_end, so network polling
         * (shq_wait_import can take up to 60s) does not block session exports. */
        uploader_lease_give();

        res = be->day_end ? be->day_end(daystr, day_any) : UPLOAD_OK;
        if (res != UPLOAD_OK) {
            /* Finalisation failed (e.g. SleepHQ process_files): the files may
             * be there but the import is not processed, so do not claim the
             * day. Roll the day's units back to pending. */
            ESP_LOGW(TAG, "%s: finalise failed for %s — day stays pending",
                     be->id, daystr);
            for (int g = 0; g < d->n_groups; g++) {
                if (d->groups[g].be[r->slot].status == UG_OK)
                    d->groups[g].be[r->slot].status = UG_PENDING;
            }
            bundle_ok = false;
            fails++;
            set_be_error(r, "remote finalise failed");
        }

        upload_index_save_day(d);

        /* The bundle counts as delivered only once a day that carried it also
         * finalised cleanly (for SleepHQ that means its import was processed). */
        if (bundle_ok && bundle_pushed) bundle_committed = true;
    }

    /* Oximetry packages are self-contained and are tracked independently from
     * EDF groups. A backend connection is reused, but each noon-day gets its
     * own transport scope so SleepHQ can create one O2 import per day. */
    if (ox_pending > 0 && be->put_oximetry) {
        char ox_day[12] = {0};
        bool ox_day_any = false;
        for (int oi = 0; oi < n_ox && fails < FAILS_BEFORE_SWITCH; oi++) {
            if (upload_ox_status(&ox_refs[oi], r->slot) == UG_OK) continue;
            if (strcmp(ox_day, ox_refs[oi].day) != 0) {
                if (ox_day_any && (be->ox_day_end || be->day_end)) {
                    res = be->ox_day_end ? be->ox_day_end(ox_day, true) :
                          be->day_end(ox_day, true);
                    if (res != UPLOAD_OK) {
                        ox_mark_day_failed(ox_refs, n_ox, r->slot, ox_day);
                        fails++;
                        set_be_error(r, "oximetry finalise failed");
                    }
                }
                strlcpy(ox_day, ox_refs[oi].day, sizeof(ox_day));
                ox_day_any = false;
                res = be->ox_day_begin ? be->ox_day_begin(ox_day) :
                      (be->day_begin ? be->day_begin(ox_day) : UPLOAD_OK);
                if (res != UPLOAD_OK) {
                    fails++;
                    set_be_error(r, "cannot open oximetry day");
                    break;
                }
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(r->cur_day, ox_refs[oi].day, sizeof(r->cur_day));
            xSemaphoreGive(s_lock);

            bool ox_leased = uploader_lease_take(LEASE_WAIT_MS);
            if (!ox_leased) {
                ESP_LOGI(TAG, "%s: storage busy, deferring oximetry %s",
                         be->id, ox_refs[oi].recording_id);
                deferred_busy = true;
                continue;
            }
            res = be->put_oximetry(&ox_refs[oi]);
            uploader_lease_give();
            upload_ox_mark(&ox_refs[oi], r->slot,
                           res == UPLOAD_OK ? UG_OK : UG_FAILED, NULL);
            if (res == UPLOAD_OK) {
                ox_day_any = true;
                any_ok = true;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                r->cur_unit++;
                xSemaphoreGive(s_lock);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                r->last_ok_s = now_s();
                xSemaphoreGive(s_lock);
            } else {
                fails++;
                ESP_LOGW(TAG, "%s: oximetry %s failed", be->id,
                         ox_refs[oi].recording_id);
            }
        }
        if (ox_day_any && ox_day[0] && (be->ox_day_end || be->day_end)) {
            res = be->ox_day_end ? be->ox_day_end(ox_day, true) :
                  be->day_end(ox_day, true);
            if (res != UPLOAD_OK) {
                ox_mark_day_failed(ox_refs, n_ox, r->slot, ox_day);
                fails++;
                set_be_error(r, "oximetry finalise failed");
            }
        }
    }

    free(refs);
    free(ox_refs);
    if (be->session_end) be->session_end();

    if (bundle_committed) {
        upload_index_set_bundle_ok(r->slot, bundle.fp);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    r->cur_day[0] = '\0';
    xSemaphoreGive(s_lock);

    if (fails > 0) {
        if (r->state != SB_COOLDOWN)
            cooldown_enter(r, r->err[0] ? r->err : "upload failed", false);
    } else if (!any_ok && !bundle_committed) {
        if (deferred_busy) {
            ESP_LOGI(TAG, "%s: storage busy during upload, deferring without cooldown", be->id);
            set_be_state(r, SB_IDLE);
        } else {
            /* We connected and transferred nothing.  That is not success — it means
             * the work list and the card disagreed — and if it were reported as
             * success the backend would reconnect on every trigger forever.  Back
             * off and say so; the next scan reconciles the disagreement. */
            ESP_LOGW(TAG, "%s: connected but transferred nothing", be->id);
            cooldown_enter(r, "nothing to send (state out of sync)", false);
        }
    } else {
        cooldown_reset(r);
        set_be_state(r, SB_IDLE);
        ESP_LOGI(TAG, "%s: up to date%s", be->id,
                 bundle_committed && !any_ok ? " (root files)" : "");
    }
    return true;
}

/* ── Scheduling pass over all backends ────────────────────────────── */

static void run_pass(void)
{
    const upload_backend_t *bes[UPLOAD_MAX_BACKENDS];
    int n = uploader_enabled_backends(bes, UPLOAD_MAX_BACKENDS);
    int max_days = uploader_max_days();

    if (n == 0) {
        refresh_index_progress_cache();
        set_summary_pending(0);
        set_status("No upload backend configured");
        return;
    }

    for (int i = 0; i < n; i++) {
        backend_rt_t *r = rt_for(bes[i]);
        if (!r || r->slot < 0) continue;

        if (!bes[i]->is_configured || !bes[i]->is_configured()) {
            set_be_state(r, SB_DISABLED);
            continue;
        }
        if (r->state == SB_COOLDOWN && now_us() < r->retry_at_us) continue;

        set_status("Uploading to %s", bes[i]->label ? bes[i]->label : bes[i]->id);
        run_backend(r, max_days);
    }

    refresh_index_progress_cache();

    /* Summarise for the UI. */
    int pending = 0;
    bool cooling = false;
    upload_ox_ref_t *ox_refs = heap_caps_malloc(sizeof(upload_ox_ref_t) * UPLOAD_OX_MAX_UNITS, MALLOC_CAP_SPIRAM);
    if (!ox_refs) {
        set_summary_pending(0);
        set_status("Memory low");
        return;
    }
    if (!uploader_lease_take(LEASE_WAIT_MS)) {
        free(ox_refs);
        set_status("Storage busy");
        return;
    }
    int n_ox = upload_ox_reconcile(ox_refs, UPLOAD_OX_MAX_UNITS, max_days);
    uploader_lease_give();
    for (int i = 0; i < s_n_rt; i++) {
        if (s_rt[i].state == SB_DISABLED) continue;
        pending += upload_index_backend_pending(s_rt[i].slot, max_days);
        pending += upload_ox_pending(ox_refs, n_ox, s_rt[i].slot);
        if (s_rt[i].state == SB_COOLDOWN) cooling = true;
    }
    free(ox_refs);
    set_summary_pending(pending);
    if (pending == 0) set_status("All uploaded");
    else if (cooling)  set_status("%d parts pending — waiting to retry", pending);
    else               set_status("%d parts pending", pending);
}

/* ── Task ─────────────────────────────────────────────────────────── */

/* Reconcile one day's exported files against the index.  Holds the storage
 * lease: the reconcile walks the card with opendir()/readdir(), and without
 * the lease it can run while sd_storage_format() is calling f_mkfs on the same
 * volume, which corrupts the SDMMC driver and panics this task. */
static void reconcile_day_leased(uint32_t day)
{
    if (!uploader_lease_take(LEASE_WAIT_MS)) {
        ESP_LOGD(TAG, "reconcile deferred: storage busy");
        return;
    }
    upload_scan_reconcile_day(day);
    refresh_index_progress_cache();
    uploader_lease_give();
}

static void do_scan(void)
{
    int max_days = uploader_max_days();
    const upload_backend_t *bes[UPLOAD_MAX_BACKENDS];
    int n = uploader_enabled_backends(bes, UPLOAD_MAX_BACKENDS);

    int slots[UPLOAD_MAX_BACKENDS];
    int n_slots = 0;
    for (int i = 0; i < n; i++) {
        if (!bes[i]->is_configured || !bes[i]->is_configured()) continue;
        backend_rt_t *r = rt_for(bes[i]);
        if (r && r->slot >= 0) slots[n_slots++] = r->slot;
    }
    if (n_slots == 0) {
        refresh_index_progress_cache();
        set_next_scan(now_us() + (int64_t)SCAN_INTERVAL_MS * 1000);
        return;
    }

    /* The card walk shares admission with upload and destructive operations. */
    if (!uploader_lease_take(LEASE_WAIT_MS)) {
        ESP_LOGD(TAG, "scan deferred: storage busy");
        set_next_scan(now_us() + (int64_t)SCAN_INTERVAL_MS * 1000);
        return;
    }

    set_scanning(true);
    set_status("Scanning for new data");
    upload_scan_reconcile_all(max_days, slots, n_slots);
    upload_ox_ref_t *ox_refs = heap_caps_malloc(sizeof(upload_ox_ref_t) * UPLOAD_OX_MAX_UNITS, MALLOC_CAP_SPIRAM);
    if (ox_refs) {
        upload_ox_reconcile(ox_refs, UPLOAD_OX_MAX_UNITS, max_days);
        free(ox_refs);
    }
    refresh_index_progress_cache();
    set_scanning(false);
    uploader_lease_give();
    set_next_scan(now_us() + (int64_t)SCAN_INTERVAL_MS * 1000);
}

static void sched_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "upload scheduler started on core %d", xPortGetCoreID());

    set_next_scan(now_us() + (int64_t)FIRST_SCAN_DELAY_MS * 1000);
    set_status("Waiting for first scan");

    while (1) {
        /* Sleep until the next scan or the earliest cooldown expiry. */
        int64_t wake_us = s_next_scan_us;
        for (int i = 0; i < s_n_rt; i++) {
            if (s_rt[i].state == SB_COOLDOWN && s_rt[i].retry_at_us > 0 &&
                s_rt[i].retry_at_us < wake_us) {
                wake_us = s_rt[i].retry_at_us;
            }
        }
        int64_t delay_us = wake_us - now_us();
        if (delay_us < 0) delay_us = 0;
        TickType_t wait = pdMS_TO_TICKS(delay_us / 1000);

        sched_ev_t ev;
        bool got = (xQueueReceive(s_queue, &ev, wait) == pdTRUE);

        if (got) {
            switch (ev.type) {
            case EV_EXPORT:
                ESP_LOGI(TAG, "export complete for %08u", (unsigned)ev.day);
                reconcile_day_leased(ev.day);
                run_pass();
                break;

            case EV_INVALIDATE:
                ESP_LOGI(TAG, "day %08u invalidated — will re-upload",
                         (unsigned)ev.day);
                upload_index_forget_day(ev.day);
                reconcile_day_leased(ev.day);
                run_pass();
                break;

            case EV_RESET:
                ESP_LOGW(TAG, "upload state reset — re-uploading newest %d day(s)",
                         uploader_max_days());
                upload_index_clear();
                refresh_index_progress_cache();
                for (int i = 0; i < s_n_rt; i++) {
                    cooldown_reset(&s_rt[i]);
                    set_be_state(&s_rt[i], SB_IDLE);
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    s_rt[i].last_ok_s = 0;
                    xSemaphoreGive(s_lock);
                }
                do_scan();
                run_pass();
                break;

            case EV_SCAN:
            default:
                do_scan();
                run_pass();
                break;
            }
            continue;
        }

        /* Timed out: either a scan is due or a cooldown expired. */
        if (now_us() >= s_next_scan_us) {
            if (s_busy_fn && s_busy_fn()) {
                /* A therapy recording has priority over a housekeeping scan;
                 * try again on the next tick rather than competing for the
                 * card. Event-driven uploads are unaffected. */
                ESP_LOGD(TAG, "scan deferred: storage busy");
                set_next_scan(now_us() + (int64_t)SCAN_INTERVAL_MS * 1000);
            } else {
                do_scan();
            }
        }
        run_pass();
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t upload_sched_init(void)
{
    if (s_task) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(SCHED_QUEUE_LEN, sizeof(sched_ev_t));
    upload_ox_init();
    if (!s_lock || !s_queue) return ESP_ERR_NO_MEM;

    /* Pre-create runtime slots so the progress API can report a backend
     * before its first run. */
    const upload_backend_t *bes[UPLOAD_MAX_BACKENDS];
    int n = uploader_enabled_backends(bes, UPLOAD_MAX_BACKENDS);
    for (int i = 0; i < n; i++) rt_for(bes[i]);
    refresh_index_progress_cache();

    StackType_t *stack = heap_caps_malloc(SCHED_TASK_STACK, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb  = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (stack && tcb) {
        s_task = xTaskCreateStaticPinnedToCore(sched_task, "up_sched",
                                               SCHED_TASK_STACK, NULL, 4,
                                               stack, tcb, 0);
    }
    if (!s_task) {
        ESP_LOGE(TAG, "failed to create scheduler task");
        free(stack);
        free(tcb);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void post(uint8_t type, uint32_t day)
{
    if (!s_queue) return;
    sched_ev_t ev = { .type = type, .day = day };
    xQueueSend(s_queue, &ev, 0);
}

void upload_sched_notify_export(uint32_t day)     { post(EV_EXPORT, day); }
void upload_sched_notify_invalidate(uint32_t day) { post(EV_INVALIDATE, day); }
void upload_sched_request_scan(void)              { post(EV_SCAN, 0); }
void upload_sched_request_reset(void)             { post(EV_RESET, 0); }

void upload_sched_set_busy_fn(upload_sched_busy_fn_t fn) { s_busy_fn = fn; }

bool upload_sched_uploading(void)
{
    for (int i = 0; i < s_n_rt; i++) {
        if (s_rt[i].be && s_rt[i].state == SB_UPLOADING) return true;
    }
    return false;
}

/* ── Progress reporting ───────────────────────────────────────────── */

static uploader_backend_state_t public_state(sb_state_t state)
{
    switch (state) {
    case SB_UPLOADING: return UPLOADER_BACKEND_UPLOADING;
    case SB_COOLDOWN:  return UPLOADER_BACKEND_COOLDOWN;
    case SB_DISABLED:  return UPLOADER_BACKEND_DISABLED;
    default:           return UPLOADER_BACKEND_IDLE;
    }
}

static const char *public_state_name(uploader_backend_state_t state)
{
    switch (state) {
    case UPLOADER_BACKEND_UPLOADING: return "uploading";
    case UPLOADER_BACKEND_COOLDOWN:  return "cooldown";
    case UPLOADER_BACKEND_DISABLED:  return "disabled";
    default:                         return "idle";
    }
}

typedef struct {
    const upload_backend_t *be;
    int slot;
    sb_state_t state;
    int64_t retry_at_us;
    uint32_t last_ok_s;
    bool last_err_permanent;
    char err[sizeof(((backend_rt_t *)0)->err)];
    char cur_day[sizeof(((backend_rt_t *)0)->cur_day)];
    int cur_unit;
    int n_units;
    int days_done;
    int days_total;
} backend_rt_snapshot_t;

esp_err_t upload_sched_progress_snapshot(uploader_progress_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    /* s_task is assigned last, after the lock and runtime slots exist. */
    if (!s_task) return ESP_ERR_INVALID_STATE;

    backend_rt_snapshot_t runtime[UPLOAD_MAX_BACKENDS] = {0};
    int n_runtime;
    int64_t next_scan_us;
    int64_t snapshot_us = now_us();
    int progress_max_days;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out->status, s_status, sizeof(out->status));
    out->scanning = s_scanning;
    next_scan_us = s_next_scan_us;
    progress_max_days = s_progress_max_days;
    n_runtime = s_n_rt < UPLOAD_MAX_BACKENDS ? s_n_rt : UPLOAD_MAX_BACKENDS;
    for (int i = 0; i < n_runtime; ++i) {
        const backend_rt_t *src = &s_rt[i];
        backend_rt_snapshot_t *dst = &runtime[i];
        dst->be = src->be;
        dst->slot = src->slot;
        dst->state = src->state;
        dst->retry_at_us = src->retry_at_us;
        dst->last_ok_s = src->last_ok_s;
        dst->last_err_permanent = src->last_err_permanent;
        strlcpy(dst->err, src->err, sizeof(dst->err));
        strlcpy(dst->cur_day, src->cur_day, sizeof(dst->cur_day));
        dst->cur_unit = src->cur_unit;
        dst->n_units = src->n_units;
        dst->days_done = src->days_done;
        dst->days_total = src->days_total;
    }
    xSemaphoreGive(s_lock);

    int64_t until = next_scan_us - snapshot_us;
    out->next_scan_s = until > 0 ? (uint32_t)(until / 1000000) : 0;
    out->max_days = progress_max_days;

    for (int i = 0; i < n_runtime; ++i) {
        const backend_rt_snapshot_t *src = &runtime[i];
        if (!src->be) continue;

        uploader_backend_progress_t *dst =
            &out->backends[out->backend_count++];
        strlcpy(dst->id, src->be->id ? src->be->id : "", sizeof(dst->id));
        strlcpy(dst->label,
                src->be->label ? src->be->label : src->be->id,
                sizeof(dst->label));
        dst->configured = src->be->is_configured &&
                          src->be->is_configured();
        dst->state = dst->configured ? public_state(src->state)
                                     : UPLOADER_BACKEND_DISABLED;

        dst->days_done = src->days_done;
        dst->days_total = src->days_total;

        dst->last_success_valid = src->last_ok_s != 0;
        dst->last_success_epoch_s = src->last_ok_s;

        dst->current_valid = dst->state == UPLOADER_BACKEND_UPLOADING &&
                             src->cur_day[0] != '\0';
        if (dst->current_valid) {
            strlcpy(dst->current_day, src->cur_day,
                    sizeof(dst->current_day));
            dst->current_unit = src->cur_unit;
            dst->current_units = src->n_units;
        }

        dst->retry_valid = dst->state == UPLOADER_BACKEND_COOLDOWN;
        if (dst->retry_valid) {
            int64_t retry_in_us = src->retry_at_us - snapshot_us;
            dst->retry_in_s = retry_in_us > 0
                                  ? (uint32_t)(retry_in_us / 1000000) : 0;
            dst->error_permanent = src->last_err_permanent;
            dst->error_valid = src->err[0] != '\0';
            if (dst->error_valid)
                strlcpy(dst->error, src->err, sizeof(dst->error));
        }
    }
    return ESP_OK;
}

esp_err_t upload_sched_progress_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;
    uploader_progress_snapshot_t progress;
    esp_err_t ret = upload_sched_progress_snapshot(&progress);
    if (ret != ESP_OK) return ret;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "status", progress.status);
    cJSON_AddBoolToObject(root, "scanning", progress.scanning);
    cJSON_AddNumberToObject(root, "next_scan_s", progress.next_scan_s);
    cJSON_AddNumberToObject(root, "max_days", progress.max_days);

    cJSON *arr = cJSON_AddArrayToObject(root, "backends");
    for (size_t i = 0; i < progress.backend_count; i++) {
        const uploader_backend_progress_t *p = &progress.backends[i];

        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", p->id);
        cJSON_AddStringToObject(o, "label", p->label);
        cJSON_AddBoolToObject(o, "configured", p->configured);
        cJSON_AddStringToObject(o, "state", public_state_name(p->state));
        cJSON_AddNumberToObject(o, "days_done", p->days_done);
        cJSON_AddNumberToObject(o, "days_total", p->days_total);

        if (p->last_success_valid)
            cJSON_AddNumberToObject(o, "last_ok_s", p->last_success_epoch_s);

        if (p->current_valid) {
            cJSON *cur = cJSON_AddObjectToObject(o, "cur");
            cJSON_AddStringToObject(cur, "day", p->current_day);
            cJSON_AddNumberToObject(cur, "unit", p->current_unit);
            cJSON_AddNumberToObject(cur, "units", p->current_units);
        }
        if (p->retry_valid) {
            cJSON_AddNumberToObject(o, "retry_in_s", p->retry_in_s);
            if (p->error_valid) cJSON_AddStringToObject(o, "err", p->error);
            cJSON_AddBoolToObject(o, "err_permanent", p->error_permanent);
        }
        cJSON_AddItemToArray(arr, o);
    }

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

void upload_sched_summary(int *out_pending, const char **out_worst)
{
    int pending = 0;
    const char *worst = "idle";

    /* This getter is used by /api/status and the bedside display. Keep it a
     * bounded in-memory snapshot: the scheduler owns all SD reconciliation. */
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    pending = s_summary_pending;
    for (int i = 0; i < s_n_rt; i++) {
        if (s_rt[i].state == SB_COOLDOWN) worst = "cooldown";
        else if (s_rt[i].state == SB_UPLOADING && strcmp(worst, "cooldown") != 0)
            worst = "uploading";
    }
    if (s_lock) xSemaphoreGive(s_lock);
    if (out_pending) *out_pending = pending;
    if (out_worst) *out_worst = worst;
}
