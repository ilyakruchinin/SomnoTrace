#!/usr/bin/env python3
"""Run production Maintenance requests, worker retirement, UI events and scans."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
service = (root / 'main/touch_maintenance.c').read_text()
ui = (root / 'main/touch_maintenance_ui.c').read_text()


def function(source, name):
    match = re.search(r'^(?:static )?(?:bool|void|esp_err_t) ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "touch_maintenance.h"
#include "touch_maintenance_ui.h"
#include "maintenance_fs.h"
#include "maintenance_ota.h"
#define MALLOC_CAP_SPIRAM 1
#define SD_LEASE_UPLOAD 1
#define CONFIG_SOMNOTRACE_BOARD_QEMU 0
#define tskNO_AFFINITY -1
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define pdMS_TO_TICKS(n) (n)
static maintenance_snapshot_t *s_snapshot;
static bool s_busy, s_read_job_active, s_read_cancelled;
static uint32_t s_next_job_id;
static bool therapy, recording, pending, lease, fail_task, immediate_task;
static unsigned fail_alloc, allocations, task_starts, reads, cancel_at_read, open_files, open_dirs;
static void (*queued_task)(void *);
static void *queued_argument;
static char streams[256], datalog[256], ox_path[256], mount_path[256];
#define SD_STREAMS_DIR streams
#define SD_SESSIONS_DIR streams
#define SD_SDCARD_DATALOG datalog
#define SD_SDCARD_DIR datalog
#define SD_OXYMETRY_DIR ox_path
#define SD_MOUNT_POINT mount_path
static bool bsp_display_is_therapy_active(void) { return therapy; }
static bool sd_storage_recording_active(void) { return recording; }
static bool sd_storage_recording_pending(void) { return pending; }
static bool sd_storage_is_ready(void) { return true; }
static uint32_t sd_storage_content_generation(void) { return 17; }
static bool sd_storage_lease_acquire(int role, unsigned timeout) {
    assert(role == 1 && !timeout && !lease); return lease = true;
}
static void sd_storage_lease_release(int role) { assert(role == 1 && lease); lease = false; }
static esp_err_t sd_storage_get_free(uint64_t *a, uint64_t *b) { *a=100000;*b=200000;return 0; }
static void *heap_caps_calloc(size_t n,size_t size,int cap) {
    assert(cap==1); return ++allocations==fail_alloc ? NULL : calloc(n,size);
}
static void *heap_caps_malloc(size_t size,int cap) { assert(cap==1);return malloc(size); }
static FILE *checked_open(const char *path,const char *mode) {
    FILE *f=fopen(path,mode);if(f)++open_files;return f;
}
static int checked_close(FILE *f) { assert(open_files);--open_files;return fclose(f); }
static DIR *checked_opendir(const char *path) { DIR *d=opendir(path);if(d)++open_dirs;return d; }
static int checked_closedir(DIR *d) { assert(open_dirs);--open_dirs;return closedir(d); }
static size_t checked_read(void *buf,size_t size,size_t count,FILE *f) {
    size_t n=fread(buf,size,count,f);
    if (++reads==cancel_at_read) touch_maintenance_cancel_reads();
    return n;
}
#define fopen checked_open
#define fclose checked_close
#define opendir checked_opendir
#define closedir checked_closedir
#define fread checked_read
/* Metadata parsing is a controlled valid/invalid fixture. The production
 * directory traversal, read, cancellation and cleanup are exercised intact. */
typedef struct { double valuedouble; } cJSON;
static cJSON parsed[2];
static cJSON *cJSON_Parse(const char *text) {
    double start,end;
    if(sscanf(text,"{\"start_epoch_ms\":%lf,\"end_epoch_ms\":%lf}",&start,&end)!=2)return NULL;
    parsed[0].valuedouble=start;parsed[1].valuedouble=end;return parsed;
}
static cJSON *cJSON_GetObjectItem(cJSON *j,const char *key) {
    return j ? j + !strcmp(key,"end_epoch_ms") : NULL;
}
static bool cJSON_IsNumber(cJSON *j) { return j!=NULL; }
static void cJSON_Delete(cJSON *j) { (void)j; }
static void vTaskDelay(unsigned n) { (void)n; }
static void psram_task_delete(void *p) { assert(!p); }
static void *psram_task_create(void (*fn)(void *),const char *name,unsigned stack,void *arg,
                               unsigned priority,int core,void *a,void *b) {
    assert(!strcmp(name,"maintenance") && stack==16384 && priority==3 && core==-1 && !a && !b);
    if (fail_task) return NULL;
    assert(!queued_task);++task_starts;
    if(immediate_task)fn(arg);else {queued_task=fn;queued_argument=arg;}
    return (void *)1;
}
static void run_worker(void) {
    assert(queued_task);void (*fn)(void *)=queued_task;void *arg=queued_argument;
    queued_task=NULL;queued_argument=NULL;fn(arg);
    assert(!s_busy && !s_read_cancelled && !s_read_job_active && !lease && !open_files && !open_dirs);
}
static esp_err_t release_check(maintenance_snapshot_t *s) { (void)s;return 0; }
static esp_err_t export_report(maintenance_snapshot_t *s) { (void)s;return 0; }
static esp_err_t recreate(maintenance_snapshot_t *s) { (void)s;return 0; }
static esp_err_t destructive(maintenance_snapshot_t *s) { (void)s;return 0; }
static esp_err_t uploader_reset_state(void) { return 0; }
static esp_err_t device_settings_save_current(void) { return 0; }
esp_err_t maintenance_format_start(void) { return 0; }
esp_err_t maintenance_factory_reset_start(void) { return 0; }
void maintenance_format_snapshot(bool *a,bool *done,bool *ok,char *error,size_t cap) {
    *a=false;*done=*ok=true;if(cap)error[0]=0;
}
esp_err_t maintenance_ota_start_sd(const char *s) { (void)s;return 0; }
esp_err_t maintenance_ota_start_url(const char *s) { (void)s;return 0; }
bool maintenance_ota_cancel(void) { return true; }
'''
fixture += service[service.index('typedef struct {\n    maintenance_snapshot_t value;'):service.index('static void publish(')]
for name in ['publish', 'touch_maintenance_snapshot', 'cancelled', 'touch_maintenance_cancel_reads',
             'exists_dir', 'insert', 'count_tree', 'completed_day', 'scan_card', 'list_files',
             'worker', 'touch_maintenance_request_tracked', 'touch_maintenance_request']:
    fixture += function(service, name)
fixture += ui[ui.index('typedef enum {\n    VIEW_STORAGE,'):ui.index('static void render(void);')]
fixture += r'''
static void *lv_event_get_user_data(lv_event_t *e) { return e->user_data; }
static const char *lv_textarea_get_text(lv_obj_t *o) { (void)o;return ""; }
'''
for name in ['blocked', 'show', 'is_read_action', 'read_result_current', 'read_rows_current',
             'read_rows_rendered', 'refresh_read_snapshot', 'request', 'event']:
    fixture += function(ui, name)
fixture += r'''
static void click(int action) {
    /* Invoke the actual production CLICKED event callback with LVGL user data.
     * Gesture synthesis/rendering are separate root-owned acceptance gates. */
    lv_event_t e={.user_data=(void *)(intptr_t)action};event(&e);
}
static void reset_ui(ui_t *u,view_t view) {
    memset(u,0,sizeof(*u));s=u;u->ready=true;u->view=view;u->view_generation=1;
}
static unsigned walk_checks;
static bool depart_during_walk(void *unused) {
    if (++walk_checks==5) touch_maintenance_cancel_reads();
    return cancelled(unused);
}
int main(int argc,char **argv) {
    assert(argc==2);
    snprintf(streams,sizeof(streams),"%s/raw",argv[1]);
    snprintf(datalog,sizeof(datalog),"%s/edf",argv[1]);
    snprintf(ox_path,sizeof(ox_path),"%s/ox",argv[1]);
    snprintf(mount_path,sizeof(mount_path),"%s",argv[1]);
    ui_t u;reset_ui(&u,VIEW_FILES);strlcpy(u.day,"20260902",sizeof(u.day));
    request(MAINT_FILES,u.day);uint32_t files_job=u.read_job_id;assert(files_job && s_busy);
    click(BUTTON_BACK);
    assert(u.view==VIEW_STORAGE && u.read_pending && s_read_cancelled && !u.snapshot.count);
    unsigned started=task_starts;
    refresh_read_snapshot();assert(task_starts==started && u.read_pending);
    run_worker(); /* Cancelled file result cannot become day rows. */
    assert(s_snapshot->result==ESP_ERR_INVALID_STATE && !s_snapshot->count);
    refresh_read_snapshot();
    assert(task_starts==started+1 && !u.read_pending && u.read_job_id!=files_job && !u.snapshot.count);
    uint32_t scan_job=u.read_job_id;run_worker();refresh_read_snapshot();
    assert(u.snapshot.job_id==scan_job && read_result_current() && u.snapshot.count==1);
    assert(!strcmp(u.snapshot.entries[0].name,"20260902"));
    /* Same action, different identity is stale even if directory/day matches. */
    s_snapshot->job_id=files_job;s_snapshot->action=MAINT_SCAN;
    refresh_read_snapshot();assert(!u.snapshot.count && !read_result_current());
    click(BUTTON_ROW);assert(u.view==VIEW_STORAGE);
    s_snapshot->job_id=scan_job;refresh_read_snapshot();read_rows_rendered();
    /* Refresh supersedes even a same-view, same-action request. */
    click(BUTTON_FIRST);uint32_t first=u.read_job_id;assert(first);
    click(BUTTON_SCAN);assert(u.read_pending && s_read_cancelled);
    run_worker();refresh_read_snapshot();assert(u.read_job_id && u.read_job_id!=first);
    run_worker();refresh_read_snapshot();assert(u.snapshot.count==1);
    /* Deferred repaint must not reinterpret the still-held previous row. */
    click(BUTTON_ROW);assert(u.view==VIEW_STORAGE);
    read_rows_rendered();
    /* Repeated navigation drops an obsolete pending destination intent. */
    click(BUTTON_ROW);assert(u.view==VIEW_FILES && s_busy);
    click(BUTTON_BACK);assert(u.read_pending);
    show(VIEW_SYSTEM);assert(!u.read_pending && !u.read_job_id);
    started=task_starts;run_worker();refresh_read_snapshot();assert(task_starts==started && !u.snapshot.count);
    /* Image Back cancels read work; re-entry waits for retirement. */
    show(VIEW_FIRMWARE);click(BUTTON_SD);uint32_t image_job=u.read_job_id;
    click(BUTTON_BACK);assert(u.view==VIEW_FIRMWARE && !u.read_pending && s_read_cancelled);
    click(BUTTON_SD);assert(u.view==VIEW_IMAGES && u.read_pending);
    run_worker();refresh_read_snapshot();assert(u.read_job_id!=image_job);
    run_worker();refresh_read_snapshot();assert(u.snapshot.count==1 && read_result_current());
    /* A finished departed image result also cannot be adopted by a new Storage view. */
    reset_ui(&u,VIEW_STORAGE);request(MAINT_SCAN,NULL);assert(!u.snapshot.count);
    run_worker();refresh_read_snapshot();assert(u.snapshot.count==1);
    /* Mutations retain ownership, then a queued read starts exactly once. */
    assert(!touch_maintenance_request(MAINT_RESET_UPLOAD,NULL));
    show(VIEW_STORAGE);request(MAINT_SCAN,NULL);
    assert(u.read_pending && s_busy && !s_read_job_active && !s_read_cancelled);
    run_worker();assert(s_snapshot->result==ESP_OK);
    refresh_read_snapshot();assert(!u.read_pending && s_read_job_active);
    run_worker();refresh_read_snapshot();
    /* Inner metadata cancellation propagates, rather than an incomplete-day estimate. */
    reads=0;cancel_at_read=1;request(MAINT_SCAN,NULL);run_worker();
    assert(reads==1 && s_snapshot->result==ESP_ERR_INVALID_STATE && !s_snapshot->census_valid && !s_snapshot->count);
    cancel_at_read=0;request(MAINT_SCAN,NULL);run_worker();refresh_read_snapshot();
    assert(s_snapshot->result==ESP_OK && s_snapshot->estimate_samples==1);
    /* Independently distinguish an incomplete/absent night from cancellation. */
    bool complete=true;assert(completed_day("20260101",&complete)==ESP_OK && !complete);
    pending=true;assert(completed_day("20260902",&complete)==ESP_ERR_INVALID_STATE && !complete);pending=false;
    /* Admission and task allocation failures terminate intent, without retry loops. */
    fail_alloc=allocations+2;request(MAINT_SCAN,NULL);
    assert(!u.read_pending && !s_busy && !u.read_job_id);fail_alloc=0;
    fail_task=true;request(MAINT_SCAN,NULL);fail_task=false;
    assert(!u.read_pending && !s_busy && !u.read_job_id && !u.snapshot.count);
    started=task_starts;refresh_read_snapshot();assert(task_starts==started);
    /* Completion before admission returns cannot read a freed job for its ID. */
    immediate_task=true;request(MAINT_SCAN,NULL);immediate_task=false;
    assert(u.read_job_id && read_result_current() && !u.snapshot.busy && u.snapshot.count==1);
    uint32_t id=99;assert(touch_maintenance_request_tracked(MAINT_NONE,NULL,&id)==ESP_ERR_INVALID_ARG && !id);
    s_next_job_id=UINT32_MAX;assert(!touch_maintenance_request_tracked(MAINT_SCAN,NULL,&id) && id==1);run_worker();
    s_busy=s_read_job_active=true;
    maintenance_fs_totals_t totals={0};
    assert(maintenance_fs_walk(streams,MAINT_FS_COUNT,false,&totals,depart_during_walk,NULL)==ECANCELED);
    assert(totals.files<40 && s_read_cancelled);
    s_busy=s_read_job_active=s_read_cancelled=false;
    assert(!open_files && !open_dirs && !lease);free(s_snapshot);
    puts("Maintenance production navigation/retirement: Back, refresh, re-entry, stale identities, mutations, metadata cancellation and OOM passed");
}
'''
with tempfile.TemporaryDirectory(prefix='somno-maintenance-navigation-') as directory:
    d = Path(directory)
    (d / 'lvgl.h').write_text('#pragma once\ntypedef struct {int unused;} lv_obj_t;\ntypedef struct {int unused;} lv_timer_t;\ntypedef struct {void *user_data;} lv_event_t;\n')
    for sub in ['raw/20260902', 'edf/20260902', 'ox']:
        (d / sub).mkdir(parents=True)
    for i in range(40):
        (d / f'raw/20260902/{i:06d}_session.json').write_text('{"start_epoch_ms":1,"end_epoch_ms":2}')
    (d / 'somnotrace-7b-fixture.bin').write_bytes(b'x')
    (d / 'test.c').write_text(fixture)
    binary = d / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                    '-D_DARWIN_C_SOURCE', '-D_POSIX_C_SOURCE=200809L', '-fsanitize=address,undefined',
                    '-I', str(d), '-I', str(root / 'scripts/test_include'), '-I', str(root / 'main'),
                    str(d / 'test.c'), str(root / 'main/maintenance_fs.c'),
                    str(root / 'main/maintenance_model.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(d)], check=True)
