/*
 * SomnoTrace - SomnoStage worker allocation and lifecycle regression tests
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
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>
#include "somno_ml_features.h"
#include "somno_stage_csv.h"
#include "esp_heap_caps.h"
int64_t host_now_us = 1000000;
int host_heap_caps_malloc_fail;
static int worker_alloc_calls, worker_fail_alloc_at;
static void *host_worker_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    worker_alloc_calls++;
    if (host_heap_caps_malloc_fail ||
        (worker_fail_alloc_at && worker_alloc_calls == worker_fail_alloc_at))
        return NULL;
    return malloc(size);
}
static int created, destroyed;
somno_feat_t *__real_somno_feat_create(double dt, int motion);
somno_feat_t *__wrap_somno_feat_create(double dt, int motion)
{
    created++;
    return __real_somno_feat_create(dt, motion);
}
void __real_somno_feat_destroy(somno_feat_t *fe);
void __wrap_somno_feat_destroy(somno_feat_t *fe)
{
    if (fe) destroyed++;
    __real_somno_feat_destroy(fe);
}
/* The worker hard-codes its on-device roots (/somnotrace/...).  Paths under
 * that prefix are re-rooted into a mkdtemp'd tree so the real reconcile /
 * fragment-group / CSV-export code runs unmodified on the host. */
static char host_root[64];
static const char *host_path(const char *p, char *buf, size_t cap)
{
    if (!host_root[0] || strncmp(p, "/somnotrace/", 12) != 0) return p;
    snprintf(buf, cap, "%s%s", host_root, p);
    return buf;
}
static FILE *host_fopen(const char *p, const char *mode)
{
    char b[600]; return fopen(host_path(p, b, sizeof(b)), mode);
}
static DIR *host_opendir(const char *p)
{
    char b[600]; return opendir(host_path(p, b, sizeof(b)));
}
static int host_stat(const char *p, struct stat *st)
{
    char b[600]; return stat(host_path(p, b, sizeof(b)), st);
}
static int host_rename(const char *a, const char *b)
{
    char x[600], y[600];
    return rename(host_path(a, x, sizeof(x)), host_path(b, y, sizeof(y)));
}
static int host_unlink(const char *p)
{
    char b[600]; return unlink(host_path(p, b, sizeof(b)));
}
static int zeo_calls;
static char zeo_last[600];
static int host_zeo(const char *csv, const somno_sst_header_t *h, const uint8_t *st)
{
    char b[600];
    zeo_calls++;
    snprintf(zeo_last, sizeof(zeo_last), "%s", csv);
    return somno_stage_write_zeo_csv_file(host_path(csv, b, sizeof(b)), h, st);
}
static int host_backfill(const char *sst, const char *csv)
{
    char a[600], b[600];
    return somno_stage_export_csv_from_sst(host_path(sst, a, sizeof(a)),
                                           host_path(csv, b, sizeof(b)));
}
static int exports;
static void on_export(void) { exports++; }
static void rm_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char sub[600];
            snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
            rm_tree(sub);
        }
        closedir(d);
        assert(rmdir(path) == 0);
    } else {
        assert(unlink(path) == 0);
    }
}
#define fopen(p, m)   host_fopen(p, m)
#define opendir(p)    host_opendir(p)
#define stat(p, s)    host_stat(p, s)
#define rename(a, b)  host_rename(a, b)
#define unlink(p)     host_unlink(p)
#define somno_stage_write_zeo_csv_file(c, h, s) host_zeo(c, h, s)
#define somno_stage_export_csv_from_sst(s, c)   host_backfill(s, c)
#undef heap_caps_malloc
#define heap_caps_malloc(size, caps) host_worker_caps_malloc((size), (caps))
#define SOMNO_ML_WORKER_TEST 1
#include "somno_ml_worker.c"

