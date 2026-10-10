/*
 * SomnoTrace - SomnoStage scoring worker, .sst writer and reconcile scan
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

/* Low-priority post-session scoring worker.  Sources:
 *   - ring: .somnotrace/oximetry/recordings/<id>/generations/<g>/data/vitals.snt
 *   - AS11: .somnotrace/sessions/streams/<day>/<prefix>_sa2.snt
 * Writes a write-once-final stages.sst next to the source via tmp+rename.
 * The component keeps its own path constants so it does not depend on the
 * main component (which itself REQUIRES somno_ml). */

#include "somno_ml.h"
#include "somno_ml_internal.h"
#include "somno_ml_features.h"
#include "somno_ml_model.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "somno_ml";

#define OX_RECORDINGS   "/somnotrace/.somnotrace/oximetry/recordings"
#define SD_STREAMS      "/somnotrace/.somnotrace/sessions/streams"

#define MIN_VALID_EPOCHS 30
#define JOB_QUEUE_LEN    8
/* Ring-off gaps shorter than this still belong to the same night: the ring
 * records per wear-session, so toilet breaks split one night into fragments.
 * Scoring each fragment alone restarts the REM-latency gate and loses the
 * night context, which crushes REM%.  Members of a group are fed through a
 * single feature/decode pass; each member still gets its own .sst slice. */
#define FRAG_MERGE_GAP_MS  (90LL * 60 * 1000)
#define MAX_RING_RECS      96
#define MAX_FRAG_GROUP     8

/* Self-healing cadence.  Live enqueues are tracked below and verified a few
 * minutes later; the bounded reconcile rescans only recent days (a dropped
 * job is always fresh — deeper history is covered by the boot scan and the
 * daily full pass). */
#define RECON_RECENT_DAYS    14
#define RECON_RECENT_MS      (30LL * 60 * 1000)
#define RECON_FULL_MS        (24LL * 60 * 60 * 1000)
#define VERIFY_FIRST_MS      (3LL * 60 * 1000)
#define VERIFY_RETRY_MS      (5LL * 60 * 1000)
#define VERIFY_MAX_TRIES     3
#define PENDING_CAP          8

/* ---- SNT wire constants ---- */
#define SNT_MISSING    (-32768)     /* INT16_MIN sentinel (SNT3 + SNT v2) */
#define SNT_MISSING_V1 (-1)

static QueueHandle_t s_queue;

/* Expected-.sst tracking for live enqueues.  Every failure mode in the
 * scoring path is a silent drop by design, so a session whose stage file
 * never materialises would stay that way until the next boot reconcile.
 * Instead, live jobs get a deadline: the worker stats the expected .sst a
 * few minutes after enqueueing and re-enqueues when it is absent.  Jobs
 * queued by reconcile are not tracked — reconcile itself is the retry. */
typedef struct {
    bool     used;
    int      type;
    char     id[64];
    char     dir[192];
    int64_t  due_ms;
    int      tries;
} pending_sst_t;

static pending_sst_t   s_pending[PENDING_CAP];
static SemaphoreHandle_t s_pending_mux;
static int64_t s_next_recon_ms = -1;
static int64_t s_next_full_ms  = -1;
static somno_ml_live_state_t s_live;
static portMUX_TYPE s_live_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ---------------- job enqueue / live state ---------------- */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Remember that a live job should produce a .sst soon.  Best-effort: when
 * the table is full the job is still queued, it just loses its watchdog. */
static void pending_add(const somno_ml_job_t *job)
{
    if (!s_pending_mux) return;
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < PENDING_CAP; i++)
        if (!s_pending[i].used) { slot = i; break; }
    if (slot < 0) {
        xSemaphoreGive(s_pending_mux);
        ESP_LOGW(TAG, "%s: verify table full — job untracked", job->id);
        return;
    }
    pending_sst_t *p = &s_pending[slot];
    p->type = job->type;
    strlcpy(p->id, job->id, sizeof(p->id));
    strlcpy(p->dir, job->dir, sizeof(p->dir));
    p->due_ms = now_ms() + VERIFY_FIRST_MS;
    p->tries = 0;
    p->used = true;
    xSemaphoreGive(s_pending_mux);
}

/* verify=true for live session-end/publish enqueues: a queue-full drop gets
 * a WARN (it used to vanish silently) and a pending entry so the worker
 * re-enqueues later.  verify=false for reconcile-sourced jobs: saturation
 * there is expected and handled by s_recon_more. */
static int enqueue_internal(const somno_ml_job_t *job, bool verify)
{
    if (!somno_ml_available() || !s_queue || !job) return -1;
    if (xQueueSend(s_queue, job, 0) != pdTRUE) {
        if (verify) {
            ESP_LOGW(TAG, "%s: enqueue dropped (queue full) — "
                     "will retry", job->id);
            pending_add(job);
        }
        return -1;
    }
    if (verify) pending_add(job);
    return 0;
}

int somno_ml_enqueue(const somno_ml_job_t *job)
{
    return enqueue_internal(job, true);
}

int somno_ml_enqueue_ring(const char *recording_id)
{
    if (!recording_id || strlen(recording_id) >= sizeof(((somno_ml_job_t *)0)->id))
        return -1;
    somno_ml_job_t j = { .type = SOMNO_ML_SRC_RING_VITALS };
    strlcpy(j.id, recording_id, sizeof(j.id));
    return enqueue_internal(&j, true);
}

int somno_ml_enqueue_as11(const char *day_dir, const char *file_prefix)
{
    somno_ml_job_t j = { .type = SOMNO_ML_SRC_AS11_SA2 };
    if (!day_dir || !file_prefix ||
        strlen(day_dir) >= sizeof(j.dir) || strlen(file_prefix) >= sizeof(j.id))
        return -1;
    strlcpy(j.dir, day_dir, sizeof(j.dir));
    strlcpy(j.id, file_prefix, sizeof(j.id));
    return enqueue_internal(&j, true);
}

void somno_ml_live_state(somno_ml_live_state_t *out)
{
    portENTER_CRITICAL(&s_live_mux);
    *out = s_live;
    portEXIT_CRITICAL(&s_live_mux);
}

