#!/usr/bin/env python3
"""Run production availability scans with cancellation at real read boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/touch_history.c').read_text()

def function(name):
    match = re.search(r'^(?:static )?[\w\s*]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'

session = source[source.index('typedef struct {\n    char id[TOUCH_HISTORY_SESSION_ID_LEN];'):source.index('static esp_err_t history_collect_eligible_intervals_leased(')]
candidate_start = source.index('typedef struct {\n    char path[OXIMETRY_CANONICAL_MAX_PATH];')
candidate = source[candidate_start:source.index('} trace_candidate_t;', candidate_start) + len('} trace_candidate_t;')]
pre = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "touch_history.h"
#define HISTORY_READ_VALUES 512U
#define OXIMETRY_CANONICAL_MAX_PATH 416
#define ESP_ERR_NO_MEM 0x101
static bool pending, superseded, fail_alloc, fail_close, fail_read;
static int reads[2], positions[2], opened, closed, allocated, released, inspections;
static int cancel_source, cancel_read;
static unsigned mode;
static const int counts[2] = {1024, 256};
static bool sd_storage_recording_pending(void) { return pending; }
static bool should_cancel(void *p) { (void)p; return superseded; }
static void request_cancel(void) { if (mode) pending = true; else superseded = true; }
static void *history_alloc(size_t size, bool clear)
{ (void)clear; if (fail_alloc) return NULL; ++allocated; return malloc(size); }
static void counted_free(void *p) { if (p) ++released; free(p); }
static FILE *probe_open(const char *path, const char *mode_string)
{ assert(!strcmp(mode_string,"rb")); ++opened; return (FILE *)(uintptr_t)(path[0]=='F'?1:2); }
static int probe_seek(FILE *file, long offset, int whence)
{ (void)file; assert(offset==28 && whence==SEEK_SET); return 0; }
static size_t probe_read(void *out, size_t size, size_t count, FILE *file)
{
    int i=(int)(uintptr_t)file-1;
    assert(size==(i?12U:2U)*sizeof(int16_t));
    assert(size*count<=HISTORY_READ_VALUES*sizeof(int16_t));
    ++reads[i];
    int16_t *values=out;
    for(size_t j=0;j<size*count/sizeof(*values);++j)values[j]=INT16_MIN;
    positions[i]+=(int)count;
    /* Availability appears only at the end; cancellation must not turn into
       an early-success shortcut that skips source validation. */
    if(positions[i]==counts[i]) {
        size_t base=(count-1)*(size/sizeof(*values));
        for(size_t j=0;j<size/sizeof(*values);++j)values[base+j]=23;
    }
    if(i==cancel_source && reads[i]==cancel_read)request_cancel();
    return fail_read ? 0 : count;
}
static int probe_error(FILE *file) { (void)file; return fail_read; }
static int probe_close(FILE *file) { (void)file; ++closed; return fail_close?-1:0; }
'''
stub = r'''
static esp_err_t history_session_candidate(const char *day,
    const history_session_info_t *session, touch_history_signal_t signal,
    trace_candidate_t *out)
{
    (void)day; (void)session; ++inspections;
    int i=signal==TOUCH_HISTORY_SIGNAL_FLOW?0:1;
    out->path[0]=i?'P':'F'; out->n_channels=i?12:2;
    out->header_bytes=28; out->version=2; out->records=counts[i];
    return ESP_OK;
}
'''
main = r'''
#undef fopen
#undef fseek
#undef fread
#undef ferror
#undef fclose
#undef free
static void reset(void) {
    pending=superseded=fail_alloc=fail_close=fail_read=false;
    memset(reads,0,sizeof(reads));memset(positions,0,sizeof(positions));
    opened=closed=allocated=released=inspections=0; cancel_source=-1;cancel_read=0;
}
static void balanced(void) { assert(opened==closed && allocated==released); }
int main(void) {
    touch_history_operation_t operation={.should_cancel=should_cancel};
    history_session_info_t session={0};
    reset(); pending=true;
    assert(history_probe_session("20260901",&session,&operation)==TOUCH_HISTORY_ERR_CANCELLED);
    assert(!allocated && !inspections && !opened); balanced();
    reset(); fail_alloc=true;
    assert(history_probe_session("20260901",&session,&operation)==ESP_ERR_NO_MEM); balanced();
    /* UI supersession and recording admission both stop within one slab,
       including final Flow/PLD slabs and a concurrent close error. */
    for(mode=0;mode<2;++mode) for(int i=0;i<2;++i) {
        int last=i?7:4;
        for(int at=1;at<=last;++at) {
            reset(); cancel_source=i;cancel_read=at;fail_close=i==0&&at==1;
            memset(&session,0,sizeof(session));
            const touch_history_operation_t *op=mode?NULL:&operation;
            assert(history_probe_session("20260901",&session,op)==TOUCH_HISTORY_ERR_CANCELLED);
            assert(reads[i]==at && (i || (!reads[1] && inspections==1))); balanced();
        }
    }
    reset();memset(&session,0,sizeof(session));
    assert(history_probe_session("20260901",&session,&operation)==ESP_OK);
    assert(reads[0]==4 && reads[1]==7 && session.has_epr_companion);
    assert(session.available_signals==(
        TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_FLOW)|
        TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_PRESSURE)|
        TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_LEAK)|
        TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_FLOW_LIMIT)|
        TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_SNORE)));
    balanced();
    reset();fail_read=true;
    assert(history_probe_session("20260901",&session,&operation)==ESP_FAIL);
    assert(reads[0]==1 && !reads[1]);balanced();
    puts("Availability scans: full Flow/PLD validation, supersession and recording cancellation at every slab, allocation/IO failure, balanced cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='somno-history-cancel-') as temp:
    path = Path(temp)
    hooks = '\n#define fopen probe_open\n#define fseek probe_seek\n#define fread probe_read\n#define ferror probe_error\n#define fclose probe_close\n#define free counted_free\n'
    (path/'test.c').write_text(pre + session + candidate + stub + hooks + function('history_operation_cancelled') + function('history_raw_value_valid') + function('history_probe_session') + main)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),'-I'+str(root/'scripts/test_include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
# Verify the actual night loader carries the callback and treats cancellation
# as fatal before optional work; its existing shared epilogue releases UPLOAD.
loader=function('history_load_night_uncached')
assert 'history_probe_session(day, &sessions[i], operation)' in loader
assert 'probe_result == TOUCH_HISTORY_ERR_CANCELLED' in loader
assert 'sd_storage_lease_release(SD_LEASE_UPLOAD)' in loader