bool somno_ml_available(void) { return true; }
const char *somno_ml_model_semver(void) { return "host-test"; }
const somno_model_t *somno_ml_get_model(void) { return NULL; }
void somno_ml_predict(const somno_model_t *m, const float *features, double out[4])
{
    (void)m; (void)features;
    out[0] = 0.1; out[1] = 0.7; out[2] = 0.1; out[3] = 0.1;
}
int somno_ml_decode_night(const somno_model_t *m, const double *probs, int n,
                          uint8_t *stages, double *post)
{
    (void)m;
    for (int i = 0; i < n; i++) {
        stages[i] = 1;
        if (post) memcpy(post + i * 4, probs + i * 4, 4 * sizeof(double));
    }
    return 0;
}
static bool fail_queue, fail_mutex, fail_task;
static int queue_deleted, mutex_deleted;
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size)
{
    (void)n; (void)size; return fail_queue ? NULL : (void *)1;
}
void vQueueDelete(QueueHandle_t q) { assert(q); queue_deleted++; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return fail_mutex ? NULL : (void *)1; }
void vSemaphoreDelete(SemaphoreHandle_t mutex) { assert(mutex); mutex_deleted++; }
static int queue_sends;
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)q; (void)item; (void)ticks; queue_sends++; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    (void)q; (void)item; (void)ticks; return pdFALSE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { (void)q; return 0; }
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name,
    uint32_t size, void *arg, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *tcb, BaseType_t core)
{
    (void)fn; (void)name; (void)arg; (void)priority; (void)core;
    assert(size >= 24 * 1024);
    if (fail_task) return NULL;
    free(stack); free(tcb);
    return (void *)1;
}

static void test_worker_start_failures(void)
{
    fail_queue = true;
    somno_ml_worker_start();
    assert(!s_queue);
    fail_queue = false;

    fail_mutex = true;
    somno_ml_worker_start();
    assert(!s_queue && !s_pending_mux && queue_deleted == 1);
    fail_mutex = false;

    worker_alloc_calls = 0;
    worker_fail_alloc_at = 1;
    somno_ml_worker_start();
    assert(!s_queue && !s_pending_mux && queue_deleted == 2 && mutex_deleted == 1);

    worker_alloc_calls = 0;
    worker_fail_alloc_at = 2;
    somno_ml_worker_start();
    assert(!s_queue && !s_pending_mux && queue_deleted == 3 && mutex_deleted == 2);
    worker_fail_alloc_at = 0;

    fail_task = true;
    somno_ml_worker_start();
    assert(!s_queue && !s_pending_mux && queue_deleted == 4 && mutex_deleted == 3);
    fail_task = false;
}
static void fixture(const char *dir, int index, ring_rec_t *rec)
{
    char path[400];
    snprintf(rec->dir, sizeof(rec->dir), "%s/%d", dir, index);
    snprintf(rec->id, sizeof(rec->id), "fixture%d", index);
    assert(mkdir(rec->dir, 0700) == 0);
    snprintf(path, sizeof(path), "%s/recording.json", rec->dir);
    FILE *f = fopen(path, "w"); assert(f);
    fputs("{\"state\":\"ready\",\"active_generation\":1}", f); fclose(f);
    snprintf(path, sizeof(path), "%s/generations", rec->dir); assert(mkdir(path,0700)==0);
    snprintf(path, sizeof(path), "%s/generations/1", rec->dir); assert(mkdir(path,0700)==0);
    snprintf(path, sizeof(path), "%s/generations/1/data", rec->dir); assert(mkdir(path,0700)==0);
    snprintf(path, sizeof(path), "%s/generations/1/data/vitals.snt", rec->dir);
    f = fopen(path, "wb"); assert(f);
    uint8_t hdr[64] = {0};
    uint32_t magic = 0x33544e53u, period = 1000000, denominator = 1, count = 60;
    uint16_t header_size = 64;
    int64_t start = 1893456000000LL + index * 60000;
    memcpy(hdr, &magic, 4); hdr[4] = 3; hdr[7] = 5; hdr[8] = 2;
    memcpy(hdr + 10, &header_size, 2); memcpy(hdr + 12, &period, 4);
    memcpy(hdr + 16, &denominator, 4); memcpy(hdr + 20, &start, 8);
    memcpy(hdr + 28, &count, 4);
    assert(fwrite(hdr, 1, 64, f) == 64);
    int16_t sample[5] = {9700, 6500, 0, 0, 1};
    for (int i = 0; i < 60; i++) assert(fwrite(sample, 2, 5, f) == 5);
    fclose(f);
}
static void cleanup(const ring_rec_t *rec)
{
    char path[400];
    const char *files[] = {"vitals.snt", "stages.sst"};
    for (unsigned i=0;i<2;i++) {
        snprintf(path,sizeof(path),"%s/generations/1/data/%s",rec->dir,files[i]);
        assert(unlink(path)==0);
    }
    snprintf(path,sizeof(path),"%s/generations/1/data",rec->dir); assert(rmdir(path)==0);
    snprintf(path,sizeof(path),"%s/generations/1",rec->dir); assert(rmdir(path)==0);
    snprintf(path,sizeof(path),"%s/generations",rec->dir); assert(rmdir(path)==0);
    snprintf(path,sizeof(path),"%s/recording.json",rec->dir); assert(unlink(path)==0);
    assert(rmdir(rec->dir)==0);
}
static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w"); assert(f);
    fputs(text, f); fclose(f);
}
static void mkdirs(const char *path)
{
    char b[600];
    snprintf(b, sizeof(b), "%s", path);
    for (char *p = b + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(b, 0700); *p = '/'; }
    assert(mkdir(b, 0700) == 0 || errno == EEXIST);
}
/* A ready ring recording at rec_dir: 1 Hz, 5-channel SNT3, n records on disk,
 * header sample count hdr_count. */