static void live_update(int stage, float conf)
{
    portENTER_CRITICAL(&s_live_mux);
    s_live.active = true;
    s_live.stage = stage;
    s_live.confidence = conf;
    s_live.provisional = true;
    s_live.updated_ms = (int64_t)(esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&s_live_mux);
}

static void live_clear(void)
{
    portENTER_CRITICAL(&s_live_mux);
    s_live.active = false;
    s_live.stage = -1;
    portEXIT_CRITICAL(&s_live_mux);
}

/* ---------------- path resolution ---------------- */

/* Ring: derive vitals + stages paths from recording.json in
 * OX_RECORDINGS/<day>/<recording_id>/ (state must be "ready"). */
static int ring_recording_gen(const char *dir, int *gen_out)
{
    char pj[288];
    if (snprintf(pj, sizeof(pj), "%s/recording.json", dir) >= (int)sizeof(pj))
        return -1;
    FILE *f = fopen(pj, "rb");
    if (!f) return -1;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    if (!j) return -1;
    cJSON *state = cJSON_GetObjectItem(j, "state");
    cJSON *g = cJSON_GetObjectItem(j, "active_generation");
    int ok = cJSON_IsString(state) &&
             strcmp(state->valuestring, "ready") == 0 &&
             cJSON_IsNumber(g) && g->valueint > 0;
    int gen = ok ? g->valueint : 0;
    cJSON_Delete(j);
    if (!ok) return -1;
    *gen_out = gen;
    return 0;
}

static int ring_paths_dir(const char *dir, char *vitals, size_t vs,
                          char *stages, size_t ss)
{
    int gen;
    if (ring_recording_gen(dir, &gen) != 0) return -1;
    int n = snprintf(vitals, vs, "%s/generations/%d/data/vitals.snt", dir, gen);
    int m = snprintf(stages, ss, "%s/generations/%d/data/stages.sst", dir, gen);
    return (n > 0 && n < (int)vs && m > 0 && m < (int)ss) ? 0 : -1;
}

static int ring_paths(const char *recording_id, char *vitals, size_t vs,
                      char *stages, size_t ss)
{
    /* locate the recording under its day dir */
    DIR *days = opendir(OX_RECORDINGS);
    if (!days) return -1;
    struct dirent *de;
    int found = 0;
    char dir[256];
    while ((de = readdir(days))) {
        if (de->d_name[0] == '.' || strlen(de->d_name) != 8) continue;
        if (snprintf(dir, sizeof(dir), "%s/%s/%s", OX_RECORDINGS,
                     de->d_name, recording_id) >= (int)sizeof(dir))
            continue;
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) { found = 1; break; }
    }
    closedir(days);
    if (!found) return -1;
    return ring_paths_dir(dir, vitals, vs, stages, ss);
}

static int as11_paths(const somno_ml_job_t *j, char *sa2, size_t a,
                      char *stages, size_t s)
{
    int n = snprintf(sa2, a, "%s/%s_sa2.snt", j->dir, j->id);
    int m = snprintf(stages, s, "%s/%s_stages.sst", j->dir, j->id);
    return (n > 0 && n < (int)a && m > 0 && m < (int)s) ? 0 : -1;
}

/* ---------------- .sst sidecar ---------------- */

int somno_ml_sst_validate(const char *path, char *semver_out, size_t semver_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    somno_sst_header_t h;
    size_t n = fread(&h, 1, sizeof(h), f);
    fclose(f);
    if (n != sizeof(h) || h.magic != SOMNO_SST_MAGIC ||
        h.version != SOMNO_SST_VERSION)
        return -1;
    h.model_semver[23] = '\0';
    if (semver_out && semver_len)
        strlcpy(semver_out, (const char *)h.model_semver, semver_len);
    return (int)h.flags;
}

/* Freshness = valid header + current semver + written strictly after the
 * source's last modification. The mtime rule catches sources that were
 * scored while still being appended (in-progress sessions): their .sst is
 * older than the next commit and gets rescored at finalize. */
static bool sst_fresh(const char *sst_path, const char *src_path)
{
    char sv[33];
    int flags = somno_ml_sst_validate(sst_path, sv, sizeof(sv));
    if (flags < 0 || (flags & SOMNO_SST_FLAG_PARTIAL) ||
        strcmp(sv, somno_ml_model_semver()) != 0)
        return false;
    struct stat ss, sr;
    if (stat(sst_path, &ss) != 0 || stat(src_path, &sr) != 0)
        return false;
    return ss.st_mtime > sr.st_mtime;
}

static int write_sst(const char *path, const somno_sst_header_t *hdr,
                     const uint8_t *stages, const uint8_t *conf)
{
    char tmp[300];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(hdr, 1, sizeof(*hdr), f) == sizeof(*hdr) &&
             fwrite(stages, 1, hdr->n_epochs, f) == hdr->n_epochs &&
             fwrite(conf, 1, hdr->n_epochs, f) == hdr->n_epochs;
    fflush(f);
    fclose(f);
    if (!ok) { unlink(tmp); return -1; }
    /* FatFs f_rename fails with FR_EXIST when the destination already
     * exists — remove it first (rescoring replaces stale/partial .sst). */
    if (rename(tmp, path) != 0) {
        unlink(path);
        if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    }
    return 0;
}

/* Write a 64-byte header-only INSUFFICIENT stub for a source that can never
 * be scored (deterministic parse failure).  Without it the file stays stale
 * forever and every reconcile re-enqueues it — enough of those poison the
 * job queue.  Environmental failures (fopen, alloc) deliberately do NOT
 * mark so they can succeed on retry. */
static int mark_unscorable(const char *sst_path, uint8_t source_type,
                           const char *job_id)
{
    somno_sst_header_t hdr = {0};
    hdr.magic = SOMNO_SST_MAGIC;
    hdr.version = SOMNO_SST_VERSION;
    hdr.flags = SOMNO_SST_FLAG_INSUFFICIENT;
    hdr.source_type = source_type;
    strlcpy((char *)hdr.model_semver, somno_ml_model_semver(),
            sizeof(hdr.model_semver));
    int rc = write_sst(sst_path, &hdr, NULL, NULL);
    if (rc == 0)
        ESP_LOGW(TAG, "%s: unscorable source — marked %s", job_id, sst_path);
    return rc;
}

