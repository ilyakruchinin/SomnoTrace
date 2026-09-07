#!/usr/bin/env python3
"""Exercise production day transaction with failing session/shared converters."""
from pathlib import Path
from session_storage_behavior_test import COMMON, EDF, function, run

EDF_HEADER = (Path(__file__).resolve().parents[1]/'main/edf_header.c').read_text()

code = COMMON + r'''
#include <dirent.h>
#define SD_LEASE_EXPORT 1
#define REBUILD_MAX_SESSIONS 64
#define EDF_GEN_PER_SESSION 1
#define EDF_GEN_SHARED 2
static char SD_STREAMS_DIR[300],SD_SDCARD_DIR[300],SD_SDCARD_DATALOG[300];
static char REBUILD_STAGING_DIR[300],REBUILD_SENTINEL[300];
static int lease_depth, failure_mode, generated;
static esp_err_t session_writer_mark_upload_invalidation(const char *day) {
    assert(!strcmp(day,"20260906"));assert(lease_depth>0);
    if(failure_mode==5)return ESP_FAIL;
    return ESP_OK;
}
static bool sd_storage_is_ready(void) { return true; }
static bool sd_storage_lease_acquire(int a,int b) {(void)a;(void)b;lease_depth++;return true;}
static void sd_storage_lease_release(int a) {(void)a;lease_depth--;assert(lease_depth>=0);}
static const char *esp_err_to_name(int e) {(void)e;return "failure";}
typedef struct {char session_id[40];int64_t start_epoch_ms,end_epoch_ms,clock_drift_ms;bool interrupted;} rebuild_session_t;
static esp_err_t rebuild_read_manifest(const char *p,const char *f,rebuild_session_t *out) {
    (void)p;snprintf(out->session_id,sizeof out->session_id,"%.*s",(int)strlen(f)-13,f);
    out->start_epoch_ms=1700000000000LL+(f[0]=='a'?0:100000);out->interrupted=true;return ESP_OK;
}
static void file(const char *p,const char *v) {FILE *f=fopen(p,"wb");assert(f);fputs(v,f);fclose(f);}
static void expect(const char *p,const char *v) {
    char b[40]={0};FILE *f=fopen(p,"rb");assert(f);fread(b,1,sizeof b-1,f);fclose(f);assert(!strcmp(b,v));
}
static esp_err_t edf_gen_generate_ex(const char *root,const char *dir,const char *id,
    int64_t a,int64_t b,int64_t c,uint32_t flags) {
    (void)dir;(void)a;(void)b;(void)c;generated++;
    char p[700];mkdir(root,0700);snprintf(p,sizeof p,"%s/DATALOG",root);mkdir(p,0700);
    snprintf(p,sizeof p,"%s/DATALOG/20260906",root);mkdir(p,0700);
    snprintf(p,sizeof p,"%s/SETTINGS",root);mkdir(p,0700);
    if(flags==EDF_GEN_PER_SESSION) {
        snprintf(p,sizeof p,"%s/DATALOG/20260906/%s.edf",root,id);file(p,"NEW");
        if(id[0]=='b' && failure_mode==1) return ESP_ERR_NO_MEM;
        if(id[0]=='b' && failure_mode==2) return ESP_FAIL;
        if(id[0]=='b' && failure_mode==3) return EDF_GEN_ERR_POSITION_GAPS;
    } else {
        snprintf(p,sizeof p,"%s/STR.edf",root);file(p,"SHARED");
        if(failure_mode==4) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
'''
code += ''.join(function(EDF_HEADER,n) for n in [
    'edf_open_atomic_file','edf_publish_atomic_path',
    'edf_finalize_atomic_file','edf_discard_atomic_file'])
code += ''.join(function(EDF,n) for n in [
    'rebuild_rmtree','rebuild_move_dir','edf_gen_rebuild_day',
    'edf_gen_take_interrupted_rebuild'])
code += r'''
int main(void) {
    char base[]="/tmp/somno-rebuild-XXXXXX";assert(mkdtemp(base));
    snprintf(SD_STREAMS_DIR,sizeof SD_STREAMS_DIR,"%s/raw",base);mkdir(SD_STREAMS_DIR,0700);
    snprintf(SD_SDCARD_DIR,sizeof SD_SDCARD_DIR,"%s/out",base);mkdir(SD_SDCARD_DIR,0700);
    snprintf(SD_SDCARD_DATALOG,sizeof SD_SDCARD_DATALOG,"%s/DATALOG",SD_SDCARD_DIR);mkdir(SD_SDCARD_DATALOG,0700);
    snprintf(REBUILD_STAGING_DIR,sizeof REBUILD_STAGING_DIR,"%s/.rebuild",SD_SDCARD_DIR);
    snprintf(REBUILD_SENTINEL,sizeof REBUILD_SENTINEL,"%s/.rebuilding",SD_SDCARD_DIR);
    char p[700],good[700],shared[700];
    snprintf(p,sizeof p,"%s/20260906",SD_STREAMS_DIR);mkdir(p,0700);
    snprintf(p,sizeof p,"%s/20260906/a_session.json",SD_STREAMS_DIR);file(p,"{}");
    snprintf(p,sizeof p,"%s/20260906/b_session.json",SD_STREAMS_DIR);file(p,"{}");
    snprintf(p,sizeof p,"%s/20260906",SD_SDCARD_DATALOG);mkdir(p,0700);
    snprintf(good,sizeof good,"%s/20260906/good.edf",SD_SDCARD_DATALOG);file(good,"GOOD");
    snprintf(shared,sizeof shared,"%s/STR.edf",SD_SDCARD_DIR);file(shared,"OLD-SHARED");
    for(failure_mode=1;failure_mode<=4;failure_mode++) {
        generated=0;int ret=edf_gen_rebuild_day("20260906");assert(ret!=ESP_OK);
        assert(lease_depth==0);assert(generated==(failure_mode==4?3:2));
        expect(good,"GOOD");expect(shared,"OLD-SHARED");
        assert(access(REBUILD_STAGING_DIR,F_OK)!=0);assert(access(REBUILD_SENTINEL,F_OK)!=0);
    }
    failure_mode=5;assert(edf_gen_rebuild_day("20260906")==ESP_FAIL);
    assert(access(REBUILD_SENTINEL,F_OK)==0 && lease_depth==0);
    // A reboot check must not consume the only durable retry intent.
    file(REBUILD_SENTINEL,"20260906\n");char day[16];
    assert(edf_gen_take_interrupted_rebuild(day,sizeof day));assert(!strcmp(day,"20260906"));
    assert(edf_gen_take_interrupted_rebuild(day,sizeof day));
    failure_mode=0;assert(edf_gen_rebuild_day("20260906")==ESP_OK);assert(lease_depth==0);
    assert(access(REBUILD_SENTINEL,F_OK)!=0);expect(shared,"SHARED");
    snprintf(p,sizeof p,"%s/20260906/a.edf",SD_SDCARD_DATALOG);expect(p,"NEW");
    snprintf(p,sizeof p,"%s/20260906/b.edf",SD_SDCARD_DATALOG);expect(p,"NEW");
    rebuild_rmtree(base);
    puts("production rebuild transaction: OOM/I/O/gap failures abort whole staged day; shared failure preserves live output; reboot intent retained");
}
'''
if __name__ == '__main__':
    run('rebuild',code)
