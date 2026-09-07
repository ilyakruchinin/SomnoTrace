#!/usr/bin/env python3
"""Production scheduler/index handoff under queue loss, restart and I/O faults.

Compiles real public types, scheduler loop/polling, index deletion and group
reconciliation. Platform scheduling and storage callback persistence are explicit
test doubles over temporary host files; this is not a FAT durability simulation.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from session_storage_behavior_test import COMMON, function
import pending_export_behavior_test as pending_fixture

ROOT = Path(__file__).resolve().parents[1]
INDEX = (ROOT / 'components/uploader/upload_index.c').read_text()
SCHED = (ROOT / 'components/uploader/upload_sched.c').read_text()
SCAN = (ROOT / 'components/uploader/upload_scan.c').read_text()

code = COMMON + r'''
#include <setjmp.h>
#include "uploader.h"
#include "upload_sched.h"
#undef UPLOAD_STATE_DIR
#define UPLOAD_STATE_DIR "./upload_state"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define portMAX_DELAY -1
#define pdMS_TO_TICKS(x) (x)
typedef int TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
static upload_day_t *s_days[UPLOAD_MAX_DAYS_CAP];
static int s_n_days;
static int lease_depth, unlink_error, next_calls, ack_calls, ack_error;
static bool cancel, deny_lease, deny_callback, cancel_after_next;
static bool cancel_after_take, cancel_after_give, newer_after_give;
static int64_t clock_us;
bool uploader_should_cancel(void) { return cancel; }
static void *heap_caps_calloc(size_t n,size_t z,int caps) {(void)caps;return calloc(n,z);}
static void *idx_alloc(size_t n) {return calloc(1,n);}
static void put(const char *path,const char *value) {
    FILE *f=fopen(path,"w");assert(f);assert(fputs(value,f)>=0);
    assert(fflush(f)==0 && fsync(fileno(f))==0);assert(fclose(f)==0);
}
static bool get(const char *path,char *out,size_t cap) {
    FILE *f=fopen(path,"r");if(!f)return false;
    size_t n=fread(out,1,cap-1,f);out[n]=0;assert(fclose(f)==0);return true;
}
static int checked_unlink(const char *path) {
    assert(lease_depth==1);
    if(unlink_error) {errno=unlink_error;return -1;}
    return unlink(path);
}
#define unlink checked_unlink
'''
code += ''.join(function(INDEX,n) for n in [
    'day_path','find_day_idx','upload_index_day','upload_index_group',
    'upload_index_drop_group','upload_index_forget_day'])
code += r'''
#undef unlink
static bool next_work(uint32_t *day,char *token,size_t cap) {
    assert(lease_depth==0);next_calls++;
    if(deny_callback || !get("./pending",token,cap))return false;
    *day=20260906;if(cancel_after_next)cancel=true;return true;
}
static esp_err_t ack_work(uint32_t day,const char *token) {
    assert(lease_depth==0 && day==20260906);ack_calls++;
    char current[128];if(ack_error)return ESP_FAIL;
    if(!get("./pending",current,sizeof current) || strcmp(current,token))
        return ESP_ERR_INVALID_STATE;
    assert(unlink("./pending")==0);return ESP_OK;
}
bool uploader_lease_take(uint32_t timeout) {
    assert(timeout==0 && lease_depth==0);
    if(cancel || deny_lease)return false;
    lease_depth=1;if(cancel_after_take)cancel=true;return true;
}
void uploader_lease_give(void) {
    assert(lease_depth==1);lease_depth=0;
    if(cancel_after_give)cancel=true;
    if(newer_after_give) {put("./pending","session:new-generation");newer_after_give=false;}
}
int upload_scan_day_groups(const char *day,upload_group_ref_t *out,int max_out) {
    assert(!strcmp(day,"20260906") && max_out>0);memset(out,0,sizeof *out);
    out->prefix_sec=100;out->kinds=UGK_BRP;out->n_files=1;return 1;
}
esp_err_t upload_index_save_day(upload_day_t *d) {
    char path[160];day_path(d->day,path,sizeof path);
    put(path,d->groups[0].be[0].status==UG_OK?"old-ok":"new-pending");
    d->dirty=false;return ESP_OK;
}
'''
code += function(SCAN,'upload_scan_reconcile_day')
code += SCHED[SCHED.index('#define SCAN_INTERVAL_MS'):SCHED.index('/* Per-backend cooldown')]
code += SCHED[SCHED.index('typedef enum {\n    SB_DISABLED'):SCHED.index('/* ── Helpers')]
code += r'''
static int scans,passes,queue_calls,queue_stop_at,queue_count,queue_head,dropped;
static sched_ev_t queue_items[8];
static jmp_buf task_exit;
static int64_t now_us(void) {return clock_us;}
static uint32_t now_s(void) {return 1700000000;}
static void set_next_scan(int64_t t) {s_next_scan_us=t;}
static void refresh_index_progress_cache(void) {}
static void set_status(const char *format,...) {(void)format;}
static int xPortGetCoreID(void) {return 1;}
static void xSemaphoreTake(void *s,int n) {(void)s;(void)n;}
static void xSemaphoreGive(void *s) {(void)s;}
static void cooldown_reset(backend_rt_t *r) {r->retry_at_us=0;}
static void set_be_state(backend_rt_t *r,sb_state_t s) {r->state=s;}
static void do_scan(void) {scans++;set_next_scan(now_us()+600000000);}
static void run_pass(void) {passes++;}
static bool run_backend(backend_rt_t *r,int d) {(void)r;(void)d;return true;}
esp_err_t uploader_smb_probe(void) {return ESP_OK;}
esp_err_t uploader_sleephq_probe(void) {return ESP_OK;}
bool upload_sched_probe_begin(void) {return true;}
void upload_sched_probe_end(void) {}
int uploader_max_days(void) {return 30;}
esp_err_t upload_index_clear(void) {return ESP_OK;}
static int xQueueSend(void *q,const void *item,int wait) {
    (void)q;assert(wait==0);
    if(queue_count==8) {dropped++;return 0;}
    queue_items[(queue_head+queue_count++)%8]=*(const sched_ev_t*)item;return pdTRUE;
}
static int xQueueReceive(void *q,void *item,int wait) {
    (void)q;if(++queue_calls>queue_stop_at)longjmp(task_exit,1);
    assert(wait>=0 && wait<=INVALIDATION_POLL_MS);
    if(queue_count) {*(sched_ev_t*)item=queue_items[queue_head];queue_head=(queue_head+1)%8;queue_count--;return pdTRUE;}
    clock_us+=(int64_t)wait*1000;return 0;
}
'''
code += ''.join(function(SCHED,n) for n in [
    'service_pending_invalidation','poll_pending_invalidation','sched_task',
    'upload_sched_set_invalidation_hooks','post','upload_sched_notify_invalidate'])
code += r'''
static void reset_ram(void) {
    for(int i=0;i<s_n_days;i++)free(s_days[i]);s_n_days=0;
    lease_depth=unlink_error=next_calls=ack_calls=ack_error=0;
    cancel=deny_lease=deny_callback=cancel_after_next=false;
    cancel_after_take=cancel_after_give=newer_after_give=false;
    clock_us=s_next_invalidation_poll_us=0;s_task=NULL;s_queue=(void*)1;
    scans=passes=queue_calls=queue_count=queue_head=dropped=0;s_n_rt=0;
    upload_sched_set_invalidation_hooks(next_work,ack_work);
}
static upload_day_t *seed(void) {
    reset_ram();mkdir(UPLOAD_STATE_DIR,0700);put("./pending","session:old-generation");
    upload_day_t *d=upload_index_day(20260906,true);assert(d);
    upload_group_t *g=upload_index_group(d,100,true);assert(g);
    g->kinds=UGK_BRP;g->n_files=1;g->be[0].status=UG_OK;upload_index_save_day(d);
    return d;
}
static bool pending(void) {return access("./pending",F_OK)==0;}
static bool index_file(void) {return access(UPLOAD_STATE_DIR "/20260906.json",F_OK)==0;}
static void scheduler_turns(int turns) {
    queue_stop_at=turns;queue_calls=0;if(setjmp(task_exit)==0)sched_task(NULL);
    assert(lease_depth==0);
}
int main(void) {
    // Failed deletion cannot evict RAM success or acknowledge persisted work.
    const int errors[]={EIO,EROFS,ENOSPC};
    for(size_t i=0;i<sizeof errors/sizeof *errors;i++) {
        upload_day_t *d=seed();unlink_error=errors[i];assert(!service_pending_invalidation());
        assert(pending() && index_file() && ack_calls==0 && s_days[0]==d);
        assert(d->groups[0].be[0].status==UG_OK && lease_depth==0);
    }
    // A pending recording or denied gate never waits or loses the token.
    for(int phase=0;phase<5;phase++) {
        seed();if(phase==0)cancel=true;if(phase==1)deny_lease=true;
        if(phase==2)cancel_after_next=true;if(phase==3)cancel_after_take=true;
        if(phase==4)deny_callback=true;
        assert(!service_pending_invalidation());assert(pending() && index_file() && ack_calls==0);
        assert(lease_depth==0);if(phase==0)assert(next_calls==0);
    }
    // Reset/cancellation after checked deletion, before acknowledgement.
    seed();cancel_after_give=true;assert(!service_pending_invalidation());
    assert(pending() && !index_file() && ack_calls==0);reset_ram();
    assert(service_pending_invalidation());assert(!pending() && !index_file());
    // Same prefix/count/kinds was already uploaded: real reconcile now reoffers it.
    assert(upload_scan_reconcile_day(20260906)==1);
    assert(s_days[0]->groups[0].be[0].status==UG_PENDING);
    // A failed ack is likewise retryable after losing all scheduler RAM state.
    seed();ack_error=1;assert(!service_pending_invalidation());
    assert(pending() && !index_file());reset_ram();assert(service_pending_invalidation());
    // Another completed publication between the short leases must survive old ack.
    seed();newer_after_give=true;assert(!service_pending_invalidation());
    char token[128];assert(get("./pending",token,sizeof token));assert(!strcmp(token,"session:new-generation"));
    assert(service_pending_invalidation());assert(!pending());
    // Queue saturation loses only a nudge. Actual scheduler polls persisted work.
    seed();for(int i=0;i<8;i++)post(EV_EXPORT,20260906);
    upload_sched_notify_invalidate(20260906);assert(dropped==1);
    scheduler_turns(12);assert(!pending() && !index_file() && ack_calls==1);
    // Process restart needs no event at all; unchanged filenames are invalidated.
    seed();reset_ram();scheduler_turns(3);assert(!pending() && !index_file());
    // Idle polling neither spins nor launches network/scans before their deadline.
    reset_ram();scheduler_turns(6);assert(next_calls==7 && clock_us==6000000);
    assert(scans==0 && passes==0);
    // Immutable startup registration rejects partial hook replacement at runtime.
    s_task=(void*)1;upload_sched_set_invalidation_hooks(NULL,NULL);
    assert(s_invalidation_next==next_work && s_invalidation_ack==ack_work);
    reset_ram();unlink(UPLOAD_STATE_DIR "/20260906.json");rmdir(UPLOAD_STATE_DIR);
    puts("production invalidation: checked deletion, full queue/restart, exact tokens, same-file-set reconcile, cancellation and idle cadence passed");
}
'''

joint = COMMON + r'''
#include "uploader.h"
#undef UPLOAD_STATE_DIR
#define UPLOAD_STATE_DIR "./upload_state"
static int lease_depth,fail_index_unlink,fail_marker_unlink;
static bool index_owner,recording,new_generation_on_release,deny_ack_on_release;
static int fault_unlink(const char *path) {
    assert(lease_depth==1);
    bool index=strstr(path,"/upload_state/")!=NULL;
    assert(index_owner==index);
    if((index && fail_index_unlink) || (!index && fail_marker_unlink)) {errno=EIO;return -1;}
    return unlink(path);
}
#define unlink fault_unlink
'''
# Reuse the storage owner's fixture without changing its test: production
# next/ack/nonce journal functions, explicit scalar JSON fields, real FILE I/O.
storage = pending_fixture.code.split('int main(void) {')[0][len(COMMON):]
# The joint fixture distinguishes index deletion from marker deletion; retain
# that stronger owner-aware injector instead of the storage-only fault toggle.
storage = storage.replace('#define unlink fake_unlink',
                          '#undef unlink\n#define unlink fault_unlink')
storage = storage.replace('(void)a;(void)b;if(!allow_lease)return false;',
                          'assert(a==SD_LEASE_EXPORT && b==0 && lease_depth==0);'
                          'if(!allow_lease)return false;')
joint += storage + r'''
static upload_day_t *s_days[UPLOAD_MAX_DAYS_CAP];
static int s_n_days;
static uploader_invalidation_next_fn_t s_invalidation_next=session_writer_next_upload_invalidation;
static uploader_invalidation_ack_fn_t s_invalidation_ack=session_writer_ack_upload_invalidation;
static void refresh_index_progress_cache(void) {assert(lease_depth==0);}
static void set_next_scan(int64_t t) {assert(t==0 && lease_depth==0);}
bool uploader_should_cancel(void) {return recording;}
bool uploader_lease_take(uint32_t timeout) {
    assert(timeout==0 && lease_depth==0);if(recording)return false;
    lease_depth=1;index_owner=true;return true;
}
void uploader_lease_give(void) {
    assert(lease_depth==1 && index_owner);lease_depth=0;index_owner=false;
    if(new_generation_on_release) {
        new_generation_on_release=false;
        assert(session_writer_mark_upload_invalidation("20260906")==ESP_OK);
    }
    if(deny_ack_on_release)allow_lease=false;
}
'''
joint += ''.join(function(INDEX,n) for n in ['day_path','find_day_idx','upload_index_forget_day'])
joint += function(SCHED,'service_pending_invalidation')
joint += r'''
#undef unlink
static void boot_ram(void) {
    assert(lease_depth==0);for(int i=0;i<s_n_days;i++)free(s_days[i]);s_n_days=0;
    fail_index_unlink=fail_marker_unlink=0;allow_lease=true;deny_ack_on_release=false;
}
static void seed_joint(void) {
    boot_ram();assert(session_writer_mark_upload_invalidation("20260906")==ESP_OK);
    mkdir(UPLOAD_STATE_DIR,0700);FILE *f=fopen(UPLOAD_STATE_DIR "/20260906.json","w");
    assert(f);fputs("old success",f);assert(fclose(f)==0);
    s_days[s_n_days++]=calloc(1,sizeof(upload_day_t));s_days[0]->day=20260906;
    s_days[0]->groups[0].be[0].status=UG_OK;
}
static void require_work(char *token) {
    uint32_t day;assert(session_writer_next_upload_invalidation(&day,token,128));assert(day==20260906);
}
int main(void) {
    char token[128],newer[128];uint32_t day;
    seed_joint();require_work(token);fail_index_unlink=1;
    assert(!service_pending_invalidation());require_work(newer);assert(!strcmp(token,newer));
    assert(s_n_days==1 && s_days[0]->groups[0].be[0].status==UG_OK);
    fail_index_unlink=0;fail_marker_unlink=1;
    assert(!service_pending_invalidation());assert(s_n_days==0);
    require_work(newer);assert(!strcmp(token,newer));
    // Both production parties recover using only persisted token/file state.
    boot_ram();assert(service_pending_invalidation());
    assert(!session_writer_next_upload_invalidation(&day,newer,sizeof newer));
    seed_joint();require_work(token);new_generation_on_release=true;
    assert(!service_pending_invalidation());require_work(newer);assert(strcmp(token,newer));
    assert(service_pending_invalidation());
    seed_joint();deny_ack_on_release=true;assert(!service_pending_invalidation());
    boot_ram();require_work(token);assert(service_pending_invalidation());
    boot_ram();unlink(UPLOAD_STATE_DIR "/20260906.json");rmdir(UPLOAD_STATE_DIR);
    rmdir(SD_PENDING_EXPORT_DIR);
    puts("joint production storage/scheduler: checked disk deletion, persisted nonce, failed ack, restart and newer-publication fencing passed");
}
'''

def compile_run(source):
    with tempfile.TemporaryDirectory(prefix='somno-invalidation-') as tmp:
        p = Path(tmp)
        (p / 'esp_err.h').write_text('#pragma once\n')
        (p / 'test.c').write_text(source)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(p), '-I', str(ROOT / 'components/uploader'),
                        str(p / 'test.c'), '-o', str(p / 'test')], check=True)
        subprocess.run([str(p / 'test')], cwd=p, check=True, timeout=15)

if __name__ == '__main__':
    compile_run(code)
    compile_run(joint)