/* ---------------- scoring core ---------------- */

typedef struct {
    somno_feat_t *fe;
    double       *fused;       /* n_epochs*4, grown on demand */
    uint32_t      cap_epochs;
    uint32_t      n_epochs;
    uint32_t      valid_epochs;
    float         feats[SOMNO_ML_N_FEATURES];
} score_ctx_t;

static bool score_sample(score_ctx_t *ctx, double spo2, double pr, double motion,
                         int sv, int pv, int mv)
{
    if (!somno_feat_push(ctx->fe, spo2, pr, motion, sv, pv, mv, ctx->feats))
        return true;
    if (ctx->n_epochs >= ctx->cap_epochs) {
        uint32_t nc = ctx->cap_epochs ? ctx->cap_epochs * 2 : 512;
        double *nb = heap_caps_realloc(ctx->fused, nc * 4 * sizeof(double),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) nb = realloc(ctx->fused, nc * 4 * sizeof(double));
        if (!nb) return false;
        ctx->fused = nb;
        ctx->cap_epochs = nc;
    }
    double *out = &ctx->fused[ctx->n_epochs * 4];
    somno_ml_predict(somno_ml_get_model(), ctx->feats, out);
    if (somno_feat_last_valid(ctx->fe))
        ctx->valid_epochs++;
    /* provisional live state (RAM only; never persisted as a hypnogram) */
    {
        double disp[4] = { out[0], out[1], out[2], out[3] };
        somno_ml_normalize4(disp);
        int best = 0;
        for (int c = 1; c < 4; c++) if (disp[c] > disp[best]) best = c;
        live_update(best, (float)disp[best]);
    }
    ctx->n_epochs++;
    return true;
}

static int finish_and_write(score_ctx_t *ctx, const char *sst_path,
                            uint8_t source_type, int64_t start_ms,
                            int motion_used, uint32_t epoch_sec, int partial,
                            const char *job_id)
{
    const somno_model_t *m = somno_ml_get_model();
    uint32_t n = ctx->n_epochs;

    somno_sst_header_t hdr = {0};
    hdr.magic = SOMNO_SST_MAGIC;
    hdr.version = SOMNO_SST_VERSION;
    hdr.decode_mode = SOMNO_ML_DECODE_FORWARD_BACKWARD;
    hdr.flags = (motion_used ? SOMNO_SST_FLAG_MOTION_USED : 0) |
                (partial ? SOMNO_SST_FLAG_PARTIAL : 0);
    hdr.source_type = source_type;
    hdr.start_ms = start_ms;
    hdr.epoch_sec = epoch_sec;
    hdr.n_epochs = n;
    strlcpy((char *)hdr.model_semver, somno_ml_model_semver(),
            sizeof(hdr.model_semver));

    uint8_t *stages = malloc(n ? n : 1);
    uint8_t *conf = malloc(n ? n : 1);
    if (!stages || !conf) { free(stages); free(conf); return -1; }

    if (ctx->valid_epochs < MIN_VALID_EPOCHS) {
        /* Write a marked stub so reconcile does not re-enqueue this source
         * forever and the UI knows to hide the band.  The stub deliberately
         * does NOT carry PARTIAL even when the source looked still-growing:
         * sst_fresh() rejects PARTIAL files, so one would poison the queue
         * every bit as much as writing nothing at all.  If the source keeps
         * growing after the stub lands, its mtime overtakes the stub's and
         * freshness fails on its own — the file gets re-scored anyway. */
        hdr.flags &= ~SOMNO_SST_FLAG_PARTIAL;
        hdr.flags |= SOMNO_SST_FLAG_INSUFFICIENT;
        memset(stages, 0xFF, n);
        memset(conf, 0, n);
        int rc = write_sst(sst_path, &hdr, stages, conf);
        free(stages);
        free(conf);
        if (rc == 0)
            ESP_LOGW(TAG, "%s: %u valid epochs < %d — wrote insufficient marker",
                     job_id, (unsigned)ctx->valid_epochs, MIN_VALID_EPOCHS);
        else
            ESP_LOGE(TAG, "%s: insufficient-marker write failed", job_id);
        return rc;
    }

    double *post = heap_caps_malloc(n * 4 * sizeof(double),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!post) post = malloc(n * 4 * sizeof(double));
    if (!post) { free(stages); free(conf); return -1; }
    if (somno_ml_decode_night(m, ctx->fused, (int)n, stages, post) != 0) {
        free(stages);
        free(conf);
        free(post);
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        double mx = post[i * 4];
        for (int c = 1; c < 4; c++)
            if (post[i * 4 + c] > mx) mx = post[i * 4 + c];
        conf[i] = (uint8_t)lrint(fmin(fmax(mx, 0.0), 1.0) * 255.0);
    }
    free(post);
    int rc = write_sst(sst_path, &hdr, stages, conf);
    free(stages);
    free(conf);
    if (rc == 0)
        ESP_LOGI(TAG, "%s: wrote %s (%u epochs)", job_id, sst_path, (unsigned)n);
    else
        ESP_LOGE(TAG, "%s: .sst write failed: %s", job_id, sst_path);
    return rc;
}

/* canonical vitals.snt: 64-byte SNT3 header + n_channels x int16 records
 *   ch0 spo2 x0.01, ch1 pulse x0.01, ch2 motion, ch3 status, ch4 source */
typedef struct {
    int64_t  start_ms;
    uint32_t sample_count;
    uint32_t read_n;
    uint8_t  n_ch;
    double   dt;
} vitals_meta_t;

/* Open + validate a vitals.snt, positioned at the first record.
 * Returns NULL on any failure; *bad_header (when given) is set only for
 * deterministic content problems (bad magic/fields), not for fopen errors
 * or truncated reads which may still be growing or transient. */
