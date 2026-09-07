#!/usr/bin/env python3
"""Compile actual production C against deterministic faulting transport/RTOS doubles."""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]

def function(source, name):
    # Extraction chooses the production function; none of its logic is modeled.
    import re
    m = re.search(r'^(?:static\s+)?[\w\s*]+\b'+re.escape(name)+r'\([^;]*?\)\s*\{', source, re.M)
    assert m, name
    start=m.start(); i=m.end(); depth=1
    while depth:
        if source[i]=='{': depth+=1
        elif source[i]=='}': depth-=1
        i+=1
    return source[start:i]

def run(name, source, includes=()):
    with tempfile.TemporaryDirectory(prefix='somno-backend-') as tmp:
        p=Path(tmp); (p/'test.c').write_text(source)
        subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-Wall','-Wextra','-Werror',
            '-Wno-unused-function','-Wno-unused-parameter','-pthread',
            *sum((['-I',str(ROOT/x)] for x in includes),[]),str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True,timeout=10)
    print(name+' passed')

COMMON = '''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
'''

ble=(ROOT/'main/as11_ble.c').read_text()
smb=(ROOT/'components/uploader/uploader_smb.c').read_text()
tls=(ROOT/'components/uploader/uploader_sleephq.c').read_text()

# SMB's *same* wait pump is used for connect, open, mkdir, writes, close,
# mtime and disconnect. Destroy invokes any pending callback synchronously.
smba=smb[smb.index('static struct smb2_context *s_smb;'):smb.index('/* Upload a single local')]
run('SMB idle timeout, cancellation, late callback ownership', COMMON+r'''
#include <time.h>
#include <sys/time.h>
#include <sys/poll.h>
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#define ESP_LOGW(...) ((void)0)
static int64_t clock_us, cancel_at;
static int destroyed, serviced, completed_at, submitted_status, write_slice, write_seen;
static smb2_command_cb cb;
static void *cb_arg;
static struct smb2_context *ctx_value=(struct smb2_context *)(uintptr_t)1;
static struct smb2fh *fh_value=(struct smb2fh *)(uintptr_t)2;
static bool uploader_should_cancel(void) { return cancel_at >= 0 && clock_us >= cancel_at; }
static int64_t esp_timer_get_time(void) { return clock_us; }
int poll(struct pollfd *fd, nfds_t n, int ms) { assert(ms<=20); clock_us+=ms*1000; fd->revents=0; return 0; }
void smb2_destroy_context(struct smb2_context *ctx) { assert(ctx==ctx_value); ++destroyed; if(cb) { smb2_command_cb f=cb; cb=NULL; f(ctx,-ECANCELED,NULL,cb_arg); } }
const char *smb2_get_error(struct smb2_context *ctx) { assert(!destroyed); return "fake"; }
int smb2_get_fd(struct smb2_context *ctx) { assert(!destroyed); return 7; }
int smb2_which_events(struct smb2_context *ctx) { return POLLIN; }
int smb2_service(struct smb2_context *ctx, int events) { ++serviced; if(completed_at && clock_us>=completed_at && cb) { smb2_command_cb f=cb; cb=NULL; f(ctx,submitted_status,fh_value,cb_arg); } return 0; }
static int submit(smb2_command_cb f, void *arg) { cb=f; cb_arg=arg; submitted_status=0; return 0; }
int smb2_connect_share_async(struct smb2_context *c,const char *h,const char *s,const char *u,smb2_command_cb f,void *a) { return submit(f,a); }
int smb2_mkdir_async(struct smb2_context *c,const char *p,smb2_command_cb f,void *a) { return submit(f,a); }
int smb2_open_async(struct smb2_context *c,const char *p,int flags,smb2_command_cb f,void *a) { return submit(f,a); }
int smb2_pwrite_async(struct smb2_context *c,struct smb2fh *h,const uint8_t *b,uint32_t n,uint64_t off,smb2_command_cb f,void *a) { submit(f,a); if(write_slice) { assert(off==(uint64_t)write_seen && b[0]=='A'+write_seen); submitted_status=n<(uint32_t)write_slice?(int)n:write_slice; write_seen+=submitted_status; } return 0; }
int smb2_close_async(struct smb2_context *c,struct smb2fh *h,smb2_command_cb f,void *a) { return submit(f,a); }
int smb2_disconnect_share_async(struct smb2_context *c,smb2_command_cb f,void *a) { return submit(f,a); }
smb2_file_id *smb2_get_file_id(struct smb2fh *fh) { static smb2_file_id id; return &id; }
struct smb2_pdu *smb2_cmd_set_info_async(struct smb2_context *c,struct smb2_set_info_request *r,smb2_command_cb f,void *a) { submit(f,a); return (struct smb2_pdu *)3; }
void smb2_queue_pdu(struct smb2_context *c,struct smb2_pdu *p) { }
'''+smba+r'''
static void reset(void) { clock_us=0; cancel_at=-1; destroyed=serviced=completed_at=submitted_status=write_slice=write_seen=0; cb=NULL; s_smb=ctx_value; }
int main(void) {
 for(int op=0;op<7;++op) {
  reset(); cancel_at=41000;
  switch(op) {
   case 0: assert(smb_connect(s_smb,"1.2.3.4","share","user")<0); break;
   case 1: assert(smb_mkdir(s_smb,"path")<0); break;
   case 2: assert(!smb_open(s_smb,"path",0)); break;
   case 3: { uint8_t *buf=malloc(32); assert(smb_write(s_smb,fh_value,buf,32,0)<0); assert(destroyed==1); free(buf); break; }
   case 4: assert(smb_close(s_smb,fh_value)<0); break;
   case 5: assert(smb_set_mtime(s_smb,fh_value,99)<0); break;
   case 6: assert(smb_disconnect(s_smb)<0); break;
  }
  assert(clock_us<=61000 && destroyed==1 && !s_smb && !cb);
  assert(smb_close(ctx_value,fh_value)<0); assert(destroyed==1);
 }
 reset(); assert(smb_set_mtime(s_smb,fh_value,0)<0);
 assert(clock_us==5000000 && serviced==250 && destroyed==1);
 reset(); completed_at=40000; assert(smb_open(s_smb,"path",0)==fh_value); assert(!destroyed);
 smb_abort(); assert(destroyed==1);
 reset(); completed_at=20000; write_slice=2;
 assert(smb_write_all(s_smb,fh_value,(const uint8_t *)"ABCDE",5,0)==5 && write_seen==5);
 smb_abort(); assert(destroyed==1);
}
''', ('third_party/libsmb2/include/smb2','third_party/libsmb2/include'))

