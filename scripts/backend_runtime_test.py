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


# Extraction boundary: all present public command owners release their bus.
for name in ("as11_ble_get_values", "as11_ble_get_datetime", "as11_ble_spool_pull",
             "therapy_command"):
    body = function(ble, name)
    assert "xSemaphoreTake(s_cmd_mtx" in body, name
    assert "xSemaphoreGive(s_cmd_mtx)" in body, name
    assert "clear_response()" in body, name
assert "do_setup_encrypted_session(" in function(ble, "reconnect_task")
print("Runtime extraction command ownership and reusable encrypted handshake passed")