static FILE *vitals_open(const char *vitals_path, vitals_meta_t *m,
                         int *bad_header)
{
    if (bad_header) *bad_header = 0;
    FILE *f = fopen(vitals_path, "rb");
    if (!f) return NULL;
    uint8_t hdr[64];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return NULL;
    }
    if (rd_u32(hdr) != 0x33544e53u || hdr[4] != 3) {   /* "SNT3" v3 */
        fclose(f);
        if (bad_header) *bad_header = 1;
        return NULL;
    }
    m->n_ch = hdr[7];
    uint8_t sample_bytes = hdr[8];
    uint16_t header_bytes;
    memcpy(&header_bytes, hdr + 10, 2);
    uint32_t period_us = rd_u32(hdr + 12);
    uint32_t period_den = rd_u32(hdr + 16);
    memcpy(&m->start_ms, hdr + 20, 8);
    m->sample_count = rd_u32(hdr + 28);
    if (m->n_ch < 4 || m->n_ch > 16 || sample_bytes != 2 ||
        header_bytes < 64 || period_us == 0 || period_den == 0) {
        fclose(f);
        if (bad_header) *bad_header = 1;
        return NULL;
    }
    m->dt = (double)period_us / (double)period_den / 1e6;
    m->read_n = 0;
    return f;
}

/* Feed every record of an open vitals.snt into the shared score context. */
static bool vitals_feed(score_ctx_t *ctx, FILE *f, vitals_meta_t *m)
{
    int16_t rec[16];
    while (fread(rec, sizeof(int16_t), m->n_ch, f) == m->n_ch) {
        m->read_n++;
        uint16_t status = (uint16_t)rec[3];
        double spo2 = (rec[0] == SNT_MISSING) ? NAN : rec[0] / 100.0;
        double pr = (rec[1] == SNT_MISSING) ? NAN : rec[1] / 100.0;
        double mot = (rec[2] == SNT_MISSING) ? NAN : (double)rec[2];
        int sv = !(status & 1) && rec[0] != SNT_MISSING;
        int pv = !(status & 2) && rec[1] != SNT_MISSING;
        if (!score_sample(ctx, spo2, pr, mot, sv, pv, rec[2] != SNT_MISSING))
            return false;
    }
    return true;
}

/* Decode ctx->fused into per-epoch stages + posteriors (caller frees). */
static int decode_ctx(score_ctx_t *ctx, uint8_t **stages_out,
                      double **post_out)
{
    const somno_model_t *m = somno_ml_get_model();
    uint32_t n = ctx->n_epochs;
    uint8_t *stages = malloc(n ? n : 1);
    double *post = heap_caps_malloc(n * 4 * sizeof(double),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!post) post = malloc(n * 4 * sizeof(double));
    if (!stages || !post) { free(stages); free(post); return -1; }
    if (somno_ml_decode_night(m, ctx->fused, (int)n, stages, post) != 0) {
        free(stages);
        free(post);
        return -1;
    }
    *stages_out = stages;
    *post_out = post;
    return 0;
}

/* Write one member's .sst slice from a decoded run: stages[lo:hi). */
static int write_member_sst(const char *sst_path, uint8_t source_type,
                            int64_t start_ms, int motion_used,
                            uint32_t epoch_sec, int partial, int insufficient,
                            const uint8_t *stages, const double *post,
                            uint32_t lo, uint32_t hi, const char *job_id)
{
    uint32_t n = hi - lo;
    somno_sst_header_t hdr = {0};
    hdr.magic = SOMNO_SST_MAGIC;
    hdr.version = SOMNO_SST_VERSION;
    hdr.decode_mode = SOMNO_ML_DECODE_FORWARD_BACKWARD;
    hdr.flags = (motion_used ? SOMNO_SST_FLAG_MOTION_USED : 0) |
                (insufficient ? SOMNO_SST_FLAG_INSUFFICIENT
                              : (partial ? SOMNO_SST_FLAG_PARTIAL : 0));
    hdr.source_type = source_type;
    hdr.start_ms = start_ms;
    hdr.epoch_sec = epoch_sec;
    hdr.n_epochs = n;
    strlcpy((char *)hdr.model_semver, somno_ml_model_semver(),
            sizeof(hdr.model_semver));

    uint8_t *conf = malloc(n ? n : 1);
    uint8_t *sl = malloc(n ? n : 1);
    if (!conf || !sl) { free(conf); free(sl); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        sl[i] = insufficient ? 0xFF : stages[lo + i];
        double mx = insufficient ? 0.0 : post[(lo + i) * 4];
        if (!insufficient)
            for (int c = 1; c < 4; c++)
                if (post[(lo + i) * 4 + c] > mx) mx = post[(lo + i) * 4 + c];
        conf[i] = (uint8_t)lrint(fmin(fmax(mx, 0.0), 1.0) * 255.0);
    }
    int rc = write_sst(sst_path, &hdr, sl, conf);
    free(conf);
    free(sl);
    if (rc == 0)
        ESP_LOGI(TAG, "%s: wrote %s (%u epochs)", job_id, sst_path, (unsigned)n);
    else
        ESP_LOGE(TAG, "%s: .sst write failed: %s", job_id, sst_path);
    return rc;
}

static int score_vitals(const char *vitals_path, const char *sst_path,
                        const char *job_id)
{
    vitals_meta_t m;
    int bad_header = 0;
    FILE *f = vitals_open(vitals_path, &m, &bad_header);
    if (!f) {
        /* Deterministic corruption can never become scorable — mark it so
         * reconcile stops re-enqueueing it.  Transient opens stay stale. */
        if (bad_header)
            return mark_unscorable(sst_path, SOMNO_SST_SOURCE_RING, job_id);
        return -1;
    }

    score_ctx_t ctx = {0};
    ctx.fe = somno_feat_create(m.dt, 1);
    if (!ctx.fe) { fclose(f); return -1; }
    ctx.cap_epochs = m.sample_count / (uint32_t)lrint(30.0 / m.dt) + 2;
    ctx.fused = heap_caps_malloc(ctx.cap_epochs * 4 * sizeof(double),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx.fused) ctx.fused = malloc(ctx.cap_epochs * 4 * sizeof(double));
    if (!ctx.fused) { somno_feat_destroy(ctx.fe); fclose(f); return -1; }

    vitals_feed(&ctx, f, &m);
    fclose(f);
    /* Gen1 (0.25 Hz): epochs round to 8 samples = 32 s, matching the
     * reference's round(30/dt) grid. */
    uint32_t ep_sec = (uint32_t)lrint(m.dt * lrint(30.0 / m.dt));
    int rc = finish_and_write(&ctx, sst_path, SOMNO_SST_SOURCE_RING,
                              m.start_ms, 1, ep_sec,
                              m.sample_count && m.read_n < m.sample_count,
                              job_id);
    somno_feat_destroy(ctx.fe);
    free(ctx.fused);
    return rc;
}