static void ring_rec(const char *rec_dir, int64_t start, uint32_t hdr_count, int n)
{
    char path[600], real[600];
    snprintf(path, sizeof(path), "%s/generations/1/data", rec_dir);
    mkdirs(host_path(path, real, sizeof(real)));
    snprintf(path, sizeof(path), "%s/recording.json", rec_dir);
    put(path, "{\"state\":\"ready\",\"active_generation\":1}");
    snprintf(path, sizeof(path), "%s/generations/1/data/vitals.snt", rec_dir);
    FILE *f = fopen(path, "wb"); assert(f);
    uint8_t hdr[64] = {0};
    uint32_t magic = 0x33544e53u, period = 1000000, denominator = 1;
    uint16_t header_size = 64;
    memcpy(hdr, &magic, 4); hdr[4] = 3; hdr[7] = 5; hdr[8] = 2;
    memcpy(hdr + 10, &header_size, 2); memcpy(hdr + 12, &period, 4);
    memcpy(hdr + 16, &denominator, 4); memcpy(hdr + 20, &start, 8);
    memcpy(hdr + 28, &hdr_count, 4);
    assert(fwrite(hdr, 1, 64, f) == 64);
    int16_t sample[5] = {9700, 6500, 0, 0, 1};
    for (int i = 0; i < n; i++) assert(fwrite(sample, 2, 5, f) == 5);
    fclose(f);
}
/* AS11 _sa2.snt: SNTB v2, 2 channels {hr, spo2} x100 at 1 Hz. */
static void sa2_file(const char *path, uint32_t hdr_count, int n)
{
    FILE *f = fopen(path, "wb"); assert(f);
    uint8_t hdr[28] = {0};
    uint32_t magic = 0x534e5442u;
    uint16_t hz_x10 = 10;
    int64_t start = 1893456000000LL;
    memcpy(hdr, &magic, 4); hdr[4] = 2; hdr[6] = 2;
    memcpy(hdr + 8, &hz_x10, 2); memcpy(hdr + 12, &start, 8);
    memcpy(hdr + 20, &hdr_count, 4);
    assert(fwrite(hdr, 1, 28, f) == 28);
    int16_t sample[2] = {6500, 9700};
    for (int i = 0; i < n; i++) assert(fwrite(sample, 2, 2, f) == 2);
    fclose(f);
}
static uint32_t read_sst(const char *path, somno_sst_header_t *h,
                         uint8_t *stages, uint8_t *conf, uint32_t cap)
{
    FILE *f = fopen(path, "rb"); assert(f);
    assert(fread(h, 1, sizeof(*h), f) == sizeof(*h));
    assert(h->n_epochs <= cap);
    assert(fread(stages, 1, h->n_epochs, f) == h->n_epochs);
    assert(fread(conf, 1, h->n_epochs, f) == h->n_epochs);
    fclose(f);
    return h->n_epochs;
}
/* recording.json gate and the path builders' truncation boundary. */
static void test_ring_state(const char *dir)
{
    char rec[300], path[400], v[400], s[400];
    int gen = -1;
    snprintf(rec, sizeof(rec), "%s/state", dir);
    assert(mkdir(rec, 0700) == 0);
    snprintf(path, sizeof(path), "%s/recording.json", rec);
    put(path, "{\"state\":\"recording\",\"active_generation\":1}");
    assert(ring_recording_gen(rec, &gen) == -1);
    put(path, "{\"active_generation\":1}");
    assert(ring_recording_gen(rec, &gen) == -1);
    put(path, "{\"state\":\"ready\",\"active_generation\":0}");
    assert(ring_recording_gen(rec, &gen) == -1);
    /* larger than any read buffer: the leading object must still be read
     * without overrunning (ASan) */
    FILE *f = fopen(path, "w"); assert(f);
    fputs("{\"state\":\"ready\",\"active_generation\":7}", f);
    for (int i = 0; i < 8192; i++) fputc(' ', f);
    fclose(f);
    assert(ring_recording_gen(rec, &gen) == 0 && gen == 7);
    /* a path that only fits by losing its NUL is a failure, one more byte is not */
    assert(ring_paths_dir(rec, v, sizeof(v), s, sizeof(s)) == 0);
    size_t vl = strlen(v), sl = strlen(s);
    assert(ring_paths_dir(rec, v, vl, s, sizeof(s)) == -1);
    assert(ring_paths_dir(rec, v, sizeof(v), s, sl) == -1);
    assert(ring_paths_dir(rec, v, vl + 1, s, sl + 1) == 0);
    assert(unlink(path) == 0 && rmdir(rec) == 0);
    somno_ml_job_t j = { .type = SOMNO_ML_SRC_AS11_SA2 };
    strlcpy(j.dir, dir, sizeof(j.dir));
    strlcpy(j.id, "20300101_000000", sizeof(j.id));
    assert(as11_paths(&j, v, sizeof(v), s, sizeof(s)) == 0);
    vl = strlen(v); sl = strlen(s);
    assert(as11_paths(&j, v, vl, s, sizeof(s)) == -1);
    assert(as11_paths(&j, v, sizeof(v), s, sl) == -1);
    assert(as11_paths(&j, v, vl + 1, s, sl + 1) == 0);
}
/* NULL and over-long dir/id are ignored, never truncated into a match. */
static void test_as11_guards(void)
{
    const size_t dcap = sizeof(((somno_ml_job_t *)0)->dir);
    const size_t icap = sizeof(((somno_ml_job_t *)0)->id);
    char longdir[512], longid[512];
    memset(longdir, 'd', dcap); longdir[0] = '/'; longdir[dcap] = '\0';
    memset(longid, 'i', icap); longid[icap] = '\0';
    somno_ml_set_as11_active("/guard/day", "keep", true);
    somno_ml_set_as11_active(NULL, "keep", true);
    somno_ml_set_as11_active("/guard/day", NULL, true);
    somno_ml_set_as11_active(longdir, "keep", true);
    somno_ml_set_as11_active("/guard/day", longid, true);
    assert(as11_is_recording("/guard/day", "keep"));
    longdir[dcap - 1] = '\0';
    longid[icap - 1] = '\0';
    somno_ml_set_as11_active(longdir, longid, true);
    assert(as11_is_recording(longdir, longid));
    assert(!as11_is_recording("/guard/day", "keep"));
    somno_ml_set_as11_active(longdir, longid, false);
    assert(!as11_is_recording(longdir, longid));
}
/* An sa2 that grew past its header count: every record on disk is scored,
 * each epoch gets the stub's (constant) posterior, and the final .sst is
 * then idempotent. */
