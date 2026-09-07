#!/usr/bin/env python3
"""Run the actual storage queue loop with ordered event and commit commands."""
from session_storage_behavior_test import COMMON, DEFS, SW, function, run

code=COMMON+r'''
#include <setjmp.h>
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef struct session_writer session_writer_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
'''+DEFS+r'''
static session_writer_t state;
static sw_cmd_t q[32];static int head,tail,commits;static int64_t now;
static void *s_storage_q=(void*)1;static jmp_buf complete;
static int64_t esp_timer_get_time(void){return now;}
static int xPortGetCoreID(void){return 0;}
static void vTaskSuspend(void *p){(void)p;}
static void io_fail(session_writer_t *s,const char *p){(void)p;s->storage_failed=true;}
static int xQueueReceive(void *queue,void *out,int timeout) {
    (void)queue;(void)timeout;
    if(head==tail)longjmp(complete,1);
    sw_cmd_t cmd=q[head++];memcpy(out,&cmd,sizeof cmd);
    now=head==1?1000:31000000;
    return pdTRUE;
}
static int xQueueSend(void *queue,const void *p,int timeout){
    (void)queue;(void)timeout;assert(tail<32);q[tail++]=*(const sw_cmd_t*)p;return pdTRUE;
}
static unsigned uxQueueSpacesAvailable(void *p){(void)p;return 24-(tail-head);}
static session_writer_t *active_session_try_lock(bool *sampled){*sampled=true;return &state;}
static void active_session_unlock(session_writer_t *s){(void)s;}
static bool swap_and_enqueue_locked(session_writer_t *s){(void)s;return true;}
static void storage_open(session_writer_t *s){s->files_open=true;}
static void storage_write_batch(session_writer_t *s,stream_batch_t *b){(void)s;(void)b;}
static void storage_finish_and_dispatch(session_writer_t *s,const sw_cmd_t *cmd){(void)s;(void)cmd;assert(0);}
static void sw_request_finalize(session_writer_t *s,const char *a,int64_t b,bool c){(void)s;(void)a;(void)b;(void)c;assert(0);}
static void storage_commit(session_writer_t *s){
    assert(s->have_uncommitted);fflush(s->f_events);long end=ftell(s->f_events);
    rewind(s->f_events);char b[32]={0};fread(b,1,sizeof b-1,s->f_events);
    assert(!strcmp(b,"older-event\nnewer-event\n"));fseek(s->f_events,end,SEEK_SET);
    commits++;s->have_uncommitted=false;
}
'''+function(SW,'sw_storage_task')+r'''
int main(void){
    state.f_events=tmpfile();assert(state.f_events);state.last_stream_us=1000;
    q[tail++]=(sw_cmd_t){.type=SW_CMD_OPEN,.s=&state};
    q[tail++]=(sw_cmd_t){.type=SW_CMD_EVENT,.s=&state,.event_json=strdup("older-event")};
    q[tail++]=(sw_cmd_t){.type=SW_CMD_EVENT,.s=&state,.event_json=strdup("newer-event")};
    if(setjmp(complete)==0)sw_storage_task(NULL);
    assert(commits==1 && head==4 && tail==4 && !state.commit_queued);
    assert(q[3].type==SW_CMD_COMMIT);fclose(state.f_events);
    puts("production FIFO barrier: prior queued events become dirty and precede one checkpoint");
}
'''
if __name__=='__main__':
    run('barrier',code)