/* ---------------- fragment-group scoring (ring) ----------------
 * The ring ends a recording when it leaves the finger, so one night often
 * lands as several same-day fragments (toilet breaks, ring adjustments).
 * Scoring each fragment alone resets the REM-latency gate and starves the
 * decoder of night context — merged scoring treats the group as one night
 * and writes each member's own .sst slice afterwards. */

typedef struct {
    char     dir[288];      /* OX_RECORDINGS/<day>/<id> */
    char     id[16];
    int64_t  start_ms;
    int64_t  end_ms;
} ring_rec_t;

/* start/end span of a ready ring recording; -1 when unreadable. */
static int ring_span(const char *rec_dir, ring_rec_t *r)
{
    char vitals[340], sst[340];
    if (ring_paths_dir(rec_dir, vitals, sizeof(vitals),
                       sst, sizeof(sst)) != 0)
        return -1;
    vitals_meta_t m;
    FILE *f = vitals_open(vitals, &m, NULL);
    if (!f) return -1;
    fclose(f);
    r->start_ms = m.start_ms;
    r->end_ms = m.start_ms + (int64_t)(m.sample_count * m.dt * 1000.0);
    return 0;
}

/* Collect all ready ring recordings, sorted by start_ms. */
static int collect_ring_recs(ring_rec_t *out, int cap)
{
    int n = 0;
    DIR *days = opendir(OX_RECORDINGS);
    if (!days) return 0;
    struct dirent *de;
    while ((de = readdir(days)) && n < cap) {
        if (de->d_name[0] == '.' || strlen(de->d_name) != 8) continue;
        char day_path[288];
        if (snprintf(day_path, sizeof(day_path), "%s/%s",
                     OX_RECORDINGS, de->d_name) >= (int)sizeof(day_path))
            continue;
        DIR *recs = opendir(day_path);
        if (!recs) continue;
        struct dirent *re;
        while ((re = readdir(recs)) && n < cap) {
            if (re->d_name[0] == '.') continue;
            ring_rec_t *r = &out[n];
            if (snprintf(r->dir, sizeof(r->dir), "%s/%s", day_path,
                         re->d_name) >= (int)sizeof(r->dir))
                continue;
            strlcpy(r->id, re->d_name, sizeof(r->id));
            if (ring_span(r->dir, r) != 0) continue;
            n++;
        }
        closedir(recs);
    }
    closedir(days);
    /* insertion sort — n is small */
    for (int i = 1; i < n; i++) {
        ring_rec_t t = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].start_ms > t.start_ms) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = t;
    }
    return n;
}

/* Score a fragment group as one night and write every member's .sst.
 * grp[] must be sorted by start_ms and share < FRAG_MERGE_GAP_MS gaps. */
static int score_ring_group(const ring_rec_t *grp, int nmem, const char *job_id)
{
    vitals_meta_t metas[MAX_FRAG_GROUP];
    char ssts[MAX_FRAG_GROUP][340];
    FILE *fs[MAX_FRAG_GROUP] = {0};
    score_ctx_t ctx = {0};
    int rc = -1;

    for (int k = 0; k < nmem; k++) {
        char vitals[340];
        if (ring_paths_dir(grp[k].dir, vitals, sizeof(vitals),
                           ssts[k], sizeof(ssts[k])) != 0 ||
            !(fs[k] = vitals_open(vitals, &metas[k], NULL))) {
            ESP_LOGW(TAG, "%s: unreadable fragment %s — scoring solo",
                     job_id, grp[k].id);
            goto solo;
        }
        if (k > 0 && fabs(metas[k].dt - metas[0].dt) > metas[0].dt * 0.01) {
            ESP_LOGW(TAG, "%s: dt mismatch in fragment %s — scoring solo",
                     job_id, grp[k].id);
            goto solo;
        }
    }

    ctx.fe = somno_feat_create(metas[0].dt, 1);
    if (!ctx.fe) goto solo;
    ctx.cap_epochs = 2;
    for (int k = 0; k < nmem; k++)
        ctx.cap_epochs += metas[k].sample_count /
                          (uint32_t)lrint(30.0 / metas[k].dt) + 2;
    ctx.fused = heap_caps_malloc(ctx.cap_epochs * 4 * sizeof(double),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx.fused) ctx.fused = malloc(ctx.cap_epochs * 4 * sizeof(double));
    if (!ctx.fused) { somno_feat_destroy(ctx.fe); ctx.fe = NULL; goto solo; }

    ctx.fe = somno_feat_create(metas[0].dt, 1);
    if (!ctx.fe) goto solo;
    ctx.cap_epochs = 2;
    for (int k = 0; k < nmem; k++)
        ctx.cap_epochs += metas[k].sample_count /
                          (uint32_t)lrint(30.0 / metas[k].dt) + 2;
    ctx.fused = heap_caps_malloc(ctx.cap_epochs * 4 * sizeof(double),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx.fused) ctx.fused = malloc(ctx.cap_epochs * 4 * sizeof(double));
    if (!ctx.fused) { somno_feat_destroy(ctx.fe); goto solo; }

    uint32_t bounds[MAX_FRAG_GROUP + 1];
    bounds[0] = 0;
    for (int k = 0; k < nmem; k++) {
        vitals_feed(&ctx, fs[k], &metas[k]);
        fclose(fs[k]);
        fs[k] = NULL;
        bounds[k + 1] = ctx.n_epochs;
    }

    if (ctx.valid_epochs < MIN_VALID_EPOCHS) {
        /* merged total still too small — mark every member insufficient */
        for (int k = 0; k < nmem; k++) {
            uint32_t ep = (uint32_t)lrint(metas[k].dt *
                                          lrint(30.0 / metas[k].dt));
            write_member_sst(ssts[k], SOMNO_SST_SOURCE_RING,
                             metas[k].start_ms, 1, ep,
                             metas[k].sample_count &&
                                 metas[k].read_n < metas[k].sample_count,
                             1, NULL, NULL, bounds[k], bounds[k + 1],
                             grp[k].id);
        }
        ESP_LOGW(TAG, "%s: group of %d fragments — %u valid epochs < %d",
                 job_id, nmem, (unsigned)ctx.valid_epochs, MIN_VALID_EPOCHS);
        rc = 0;
        goto done;
    }

    {
        uint8_t *stages;
        double *post;
        if (decode_ctx(&ctx, &stages, &post) == 0) {
            for (int k = 0; k < nmem; k++) {
                uint32_t ep = (uint32_t)lrint(metas[k].dt *
                                              lrint(30.0 / metas[k].dt));
                write_member_sst(ssts[k], SOMNO_SST_SOURCE_RING,
                                 metas[k].start_ms, 1, ep,
                                 metas[k].sample_count &&
                                     metas[k].read_n < metas[k].sample_count,
                                 0, stages, post, bounds[k], bounds[k + 1],
                                 grp[k].id);
            }
            ESP_LOGI(TAG, "%s: scored fragment group of %d (%u epochs)",
                     job_id, nmem, (unsigned)ctx.n_epochs);
            free(stages);
            free(post);
            rc = 0;
        }
    }

done:
    if (ctx.fe) somno_feat_destroy(ctx.fe);
    free(ctx.fused);
solo:
    for (int k = 0; k < nmem; k++)
        if (fs[k]) fclose(fs[k]);
    if (rc != 0) {  /* merged path failed — score each member independently */
        for (int k = 0; k < nmem; k++) {
            char vitals[340], sst[340];
            if (ring_paths_dir(grp[k].dir, vitals, sizeof(vitals),
                               sst, sizeof(sst)) == 0)
                score_vitals(vitals, sst, grp[k].id);
        }
        rc = 0;
    }
    return rc;
}

