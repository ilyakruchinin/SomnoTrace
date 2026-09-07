#!/usr/bin/env python3
"""Exercise production controller publication and mailbox code with delayed IO."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'main/touch_history_controller.c').read_text()
def function(name, source=source):
    match = re.search(r'^(?:static )?[\w\s*]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'
types = source[source.index('typedef enum {'):source.index('static esp_err_t history_controller_enqueue_locked(')]
preamble = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include "touch_history_controller.h"
typedef int SemaphoreHandle_t, QueueHandle_t, BaseType_t;
typedef void *TaskHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define HISTORY_CONTROLLER_PROGRESS_STEP 20
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static int xSemaphoreTake(int mutex, int delay) { (void)mutex; (void)delay; return 1; }
static void xSemaphoreGive(int mutex) { (void)mutex; }
static void xTaskNotifyGive(TaskHandle_t worker) { (void)worker; }
static uint32_t epoch = 7;
static uint32_t sd_storage_content_generation(void) { return epoch; }
'''
stubs = r'''
static history_job_t mailbox;
static touch_history_controller_t *observed;
static int stats_calls, events_calls, stage, behavior;
static int xQueueOverwrite(int queue, const history_job_t *job) { (void)queue; mailbox = *job; return 1; }
static void history_controller_reconcile_calendar_locked(touch_history_controller_t *c, const history_job_t *j)
{ (void)c; (void)j; }
static esp_err_t history_controller_load_events(const history_job_t *job, history_model_t *model,
                                                const touch_history_operation_t *op)
{
    (void)job; (void)op;
    assert(stage == 1 && observed->model.has_overview && !observed->model.overview.preview);
    assert(!observed->model.stats.loaded && observed->model.state == TOUCH_HISTORY_UI_STATE_READY);
    assert(touch_history_controller_apply(observed, (void *)1) == ESP_OK);
    ++events_calls; stage = 2;
    model->event_count = model->event_total_count = 1; model->event_state = TOUCH_HISTORY_UI_EVENT_STATE_COMPLETE;
    return ESP_OK;
}
esp_err_t touch_history_load_night_ex(const char *day, touch_history_night_t *night,
    touch_history_session_t *sessions, size_t count, const touch_history_operation_t *operation)
{
    (void)day; (void)sessions; (void)count; (void)operation; assert(behavior==3);
    *night=observed->model.night;night->axis_start_ms=3900000;night->axis_end_ms=4500000;
    return ESP_OK;
}
esp_err_t touch_history_load_view_ex(const char *day, touch_history_signal_t signal,
    int64_t start, int64_t end, bool filter, touch_history_overview_t *out,
    const touch_history_operation_t *op)
{
    (void)day; (void)filter; (void)op;
    assert(stage == 0); stage = 1;
    if (behavior == 4) return ESP_ERR_TIMEOUT;
    memset(out,0,sizeof(*out)); out->loaded = out->has_data = true;
    out->axis_start_ms = start ? start : 3900000;
    out->axis_end_ms = end ? end : 4500000; out->signal = signal;
    return ESP_OK;
}
'''
selected = ['history_controller_notify', 'history_controller_text',
    'history_controller_generation_current', 'history_controller_should_cancel',
    'history_controller_progress', 'history_controller_operation',
    'history_controller_publish', 'history_controller_publish_error',
    'history_controller_first_signal', 'history_controller_signal_available',
    'history_controller_update_selection', 'history_controller_enqueue_locked',
    'touch_history_controller_set_active']
stats = r'''
esp_err_t touch_history_load_stats_ex(const char *day, touch_history_signal_t signal,
    int64_t start, int64_t end, bool filter, touch_history_stats_t *out,
    const touch_history_operation_t *op)
{
    (void)day; (void)signal; (void)filter;
    assert(stage == 2); stage = 3; ++stats_calls;
    assert(observed->model.overview.axis_start_ms == start);
    assert(observed->model.overview.axis_end_ms == end);
    assert(observed->model.event_count == 1);
    assert(touch_history_controller_apply(observed, (void *)1) == ESP_OK);
    assert(strstr(observed->model.stats_warning,"Calculating"));
    if (behavior == 1) {
        history_job_t next = mailbox;
        next.window_start_ms += 100000; next.window_end_ms += 100000;
        assert(history_controller_enqueue_locked(observed, &next, NULL) == ESP_OK);
        assert(op->should_cancel(op->context));
        return TOUCH_HISTORY_ERR_CANCELLED;
    }
    if (behavior == 2) return ESP_ERR_NO_MEM;
    observed->model.cursor_ms = start + 12345; observed->model.cursor_valid = true;
    out->loaded = out->exact = out->source_raw = true;
    out->start_ms = start; out->end_ms = end;
    out->value_count = 1; out->values[0].available = true; out->values[0].value_x100 = 17;
    return ESP_OK;
}
'''
main = r'''
static void reset(touch_history_controller_t *c)
{
    memset(c,0,sizeof(*c)); c->active = true; c->worker = (void *)1; c->generation = 3;
    c->model.selected_row = c->model.selected_global_index = SIZE_MAX;
    c->model.has_night = c->model.has_overview = true; c->model.source_generation = epoch;
    c->model.rail_mode = TOUCH_HISTORY_UI_RAIL_CALENDAR; c->model.has_month = true;
    c->model.month.year = 2026; c->model.month.month = 8;
    memcpy(c->model.night.day,"20260901",9);
    c->model.night.axis_start_ms = 1000000; c->model.night.axis_end_ms = 29800000;
    c->model.night.available_signals = TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_FLOW);
    c->preview_source_generation = epoch;
    touch_history_overview_t *p = &c->preview_source;
    p->loaded = p->has_data = true; p->point_count = 480;
    p->axis_start_ms = 1000000; p->axis_end_ms = 29800000;
    p->aggregation = TOUCH_HISTORY_AGGREGATION_ENVELOPE;
    for(size_t i=0;i<480;++i) {
        p->flags[i] = TOUCH_HISTORY_POINT_VALID | TOUCH_HISTORY_POINT_UPPER_VALID;
        p->value_x100[i] = -20; p->upper_x100[i] = 20;
    }
    p->value_x100[50] = -31999; p->upper_x100[50] = 32700; p->flags[55] = 0;
    c->model.overview = c->preview_source;
    c->model.event_state = TOUCH_HISTORY_UI_EVENT_STATE_COMPLETE;
    c->model.event_count = c->model.event_total_count = 1;
    stage = events_calls = stats_calls = 0;
}
int main(void)
{
    observed = calloc(1,sizeof(*observed)); history_model_t *result = calloc(1,sizeof(*result));
    assert(observed && result);
    for (behavior=0;behavior<5;++behavior) {
        reset(observed);
        /* Previous filtered/truncated detail must not poison the pending tuple. */
        if (behavior == 1) observed->model.event_count = 0;
        if (behavior == 2) { observed->model.event_total_count = 7; observed->model.events_truncated = true; }
        assert(touch_history_controller_apply(observed, (void *)1) == ESP_OK);
        if(behavior==3)observed->model.source_generation=epoch-1;
        history_job_t job = {.kind=HISTORY_JOB_VIEW,.signal=TOUCH_HISTORY_SIGNAL_FLOW,
            .window_start_ms=3000000,.window_end_ms=5000000,.non_cancellable=true};
        memcpy(job.day,"20260901",9);
        bool changed = false;
        assert(history_controller_enqueue_locked(observed,&job,&changed)==ESP_OK && changed);
        assert(observed->model.window_start_ms==job.window_start_ms);
        assert(observed->model.overview.preview && !observed->model.stats.loaded);
        bool peak=false, gap=false;
        for(size_t i=0;i<480;++i) {
            if(observed->model.overview.upper_x100[i]==32700) peak=true;
            if(!observed->model.overview.flags[i]) gap=true;
        }
        assert(touch_history_controller_apply(observed, (void *)1) == ESP_OK);
        assert(observed->model.event_total_count == 0 && !observed->model.events_truncated);
        history_controller_publish_error(observed, &job, ESP_ERR_TIMEOUT);
        assert(touch_history_controller_apply(observed, (void *)1) == ESP_OK);
        assert(observed->model.overview.preview);
        assert(peak && gap); /* immediate preview kept extrema and missing run */
        *result = observed->model;
        esp_err_t status = history_controller_load_view(observed,&job,result,false);
        if (behavior == 4) {
            assert(status == ESP_ERR_TIMEOUT && !events_calls && !stats_calls);
            history_controller_publish_error(observed, &job, status);
            assert(observed->model.overview.preview && observed->model.has_overview);
            assert(touch_history_controller_apply(observed, (void *)1) == ESP_OK);
            continue;
        }
        assert(events_calls==1 && stats_calls==1);
        if (behavior==1) {
            assert(status==TOUCH_HISTORY_ERR_CANCELLED);
            assert(!history_controller_publish(observed,&job,result));
            assert(observed->model.window_start_ms==job.window_start_ms+100000);
            assert(!observed->model.stats.loaded);
            history_job_t expected = mailbox;
            observed->ever_loaded = true;
            assert(touch_history_controller_set_active(observed,false)==ESP_OK);
            assert(touch_history_controller_set_active(observed,true)==ESP_OK);
            assert(mailbox.window_start_ms==expected.window_start_ms && mailbox.generation>expected.generation);
        } else {
            assert(status==ESP_OK && history_controller_publish(observed,&job,result));
            assert(observed->model.state==TOUCH_HISTORY_UI_STATE_READY);
            assert(touch_history_controller_apply(observed, (void *)1) == ESP_OK);
            assert(observed->model.rail_mode==TOUCH_HISTORY_UI_RAIL_CALENDAR);
            assert(observed->model.month.month==8);
            if(behavior==0) {
                assert(observed->model.stats.values[0].value_x100==17);
                assert(observed->model.stats.exact && observed->model.stats.source_raw);
                assert(observed->model.cursor_ms==job.window_start_ms+12345);
            } else if(behavior==2) assert(strstr(observed->model.stats_warning,"unavailable"));
            else {
                assert(observed->model.window_start_ms==3900000 && observed->model.window_end_ms==4500000);
                assert(observed->model.cursor_ms==3912345);
            }
            ++epoch;
            assert(!history_controller_publish(observed,&job,result));
            --epoch;
            history_controller_publish_error(observed,&job,ESP_ERR_TIMEOUT);
            assert(observed->model.has_overview && observed->model.state==TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN);
        }
    }
    free(result);free(observed);
    puts("Progressive History: actual UI validator accepts pending/error/graph/details snapshots; graph precedes events and raw stats; extrema/gaps, newest intent, cursor, source fences and allocation failure passed");
}
'''
ui_source = (root / 'main/touch_history_ui.c').read_text()
bridge = function('history_ui_validate_snapshot', ui_source) + r"""
esp_err_t touch_history_ui_apply(touch_history_ui_t *ui,
                                const touch_history_ui_snapshot_t *snapshot)
{ (void)ui; return history_ui_validate_snapshot(snapshot); }
"""
helpers = '\n'.join(function(x) for x in [
    'history_controller_can_advance_month', 'history_controller_stat_label',
    'history_controller_signal_unit', 'touch_history_controller_apply'])
with tempfile.TemporaryDirectory(prefix='somno-history-progressive-') as temp:
    path = Path(temp)
    (path/'lvgl.h').write_text('#pragma once\ntypedef struct lv_obj_t lv_obj_t;\n')
    fixture = preamble + types + stubs + '\n'.join(function(x) for x in selected) + stats + function('history_controller_load_view') + bridge + helpers + main
    (path/'test.c').write_text(fixture)
    args = ['cc','-std=c11','-Wall','-Wextra','-Werror','-DTOUCH_HISTORY_MODEL_TEST',
        '-I'+str(path),'-I'+str(root/'scripts/test_include'),'-I'+str(root/'main'),
        str(path/'test.c'),str(root/'main/touch_history.c'),'-lm','-o',str(path/'test')]
    subprocess.run(args,check=True)
    subprocess.run([str(path/'test')],check=True)
