#!/usr/bin/env python3
"""Fault injection against production export finalization and post file gates."""
from pathlib import Path
from session_storage_behavior_test import COMMON, DEFS, SNT_DEFS, SW, EDF, SNT, function, run

POST = (Path(__file__).resolve().parents[1]/'main/post_therapy.c').read_text()
EDF_HEADER = (Path(__file__).resolve().parents[1]/'main/edf_header.c').read_text()
code = COMMON + SNT_DEFS + r'''
#include <dirent.h>
static int fault, closed, sync_calls, lease_depth;
static bool pending, ready=true;
static int real_close(FILE *f) { return fclose(f); }
static int test_close(FILE *f) { closed++;int r=fclose(f);if(fault==3 || fault==6){errno=EIO;return -1;}return r; }
static int test_flush(FILE *f) { if(fault==1 || fault==6){errno=ENOSPC;return -1;}return fflush(f); }
static int test_sync(int fd) { (void)fd;sync_calls++;if(fault==2){errno=EROFS;return -1;}return 0; }
static int test_rename(const char *a,const char *b) {
    if(fault==4 && strstr(a,".tmp")){errno=EBUSY;return -1;}return rename(a,b);
}
static int test_stat(const char *p, struct stat *s) {
    if(fault==5){errno=EIO;return -1;}return stat(p,s);
}
#define fclose test_close
#define fflush test_flush
#define fsync test_sync
#define rename test_rename
#define stat test_stat
#define SD_LEASE_EXPORT 1
static bool sd_storage_lease_acquire(int x,int y) { (void)x;(void)y;lease_depth++;return true; }
static void sd_storage_lease_release(int x) { (void)x;assert(lease_depth>0);lease_depth--; }
static bool sd_storage_is_ready(void) { return ready; }
static bool sd_storage_recording_pending(void) { return pending; }
static bool sd_storage_recording_active(void) { return false; }
'''
# A function-like macro preserves struct stat while redirecting calls.
code = code.replace('#define stat test_stat', '#define stat(...) test_stat(__VA_ARGS__)')
code += ''.join(function(EDF_HEADER,n) for n in [
    'edf_open_atomic_file','edf_publish_atomic_path',
    'edf_finalize_atomic_file','edf_discard_atomic_file'])
code += ''.join(function(POST,n) for n in ['post_storage_begin','write_bin_atomic'])
code += r'''
static bool rpc_saw_no_lease;
static esp_err_t as11_ble_spool_pull(const char *key,const char *from,uint8_t **data,size_t *len) {
    (void)key;(void)from;assert(lease_depth==0);rpc_saw_no_lease=true;
    *data=malloc(4);memcpy(*data,"RESP",4);*len=4;return ESP_OK;
}
'''
code += function(POST,'collect_resp_events')
code += function(SNT,'snt_read_header') + function(EDF,'validate_positioned_sources')
code += r'''
static void good(const char *path) {
    fault=0;FILE *f=fopen(path,"wb");assert(f);fputs("GOOD",f);assert(real_close(f)==0);
}
static void expect(const char *path,const char *text) {
    int save=fault;fault=0;FILE *f=fopen(path,"rb");assert(f);
    char b[40]={0};assert(fread(b,1,sizeof(b)-1,f)==strlen(text));assert(!strcmp(b,text));
    real_close(f);fault=save;
}
int main(void) {
    char dir[]="/tmp/somno-export-XXXXXX";assert(mkdtemp(dir));
    char path[300],tmp[320];snprintf(path,sizeof path,"%s/result.edf",dir);
    for(int e=1;e<=6;e++) {
        good(path);FILE *f=edf_open_atomic_file(path,tmp,sizeof tmp);assert(f);fputs("NEW",f);
        closed=sync_calls=0;fault=e;assert(edf_finalize_atomic_file(f,tmp,path)==ESP_FAIL);
        assert(closed==1);assert(errno==(e==1||e==6 ? ENOSPC : e==2 ? EROFS : e==4 ? EBUSY : EIO));
        expect(path,"GOOD");fault=0;assert(access(tmp,F_OK)!=0);
    }
    good(path);FILE *f=edf_open_atomic_file(path,tmp,sizeof tmp);fputs("NEW",f);closed=0;
    assert(edf_finalize_atomic_file(f,tmp,path)==ESP_OK && closed==1);expect(path,"NEW");
    // A saved prior file survives restart between FAT renames and is recovered.
    char bak[330];snprintf(bak,sizeof bak,"%s.bak",path);assert(rename(path,bak)==0);
    f=edf_open_atomic_file(path,tmp,sizeof tmp);fputs("LATEST",f);fault=4;
    assert(edf_finalize_atomic_file(f,tmp,path)==ESP_FAIL);expect(path,"NEW");fault=0;
    // The post transaction closes/releases on every write/flush/sync/rename failure.
    for(int e=1;e<=6;e++) {
        good(path);fault=e;closed=0;
        assert(write_bin_atomic(path,(const uint8_t*)"POST",4)==ESP_FAIL);
        assert(lease_depth==0);assert(closed==(e==5 ? 0 : 1));expect(path,"GOOD");
    }
    fault=0;pending=true;closed=0;
    assert(write_bin_atomic(path,(const uint8_t*)"POST",4)==ESP_ERR_TIMEOUT);
    assert(closed==0 && lease_depth==0);expect(path,"GOOD");pending=false;
    assert(write_bin_atomic(path,(const uint8_t*)"POST",4)==ESP_OK);expect(path,"POST");
    assert(collect_resp_events(dir,"session","2026-09-06")==ESP_OK);
    assert(rpc_saw_no_lease && lease_depth==0);
    char resp[340];snprintf(resp,sizeof resp,"%s/session_resp_events.bin",dir);expect(resp,"RESP");unlink(resp);
    // Legacy v1 and unflagged v2 accept; newly positioned gaps fail before output.
    char raw[320];snprintf(raw,sizeof raw,"%s/s_flow.snt",dir);
    for(int version=1;version<=2;version++) {
        f=fopen(raw,"wb");snt_header_t h={.magic=SNT_MAGIC,.version=version};
        fwrite(&h,sizeof h,1,f);real_close(f);
        assert(validate_positioned_sources(dir,"s")==ESP_OK);
    }
    f=fopen(raw,"r+b");snt_header_t h={.magic=SNT_MAGIC,.version=2,.reserved=1};
    fwrite(&h,sizeof h,1,f);real_close(f);
    assert(validate_positioned_sources(dir,"s")==EDF_GEN_ERR_POSITION_GAPS);
    expect(path,"POST");
    f=fopen(raw,"wb");fwrite(&h,1,10,f);real_close(f);
    assert(validate_positioned_sources(dir,"s")==ESP_FAIL);expect(path,"POST");
    unlink(raw);unlink(path);rmdir(dir);
    puts("production export faults: always-close, first-error, prior-output recovery, gates, gap refusal passed");
}
'''
if __name__ == '__main__':
    run('export_faults',code)