/* Find the fragment group containing recording_id. Returns member count
 * (1 = standalone) and fills grp[] sorted by start_ms. */
static int ring_fragment_group(const char *recording_id, ring_rec_t *grp,
                               int cap)
{
    ring_rec_t *all = heap_caps_malloc(MAX_RING_RECS * sizeof(ring_rec_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!all) all = malloc(MAX_RING_RECS * sizeof(ring_rec_t));
    if (!all) return 0;
    int n = collect_ring_recs(all, MAX_RING_RECS);
    int self = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(all[i].id, recording_id) == 0) { self = i; break; }
    if (self < 0) { free(all); return 0; }
    /* expand left/right while consecutive gaps stay under the threshold */
    int lo = self, hi = self;
    while (lo > 0 && all[lo].start_ms - all[lo - 1].end_ms < FRAG_MERGE_GAP_MS &&
           hi - lo + 2 <= MAX_FRAG_GROUP)
        lo--;
    while (hi + 1 < n && all[hi + 1].start_ms - all[hi].end_ms < FRAG_MERGE_GAP_MS &&
           hi - lo + 2 <= MAX_FRAG_GROUP)
        hi++;
    int cnt = hi - lo + 1;
    if (cnt > cap) cnt = cap;
    for (int i = 0; i < cnt; i++) grp[i] = all[lo + i];
    free(all);
    return cnt;
}

/* AS11 *_sa2.snt: 28-byte SNTB header + 2x int16 records {hr, spo2} */
static int score_sa2(const char *sa2_path, const char *sst_path,
                     const char *job_id)
{
    FILE *f = fopen(sa2_path, "rb");
    if (!f) return -1;
    uint8_t hdr[28];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return -1;   /* short read: possibly still growing — stay stale */
    }
    if (rd_u32(hdr) != 0x534e5442u) {   /* SNT_MAGIC (disk bytes "BTNS") */
        fclose(f);
        return mark_unscorable(sst_path, SOMNO_SST_SOURCE_AS11, job_id);
    }
    uint8_t ver = hdr[4];
    uint8_t nch = hdr[6];
    uint16_t hz_x10;
    memcpy(&hz_x10, hdr + 8, 2);
    int64_t start_ms;
    memcpy(&start_ms, hdr + 12, 8);
    uint32_t sample_count;
    memcpy(&sample_count, hdr + 20, 4);
    if (nch != 2 || hz_x10 == 0) {
        fclose(f);
        return mark_unscorable(sst_path, SOMNO_SST_SOURCE_AS11, job_id);
    }
    int16_t missing = ver >= 2 ? SNT_MISSING : SNT_MISSING_V1;
    double dt = 10.0 / (double)hz_x10;

    score_ctx_t ctx = {0};
    ctx.fe = somno_feat_create(dt, 0);
    if (!ctx.fe) { fclose(f); return -1; }
    ctx.cap_epochs = sample_count / (uint32_t)lrint(30.0 / dt) + 2;
    ctx.fused = heap_caps_malloc(ctx.cap_epochs * 4 * sizeof(double),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx.fused) ctx.fused = malloc(ctx.cap_epochs * 4 * sizeof(double));
    if (!ctx.fused) { somno_feat_destroy(ctx.fe); fclose(f); return -1; }

    int16_t rec[2];
    uint32_t read_n = 0;
    while (fread(rec, sizeof(int16_t), 2, f) == 2) {
        read_n++;
        /* sa2 values are stored scaled x100; <=0 is invalid like the portal */
        int pv = rec[0] != missing && rec[0] > 0;
        int sv = rec[1] != missing && rec[1] > 0;
        double pr = pv ? rec[0] / 100.0 : NAN;
        double spo2 = sv ? rec[1] / 100.0 : NAN;
        if (!score_sample(&ctx, spo2, pr, 0.0, sv, pv, 0))
            break;
    }
    fclose(f);
    uint32_t ep_sec = (uint32_t)lrint(dt * lrint(30.0 / dt));
    int rc = finish_and_write(&ctx, sst_path, SOMNO_SST_SOURCE_AS11,
                              start_ms, 0, ep_sec,
                              sample_count && read_n < sample_count, job_id);
    somno_feat_destroy(ctx.fe);
    free(ctx.fused);
    return rc;
}

