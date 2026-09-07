#!/usr/bin/env python3
"""Production C lifecycle and task ownership under saturated wake queues."""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'components/therapy_alert/therapy_alert.c').read_text()
def function(name):
    m=re.search(r'^(?:static\s+)?[\w\s*]+\b'+name+r'\([^;]*?\)\s*\{',s,re.M)
    assert m,name
    i=m.end(); depth=1
    while depth:
        if s[i]=='{':depth+=1
        elif s[i]=='}':depth-=1
        i+=1
    return s[m.start():i]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "therapy_alert.h"
#include "alert_lifecycle.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdTRUE 1
#define ALERT_DELIVERY_FAILED 1
#define pdMS_TO_TICKS(x) (x)
typedef void *TaskHandle_t;
typedef struct { uint32_t gen, history_id; } alert_routine_args_t;
typedef enum { EVT_THERAPY_START,EVT_THERAPY_STOP,EVT_BLE_DISCONNECT,EVT_ACK,EVT_ROUTINE_EXIT } alert_evt_type_t;
typedef struct { uint8_t type,state; uint32_t gen; } alert_evt_t;
static void *s_evt_q=(void *)1,*s_ack_pending=(void *)2;
static int wakes,ack_latched,s_lifecycle_lock,live_tasks,created,deleted;
static bool clock_set=true,window=true,therapy_on=true,spawn_fail,ack_during_history;
static therapy_alert_config_t s_cfg={.enabled=true};
static alert_state_t s_state;
static uint32_t s_routine_gen,s_active_history_id;
static bool s_routine_active;
static alert_lifecycle_t s_lifecycle,s_lifecycle_cursor;
static int xQueueSend(void *q,const void *p,int timeout) { ++wakes; return 0; } // all 8 ordinary slots occupied
static void xSemaphoreGive(void *p) { ++ack_latched; }
static void set_state(alert_state_t st) { s_state=st; }
static uint32_t alert_history_begin(bool b) { if(ack_during_history) therapy_alert_acknowledge(); return 1; }
static void alert_history_result(uint32_t id,int result,bool b) {}
static void alert_history_cancel(uint32_t id) {}
static void alert_history_ack(uint32_t id) {}
static bool time_is_set(void) { return clock_set; }
static int current_minutes_from_midnight(void) { return 0; }
static bool time_in_window(int now,int a,int b) { return window; }
static bool currently_in_window(void) { return window; }
static bool therapy_is_active(void) { return therapy_on; }
alert_state_t therapy_alert_get_state(void) { return s_state; }
static void report_stack_headroom(const char *s) {}
static void alert_routine_task(void *p) {}
static TaskHandle_t create_task(void (*fn)(void *),const char *name,uint32_t bytes,void *arg,unsigned priority,int core,void *stack,void *tcb) {
 assert(fn==alert_routine_task && bytes==16384 && !stack && !tcb);
 if(spawn_fail) return NULL;
 free(arg); ++created; ++live_tasks; return (void *)3;
}
static void delete_task(TaskHandle_t task) { assert(!task); --live_tasks; ++deleted; }
static TaskHandle_t (*s_task_create)(void (*)(void *),const char*,uint32_t,void*,unsigned,int,void*,void*)=create_task;
static void (*s_task_delete)(TaskHandle_t)=delete_task;
'''
prod='\n'.join(function(n) for n in ['therapy_alert_is_actionable','post_evt','finish_alert_routine','start_alert_routine','cancel_alert_routine','handle_therapy_start','handle_therapy_stop','handle_ble_disconnect','handle_ack','drain_lifecycle','therapy_alert_on_therapy_start','therapy_alert_on_therapy_stop','therapy_alert_on_ble_disconnect','therapy_alert_acknowledge','therapy_alert_on_transport_loss','therapy_alert_transport_uncertain','reevaluate_state'])
tests=r'''
int main(void) {
 // START STOP must survive a permanently saturated ordinary queue.
 therapy_alert_on_therapy_start(); therapy_alert_on_therapy_stop(); drain_lifecycle();
 assert(s_state==ALERT_PENDING && live_tasks==1);
 therapy_alert_acknowledge(); uint32_t after_ack=s_routine_gen;
 assert(ack_latched && after_ack>0); drain_lifecycle(); assert(s_state==ALERT_ACKED);
 finish_alert_routine(after_ack); assert(!live_tasks);
 // Delayed ordinary queue wakeups cannot replay START and clear sticky ACK.
 for(int i=0;i<20;++i) { drain_lifecycle(); reevaluate_state(); }
 assert(s_state==ALERT_ACKED);
 therapy_alert_on_ble_disconnect(); drain_lifecycle(); reevaluate_state(); assert(s_state==ALERT_ACKED);
 // A genuinely new cycle clears old ACK, including an undrained old ACK.
 therapy_alert_on_therapy_start(); therapy_alert_acknowledge(); therapy_alert_on_therapy_stop();
 therapy_alert_on_therapy_start(); therapy_alert_on_therapy_stop(); drain_lifecycle();
 assert(s_state==ALERT_PENDING && live_tasks==1); finish_alert_routine(s_routine_gen);
 // ACK before STOP in the same cycle suppresses escalation, even when both queued wakes drop.
 therapy_alert_on_therapy_start(); therapy_alert_acknowledge(); therapy_alert_on_therapy_stop(); drain_lifecycle();
 assert(s_state==ALERT_ACKED && !live_tasks);
 therapy_alert_on_therapy_start(); drain_lifecycle(); assert(s_state==ALERT_ARMED);
 for(int i=0;i<100;++i) therapy_alert_on_therapy_start();
 therapy_alert_acknowledge(); drain_lifecycle(); assert(s_state==ALERT_ACKED);
 // Loss is explicit; neither periodic stale active-checker nor disconnect can clear it.
 therapy_alert_on_transport_loss(); drain_lifecycle(); therapy_alert_on_ble_disconnect(); drain_lifecycle();
 assert(therapy_alert_transport_uncertain()); reevaluate_state(); assert(s_state==ALERT_ACKED);
 therapy_alert_on_therapy_start(); drain_lifecycle(); assert(!therapy_alert_transport_uncertain() && s_state==ALERT_ARMED);
 therapy_alert_on_ble_disconnect(); drain_lifecycle(); reevaluate_state(); assert(s_state==ALERT_DISARMED);
 // ACK already retained after STOP must not launch a zero-delay push task.
 therapy_alert_on_therapy_start(); therapy_alert_on_therapy_stop(); therapy_alert_acknowledge(); drain_lifecycle();
 assert(s_state==ALERT_ACKED && !live_tasks);
 // An ACK arriving during synchronous history creation also suppresses launch.
 ack_during_history=true; therapy_alert_on_therapy_start(); therapy_alert_on_therapy_stop(); drain_lifecycle();
 assert(s_state==ALERT_PENDING && !live_tasks); drain_lifecycle(); assert(s_state==ALERT_ACKED);
 ack_during_history=false;
 // ACK and disconnect both retained before draining must keep ACK sticky.
 therapy_alert_on_therapy_start(); therapy_alert_on_therapy_stop(); drain_lifecycle();
 assert(live_tasks==1); therapy_alert_acknowledge(); therapy_alert_on_ble_disconnect(); drain_lifecycle();
 assert(s_state==ALERT_ACKED); finish_alert_routine(s_routine_gen);
 // Every normal cycle uses paired create/delete; no detached static stack or TCB.
 for(int i=0;i<10000;++i) {
  therapy_alert_on_therapy_start(); therapy_alert_on_therapy_stop(); drain_lifecycle();
  assert(s_state==ALERT_PENDING && live_tasks==1);
  finish_alert_routine(s_routine_gen); assert(!live_tasks);
 }
 assert(created==deleted);
 spawn_fail=true; therapy_alert_on_therapy_start(); therapy_alert_on_therapy_stop(); drain_lifecycle();
 assert(s_state==ALERT_DISARMED && !live_tasks);
}
'''
with tempfile.TemporaryDirectory(prefix='somno-alert-') as tmp:
 p=Path(tmp); (p/'test.c').write_text(pre+prod+tests)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
 '-I',str(ROOT/'scripts/test_include'),'-I',str(ROOT/'components/therapy_alert'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Production alert saturated queue, cycle ordering, sticky ACK, uncertainty and 10000 paired task lifetimes passed')
