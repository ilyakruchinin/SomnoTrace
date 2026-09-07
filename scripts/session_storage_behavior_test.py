#!/usr/bin/env python3
"""Compile the production parser/storage functions and replay positioned raw data.

Platform scheduling and allocation are stubbed. Samples, headers, timestamps,
missing runs, min/max aggregation, and checkpoint decisions are production C.
No physical SD/RGB timing is claimed by this deterministic host test.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SW = (ROOT / 'main/session_writer.c').read_text()
EDF = (ROOT / 'main/edf_gen.c').read_text()
SNT = (ROOT / 'main/snt_format.h').read_text()


def function(source, name):
    m = re.search(r'^(?:static\s+)?[\w *]+\b' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert m, name
    i, depth = m.end(), 1
    while depth:
        depth += (source[i] == '{') - (source[i] == '}')
        i += 1
    return source[m.start():i] + '\n'


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define EDF_GEN_ERR_POSITION_GAPS 0x7e01
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
'''
SNT_DEFS = SNT[SNT.index('#define SNT_MAGIC'):SNT.index('/* ── Inline validation')]
SW_DEFS = SW[SW.index('/* ── Recovery journal'):SW.index('/* ── Module state')]
DEFS = SNT_DEFS + SW_DEFS
PRE = COMMON + r'''
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef struct session_writer session_writer_t;
''' + DEFS + r'''
static session_writer_t state;
static stream_batch_t fill;
static bool allow_start, s_start_intent, s_therapy_stopped;
static bool s_in_mask_fit, s_in_cooldown, s_started_from_event, s_transport_uncertain;
static int64_t now_us = 1000000;
static uint32_t ui_valid, ui_missing, checkpoints;
static int sync_fail;
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(int n) { (void)n; }
static void bsp_display_push_flow(float x) { if (isfinite(x)) ui_valid++; else ui_missing++; }
static void bsp_display_push_flow_gap(uint32_t n) { ui_missing += n; }
static void bsp_display_push_leak(float x) { (void)x; }
static void bsp_display_push_metrics(float a,float b,float c) { (void)a;(void)b;(void)c; }
static bool bsp_display_set_therapy_active(bool x) { return x; }
static void bsp_display_set_therapy_start_time(int64_t x) { (void)x; }
static session_writer_t *active_session_lock(void) { return state.active ? &state : NULL; }
static void active_session_unlock(session_writer_t *s) { (void)s; }
static session_writer_t *session_writer_start(void) {
    s_start_intent = !allow_start;
    if (!allow_start) return NULL;
    state.active = true;
    return &state;
}
static void sw_request_finalize(session_writer_t *s,const char *a,int64_t b,bool c) {
    (void)a;(void)b;(void)c; s->active = false;
}
static bool swap_and_enqueue_locked(session_writer_t *s) { (void)s; return false; }
static void io_fail(session_writer_t *s, const char *w) { (void)w;s->storage_failed=true; }
static bool write_exact(session_writer_t *s,FILE *f,const void *v,size_t n,const char *w) {
    if (fwrite(v,1,n,f) == n) return true;
    io_fail(s,w);return false;
}
static int test_sync(int fd) { (void)fd; if(sync_fail) { errno=EIO;return -1; } return 0; }
#define fsync test_sync
static int64_t lat_begin(void) { return 0; }
static void lat_end(int a,int64_t b) { (void)a;(void)b; }
static void lat_report(void) {}
#define SW_OP_FLUSH 0
#define SW_OP_FSYNC 1
static void update_snt_header_sample_count(session_writer_t *s,FILE *f,uint32_t n) {
    (void)s;long p=ftell(f);fseek(f,offsetof(snt_header_t,sample_count),SEEK_SET);
    fwrite(&n,sizeof(n),1,f);fseek(f,p,SEEK_SET);
}
static void ckpt_write(session_writer_t *s) { assert(!s->storage_failed);checkpoints++;s->ckpt_seq++; }
'''
raw = PRE + ''.join(function(SW, n) for n in [
    'batch_reset', 'storage_mark_position_gap', 'storage_write_brp_frame',
    'storage_fill_brp_until', 'storage_fill_stream_until', 'storage_write_batch',
    'storage_commit'])
raw += SW[SW.index('enum {\n    KEY_PATIENT_FLOW'):SW.index('bool session_writer_try_stream_data_raw')]
raw += function(SW, 'session_writer_try_stream_data_raw')
raw += r'''
static FILE *stream(void) {
    FILE *f=tmpfile();assert(f);
    snt_header_t h={.magic=SNT_MAGIC,.version=2,.sample_bytes=2,.n_channels=1,.sample_hz_x10=250};
    assert(fwrite(&h,sizeof h,1,f)==1);return f;
}
static void begin(void) {
    memset(&state,0,sizeof state);memset(&fill,0,sizeof fill);
    state.fill=&fill;state.active=true;state.files_open=true;
    state.flow.f_l0=stream();state.press.f_l0=stream();state.flow.f_l1=stream();
    state.sa2.f_l0=stream();state.pld_f.f_l0=stream();state.f_events=tmpfile();
    ui_valid=ui_missing=0; s_start_intent=false;s_transport_uncertain=false;
}
static void end(void) {
    fclose(state.flow.f_l0);fclose(state.press.f_l0);fclose(state.flow.f_l1);
    fclose(state.sa2.f_l0);fclose(state.pld_f.f_l0);fclose(state.f_events);
}
static bool packet(int t,int base) {
    char j[512];int h=t/3600000,m=(t/60000)%60,sec=(t/1000)%60,ms=t%1000;
    snprintf(j,sizeof j,"{\"startTime\":\"2026-09-06T%02d:%02d:%02d.%03dZ\","
             "\"PatientFlow\":[%d,%d,%d,%d,%d],\"MaskPressure\":[4,4,4,4,4],"
             "\"HeartRate\":[65],\"SpO2\":[95],\"Leak\":[1]}",
             h,m,sec,ms,base,base+1,base+2,base+3,base+4);
    now_us+=20000;return session_writer_try_stream_data_raw(j,(int)strlen(j));
}
static int16_t value(FILE *f,uint32_t index) {
    long p=ftell(f);fflush(f);assert(fseek(f,sizeof(snt_header_t)+index*2,SEEK_SET)==0);
    int16_t v;assert(fread(&v,2,1,f)==1);fseek(f,p,SEEK_SET);return v;
}
static uint16_t flags(FILE *f) {
    long p=ftell(f);fflush(f);rewind(f);snt_header_t h;assert(fread(&h,sizeof h,1,f)==1);
    fseek(f,p,SEEK_SET);return h.reserved;
}
static void drain(void) { storage_write_batch(&state,&fill);batch_reset(&fill); }
int main(void) {
    begin();
    assert(packet(0,1));assert(packet(400,6));assert(packet(10600,11));
    drain();assert(state.flow.sample_count==270 && state.press.sample_count==270);
    assert(state.committed_elapsed_us==10800000);
    assert(value(state.flow.f_l0,0)==100 && value(state.flow.f_l0,4)==500);
    for(int i=5;i<10;i++) assert(value(state.flow.f_l0,i)==SNT_MISSING);
    assert(value(state.flow.f_l0,10)==600 && value(state.flow.f_l0,14)==1000);
    for(int i=15;i<265;i++) assert(value(state.flow.f_l0,i)==SNT_MISSING);
    assert(value(state.flow.f_l0,265)==1100 && value(state.flow.f_l0,269)==1500);
    assert(state.sa2.sample_count==11 && state.pld_f.sample_count==6);
    assert(value(state.sa2.f_l0,2)==SNT_MISSING && value(state.sa2.f_l0,20)==6500);
    assert(flags(state.flow.f_l0)&1);assert(flags(state.sa2.f_l0)&1);
    assert(ui_valid==15 && ui_missing==255);
    packet(10600,11);packet(10400,11);
    assert(ui_valid==15 && ui_missing==255 && fill.n_brp==0);
    end();
    begin();packet(0,1);packet(119000,6);drain();
    assert(state.flow.sample_count==2980 && value(state.flow.f_l0,2975)==600);
    assert(state.committed_elapsed_us==119200000);
    assert(value(state.flow.f_l0,2974)==SNT_MISSING && state.sa2.sample_count==120);
    end();
    // Batch boundaries do not drop the partial 1-second min/max accumulator.
    begin();packet(0,1);packet(200,6);packet(400,11);drain();
    packet(600,16);packet(800,21);drain();
    assert(state.flow_mm_count==1);assert(value(state.flow.f_l1,0)==100);
    assert(value(state.flow.f_l1,1)==2500);assert(flags(state.flow.f_l0)==0);end();
    // Fork v2 sentinel stays unambiguous in the positioned writer. Optional
    // channel absence still triggers the current conservative export refusal;
    // deciding which absent channels may export is a separate acceptance gate.
    begin();
    const char *absent="{\"startTime\":\"2026-09-06T00:00:00.000Z\","
        "\"PatientFlow\":[-0.01,0,0,0,0],\"MaskPressure\":[4,4,4,4,4],\"Leak\":[1]}";
    assert(session_writer_try_stream_data_raw(absent,(int)strlen(absent)));drain();
    assert(value(state.flow.f_l0,0)==-1 && flags(state.flow.f_l0)==0);
    assert(value(state.sa2.f_l0,0)==SNT_MISSING && value(state.sa2.f_l0,1)==SNT_MISSING);
    assert(flags(state.sa2.f_l0)&1);
    bool pld_missing=false,pld_present=false;
    for(int ch=0;ch<12;ch++) {
        int16_t v=value(state.pld_f.f_l0,(uint32_t)ch);
        pld_missing |= v==SNT_MISSING;pld_present |= v!=SNT_MISSING;
    }
    assert(pld_missing && pld_present && (flags(state.pld_f.f_l0)&1));end();
    // Source time controls position even when every replay arrives 20ms apart.
    begin();packet(86399800,1);packet(0,6);drain();
    assert(state.flow.sample_count==10 && !state.flow.position_gap);end();
    // Capacity pressure loses values, never their positions, including final tail.
    begin();for(int i=0;i<302;i++) packet(i*200,1);
    assert(fill.n_brp==BRP_CAP && state.brp_dropped==10);drain();
    assert(state.flow.sample_count==1510);
    assert(value(state.flow.f_l0,1499)==500);
    for(int i=1500;i<1510;i++) assert(value(state.flow.f_l0,i)==SNT_MISSING);
    assert(flags(state.flow.f_l0)&1);end();
    // Admission failure does not consume samples or publish duplicate UI points.
    begin();state.active=false;allow_start=false;
    assert(!packet(0,1));assert(fill.n_brp==0 && ui_valid==0);
    allow_start=true;assert(packet(0,1));assert(fill.n_brp==5 && ui_valid==5);end();
    // A failed sync cannot advertise new durability; event-only commit is real.
    begin();state.have_uncommitted=true;sync_fail=1;checkpoints=0;
    storage_commit(&state);assert(checkpoints==0 && state.have_uncommitted);
    sync_fail=0;state.storage_failed=false;storage_commit(&state);
    assert(checkpoints==1 && !state.have_uncommitted);end();
    puts("production storage replay: gaps, positions, capacity, admission, checkpoints passed");
}
'''


def run(name, text):
    with tempfile.TemporaryDirectory(prefix='somno-storage-') as td:
        src = Path(td)/f'{name}.c'; exe = Path(td)/name
        src.write_text(text)
        subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(src), '-o', str(exe), '-lm'], check=True)
        subprocess.run([str(exe)], cwd=td, check=True)


if __name__ == '__main__':
    run('replay', raw)