int somno_ml_score_file(const somno_ml_job_t *job)
{
    if (!somno_ml_available() || !job) return -1;
    char src[320], sst[320];
    int rc;
    if (job->type == SOMNO_ML_SRC_RING_VITALS)
        rc = ring_paths(job->id, src, sizeof(src), sst, sizeof(sst));
    else
        rc = as11_paths(job, src, sizeof(src), sst, sizeof(sst));
    if (rc != 0) {
        ESP_LOGW(TAG, "%s: path resolution failed", job->id);
        return -1;
    }
    struct stat st;
    if (stat(src, &st) != 0) {
        ESP_LOGW(TAG, "%s: source missing: %s", job->id, src);
        return -1;
    }
    if (sst_fresh(sst, src)) return 0;   /* idempotent: same model version */

    if (job->type == SOMNO_ML_SRC_RING_VITALS) {
        /* Fragment groups: a ring-off break splits one night into several
         * recordings.  Score the whole group in one pass so the decoder and
         * the REM-latency gate see the full night, then write each member's
         * own .sst slice.  Other members' jobs become no-ops via sst_fresh. */
        ring_rec_t grp[MAX_FRAG_GROUP];
        int nmem = ring_fragment_group(job->id, grp, MAX_FRAG_GROUP);
        if (nmem > 1) {
            ESP_LOGI(TAG, "%s: fragment group of %d — merged scoring",
                     job->id, nmem);
            return score_ring_group(grp, nmem, job->id);
        }
    }

    ESP_LOGI(TAG, "%s: scoring %s", job->id, src);
    if (job->type == SOMNO_ML_SRC_RING_VITALS)
        rc = score_vitals(src, sst, job->id);
    else
        rc = score_sa2(src, sst, job->id);
    if (rc != 0)
        ESP_LOGW(TAG, "%s: scoring failed (%d)", job->id, rc);
    return rc;
}

/* ---------------- reconcile scan ---------------- */

static int s_recon_n;   /* reconcile enqueue counter (diag log) */
static volatile bool s_recon_more; /* set when the job queue was full */

/* cutoff is a "YYYYMMDD" day-dir name; entries lexically older are skipped.
 * NULL scans everything.  Day names are chronological, so strcmp works. */
static void reconcile_ring(const char *cutoff)
{
    DIR *days = opendir(OX_RECORDINGS);
    if (!days) return;
    struct dirent *de;
    while ((de = readdir(days))) {
        if (de->d_name[0] == '.' || strlen(de->d_name) != 8) continue;
        if (cutoff && strcmp(de->d_name, cutoff) < 0) continue;
        char day_path[288];
        if (snprintf(day_path, sizeof(day_path), "%s/%s",
                     OX_RECORDINGS, de->d_name) >= (int)sizeof(day_path))
            continue;
        DIR *recs = opendir(day_path);
        if (!recs) continue;
        struct dirent *re;
        while ((re = readdir(recs))) {
            if (re->d_name[0] == '.') continue;
            char rec_dir[320], vitals[340], sst[340];
            if (snprintf(rec_dir, sizeof(rec_dir), "%s/%s", day_path,
                         re->d_name) >= (int)sizeof(rec_dir))
                continue;
            if (ring_paths_dir(rec_dir, vitals, sizeof(vitals),
                               sst, sizeof(sst)) != 0)
                continue;
            struct stat st;
            if (stat(vitals, &st) != 0) continue;
            if (sst_fresh(sst, vitals)) continue;
            somno_ml_job_t j = { .type = SOMNO_ML_SRC_RING_VITALS };
            strlcpy(j.id, re->d_name, sizeof(j.id));
            if (enqueue_internal(&j, false) != 0) {
                s_recon_more = true;   /* queue full — rescan when it drains */
                closedir(recs);
                closedir(days);
                return;
            }
            s_recon_n++;
        }
        closedir(recs);
    }
    closedir(days);
}

static void reconcile_as11(const char *cutoff)
{
    DIR *days = opendir(SD_STREAMS);
    if (!days) return;
    struct dirent *de;
    while ((de = readdir(days))) {
        if (strlen(de->d_name) != 8) continue;
        if (cutoff && strcmp(de->d_name, cutoff) < 0) continue;
        int digits = 1;
        for (int i = 0; i < 8; i++)
            if (de->d_name[i] < '0' || de->d_name[i] > '9') { digits = 0; break; }
        if (!digits) continue;
        char day[256];
        snprintf(day, sizeof(day), "%s/%s", SD_STREAMS, de->d_name);
        DIR *dd = opendir(day);
        if (!dd) continue;
        struct dirent *fe;
        while ((fe = readdir(dd))) {
            const char *suf = "_sa2.snt";
            size_t fl = strlen(fe->d_name), sl = strlen(suf);
            if (fl <= sl || strcmp(fe->d_name + fl - sl, suf) != 0) continue;
            char prefix[64];
            size_t pl = fl - sl;
            if (pl >= sizeof(prefix)) continue;
            memcpy(prefix, fe->d_name, pl);
            prefix[pl] = '\0';
            char sst[400], sa2[400];
            if (snprintf(sst, sizeof(sst), "%s/%s_stages.sst", day, prefix) >=
                    (int)sizeof(sst) ||
                snprintf(sa2, sizeof(sa2), "%s/%s", day, fe->d_name) >=
                    (int)sizeof(sa2))
                continue;
            if (sst_fresh(sst, sa2)) continue;
            somno_ml_job_t j = { .type = SOMNO_ML_SRC_AS11_SA2 };
            strlcpy(j.dir, day, sizeof(j.dir));
            strlcpy(j.id, prefix, sizeof(j.id));
            if (enqueue_internal(&j, false) != 0) {
                s_recon_more = true;
                closedir(dd);
                closedir(days);
                return;
            }
            s_recon_n++;
        }
        closedir(dd);
    }
    closedir(days);
}

static int reconcile_impl(int max_age_days)
{
    if (!somno_ml_available()) return 0;
    char cutoff[16] = "";
    if (max_age_days > 0) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_year + 1900 >= 2020) {   /* clock unset → scan unbounded */
            tm.tm_mday -= max_age_days;
            mktime(&tm);
            strftime(cutoff, sizeof(cutoff), "%Y%m%d", &tm);
        }
    }
    s_recon_n = 0;
    s_recon_more = false;
    reconcile_ring(cutoff[0] ? cutoff : NULL);
    reconcile_as11(cutoff[0] ? cutoff : NULL);
    ESP_LOGI(TAG, "reconcile%s: %d job(s) queued",
             cutoff[0] ? " (recent)" : "", s_recon_n);
    return 0;
}