# Bound every TLS request even when peer trickles data forever.
a=tls.index('static int64_t s_request_deadline;');b=tls.index('/* Read a complete',a)
run('TLS WANT_READ/WANT_WRITE, trickle deadline and cancellation',COMMON+r'''
#include <sys/types.h>
typedef int esp_tls_t;
#define ESP_TLS_ERR_SSL_WANT_READ -2
#define ESP_TLS_ERR_SSL_WANT_WRITE -3
#define pdMS_TO_TICKS(x) (x)
static int64_t now, cancel_at; static int mode, calls;
static bool uploader_should_cancel(void) { return cancel_at>=0 && now>=cancel_at; }
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(int ms) { assert(ms<=20); now+=ms*1000; }
static ssize_t esp_tls_conn_read(esp_tls_t *tls,void *buf,size_t n) { ++calls; return ESP_TLS_ERR_SSL_WANT_READ; }
static ssize_t esp_tls_conn_write(esp_tls_t *tls,const void *buf,size_t n) { ++calls; if(mode) { now+=10000; return 1; } return ESP_TLS_ERR_SSL_WANT_WRITE; }
'''+tls[a:b]+r'''
int main(void) {
 char buf[1000]={0}; s_request_deadline=100000; cancel_at=-1;
 assert(shq_tls_read(NULL,buf,1)==-1 && now==100000 && calls==5);
 now=calls=0; cancel_at=21000;
 assert(shq_tls_write_all(NULL,buf,10)==-1 && now==40000 && calls==2);
 now=calls=0; cancel_at=-1; mode=1;
 assert(shq_tls_write_all(NULL,buf,1000)==-1 && now==100000 && calls==10);
 now=calls=0; assert(shq_tls_write_all(NULL,buf,3)==3 && calls==3);
}
''')

