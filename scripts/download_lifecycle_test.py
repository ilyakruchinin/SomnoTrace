#!/usr/bin/env python3
"""Exercise production async download admission, cancellation, and descriptor lifetime."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'main/net_provision.c').read_text()
def function(name):
    m=re.search(r'^static (?:esp_err_t|void|bool|int) '+name+r'\([^;]*?\)\s*\{',source,re.M)
    assert m,name
    end=m.end();depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[m.start():end]+'\n'
fixture=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/select.h>
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_TIMEOUT 2
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_404_NOT_FOUND 404
#define SD_LEASE_UPLOAD 1
#define MALLOC_CAP_SPIRAM 1
#define MSG_DONTWAIT 64
#define HTTPD_SOCK_ERR_TIMEOUT -7
#define HTTPD_SOCK_ERR_FAIL -8
#define SHUT_RDWR 2
#define pdMS_TO_TICKS(n) (n)
#define tskNO_AFFINITY -1
typedef int esp_err_t;
typedef int httpd_handle_t;
typedef struct {httpd_handle_t handle;} httpd_req_t;
static bool s_download_active,s_download_closing,lease,pending,denied,open_fail,alloc_fail,send_fail,option_fail,task_fail,async_fail;
static unsigned releases,opens,closes,sends,finishes,completed,closed,tasks,deleted;
static int http_error;
static int64_t now=1000000, s_download_deadline_us;
static bool session_async, would_block, partial_send;
static unsigned socket_calls;
static int (*send_override)(httpd_handle_t,int,const char *,size_t,int);
static void vTaskDelay(unsigned n) {assert(n==20);now+=20000;}
static int send(int socket,const char *bytes,size_t size,int flags) {
    (void)bytes;assert(socket==3 && (flags&MSG_DONTWAIT));++socket_calls;
    if(would_block){errno=EAGAIN;return -1;}
    if(partial_send){now+=40000;if(socket_calls==3)pending=true;return 1;}
    return (int)size;
}
static int checked_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t) {
    assert(n==4 && !r && w && !e && t->tv_usec==20000);now+=20000;return 0;
}
#define select checked_select
static const char *path;
static bool get_query_param(httpd_req_t *r,const char *k,char *p,size_t n) {(void)r;(void)k;snprintf(p,n,"%s",path);return true;}
static bool path_is_safe(const char *p) {(void)p;return true;}
static bool sd_storage_lease_acquire(int role,unsigned wait) {assert(role==1 && !wait && !lease);return lease=!denied;}
static void sd_storage_lease_release(int role) {assert(role==1 && lease);lease=false;++releases;}
static bool sd_storage_recording_pending(void) {return pending;}
static bool sd_storage_recording_active(void) {return false;}
static int64_t esp_timer_get_time(void) {return now;}
static void *heap_caps_malloc(size_t n,int cap) {assert(n==2048 && cap==1);return alloc_fail?NULL:malloc(n);}
static FILE *checked_open(const char *p,const char *mode) {assert(lease);++opens;return open_fail?NULL:fopen(p,mode);}
static int checked_close(FILE *file) {assert(lease);++closes;return fclose(file);}
#define fopen checked_open
#define fclose checked_close
static int httpd_resp_send_err(httpd_req_t *r,int e,const char *s) {(void)r;(void)s;http_error=e;return 0;}
static int httpd_resp_send_500(httpd_req_t *r) {(void)r;http_error=500;return 0;}
static void httpd_resp_set_status(httpd_req_t *r,const char *s) {(void)r;assert(!strcmp(s,"503 Service Unavailable"));http_error=503;}
static int httpd_resp_sendstr(httpd_req_t *r,const char *s) {(void)r;(void)s;return 0;}
static void httpd_resp_set_type(httpd_req_t *r,const char *s) {(void)r;(void)s;}
static void httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *s) {(void)r;(void)k;(void)s;}
static int httpd_req_to_sockfd(httpd_req_t *r) {(void)r;return 3;}
static int httpd_sess_set_send_override(int h,int socket,int (*callback)(httpd_handle_t,int,const char *,size_t,int)) {
    assert(h==1 && socket==3);send_override=callback;return option_fail?1:0;
}
static int httpd_resp_send_chunk(httpd_req_t *r,const char *bytes,size_t n) {
    (void)r;assert(lease);
    if(!bytes){assert(!n);++finishes;return 0;}
    assert(n && n<=2048);++sends;
    if(send_fail)return 1;
    size_t sent=0;
    while(sent<n) {int count=send_override(r->handle,3,bytes+sent,n-sent,0);if(count<=0)return 1;sent+=(size_t)count;}
    if(tasks==99) pending=true;
    return 0;
}
static int httpd_req_async_handler_begin(httpd_req_t *r,httpd_req_t **out) {
    if(async_fail)return 1;*out=r;session_async=true;return 0;
}
static int httpd_req_async_handler_complete(httpd_req_t *r) {(void)r;assert(!lease && session_async && closed>completed);session_async=false;++completed;return 0;}
static int shutdown(int socket,int how) {assert(socket==3 && how==SHUT_RDWR && session_async && !lease);++closed;return 0;}
static void psram_task_delete(void *task) {assert(!task && completed && !s_download_active);++deleted;}
static void *psram_task_create(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,int core,void *a,void *b) {
    (void)fn;(void)arg;(void)a;(void)b;assert(!strcmp(name,"file_download") && stack==8192 && priority==3 && core==-1);
    ++tasks;return task_fail?NULL:(void *)1;
}
'''
for name in ['download_cancelled','download_send','download_cancel_and_wait','download_finish','download_file','download_task','download_get_handler']:fixture+=function(name)
fixture+=r'''
int main(int argc,char **argv) {
    assert(argc==2);path=argv[1];httpd_req_t req={.handle=1};
    denied=true;assert(download_file(&req)!=0 && !opens && !releases && http_error==503);denied=false;
    open_fail=true;assert(download_file(&req)!=0 && !closes && releases==1);open_fail=false;
    alloc_fail=true;assert(download_file(&req)!=0 && closes==1 && releases==2);alloc_fail=false;
    option_fail=true;assert(download_file(&req)!=0 && closes==1 && releases==2);option_fail=false;
    send_fail=true;assert(download_file(&req)!=0 && closes==2 && releases==3 && !finishes);send_fail=false;
    tasks=99;assert(download_file(&req)==ESP_ERR_TIMEOUT && pending && !lease && !finishes);tasks=0;pending=false;
    assert(download_file(&req)==0 && finishes==1 && !lease);
    async_fail=true;assert(download_get_handler(&req)!=0 && !s_download_active);async_fail=false;
    task_fail=true;assert(download_get_handler(&req)==0 && !s_download_active && completed==1);task_fail=false;
    assert(download_get_handler(&req)==0 && s_download_active);
    unsigned before=tasks;assert(download_get_handler(&req)==0 && tasks==before && http_error==503);
    send_fail=true;download_task(&req);assert(!s_download_active && completed==2 && closed==2 && deleted==1);
    send_fail=false;partial_send=true;socket_calls=0;pending=false;
    assert(download_file(&req)!=0 && pending && socket_calls==3 && !lease);
    partial_send=false;pending=false;would_block=true;s_download_deadline_us=now+300000000;
    int64_t started=now;assert(download_send(1,3,"x",1,0)==HTTPD_SOCK_ERR_TIMEOUT && now-started==200000);
    would_block=false;
    s_download_active=true;started=now;
    assert(!download_cancel_and_wait() && now-started==5000000 && s_download_active && s_download_closing);
    s_download_active=false;assert(download_cancel_and_wait());
    unsigned old_tasks=tasks;assert(download_get_handler(&req)==0 && !s_download_active && tasks==old_tasks);
    s_download_closing=false;
    puts("download production lifetime: bounded admission/send, recording cancellation, async retirement and cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='somno-download-') as directory:
    d=Path(directory);(d/'file').write_bytes(b'x'*5000);(d/'test.c').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test'),str(d/'file')],check=True)