static void test_sa2_grown(const char *dir)
{
    char sa2[400], sst[400], sv[33];
    snprintf(sa2, sizeof(sa2), "%s/20300102_220000_sa2.snt", dir);
    snprintf(sst, sizeof(sst), "%s/20300102_220000_stages.sst", dir);
    sa2_file(sa2, 30, 1800);
    somno_ml_job_t job = { .type = SOMNO_ML_SRC_AS11_SA2 };
    strlcpy(job.dir, dir, sizeof(job.dir));
    strlcpy(job.id, "20300102_220000", sizeof(job.id));
    static uint8_t ref[8192], got[8192];
    assert(somno_ml_score_file(&job) == 0);
    FILE *rf = fopen(sst, "rb"); assert(rf);
    size_t ref_n = fread(ref, 1, sizeof(ref), rf); fclose(rf);
    struct utimbuf stale = { time(NULL) - 3600, time(NULL) - 3600 };
    assert(utime(sst, &stale) == 0);
    host_heap_caps_malloc_fail = 1;      /* PSRAM exhaustion must preserve the old SST */
    assert(somno_ml_score_file(&job) != 0);
    host_heap_caps_malloc_fail = 0;
    rf = fopen(sst, "rb"); assert(rf);
    assert(fread(got, 1, sizeof(got), rf) == ref_n); fclose(rf);
    assert(memcmp(ref, got, ref_n) == 0);
    assert(unlink(sst) == 0);
    assert(somno_ml_score_file(&job) == 0);
    int flags = somno_ml_sst_validate(sst, sv, sizeof(sv));
    assert(flags >= 0 && !(flags & (SOMNO_SST_FLAG_PARTIAL | SOMNO_SST_FLAG_INSUFFICIENT)));
    assert(strcmp(sv, somno_ml_model_semver()) == 0);
    assert(somno_ml_sst_validate(sst, NULL, sizeof(sv)) == flags);
    static uint8_t stages[4096], conf[4096];
    somno_sst_header_t h;
    uint32_t n = read_sst(sst, &h, stages, conf, sizeof(stages));
    assert(n > 0 && n * h.epoch_sec == 1800);
    for (uint32_t i = 0; i < n; i++)
        assert(stages[i] == 1 && conf[i] == conf[0] && conf[0] > 0);
    /* fresh final .sst: a later job must not rescore (the now-empty source
     * would otherwise turn it into an INSUFFICIENT stub) */
    struct utimbuf old = { time(NULL) - 3600, time(NULL) - 3600 };
    FILE *f = fopen(sa2, "wb"); assert(f); fclose(f);
    assert(utime(sa2, &old) == 0);
    assert(somno_ml_score_file(&job) == 0);
    assert(somno_ml_sst_validate(sst, NULL, 0) == flags);
    /* written in the same second as the source: not provably newer, rescore */
    assert(utime(sst, &old) == 0);
    assert(somno_ml_score_file(&job) == 0);
    assert(somno_ml_sst_validate(sst, NULL, 0) == SOMNO_SST_FLAG_INSUFFICIENT);
    assert(unlink(sa2) == 0 && unlink(sst) == 0);
    /* destination cannot be replaced (a directory): failure, no tmp left */
    char tmp[420];
    f = fopen(sa2, "wb"); assert(f); fclose(f);
    assert(mkdir(sst, 0700) == 0);
    assert(somno_ml_score_file(&job) == -1);
    struct stat st;
    snprintf(tmp, sizeof(tmp), "%s.tmp", sst);
    assert(stat(tmp, &st) != 0);
    assert(rmdir(sst) == 0 && unlink(sa2) == 0);
}
/* Ring fragments under the real on-device root: grouping by gap, and a
 * member with no complete epoch gets no consumer CSV. */