# Decoded bytes retained exactly; admission does not duplicate or reorder them.
a=ble.index('#define PENDING_STREAM_COUNT');b=ble.index('static void notif_proc_task',a)
run('Pending stream bounded retention, replay and STOP fence',COMMON+r'''
#define MALLOC_CAP_SPIRAM 1
static bool admitted; static int accepted, loss, allocs;
static char seen[256];
static bool session_writer_try_stream_data_raw(const char *s,int n) { if(!admitted) return false; memcpy(seen+accepted,s,n); accepted+=n; return true; }
static void *heap_caps_malloc(size_t n,int caps) { ++allocs; return malloc(n); }
static void notif_mark_loss(void) { ++loss; }
'''+ble[a:b]+r'''
int main(void) {
 pending_stream_submit("A",1); pending_stream_submit("B",1); pending_stream_replay();
 assert(s_pending_count==2 && accepted==0);
 admitted=true; pending_stream_replay(); assert(s_pending_count==0 && accepted==2 && !memcmp(seen,"AB",2));
 pending_stream_replay(); assert(accepted==2);
 admitted=false; pending_stream_submit("old",3); pending_stream_clear(); // decoded STOP fence
 admitted=true; pending_stream_replay(); pending_stream_submit("new",3);
 assert(accepted==5 && !memcmp(seen,"ABnew",5));
 admitted=false; for(int i=0;i<PENDING_STREAM_COUNT+1;++i) pending_stream_submit("x",1);
 assert(loss==1 && s_pending_count==0 && s_pending_bytes==0);
 pending_stream_submit("z",1); pending_stream_clear(); assert(!s_pending_count);
}
''')

# Timeout cleanup cannot free a semaphore/collector while decoder holds its mutex.
run('Spool withdrawal joins decoder before reclamation', COMMON+r'''
#include <pthread.h>
#include <stdatomic.h>
#define portMAX_DELAY 0
#define BLE_HS_CONN_HANDLE_NONE -1
#define BLE_ERR_REM_USER_CONN_TERM 0
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static pthread_mutex_t *s_spool_mtx=&lock;
static bool s_spool_tainted; static int s_conn_handle=-1, freed, entered;
static atomic_int decoder_live;
typedef struct { struct { void *data; } frags[32]; int frag_count; void *sem; } spool_collector_t;
static spool_collector_t *s_spool_collector;
static void xSemaphoreTake(pthread_mutex_t *m,int timeout) { pthread_mutex_lock(&gate); entered=1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&gate); pthread_mutex_lock(m); }
static void xSemaphoreGive(pthread_mutex_t *m) { pthread_mutex_unlock(m); }
static void vSemaphoreDelete(void *sem) { assert(!atomic_load(&decoder_live)); assert(!s_spool_collector); ++freed; free(sem); }
static void ble_gap_terminate(int a,int b) { }
'''+function(ble,'spool_withdraw')+r'''
static void *withdraw(void *p) { spool_withdraw(p,true); return NULL; }
int main(void) {
 spool_collector_t coll={0}; coll.sem=malloc(8); s_spool_collector=&coll;
 pthread_mutex_lock(&lock); atomic_store(&decoder_live,1);
 pthread_t thread; pthread_create(&thread,NULL,withdraw,&coll);
 pthread_mutex_lock(&gate); while(!entered) pthread_cond_wait(&cond,&gate); pthread_mutex_unlock(&gate);
 assert(!freed && s_spool_collector==&coll);
 coll.frags[coll.frag_count++].data=malloc(12); // decoder was already in flight
 atomic_store(&decoder_live,0); pthread_mutex_unlock(&lock);
 pthread_join(thread,NULL);
 assert(freed==1 && !coll.sem && !coll.frag_count && !s_spool_collector && s_spool_tainted);
 pthread_mutex_lock(&lock); assert(!s_spool_collector); pthread_mutex_unlock(&lock); // late decoder
}
''')

