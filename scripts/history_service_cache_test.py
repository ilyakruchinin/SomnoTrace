#!/usr/bin/env python3
"""Production cache adapters with counted, mutable source loaders."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'main/touch_history.c').read_text()
def function(name, s=s):
    match=re.search(r'^(?:static )?[\w\s*]+\b'+name+r'\([^;]*?\)\s*\{',s,re.M)
    assert match,name
    end=match.end();depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[match.start():end]+'\n'
session=s[s.index('typedef struct {\n    char id[TOUCH_HISTORY_SESSION_ID_LEN];'):s.index('static esp_err_t history_collect_eligible_intervals_leased(')]
vector=s[s.index('typedef struct {\n    touch_history_event_t *items;'):s.index('static bool history_',s.index('typedef struct {\n    touch_history_event_t *items;'))]
# The vector definition ends before parser helpers; do not pull those helpers in.
vector=vector[:vector.index('} history_event_vector_t;')+len('} history_event_vector_t;')]
keys=s[s.index('enum { HISTORY_MEM_SESSIONS'):s.index('typedef struct {\n    char id[TOUCH_HISTORY_SESSION_ID_LEN];')]
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "touch_history.h"
#include "history_cache.h"
#define ESP_ERR_NO_MEM 0x101
#define HISTORY_AXIS_MAX_MS (7LL*86400000)
#define SD_LEASE_UPLOAD 2
static uint32_t epoch=1;
static bool cancelled, cancel_after_read;
static int leases, session_reads, event_reads, graph_reads, stats_reads, night_reads, fail_alloc;
static size_t source_events=3;
static uint32_t sd_storage_content_generation(void) { return epoch; }
static bool history_operation_cancelled(const touch_history_operation_t *op) { (void)op;return cancelled; }
static void history_operation_progress(const touch_history_operation_t *op,uint16_t n) { (void)op;(void)n; }
static bool valid_day(const char *day) { return day && strlen(day)==8; }
static void *history_alloc(size_t n,bool clear) { if(fail_alloc){--fail_alloc;return NULL;}return clear?calloc(1,n):malloc(n); }
static esp_err_t history_lease_acquire_operation(const touch_history_operation_t *op)
{ (void)op; if(cancelled)return TOUCH_HISTORY_ERR_CANCELLED; ++leases;return ESP_OK; }
static void sd_storage_lease_release(int role) { assert(role==SD_LEASE_UPLOAD && leases>0);--leases; }
'''
mocks=r'''
static esp_err_t history_load_sessions_uncached(const char *day,history_session_info_t **out,
    size_t *count,size_t *skipped,const touch_history_operation_t *op)
{
    (void)day;(void)op; ++session_reads;
    *out=calloc(2,sizeof(**out)); assert(*out); *count=2;*skipped=1;
    (*out)[0].start_ms=1000;(*out)[1].start_ms=5000;
    return ESP_OK;
}
static esp_err_t history_collect_events_uncached(const char *day,const history_session_info_t *sessions,
    size_t count,history_event_vector_t *events,touch_history_event_totals_t *totals,
    const touch_history_operation_t *op,uint16_t a,uint16_t b)
{
    (void)day;(void)sessions;(void)count;(void)op;(void)a;(void)b;
    ++event_reads; events->count=events->capacity=source_events;
    events->items=source_events?calloc(source_events,sizeof(*events->items)):NULL;
    for(size_t i=0;i<source_events;++i){events->items[i].start_ms=1000+100*i;events->items[i].end_ms=1050+100*i;}
    totals->total_count=source_events;totals->complete=true;
    return ESP_OK;
}
static esp_err_t history_load_overview_leased(const char *day,touch_history_signal_t signal,
    int64_t start,int64_t end,touch_history_overview_t *out,bool filter,
    const touch_history_operation_t *op,uint16_t a,uint16_t b)
{
    (void)day;(void)filter;(void)op;(void)a;(void)b;
    ++graph_reads;out->loaded=true;out->axis_start_ms=start;out->axis_end_ms=end;out->signal=signal;
    out->upper_x100[0]=32700;
    if(cancel_after_read)cancelled=true;
    return ESP_OK;
}
static void history_stats_prepare(touch_history_stats_t *out,touch_history_signal_t signal,
    int64_t start,int64_t end,bool filter)
{ memset(out,0,sizeof(*out));out->signal=signal;out->start_ms=start;out->end_ms=end;out->therapy_only=filter; }
static esp_err_t history_load_stats_leased(const char *day,touch_history_signal_t signal,
    int64_t start,int64_t end,bool filter,touch_history_stats_t *out,const touch_history_operation_t *op)
{
    (void)day;(void)signal;(void)start;(void)end;(void)filter;(void)op;
    ++stats_reads;out->loaded=out->exact=out->source_raw=true;
    out->value_count=1;out->values[0].available=true;out->values[0].value_x100=23;
    return ESP_OK;
}
static esp_err_t history_load_night_uncached(const char *day,touch_history_night_t *out,
    touch_history_session_t *sessions,size_t capacity,const touch_history_operation_t *op)
{
    (void)op;++night_reads;memset(out,0,sizeof(*out));memcpy(out->day,day,9);
    out->session_count=2;out->sessions_returned=capacity<2?capacity:2;
    for(size_t i=0;i<capacity;++i){memset(&sessions[i],0,sizeof(*sessions));sessions[i].start_ms=1000+i*100;}
    return ESP_OK;
}
'''
main=r'''
int main(void)
{
    history_cache_select_day("20260901");
    history_session_info_t *sessions=NULL;size_t count,skipped;
    for(int i=0;i<5;++i){
        assert(history_load_sessions_leased("20260901",&sessions,&count,&skipped,NULL)==ESP_OK);
        assert(count==2 && skipped==1 && sessions[1].start_ms==5000);free(sessions);
    }
    assert(session_reads==1);
    for(int i=0;i<5;++i){
        history_event_vector_t events={0};touch_history_event_totals_t totals={0};
        assert(history_collect_events_leased("20260901",NULL,0,&events,&totals,NULL,0,1000)==ESP_OK);
        assert(events.count==3 && totals.total_count==3 && events.items[2].start_ms==1200);free(events.items);
    }
    assert(event_reads==1);
    touch_history_overview_t *view=calloc(1,sizeof(*view));touch_history_stats_t stats;
    touch_history_night_t night;touch_history_session_t captions[2];
    for(int i=0;i<5;++i){
        assert(touch_history_load_view_ex("20260901",0,1000,2000,false,view,NULL)==ESP_OK);
        assert(touch_history_load_stats_ex("20260901",0,1000,2000,false,&stats,NULL)==ESP_OK);
        assert(touch_history_load_night_ex("20260901",&night,captions,2,NULL)==ESP_OK);
        assert(view->upper_x100[0]==32700 && stats.values[0].value_x100==23 && stats.exact && stats.source_raw);
        assert(night.sessions_returned==2 && captions[1].start_ms==1100);
    }
    assert(graph_reads==1 && stats_reads==1 && night_reads==1 && !leases);
    /* Latest source generation forces all snapshot types back to the loader. */
    ++epoch;
    assert(touch_history_load_view_ex("20260901",0,1000,2000,false,view,NULL)==ESP_OK);
    assert(touch_history_load_stats_ex("20260901",0,1000,2000,false,&stats,NULL)==ESP_OK);
    assert(touch_history_load_night_ex("20260901",&night,captions,2,NULL)==ESP_OK);
    assert(graph_reads==2 && stats_reads==2 && night_reads==2);
    /* A cancelled read cannot seed a future hit. */
    cancel_after_read=true;
    assert(touch_history_load_view_ex("20260901",0,3000,4000,false,view,NULL)==TOUCH_HISTORY_ERR_CANCELLED);
    cancelled=cancel_after_read=false;
    assert(touch_history_load_view_ex("20260901",0,3000,4000,false,view,NULL)==ESP_OK);
    assert(graph_reads==4 && !leases);
    /* Oversized nights bypass retention; they are never silently truncated. */
    source_events=1025;
    for(int i=0;i<2;++i){
        history_event_vector_t events={0};touch_history_event_totals_t totals={0};
        assert(history_collect_events_leased("20260901",NULL,0,&events,&totals,NULL,0,1000)==ESP_OK);
        assert(events.count==1025 && totals.total_count==1025);free(events.items);
    }
    assert(event_reads==3);
    touch_history_event_t marker;touch_history_event_page_t page;
    assert(touch_history_load_window_events_ex("20260901",1050,1250,&marker,1,&page,NULL)==ESP_OK);
    assert(event_reads==4 && page.returned==1 && page.has_more && page.total_count==1025);
    assert(marker.start_ms==1000 && !page.totals.complete && !leases);
    /* Optional cache-copy allocation failure falls through to the source. */
    fail_alloc=1;
    assert(touch_history_load_night_ex("20260901",&night,captions,2,NULL)==ESP_OK);
    assert(night_reads==3 && !leases);
    free(view);history_cache_clear();
    puts("History service adapters: repeated windows/night/events reuse, raw-only stats, invalidation, cancellation, oversized bypass and low-memory fallback passed");
}
'''
with tempfile.TemporaryDirectory(prefix='somno-service-cache-') as temp:
    path=Path(temp)
    (path/'test.c').write_text(pre+session+vector+keys+mocks+'\n'.join(function(n) for n in [
        'history_load_sessions_leased','history_collect_events_leased','touch_history_load_view_ex',
        'touch_history_load_stats_ex','touch_history_load_night_ex','touch_history_load_window_events_ex'])+main)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-D_DARWIN_C_SOURCE','-DHISTORY_CACHE_HOST_TEST',
        '-I'+str(root/'main'),'-I'+str(root/'scripts/test_include'),str(path/'test.c'),
        str(root/'main/history_cache.c'),'-lpthread','-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

# Connect the epoch behavior above to every independent source-writer path.
storage=(root/'main/sd_storage.c').read_text()
for entry in ('sd_storage_init','sd_storage_deinit','sd_storage_recording_end'):
    assert re.search(r'\b'+entry+r'\(void\)\s*\{\s*sd_storage_content_changed\(\)',storage)
assert 'if (role != SD_LEASE_UPLOAD) sd_storage_content_changed();' in storage
assert 'void ftp_storage_changed(void) { sd_storage_content_changed(); }' in storage
post=(root/'main/post_therapy.c').read_text()
# Centralized post-therapy transactions replace scattered epoch notifications.
# The production write/rewrite + release behavior runs in history_generation_test.
gate=function('post_storage_begin',post)
transaction=function('write_bin_atomic',post)
assert 'sd_storage_lease_acquire(SD_LEASE_EXPORT, 250)' in gate
assert transaction.index('post_storage_begin()') < transaction.index('fopen(')
assert transaction.rindex('rename(') < transaction.rindex('sd_storage_lease_release(SD_LEASE_EXPORT)')
assert transaction.index('if (recovery_error)') < transaction.index('fopen(')
assert 'return ESP_FAIL;' in transaction[transaction.index('if (recovery_error)'):transaction.index('fopen(')]
# storage_export_fault_test runs the early stat failure and verifies zero open
# descriptors, balanced lease, original error and unchanged prior output.
assert 'sd_storage_lease_release_unchanged' not in transaction
assert 'write_bin_atomic(' in function('write_json_file',post)
for writer_name in ('collect_summary_spool','collect_resp_events','refresh_today_summary_spool'):
    assert 'write_bin_atomic(' in function(writer_name,post)
for writer_name in ('collect_identification','collect_settings','post_therapy_collect'):
    assert 'write_json_file(' in function(writer_name,post)
ftp=(root/'third_party/esp-idf-ftpServer/ftp.c').read_text()
assert 'ftp_file_writing' in ftp and ftp.count('ftp_notify_write();') >= 9
writer=(root/'main/session_writer.c').read_text().split('static void sw_post_task(',1)[1]
assert writer.index('edf_gen_generate(') < writer.index('history_flow_cache_build(')
assert 'sd_storage_lease_acquire(SD_LEASE_UPLOAD, 0)' in writer