static void test_fragments(void)
{
    char root[] = "/tmp/somno-root-XXXXXX";
    assert(mkdtemp(root));
    snprintf(host_root, sizeof(host_root), "%s", root);
    const char *day = OX_RECORDINGS "/20300101";
    const int64_t t0 = 1893456000000LL;
    char a[300], b[300], c[300];
    snprintf(a, sizeof(a), "%s/fragA", day);
    snprintf(b, sizeof(b), "%s/fragB", day);
    snprintf(c, sizeof(c), "%s/fragC", day);
    ring_rec(a, t0, 1800, 1800);                              /* 30 min      */
    ring_rec(b, t0 + (30 + 10) * 60000LL, 10, 10);            /* 10 min gap  */
    ring_rec(c, t0 + (30 + 10 + 4 * 60) * 60000LL, 1800, 1800); /* 4 h gap */
    ring_rec_t grp[MAX_FRAG_GROUP];
    assert(ring_fragment_group("fragC", grp, MAX_FRAG_GROUP) == 1);
    assert(ring_fragment_group("fragA", grp, MAX_FRAG_GROUP) == 2);
    assert(strcmp(grp[0].id, "fragA") == 0 && strcmp(grp[1].id, "fragB") == 0);
    zeo_calls = 0;
    exports = 0;
    somno_ml_set_stage_export_cb(on_export);
    assert(score_ring_group(grp, 2, "fragA") == 0);
    assert(zeo_calls == 1 && strstr(zeo_last, "/fragA.csv"));
    assert(exports == 1);
    struct stat st;
    assert(stat(zeo_last, &st) == 0 && st.st_size > 0);
    static uint8_t stages[4096], conf[4096];
    somno_sst_header_t h;
    char sst[400];
    snprintf(sst, sizeof(sst), "%s/generations/1/data/stages.sst", a);
    assert(read_sst(sst, &h, stages, conf, sizeof(stages)) > 0);
    assert(!(h.flags & SOMNO_SST_FLAG_INSUFFICIENT));
    snprintf(sst, sizeof(sst), "%s/generations/1/data/stages.sst", b);
    assert(read_sst(sst, &h, stages, conf, sizeof(stages)) == 0);
    /* reconcile backfills a deleted CSV from the fresh .sst, once */
    char real[600];
    rm_tree(host_path(c, real, sizeof(real)));
    struct utimbuf old = { time(NULL) - 3600, time(NULL) - 3600 };
    snprintf(sst, sizeof(sst), "%s/generations/1/data/vitals.snt", a);
    assert(utime(host_path(sst, real, sizeof(real)), &old) == 0);
    snprintf(sst, sizeof(sst), "%s/generations/1/data/vitals.snt", b);
    assert(utime(host_path(sst, real, sizeof(real)), &old) == 0);
    assert(unlink(zeo_last) == 0 && stat(zeo_last, &st) != 0);
    exports = 0;
    assert(somno_ml_reconcile() == 0);
    assert(exports == 1 && stat(zeo_last, &st) == 0 && st.st_size > 0);
    assert(somno_ml_reconcile() == 0);   /* CSV now current: nothing to do */
    assert(exports == 1);
    somno_ml_job_t rj = { .type = SOMNO_ML_SRC_RING_VITALS };
    strlcpy(rj.id, "fragA", sizeof(rj.id));
    assert(somno_ml_score_file(&rj) == 0);           /* found, fresh */
    strlcpy(rj.id, "fragZ", sizeof(rj.id));
    assert(somno_ml_score_file(&rj) == -1);          /* no such recording */
    somno_ml_set_stage_export_cb(NULL);
    rm_tree(root);
    host_root[0] = '\0';
}
/* .sst -> consumer CSV path mapping at its length boundaries. */
static void test_csv_paths(void)
{
    char sst[700], csv[420], id[400];
    assert(sst_to_csv_path(OX_RECORDINGS "/20300101/a/", csv, sizeof(csv)));
    assert(!sst_to_csv_path(OX_RECORDINGS "/20300101/a", csv, sizeof(csv)));
    assert(sst_to_csv_path(SD_STREAMS "/20300101/20300101_220000_stages.sst",
                           csv, sizeof(csv)));
    assert(strstr(csv, "/20300101/") && strstr(csv, "20300101_220000.csv"));
    assert(!sst_to_csv_path("/tmp/x/20300101/20300101_220000_stages.sst",
                            csv, sizeof(csv)));
    /* ids of growing length: accepted ones map to <id>.csv, and once one is
     * rejected every longer one is too (and none overruns, ASan) */
    int rejected = 0;
    for (int len = 1; len < 300; len++) {
        memset(id, 'r', len); id[len] = '\0';
        snprintf(sst, sizeof(sst), OX_RECORDINGS "/20300101/%s/generations/1/data/stages.sst", id);
        bool ok = sst_to_csv_path(sst, csv, sizeof(csv));
        if (ok) {
            assert(!rejected);
            char tail[410];
            snprintf(tail, sizeof(tail), "/%s.csv", id);
            assert(strlen(csv) > strlen(tail) &&
                   strcmp(csv + strlen(csv) - strlen(tail), tail) == 0);
        } else {
            rejected = 1;
        }
    }
    assert(rejected);
    assert(!sst_to_csv_path(SD_STREAMS "/20300101/_stages.sst", csv, sizeof(csv)));
    rejected = 0;
    for (int len = 1; len < 300; len++) {
        memset(id, 'p', len); id[len] = '\0';
        snprintf(sst, sizeof(sst), SD_STREAMS "/20300101/%s_stages.sst", id);
        bool ok = sst_to_csv_path(sst, csv, sizeof(csv));
        if (ok) {
            assert(!rejected);
            char tail[410];
            snprintf(tail, sizeof(tail), "/as11_%s.csv", id);
            assert(strlen(csv) > strlen(tail) &&
                   strcmp(csv + strlen(csv) - strlen(tail), tail) == 0);
        } else {
            rejected = 1;
        }
    }
    assert(rejected);
}
/* Deadlines of the self-healing loop, right after start. */
static void test_deadlines(void)
{
    /* nothing periodic is due in the first minute after boot */
    assert(next_wait() > pdMS_TO_TICKS(60 * 1000));
    int sent = queue_sends;
    assert(somno_ml_enqueue_as11("/synthetic/verify", "pending") == 0);
    assert(queue_sends == sent + 1);
    verify_pending();                    /* just enqueued: not due yet */
    assert(queue_sends == sent + 1);
    int slot = -1;
    for (int i = 0; i < PENDING_CAP; i++)
        if (s_pending[i].used && strcmp(s_pending[i].id, "pending") == 0) slot = i;
    assert(slot >= 0 && s_pending[slot].tries == 0);
    s_pending[slot].due_ms = now_ms() - 1;   /* deadline passed, .sst missing */
    verify_pending();
    assert(queue_sends == sent + 2 && s_pending[slot].tries == 1);
    verify_pending();                    /* retry is minutes away, not now */
    assert(queue_sends == sent + 2 && s_pending[slot].tries == 1);
    /* argument guards: nothing reaches the queue */
    const size_t dcap = sizeof(((somno_ml_job_t *)0)->dir);
    const size_t icap = sizeof(((somno_ml_job_t *)0)->id);
    char longdir[512], longid[512];
    memset(longdir, 'd', dcap); longdir[0] = '/'; longdir[dcap] = '\0';
    memset(longid, 'i', icap); longid[icap] = '\0';
    sent = queue_sends;
    assert(somno_ml_enqueue_as11(NULL, "x") == -1);
    assert(somno_ml_enqueue_as11("/synthetic/verify", NULL) == -1);
    assert(somno_ml_enqueue_as11(longdir, "x") == -1);
    assert(somno_ml_enqueue_as11("/synthetic/verify", longid) == -1);
    assert(somno_ml_enqueue_ring(NULL) == -1);
    assert(somno_ml_enqueue_ring(longid) == -1);
    assert(queue_sends == sent);
    /* a full verify table still queues the job, just untracked */
    for (int i = 0; i < 2 * PENDING_CAP; i++) {
        char id[16];
        snprintf(id, sizeof(id), "ring%d", i);
        assert(somno_ml_enqueue_ring(id) == 0);
    }
    assert(queue_sends == sent + 2 * PENDING_CAP);
    for (int i = 0; i < PENDING_CAP; i++) assert(s_pending[i].used);
}
int main(void)
{
    char dir[] = "/tmp/somno-worker-XXXXXX";
    assert(mkdtemp(dir));
    ring_rec_t group[2] = {0};
    fixture(dir,0,&group[0]); fixture(dir,1,&group[1]);
    assert(score_ring_group(group,2,"synthetic") == 0);
    assert(created == 1 && destroyed == 1);
    created = destroyed = 0;
    for (int i=0;i<2;i++) {
        char path[400];
        snprintf(path,sizeof(path),"%s/generations/1/data/stages.sst",group[i].dir);
        assert(somno_ml_sst_validate(path,NULL,0) & SOMNO_SST_FLAG_INSUFFICIENT);
        cleanup(&group[i]);
    }
    /* 0-byte / truncated _sa2.snt from an interrupted session must be marked
     * INSUFFICIENT so reconcile stops re-enqueueing it on every boot. */
    {
        char sa2[400], sst[400];
        snprintf(sa2, sizeof(sa2), "%s/20260708_165632_sa2.snt", dir);
        snprintf(sst, sizeof(sst), "%s/20260708_165632_stages.sst", dir);
        FILE *f = fopen(sa2, "wb"); assert(f); fclose(f);
        somno_ml_job_t empty_job = { .type = SOMNO_ML_SRC_AS11_SA2 };
        strlcpy(empty_job.dir, dir, sizeof(empty_job.dir));
        strlcpy(empty_job.id, "20260708_165632", sizeof(empty_job.id));
        assert(somno_ml_score_file(&empty_job) == 0);
        assert(somno_ml_sst_validate(sst, NULL, 0) & SOMNO_SST_FLAG_INSUFFICIENT);
        assert(somno_ml_sst_validate(sst, NULL, 0) == SOMNO_SST_FLAG_INSUFFICIENT);
        assert(unlink(sa2) == 0 && unlink(sst) == 0);
    }
    test_ring_state(dir);
    test_sa2_grown(dir);
    assert(rmdir(dir)==0);
    test_fragments();
    test_csv_paths();
    test_as11_guards();
    /* no queue before the worker starts */
    assert(somno_ml_enqueue_as11("/synthetic/day", "early") == -1);
    assert(somno_ml_enqueue_ring("early") == -1);
    somno_ml_job_t active = { .type = SOMNO_ML_SRC_AS11_SA2 };
    strlcpy(active.dir,"/synthetic/day",sizeof(active.dir));
    strlcpy(active.id,"recording",sizeof(active.id));
    somno_ml_set_as11_active(active.dir,active.id,true);
    assert(as11_is_recording(active.dir,active.id));
    assert(somno_ml_score_file(&active) == 0);
    somno_ml_set_as11_active(active.dir,"previous",false);
    assert(as11_is_recording(active.dir,active.id));
    somno_ml_set_as11_active(active.dir,active.id,false);
    assert(!as11_is_recording(active.dir,active.id));
    assert(somno_ml_score_file(&active) == -1);
    assert(score_ring_group(NULL,0,"synthetic") == -1);
    test_worker_start_failures();
    somno_ml_worker_start();
    assert(s_queue && s_pending_mux);
    test_deadlines();
    puts("somno_worker_test: allocations, active-source deferral and stack budget passed");
    return 0;
}