# Reply ownership and exact ID matching, using production accept/clear/wait.
run('RPC late/mismatched/duplicate response fencing',COMMON+r'''
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static int s_response_lock, sem_count, deleted;
static void *s_resp_sem;
static int64_t now;
typedef struct cJSON { double valuedouble; bool number; struct cJSON *id; } cJSON;
static cJSON *s_resp_json, *s_response_original_id;
static uint32_t s_expected_response_id;
static cJSON *cJSON_GetObjectItemCaseSensitive(cJSON *p,const char *key) { return p->id; }
static bool cJSON_IsNumber(cJSON *p) { return p && p->number; }
static void cJSON_Delete(cJSON *p) { if(p) { cJSON_Delete(p->id); ++deleted; free(p); } }
static bool cJSON_ReplaceItemInObjectCaseSensitive(cJSON *p,const char *key,cJSON *id) { cJSON_Delete(p->id); p->id=id; return true; }
static int xSemaphoreGive(void *s) { ++sem_count; return 1; }
static int xSemaphoreTake(void *s,int ticks) { if(sem_count) { --sem_count; return 1; } now+=(int64_t)ticks*1000; return 0; }
static int64_t esp_timer_get_time(void) { return now; }
'''+function(ble,'rpc_accept_response')+function(ble,'clear_response')+function(ble,'wait_response')+r'''
static cJSON *reply(int id) { cJSON *p=calloc(1,sizeof(*p)); p->id=calloc(1,sizeof(*p)); p->id->number=true; p->id->valuedouble=id; return p; }
int main(void) {
 s_expected_response_id=101; rpc_accept_response(reply(100)); assert(!s_resp_json && !sem_count);
 rpc_accept_response(reply(101)); cJSON *first=s_resp_json;
 rpc_accept_response(reply(101)); assert(s_resp_json==first && sem_count==1);
 cJSON *r=wait_response(50); assert(r==first && !s_expected_response_id); cJSON_Delete(r);
 clear_response(); s_expected_response_id=102; sem_count=3;
 assert(!wait_response(50) && now==50000 && !s_expected_response_id);
 rpc_accept_response(reply(102)); assert(!s_resp_json);
 s_expected_response_id=103; rpc_accept_response(reply(102)); assert(!s_resp_json);
 rpc_accept_response(reply(103)); r=wait_response(50); assert(r && r->id->valuedouble==103); cJSON_Delete(r);
 clear_response(); assert(!sem_count && deleted==12);
}
''')

