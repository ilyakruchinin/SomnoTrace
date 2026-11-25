/*
 * SomnoTrace - BreathingRoom backend collection and response regressions
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
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "uploader.h"
#include "upload_http.h"
#include "esp_log.h"

static int open_calls, fail_open_call, read_calls, fail_read_call;
static int warning_count, send_count, sent_n, marks, status = 201;
static bool change_directory;
static int alloc_calls, fail_alloc_call;
static upload_day_t index_day;
static char sent_paths[1000][128], sent_names[8][128], remote_id[64];
static char sent_field[32], sent_value[64];
static const char *response = "{\"created\":1,\"skipped\":0,\"deferred\":0,\"sessions\":[]}";

static DIR *mock_opendir(const char *path)
{
    open_calls++;
    if (open_calls == fail_open_call) { errno = EIO; return NULL; }
    if (open_calls == 2 && change_directory) {
        char file[256];
        snprintf(file, sizeof(file), "%s/20260102_015519_EVE.edf", path);
        FILE *f = fopen(file, "wb");
        assert(f);
        fputs("synthetic", f);
        fclose(f);
    }
    return opendir(path);
}

static struct dirent *mock_readdir(DIR *dir)
{
    if (++read_calls == fail_read_call) { errno = EIO; return NULL; }
    return readdir(dir);
}

static void *mock_malloc(size_t n)
{
    if (++alloc_calls == fail_alloc_call) return NULL;
    return malloc(n);
}

static void mock_warning(const char *tag, const char *fmt, ...)
{
    (void)tag; (void)fmt;
    warning_count++;
}

#undef ESP_LOGW
#undef ESP_LOGI
#define ESP_LOGW(...) mock_warning(__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (false) printf(fmt, ##__VA_ARGS__); } while (0)
#define uxTaskGetStackHighWaterMark(task) 4096u
#define opendir mock_opendir
#define readdir mock_readdir
#define malloc mock_malloc
#define SD_MOUNT_POINT "."
#include "uploader_breathingroom.c"
#undef opendir
#undef readdir
#undef malloc

bool uploader_is_breathingroom_configured(void) { return true; }
esp_err_t uploader_load_config(uploader_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->br_key, sizeof(cfg->br_key), "slp_synthetic");
    return ESP_OK;
}
upload_day_t *upload_index_day(uint32_t day, bool create)
{
    (void)day; (void)create;
    return &index_day;
}
int upload_index_backend_slot(const char *id) { (void)id; return 0; }
void upload_ox_mark(const upload_ox_ref_t *ref, int slot,
                    upload_unit_status_t state, const char *id)
{
    (void)ref; (void)slot;
    assert(state == UG_OK && id && id[0]);
    marks++;
    snprintf(remote_id, sizeof(remote_id), "%s", id);
}
/* Stage-unit plumbing the backend now calls.  stage_needs_tz is driven by
 * the test so the "send timeZone only when the server cannot derive it"
 * branch is exercised in both directions. */
static bool stage_needs_tz;
static int stage_marks;
bool upload_stage_needs_timezone(const upload_stage_ref_t *ref)
{
    (void)ref;
    return stage_needs_tz;
}
void uploader_tz_name(char *out, size_t n)
{
    snprintf(out, n, "%s", "Australia/Melbourne");
}
bool upload_stage_ref_has_fmt(const upload_stage_ref_t *ref,
                              upload_stage_fmt_t fmt)
{
    (void)ref;
    return fmt == UPLOAD_STAGE_FMT_SST;
}
void upload_stage_sst_filename(const upload_stage_ref_t *ref,
                               char *out, size_t n)
{
    snprintf(out, n, "%s_stages.sst", ref->unit_id);
}
void upload_stage_mark(const upload_stage_ref_t *ref, int slot,
                       upload_unit_status_t state, const char *id)
{
    (void)ref; (void)slot;
    assert(state == UG_OK && id && id[0]);
    stage_marks++;
    snprintf(remote_id, sizeof(remote_id), "%s", id);
}

