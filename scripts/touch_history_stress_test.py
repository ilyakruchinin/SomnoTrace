#!/usr/bin/env python3
"""Sanitized production History publication/worker stress; no LVGL/SD timing claim."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
progressive = root/'scripts/history_progressive_test.py'
ns = {'__file__': str(progressive)}
exec(compile(progressive.read_text().split('with tempfile.TemporaryDirectory(',1)[0],str(progressive),'exec'),ns)
function = ns['function']
stubs = 'static bool queue_has_job;\n' + ns['stubs'].replace(
    'mailbox = *job; return 1;', 'mailbox = *job; queue_has_job = true; return 1;')
scheduler = r'''
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static bool fail_worker_alloc;
static int allocated, freed, retired, notifications;
static void changed(void *context) { assert(context==observed); ++notifications; }
static int xQueueReceive(int queue, history_job_t *job, int delay)
{ (void)queue; (void)delay; if(!queue_has_job)return 0;*job=mailbox;queue_has_job=false;return 1; }
static int ulTaskNotifyTake(int clear,int delay)
{
    (void)clear;(void)delay;
    if(!queue_has_job){mailbox=(history_job_t){.kind=HISTORY_JOB_STOP};queue_has_job=true;}
    return 1;
}
static void *heap_caps_malloc(size_t size,int caps)
{ (void)caps;if(fail_worker_alloc)return NULL;++allocated;return malloc(size); }
static void heap_caps_free(void *p) { assert(p);++freed;free(p); }
static void psram_task_delete(TaskHandle_t task) { assert(!task);++retired; }
esp_err_t touch_history_prepare_day_ex(const char *day,const touch_history_operation_t *operation)
{ (void)day;(void)operation;return ESP_OK; }
'''
process = r'''
static esp_err_t history_controller_process(touch_history_controller_t *controller,
    const history_job_t *job, history_model_t *result)
{
    assert(job->kind==HISTORY_JOB_VIEW);
    assert(history_controller_copy_model(controller,result));
    return history_controller_load_view(controller,job,result,false);
}
'''
main = r'''
int main(void) {
    observed=calloc(1,sizeof(*observed));assert(observed);
    for(unsigned round=0;round<600;++round) {
        reset(observed);observed->ever_loaded=true;
        observed->config.changed=changed;observed->config.context=observed;
        history_job_t old={0},job={.kind=HISTORY_JOB_VIEW,.signal=TOUCH_HISTORY_SIGNAL_FLOW};
        memcpy(job.day,"20260901",9);
        history_model_t *late=malloc(sizeof(*late));assert(late);
        /* Coalesce fast zoom/pan bursts before a delayed worker completes. */
        for(unsigned i=0;i<8+(round%8);++i) {
            job.window_start_ms=2000000+(round%20)*10000+i*17000;
            job.window_end_ms=job.window_start_ms+600000+(i%4)*300000;
            assert(history_controller_enqueue_locked(observed,&job,NULL)==ESP_OK);
            if(!i){old=job;*late=observed->model;}
            assert(touch_history_controller_apply(observed,(void*)1)==ESP_OK);
        }
        uint32_t revision=observed->revision;
        assert(!history_controller_publish(observed,&old,late));
        history_controller_publish_error(observed,&old,ESP_ERR_NO_MEM);
        assert(observed->revision==revision);free(late);
        /* Actual worker allocation-failure and normal/error result retirement.
           Graph/details paths use the production controller and UI validator. */
        behavior=round%3==0?4:round%3==1?2:0;
        fail_worker_alloc=round%7==0;
        history_controller_worker(observed);
        assert(!observed->worker && allocated==freed);
        assert(touch_history_controller_apply(observed,(void*)1)==ESP_OK);
        /* The result has been freed by the real worker: apply must read only
           the controller's copied data, including the full bounded event set. */
        observed->model.event_count=TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS;
        observed->model.event_total_count=TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS+23;
        observed->model.event_state=TOUCH_HISTORY_UI_EVENT_STATE_INCOMPLETE;
        observed->model.events_truncated=true;
        assert(touch_history_controller_apply(observed,(void*)1)==ESP_OK);
        ++observed->model.event_count;
        assert(touch_history_controller_apply(observed,(void*)1)==ESP_ERR_INVALID_ARG);
        observed->model.event_count=0;observed->model.event_total_count=0;
        observed->model.events_truncated=false;
        observed->model.event_state=TOUCH_HISTORY_UI_EVENT_STATE_UNAVAILABLE;
        history_operation_context_t context;
        touch_history_operation_t op=history_controller_operation(&context,observed,job.generation,0,0,false);
        assert(!op.should_cancel(op.context));
        ++epoch;assert(op.should_cancel(op.context));--epoch;
        observed->worker=(void*)1;
        assert(touch_history_controller_set_active(observed,false)==ESP_OK);
        assert(op.should_cancel(op.context));
        revision=observed->revision;
        history_controller_publish_error(observed,&job,ESP_ERR_NO_MEM);
        assert(observed->revision==revision);
        bool resume=observed->model.details_pending;
        assert(touch_history_controller_set_active(observed,true)==ESP_OK);
        assert(observed->generation>job.generation);
        if(resume)assert(mailbox.generation>job.generation);
        assert(touch_history_controller_apply(observed,(void*)1)==ESP_OK);
        observed->closing=true;revision=observed->revision;
        assert(history_controller_enqueue_locked(observed,&job,NULL)==ESP_ERR_INVALID_STATE);
        history_controller_publish_error(observed,&job,ESP_ERR_TIMEOUT);
        assert(observed->revision==revision && op.should_cancel(op.context));
    }
    assert(retired==600 && notifications>600 && allocated==freed);
    free(observed);
    puts("History stress: 600 navigation bursts, real worker OOM/error/result retirement, stale publication, bounds, deactivate/resume/closing and source cancellation passed under ASan/UBSan");
}
'''
with tempfile.TemporaryDirectory(prefix='somno-history-stress-') as temp:
    path=Path(temp)
    (path/'lvgl.h').write_text('#pragma once\ntypedef struct lv_obj_t lv_obj_t;\n')
    extra = ['history_controller_copy_model','history_controller_job_current',
             'history_controller_begin_job','history_controller_take_job','history_controller_finish_job']
    fixture=ns['preamble']+ns['types']+stubs+scheduler
    fixture+='\n'.join(function(n) for n in ns['selected'])+ns['stats']+function('history_controller_load_view')
    fixture+=ns['bridge']+ns['helpers']+'\n'.join(function(n) for n in extra)
    fixture+=process+function('history_controller_worker')+ns['main'].split('int main(void)',1)[0]+main
    (path/'test.c').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-g',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-DTOUCH_HISTORY_MODEL_TEST',
        '-I'+str(path),'-I'+str(root/'main'),'-I'+str(root/'scripts/test_include'),
        str(path/'test.c'),str(root/'main/touch_history.c'),'-lm','-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
