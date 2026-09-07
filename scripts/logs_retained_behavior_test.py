#!/usr/bin/env python3
"""Run the production retained filter and SD publication code on temp fixtures.

Only filesystem/lease/capture boundaries are faked. No device or patient files
are opened; injected write/rename/wrap failures must preserve the last export.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "main/log_stream.c").read_text()


def function(name):
    match = re.search(r"(?:static )?(?:bool|esp_err_t|const char)\s+\*?" + name + r"\([^;]+?\)\s*\{", source, re.S)
    assert match, name
    start, end, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "log_stream.h"
#define ESP_ERR_TIMEOUT 0x107
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define SD_APP_DIR "card"
#define LOG_DIR "card/logs"
#define RETAINED_SAVE_FILE "touchscreen-visible.log"
#define RETAINED_SAVE_TMP_FILE RETAINED_SAVE_FILE ".tmp"
#define RETAINED_SAVE_BACKUP_FILE RETAINED_SAVE_FILE ".bak"
#define SD_LEASE_EXPORT 3
typedef struct {size_t head,count,capacity; uint64_t generation,total_count;} retained_bounds_t;
static bool ready=true, admitted=true, leased, wrap, fail_write, fail_publish;
static unsigned acquisitions,releases,progress_calls;
static size_t done,total;
static log_stream_retained_line_t slots[3];
static esp_err_t last_error;
static int s_retained_lock;
static void *s_retained_slots = slots;
static size_t s_retained_capacity=3,s_retained_head=2,s_retained_count=3;
static unsigned s_retained_generation=12;
static esp_err_t s_retained_last_error=ESP_OK;
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static bool sd_storage_is_ready(void) {return ready;}
static bool sd_storage_lease_acquire(int kind, int timeout) {
    assert(kind==SD_LEASE_EXPORT && timeout==5000 && !leased);
    ++acquisitions; return leased=admitted;
}
static void sd_storage_lease_release_unchanged(int kind) {
    assert(kind==SD_LEASE_EXPORT && leased); leased=false; ++releases;
}
static void retained_set_last_error(esp_err_t e) {last_error=e;}
esp_err_t log_stream_retained_get_info(log_stream_retained_info_t *i) {
    *i=(log_stream_retained_info_t){.available=true,.retained_count=3};return ESP_OK;
}
static esp_err_t retained_read_bounds(retained_bounds_t *b, log_stream_retained_info_t *i) {
    (void)i; assert(leased); *b=(retained_bounds_t){.head=0,.count=3,.capacity=3,.total_count=3};return ESP_OK;
}
static bool retained_copy_slot(size_t i, log_stream_retained_line_t *line) {
    assert(leased);*line=slots[i];if(wrap && i==1) line->sequence=99;return true;
}
static size_t checked_write(const void *p,size_t s,size_t n,FILE *f) {
    assert(leased); if(fail_write) return 0;return fwrite(p,s,n,f);
}
static int checked_rename(const char *a,const char *b) {
    assert(leased); if(fail_publish && strstr(a,".tmp")) {errno=EIO;return -1;}return rename(a,b);
}
#define fwrite checked_write
#define rename checked_rename
'''

tests = r'''
static void progress(size_t p,size_t t,void *ctx) {
    (void)ctx;assert(leased && p<=t);done=p;total=t;++progress_calls;
}
static void old_file(void) {
    mkdir("card",0777);mkdir("card/logs",0777);
    FILE *f=fopen(LOG_DIR "/" RETAINED_SAVE_FILE,"w");assert(f);fputs("previous export\n",f);fclose(f);
}
static void contents(const char *expected) {
    char b[256]={0};FILE *f=fopen(LOG_DIR "/" RETAINED_SAVE_FILE,"r");assert(f);fread(b,1,sizeof(b)-1,f);fclose(f);assert(strcmp(b,expected)==0);
}
int main(void) {
    const char *texts[]={"I (10) smb: simulated one","E (20) wifi: simulated two","D (30) smb: simulated three"};
    const unsigned levels[]={LOG_STREAM_RETAINED_LEVEL_INFO,LOG_STREAM_RETAINED_LEVEL_ERROR,LOG_STREAM_RETAINED_LEVEL_DEBUG};
    for(size_t n=0;n<3;++n) {slots[n].sequence=n+1;slots[n].level=levels[n];strcpy(slots[n].text,texts[n]);slots[n].length=strlen(texts[n]);}
    log_stream_retained_filter_t filter={.level_mask=LOG_STREAM_RETAINED_LEVEL_ALL,.query="SMB",.before_sequence=3};
    assert(retained_filter_matches(&slots[0],&filter));
    assert(!retained_filter_matches(&slots[1],&filter));
    assert(!retained_filter_matches(&slots[2],&filter));
    filter.query="10";assert(!retained_filter_matches(&slots[0],&filter)); // timestamp excluded
    filter.query="simulated ONE";assert(retained_filter_matches(&slots[0],&filter));
    filter.level_mask=LOG_STREAM_RETAINED_LEVEL_NONE;assert(!retained_filter_matches(&slots[0],&filter));
    filter.level_mask=0;assert(retained_filter_matches(&slots[0],&filter)); // legacy all
    filter.query="smb";filter.level_mask=LOG_STREAM_RETAINED_LEVEL_ALL;
    char path[96];size_t saved;
    old_file();
    ready=false;
    assert(log_stream_retained_save_to_sd(&filter,path,sizeof(path),&saved,progress,NULL)==ESP_ERR_INVALID_STATE);
    assert(acquisitions==0 && saved==0 && path[0]==0);contents("previous export\n");
    ready=true;admitted=false;
    assert(log_stream_retained_save_to_sd(&filter,path,sizeof(path),&saved,progress,NULL)==ESP_ERR_TIMEOUT);
    assert(releases==0);contents("previous export\n");admitted=true;
    assert(log_stream_retained_save_to_sd(&filter,path,sizeof(path),&saved,progress,NULL)==ESP_OK);
    assert(saved==1 && done==3 && total==3 && progress_calls>=4 && !leased);
    assert(strcmp(path,LOG_DIR "/" RETAINED_SAVE_FILE)==0);
    contents("I (10) smb: simulated one\n");
    for(unsigned failure=0;failure<3;++failure) {
        old_file();wrap=failure==0;fail_write=failure==1;fail_publish=failure==2;
        unsigned before=releases;
        assert(log_stream_retained_save_to_sd(&filter,path,sizeof(path),&saved,progress,NULL)!=ESP_OK);
        assert(saved==0 && path[0]==0 && !leased && releases==before+1 && last_error!=ESP_OK);
        contents("previous export\n");
        struct stat st;assert(stat(LOG_DIR "/" RETAINED_SAVE_TMP_FILE,&st)!=0);
    }
    assert(log_stream_retained_clear()==ESP_OK);
    assert(s_retained_head==0 && s_retained_count==0 && s_retained_generation==13);
    assert(s_retained_slots==slots && slots[0].sequence==1);
    contents("previous export\n");
    s_retained_slots=NULL;
    assert(log_stream_retained_clear()==ESP_ERR_INVALID_STATE);
    assert(s_retained_generation==13);
    puts("Retained filter, pause ceiling, RAM-only clear, SD lease and failed publication behavior passed");
}
'''

with tempfile.TemporaryDirectory(prefix="logs-behavior-") as temp:
    root = Path(temp)
    (root / "esp_http_server.h").write_text("typedef void *httpd_handle_t;\n")
    code = prefix + "\n".join(function(name) for name in (
        "retained_contains_case_insensitive", "retained_search_start",
        "retained_filter_matches", "retained_line_is_in_bounds",
        "retained_publish_snapshot", "log_stream_retained_save_to_sd", "log_stream_retained_clear")) + tests
    (root / "test.c").write_text(code)
    subprocess.run(["cc", "-std=c11", "-D_DARWIN_C_SOURCE", "-Wall", "-Wextra",
                    "-I", str(root), "-I", str(ROOT / "scripts/test_include"),
                    "-I", str(ROOT / "main"), str(root / "test.c"), "-o", str(root / "test")], check=True)
    subprocess.run([root / "test"], cwd=root, check=True)
