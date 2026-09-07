#!/usr/bin/env python3
"""Production log flush and lease release must preserve unrelated History keys."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
def function(file, name):
    source = (root / file).read_text()
    match = re.search(r'^(?:static )?[\w\s*]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'

pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "sd_storage.h"
#include "history_cache.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERR_TIMEOUT 0x107
#define pdTRUE 1
#define portMAX_DELAY 0
#define LOG_FILE_MAX_SIZE (128*1024)
#define LOG_LINE_MAX 256
#define WRITEBUF_SIZE 8192
#define LOG_DIR log_dir
#define LOG_FILE_PREFIX "log."
static char log_dir[256];
static int s_export_sem=1,s_lease_mutex=1,s_destructive,s_uploading;
static unsigned releases,acquires,io_calls;
static uint32_t s_content_generation=7, expected_epoch, minimum_epoch;
static uint8_t buffer[WRITEBUF_SIZE], *s_writebuf=buffer;
static int s_writebuf_mutex=1;
static size_t s_writebuf_head,s_writebuf_tail;
static bool s_sd_ready=true, open_failure;
bool sd_storage_is_ready(void){return true;}
bool sd_storage_recording_pending(void){return false;}
bool sd_storage_recording_active(void){return false;}
bool sd_storage_lease_acquire(sd_lease_t role,uint32_t timeout)
{assert(role==SD_LEASE_EXPORT&&timeout==250);++acquires;return true;}
static int xSemaphoreTake(int s,int d){(void)s;(void)d;return pdTRUE;}
static void xSemaphoreGive(int s){(void)s;}
static void xSemaphoreGiveRecursive(int s)
{(void)s;++releases; assert(expected_epoch ? sd_storage_content_generation()==expected_epoch : sd_storage_content_generation()>=minimum_epoch);}
static void ensure_log_dir(void){++io_calls;assert(false);}
static long log_file_size(void){++io_calls;return 0;}
static void rotate_logs(void){++io_calls;assert(false);}
static FILE *log_open(const char *path,const char *mode)
{++io_calls;return open_failure?NULL:fopen(path,mode);}
'''
main = r'''
#undef fopen
static history_cache_key_t key(void) {
    history_cache_key_t k={.generation=sd_storage_content_generation(),.kind=7};
    memcpy(k.day,"20260901",9);return k;
}
static void assert_hit(bool expected) {
    int value=0;history_cache_key_t k=key();
    assert((history_cache_get(&k,&value,sizeof(value))!=0)==expected);
    if(expected)assert(value==123);
}
int main(int argc,char **argv) {
    assert(argc==2);snprintf(log_dir,sizeof(log_dir),"%s",argv[1]);
    history_cache_key_t initial=key();int value=123;
    history_cache_put(&initial,&value,sizeof(value));expected_epoch=7;
    for(int i=0;i<3;++i){log_flush_once();assert_hit(true);}
    assert(sd_storage_content_generation()==7 && acquires==3 && releases==3 && !io_calls);
    const char text[]="a persisted diagnostic line\n";
    memcpy(buffer,text,sizeof(text));s_writebuf_head=sizeof(text);
    log_flush_once();assert_hit(true);
    assert(s_writebuf_tail==sizeof(text) && acquires==4 && releases==4 && io_calls==2);
    char path[320];snprintf(path,sizeof(path),"%s/log.0",log_dir);
    FILE *file=fopen(path,"rb");assert(file);char readback[sizeof(text)];
    assert(fread(readback,1,sizeof(readback),file)==sizeof(readback));fclose(file);
    assert(!memcmp(readback,text,sizeof(text)));
    /* Even failed log-only writes have no History source mutation. */
    ++s_writebuf_head;open_failure=true;log_flush_once();assert_hit(true);
    assert(s_writebuf_tail==sizeof(text));
    s_uploading=1;sd_storage_lease_release(SD_LEASE_UPLOAD);
    assert(!s_uploading);assert_hit(true);
    s_destructive=1;sd_storage_lease_release_unchanged(SD_LEASE_DESTRUCTIVE);
    assert(!s_destructive);assert_hit(true);
    /* Default source writer release still changes generation before unlock. */
    expected_epoch=8;sd_storage_lease_release(SD_LEASE_EXPORT);assert_hit(false);
    s_destructive=1;expected_epoch=9;sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
    assert(!s_destructive && sd_storage_content_generation()==9);
    sd_storage_content_changed();assert(sd_storage_content_generation()==10);
    unlink(path);
    /* The storage worker centralizes real post-therapy source mutation here.
       Exercise initial publication and same-length replacement through that
       production transaction; a default release must invalidate before unlock. */
    snprintf(path,sizeof(path),"%s/source.bin",log_dir);
    expected_epoch=0;minimum_epoch=sd_storage_content_generation()+1;
    assert(write_bin_atomic(path,(const uint8_t *)"first",5)==ESP_OK);
    history_cache_key_t before_rewrite=key();history_cache_put(&before_rewrite,&value,sizeof(value));
    assert_hit(true);minimum_epoch=sd_storage_content_generation()+1;
    assert(write_bin_atomic(path,(const uint8_t *)"other",5)==ESP_OK);assert_hit(false);
    file=fopen(path,"rb");assert(file);char raw[5];
    assert(fread(raw,1,sizeof(raw),file)==sizeof(raw) && !memcmp(raw,"other",5));fclose(file);
    unlink(path);history_cache_clear();
    puts("History generation: empty/log-only/failed-log flushes retain cache keys; writer/destructive releases invalidate before unlock; lease counters balance");
}
'''
with tempfile.TemporaryDirectory(prefix='shg-', dir='/tmp') as temp:
    path = Path(temp)
    fixture = pre + ''.join(function('main/sd_storage.c', n) for n in [
        'sd_storage_content_generation','sd_storage_content_changed',
        'sd_storage_lease_release_unchanged','sd_storage_lease_release'])
    fixture += function('main/log_stream.c','writebuf_acknowledge')
    fixture += function('main/post_therapy.c','post_storage_begin')
    fixture += function('main/post_therapy.c','write_bin_atomic')
    fixture += '\n#define fopen log_open\n' + function('main/log_stream.c','log_flush_once') + main
    (path/'test.c').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DHISTORY_CACHE_HOST_TEST',
        '-I'+str(root/'main'),'-I'+str(root/'scripts/test_include'),str(path/'test.c'),
        str(root/'main/history_cache.c'),'-lpthread','-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test'),str(path)],check=True)
log = (root/'main/log_stream.c').read_text()
assert log.count('sd_storage_lease_release_unchanged(SD_LEASE_EXPORT)') == 3
assert not re.search(r'\bsd_storage_lease_release\(', log)