# The actual scheduler transaction: preparation outside lease; interruption
# leaves groups pending and no transport finalization claims success.
sched=(ROOT/'components/uploader/upload_sched.c').read_text()
run('Scheduler recording handoff and uncommitted-day rollback',COMMON+r'''
#include "uploader.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define LEASE_WAIT_MS 5000
#define FAILS_BEFORE_SWITCH 2
#define portMAX_DELAY 0
#define SB_IDLE 0
#define SB_UPLOADING 1
#define SB_COOLDOWN 2
static int s_lock, held, prepares, ended, finalizes, put_count, cancelled, mode, commits;
typedef struct { const upload_backend_t *be; int slot,state,n_units,cur_unit; uint32_t last_ok_s; char cur_day[12],err[80]; } backend_rt_t;
static upload_day_t day={.day=20260906,.n_groups=2};
static void *heap_caps_malloc(size_t n,int cap) { return malloc(n); }
static void *heap_caps_calloc(size_t n,size_t sz,int cap) { return calloc(n,sz); }
static int xSemaphoreTake(int lock,int timeout) { return 1; }
static void xSemaphoreGive(int lock) {}
static void set_be_state(backend_rt_t *r,int state) { r->state=state; }
static void set_be_error(backend_rt_t *r,const char *s) {}
static void cooldown_enter(backend_rt_t *r,const char *s,bool permanent) { r->state=SB_COOLDOWN; }
static void cooldown_reset(backend_rt_t *r) {}
static uint32_t now_s(void) { return 100; }
bool uploader_should_cancel(void) { return cancelled; }
bool uploader_lease_take(uint32_t timeout) { assert(!held); if(cancelled)return false; held=1; return true; }
void uploader_lease_give(void) { assert(held); held=0; }
int upload_index_day_count(void) { return 1; }
upload_day_t *upload_index_day_at(int n) { return &day; }
upload_day_t *upload_index_day(uint32_t d,bool create) { return &day; }
upload_group_t *upload_index_group(upload_day_t *d,uint32_t p,bool create) { return &d->groups[0]; }
uint64_t upload_index_bundle_ok_fp(int slot) { return 0; }
esp_err_t upload_index_set_bundle_ok(int slot,uint64_t fp) { assert(held); ++commits; return ESP_OK; }
esp_err_t upload_index_save_day(upload_day_t *d) { assert(held); return ESP_OK; }
bool upload_scan_bundle(upload_bundle_ref_t *out) { assert(held); memset(out,0,sizeof(*out)); out->fp=5; if(mode==1)cancelled=1; return true; }
int upload_scan_day_groups(const char *s,upload_group_ref_t *out,int max) { assert(held); memset(out,0,sizeof(*out)); out->n_files=1; return 1; }
int upload_ox_reconcile(upload_ox_ref_t *out,int n,int days) { assert(held); return cancelled?-1:0; }
int upload_ox_pending(const upload_ox_ref_t *r,int n,int slot) { return 0; }
int upload_ox_status(const upload_ox_ref_t *r,int slot) { return UG_PENDING; }
void upload_ox_mark(const upload_ox_ref_t *r,int slot,upload_unit_status_t st,const char *remote) { assert(held); }
static upload_result_t prepare(void) { assert(!held); ++prepares; if(mode==2)cancelled=1; return UPLOAD_OK; }
static upload_result_t session_begin(void) { assert(!held); return UPLOAD_OK; }
static upload_result_t begin_day(const char *d) { assert(held); return UPLOAD_OK; }
static upload_result_t put(const char *d,const upload_group_ref_t *g) { assert(held); ++put_count; if(mode==3)cancelled=1; return UPLOAD_OK; }
static upload_result_t bundle(const char *d,const upload_bundle_ref_t *b,bool changed) { assert(held); return UPLOAD_OK; }
static upload_result_t finalize(const char *d,bool any) { assert(!held && !cancelled); ++finalizes; return UPLOAD_OK; }
static void end(void) { assert(!held); ++ended; }
'''+function(sched,'ox_mark_day_failed')+function(sched,'run_backend')+r'''
int main(void) {
 upload_backend_t be={.id="test",.bundle_only_ok=true,.prepare=prepare,.session_begin=session_begin,.day_begin=begin_day,.put_group=put,.put_bundle=bundle,.day_end=finalize,.session_end=end};
 backend_rt_t r={.be=&be};
 for(mode=0;mode<4;++mode) {
  held=prepares=ended=finalizes=put_count=cancelled=commits=0; day.groups[0].be[0].status=UG_PENDING; day.groups[1].be[0].status=UG_OK;
  run_backend(&r,30); assert(!held && prepares==1 && ended==1);
  assert(day.groups[1].be[0].status==UG_OK); // never duplicate an already committed group
  if(mode==0) { assert(day.groups[0].be[0].status==UG_OK && finalizes==1 && commits==1); }
  else { assert(day.groups[0].be[0].status==UG_PENDING && !finalizes && !commits); }
 }
}
''',('components/uploader','scripts/test_include'))