int somno_ml_reconcile(void)
{
    return reconcile_impl(0);
}

int somno_ml_reconcile_recent(int days)
{
    return reconcile_impl(days);
}

/* ---------------- worker task ---------------- */

/* Check every tracked live job whose .sst should exist by now.  Missing
 * files get re-enqueued (untracked — the entry itself carries the retry
 * budget); after VERIFY_MAX_TRIES the entry is dropped with a WARN and the
 * periodic reconcile remains as backstop. */
static void verify_pending(void)
{
    if (!s_pending_mux) return;
    int64_t now = now_ms();
    for (int i = 0; i < PENDING_CAP; i++) {
        somno_ml_job_t j;
        bool due;
        xSemaphoreTake(s_pending_mux, portMAX_DELAY);
        pending_sst_t *p = &s_pending[i];
        due = p->used && p->due_ms <= now;
        if (due) {
            j.type = p->type;
            strlcpy(j.id, p->id, sizeof(j.id));
            strlcpy(j.dir, p->dir, sizeof(j.dir));
        }
        xSemaphoreGive(s_pending_mux);
        if (!due) continue;

        char src[320], sst[320];
        int rc = (j.type == SOMNO_ML_SRC_RING_VITALS)
               ? ring_paths(j.id, src, sizeof(src), sst, sizeof(sst))
               : as11_paths(&j, src, sizeof(src), sst, sizeof(sst));
        struct stat st;
        bool exists = (rc == 0 && stat(sst, &st) == 0);

        xSemaphoreTake(s_pending_mux, portMAX_DELAY);
        if (exists) { p->used = false; xSemaphoreGive(s_pending_mux); continue; }
        p->tries++;
        if (p->tries >= VERIFY_MAX_TRIES) {
            p->used = false;
            xSemaphoreGive(s_pending_mux);
            ESP_LOGW(TAG, "%s: .sst still missing after %d attempts — "
                     "leaving to reconcile", j.id, p->tries);
            continue;
        }
        p->due_ms = now + VERIFY_RETRY_MS;
        xSemaphoreGive(s_pending_mux);
        ESP_LOGW(TAG, "%s: expected .sst missing — re-enqueueing (%d/%d)",
                 j.id, p->tries, VERIFY_MAX_TRIES);
        enqueue_internal(&j, false);
    }
}

/* Earliest pending-verify deadline, or -1 when the table is empty. */
static int64_t pending_next_due(void)
{
    int64_t due = -1;
    if (!s_pending_mux) return -1;
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    for (int i = 0; i < PENDING_CAP; i++)
        if (s_pending[i].used &&
            (due < 0 || s_pending[i].due_ms < due))
            due = s_pending[i].due_ms;
    xSemaphoreGive(s_pending_mux);
    return due;
}

/* Periodic reconcile: a bounded recent-days pass every RECON_RECENT_MS and
 * a full scan every RECON_FULL_MS.  A dropped job is always fresh, so the
 * frequent pass stays cheap regardless of history depth; the daily pass
 * covers drops that aged past the window on never-rebooted devices. */
static void service_periodic(void)
{
    int64_t now = now_ms();
    if (s_next_full_ms > 0 && now >= s_next_full_ms) {
        s_next_full_ms = now + RECON_FULL_MS;
        s_next_recon_ms = now + RECON_RECENT_MS;
        somno_ml_reconcile();
    } else if (s_next_recon_ms > 0 && now >= s_next_recon_ms) {
        s_next_recon_ms = now + RECON_RECENT_MS;
        somno_ml_reconcile_recent(RECON_RECENT_DAYS);
    }
}

/* Ticks to the next scheduled event (verify deadline or reconcile), or
 * portMAX_DELAY when nothing is pending. */
static TickType_t next_wait(void)
{
    int64_t due = pending_next_due();
    if (s_next_recon_ms > 0 && (due < 0 || s_next_recon_ms < due))
        due = s_next_recon_ms;
    if (s_next_full_ms > 0 && (due < 0 || s_next_full_ms < due))
        due = s_next_full_ms;
    if (due < 0) return portMAX_DELAY;
    int64_t dms = due - now_ms();
    return pdMS_TO_TICKS(dms > 0 ? dms : 0);
}

static void worker_task(void *arg)
{
    (void)arg;
    somno_ml_job_t job;
    for (;;) {
        /* When the queue drains and a previous reconcile could not enqueue
         * everything (queue was full), rescan for remaining work. */
        TickType_t wait = next_wait();
        if (s_recon_more && wait > pdMS_TO_TICKS(1000))
            wait = pdMS_TO_TICKS(1000);
        if (xQueueReceive(s_queue, &job, wait) != pdTRUE) {
            verify_pending();
            service_periodic();
            if (s_recon_more) somno_ml_reconcile();
            continue;
        }
        somno_ml_score_file(&job);
        live_clear();
        verify_pending();
        service_periodic();
        if (s_recon_more && uxQueueMessagesWaiting(s_queue) == 0)
            somno_ml_reconcile();
    }
}

/* Called once by somno_ml_init() after the model is parsed. */
void somno_ml_worker_start(void)
{
    if (s_queue) return;
    s_queue = xQueueCreate(JOB_QUEUE_LEN, sizeof(somno_ml_job_t));
    if (!s_queue) return;
    s_live.stage = -1;
    s_pending_mux = xSemaphoreCreateMutex();
    int64_t now = now_ms();
    s_next_recon_ms = now + RECON_RECENT_MS;
    s_next_full_ms = now + RECON_FULL_MS;

    /* PSRAM stack — internal RAM is too scarce for a 16 KB worker stack. */
    static StackType_t *s_stack;
    static StaticTask_t *s_tcb;
    s_stack = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stack || !s_tcb) {
        ESP_LOGE(TAG, "worker stack alloc failed");
        return;
    }
    xTaskCreateStaticPinnedToCore(worker_task, "somno_ml", 16384, NULL,
                                  tskIDLE_PRIORITY + 1, s_stack, s_tcb, 1);
    ESP_LOGI(TAG, "worker started (PSRAM stack)");
}