esp_tls_t *uph_connect(const char *host, int timeout)
{
    (void)host; (void)timeout;
    return (esp_tls_t *)1;
}
void uph_disconnect(esp_tls_t *tls) { (void)tls; }
int uph_put_files_named(esp_tls_t *tls, const char *host, const char *path,
                        const char *bearer, const char *accept,
                        const char *const *paths, int n,
                        const char *const *filenames,
                        const char *field, const char *value,
                        char **body, size_t cap, bool *pc)
{
    (void)tls; (void)host; (void)path; (void)bearer; (void)accept;
    (void)cap;
    snprintf(sent_field, sizeof(sent_field), "%s", field ? field : "");
    snprintf(sent_value, sizeof(sent_value), "%s", value ? value : "");
    send_count++;
    sent_n = n;
    assert(n <= 1000);
    for (int i = 0; i < n; i++) {
        snprintf(sent_paths[i], sizeof(sent_paths[i]), "%s", paths[i]);
        if (filenames) {
            assert(n <= 8);
            snprintf(sent_names[i], sizeof(sent_names[i]), "%s", filenames[i]);
        }
    }
    *body = strdup(response);
    *pc = false;
    return status;
}
int uph_put_files(esp_tls_t *tls, const char *host, const char *path,
                  const char *bearer, const char *accept,
                  const char *const *paths, int n,
                  const char *field, const char *value,
                  char **body, size_t cap, bool *pc)
{
    return uph_put_files_named(tls, host, path, bearer, accept, paths, n,
                               NULL, field, value, body, cap, pc);
}

static void reset(void)
{
    open_calls = fail_open_call = read_calls = fail_read_call = 0;
    alloc_calls = fail_alloc_call = 0;
    warning_count = send_count = sent_n = marks = stage_marks = 0;
    sent_field[0] = sent_value[0] = '\0';
    stage_needs_tz = false;
    change_directory = false;
    memset(&index_day, 0, sizeof(index_day));
    s_tls = (esp_tls_t *)1;
    s_peer_close = s_ox_blocked = false;
}
static void touch_file(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "SDCARD/DATALOG/20260101/%s", name);
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs("synthetic", f);
    fclose(f);
}
static void clean_folder(void)
{
    DIR *dir = opendir("SDCARD/DATALOG/20260101");
    assert(dir);
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "SDCARD/DATALOG/20260101/%s", de->d_name);
        assert(unlink(path) == 0);
    }
    closedir(dir);
}

int main(void)
{
    char tmp[] = "/tmp/snt-br-backend-XXXXXX";
    char *root = mkdtemp(tmp);
    assert(root && chdir(root) == 0);
    assert(mkdir("SDCARD", 0700) == 0);
    assert(mkdir("SDCARD/DATALOG", 0700) == 0);
    assert(mkdir("SDCARD/DATALOG/20260101", 0700) == 0);
    assert(br_sibling_event("20260102_015520_EVE.edf", 6927));
    assert(br_sibling_event("20260102_000005_CSL.edf", 86395));
    assert(br_sibling_event("20260102_021020_EVE.edf", 6920));
    assert(!br_sibling_event("20260102_021021_EVE.edf", 6920));
    assert(!br_sibling_event("20260102_015520_EVE.txt", 6927));
    assert(!br_sibling_event("20260102_015520_EVE.edf.bak", 6927));
    assert(!br_sibling_event("20260102_016000_EVE.edf", 7200));
    assert(!br_sibling_event("20260102_250000_EVE.edf", 3600));
    assert(!br_sibling_event("20260102_015560_EVE.edf", 7000));
    assert(!br_sibling_event("20260x02_015520_EVE.edf", 6927));
    assert(!br_sibling_event(NULL, 0));

    upload_group_ref_t g = {.prefix_sec = 6927, .kinds = UGK_BRP, .n_files = 1};
    snprintf(g.files[0], sizeof(g.files[0]), "20260102_015527_BRP.edf");
    for (int i = 0; i < 3; i++) {
        char name[40];
        snprintf(name, sizeof(name), "20260102_0155%02d_EVE.edf", 20 + i);
        touch_file(name);
        snprintf(name, sizeof(name), "20260102_0155%02d_CSL.edf", 20 + i);
        touch_file(name);
    }
    reset();
    assert(br_put_group("20260101", &g) == UPLOAD_OK && sent_n == 7);
    touch_file("20260102_015527_EVE.edf");
    g.n_files = 2;
    snprintf(g.files[1], sizeof(g.files[1]), "20260102_015527_EVE.edf");
    reset();
    assert(br_put_group("20260101", &g) == UPLOAD_OK && sent_n == 8);
    for (int i = 0; i < sent_n; i++)
        for (int j = i + 1; j < sent_n; j++)
            assert(strcmp(sent_paths[i], sent_paths[j]) != 0);
    reset(); fail_open_call = 1;
    assert(br_put_group("20260101", &g) == UPLOAD_ERR_TRANSIENT && send_count == 0);
    reset(); fail_open_call = 2;
    assert(br_put_group("20260101", &g) == UPLOAD_ERR_TRANSIENT && send_count == 0);
    reset(); fail_read_call = 1;
    assert(br_put_group("20260101", &g) == UPLOAD_ERR_TRANSIENT && send_count == 0);
    reset();
    assert(br_put_group("20260101", &g) == UPLOAD_OK);
    int second_pass = read_calls / 2 + 1;
    reset(); fail_read_call = second_pass;
    assert(br_put_group("20260101", &g) == UPLOAD_ERR_TRANSIENT && send_count == 0);
    reset(); fail_alloc_call = 1;
    assert(br_put_group("20260101", &g) == UPLOAD_ERR_TRANSIENT && send_count == 0);
    reset(); fail_alloc_call = 2;
    assert(br_put_group("20260101", &g) == UPLOAD_ERR_TRANSIENT && send_count == 0);
    reset(); change_directory = true;
    assert(br_put_group("20260101", &g) == UPLOAD_ERR_TRANSIENT && send_count == 0);
    reset();
    g.kinds = UGK_EVE; g.prefix_sec = 6930;
    index_day.n_groups = 1;
    index_day.groups[0].prefix_sec = 6927;
    index_day.groups[0].kinds = UGK_BRP;
    index_day.groups[0].be[0].status = UG_OK;
    assert(br_put_group("20260101", &g) == UPLOAD_OK && warning_count == 0);

    upload_ox_ref_t ref = {.n_files = 1};
    snprintf(ref.source_name, sizeof(ref.source_name), "20260102015549");
    snprintf(ref.relative_paths[0], sizeof(ref.relative_paths[0]), "source/source.bin");
    snprintf(ref.local_paths[0], sizeof(ref.local_paths[0]), "recording/source/source.bin");
    status = 200;
    response = "{\"results\":[{\"filename\":\"20260102015549.bin\",\"status\":\"replaced\",\"id\":\"synthetic-id\"}]}";
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_OK && marks == 1);
    assert(strcmp(remote_id, "synthetic-id") == 0);
    assert(strcmp(sent_names[0], "20260102015549.bin") == 0);
    assert(strcmp(sent_paths[0], "recording/source/source.bin") == 0);
    snprintf(ref.source_name, sizeof(ref.source_name), "20260102015549.bin");
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_OK);
    assert(strcmp(sent_names[0], "20260102015549.bin") == 0);
    snprintf(ref.source_name, sizeof(ref.source_name), "20260102015549.BIN");
    response = "{\"results\":[{\"filename\":\"20260102015549.BIN\",\"status\":\"imported\",\"id\":\"synthetic-id\"}]}";
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_OK);
    assert(strcmp(sent_names[0], "20260102015549.BIN") == 0);
    snprintf(ref.source_name, sizeof(ref.source_name), "ring.20260102015549");
    response = "{\"results\":[{\"filename\":\"ring.20260102015549.bin\",\"status\":\"imported\",\"id\":\"synthetic-id\"}]}";
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_OK);
    assert(strcmp(sent_names[0], "ring.20260102015549.bin") == 0);
    snprintf(ref.source_name, sizeof(ref.source_name), "20260102015549");
    ref.n_files = 2;
    snprintf(ref.relative_paths[1], sizeof(ref.relative_paths[1]), "source/source.vld");
    snprintf(ref.local_paths[1], sizeof(ref.local_paths[1]), "recording/source/source.vld");
    response = "{\"results\":[{\"filename\":\"20260102015549.bin\",\"status\":\"imported\",\"id\":\"synthetic-id\"}]}";
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_ERR_TRANSIENT && marks == 0);
    response = "{\"results\":[{\"filename\":\"20260102015549.vld\",\"status\":\"replaced\",\"id\":\"synthetic-id\"},"
               "{\"filename\":\"20260102015549.bin\",\"status\":\"imported\",\"id\":\"synthetic-id\"}]}";
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_OK && marks == 1);
    ref.n_files = 1;
    const char *bad[] = {
        "{\"results\":[]}",
        "{\"results\":[null]}",
        "{\"results\":[{\"filename\":\"20260102015549.bin\",\"status\":\"imported\"}]}",
        "{\"results\":[{\"filename\":\"wrong.bin\",\"status\":\"imported\",\"id\":\"synthetic-id\"}]}",
        "{\"results\":[{\"filename\":\"20260102015549.bin\",\"status\":\"imported\",\"id\":\"synthetic-id\"},{\"filename\":\"extra.bin\",\"status\":\"imported\",\"id\":\"extra-id\"}]}"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset(); response = bad[i];
        assert(br_put_oximetry(&ref) == UPLOAD_ERR_TRANSIENT && marks == 0);
    }
    /* Per-file oximetry rejection vs transient storage error vs 402 tier gate. */
    status = 200;
    response = "{\"results\":[{\"filename\":\"20260102015549.bin\",\"status\":\"rejected\",\"reason\":\"unrecognised format\"}]}";
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_ERR_UNIT_REJECTED && marks == 0);
    response = "{\"results\":[{\"filename\":\"20260102015549.bin\",\"status\":\"rejected\",\"reason\":\"storage error\"}]}";
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_ERR_TRANSIENT && marks == 0);
    status = 402;
    response = "{\"error\":\"Premium required\"}";
    reset();
    assert(br_put_oximetry(&ref) == UPLOAD_ERR_PERMANENT && s_ox_blocked);
    s_ox_blocked = false;
    status = 200;
    clean_folder();
    for (int i = 0; i < 1000; i++) {
        int sec = 6300 + i;
        char name[40];
        snprintf(name, sizeof(name), "20260102_%02d%02d%02d_EVE.edf",
                 sec / 3600, (sec / 60) % 60, sec % 60);
        touch_file(name);
    }
    reset(); g.kinds = UGK_BRP; g.prefix_sec = 6927;
    assert(br_put_group("20260101", &g) == UPLOAD_ERR_TRANSIENT && send_count == 0);
    clean_folder();
    assert(rmdir("SDCARD/DATALOG/20260101") == 0);
    assert(rmdir("SDCARD/DATALOG") == 0);
    assert(rmdir("SDCARD") == 0);
    assert(chdir("/") == 0 && rmdir(root) == 0);
    /* ── sleep stages (/api/sleep/upload) ─────────────────────────── */
    {
        upload_stage_ref_t st = {0};
        snprintf(st.unit_id, sizeof(st.unit_id), "%s", "as11_20260102_015519");
        snprintf(st.sst_path, sizeof(st.sst_path), "%s",
                 "streams/20260101/20260102_015519_stages.sst");

        status = 200;
        response = "{\"results\":[{\"filename\":\"as11_20260102_015519_stages.sst\","
                   "\"status\":\"imported\",\"id\":\"stage-id-1\",\"epochs\":42}]}";
        reset();
        assert(br_put_stage(&st) == UPLOAD_OK && stage_marks == 1);
        assert(strcmp(remote_id, "stage-id-1") == 0);
        assert(sent_n == 1);
        assert(strcmp(sent_names[0], "as11_20260102_015519_stages.sst") == 0);
        assert(strcmp(sent_paths[0], "streams/20260101/20260102_015519_stages.sst") == 0);
        assert(sent_field[0] == '\0' && sent_value[0] == '\0');

        /* The server cannot derive the offset → the zone goes with the file. */
        response = "{\"results\":[{\"filename\":\"as11_20260102_015519_stages.sst\","
                   "\"status\":\"replaced\",\"id\":\"stage-id-1\"}]}";
        reset();
        stage_needs_tz = true;
        assert(br_put_stage(&st) == UPLOAD_OK && stage_marks == 1);
        assert(strcmp(sent_field, "timeZone") == 0);
        assert(strcmp(sent_value, "Australia/Melbourne") == 0);

        /* Per-file rejection: transient only for a storage error. */
        response = "{\"results\":[{\"filename\":\"as11_20260102_015519_stages.sst\","
                   "\"status\":\"rejected\",\"reason\":\"partial scoring run\"}]}";
        reset();
        assert(br_put_stage(&st) == UPLOAD_ERR_UNIT_REJECTED && stage_marks == 0);
        response = "{\"results\":[{\"filename\":\"as11_20260102_015519_stages.sst\","
                   "\"status\":\"rejected\",\"reason\":\"storage error\"}]}";
        reset();
        assert(br_put_stage(&st) == UPLOAD_ERR_TRANSIENT && stage_marks == 0);

        /* Premium gate: 402 latches for the rest of the pass. */
        status = 402;
        response = "{\"error\":\"Premium required\",\"feature\":\"tracker\"}";
        reset();
        assert(br_put_stage(&st) == UPLOAD_ERR_PERMANENT);
        assert(send_count == 1);
        reset();
        s_stage_blocked = true;          /* what the 402 above latched */
        assert(br_put_stage(&st) == UPLOAD_ERR_PERMANENT && send_count == 0);
        s_stage_blocked = false;

        /* A dead socket is transient, not permanent; a bare ref is not sent. */
        status = 200;
        response = "{\"results\":[{\"filename\":\"as11_20260102_015519_stages.sst\","
                   "\"status\":\"imported\",\"id\":\"stage-id-1\"}]}";
        reset();
        s_tls = NULL;
        assert(br_put_stage(&st) == UPLOAD_ERR_TRANSIENT);
        reset();
        st.sst_path[0] = '\0';
        assert(br_put_stage(&st) == UPLOAD_ERR_UNIT_REJECTED);
    }

    puts("br_backend_test: matching, collection, scan failures, names and result validation passed");
    return 0;
}
