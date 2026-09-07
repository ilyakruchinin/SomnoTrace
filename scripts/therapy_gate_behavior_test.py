#!/usr/bin/env python3
"""Exercise production original-screen restart gates across racing admissions."""
from pathlib import Path
import re, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/bsp_display.c').read_text()
def function(name):
    m = re.search(r'^(?:static\s+)?[\w *]+\b'+name+r'\([^;{]*\)\s*\{', source, re.M)
    assert m, name
    i, depth = m.end(), 1
    while depth:
        depth += (source[i] == '{') - (source[i] == '}'); i += 1
    return source[m.start():i]
names = ['try_reserve_therapy_safe_restart', 'try_commit_therapy_safe_restart',
         'cancel_therapy_safe_restart', 'reserve_therapy_start', 'release_therapy_start',
         'note_as11_notification_queued', 'note_as11_notification_processed',
         'try_begin_therapy_safe_maintenance', 'try_reserve_maintenance_commit',
         'therapy_safe_maintenance_should_abort', 'end_therapy_safe_maintenance']
fixture = '''#include <assert.h>
#include <stdbool.h>
#define portMAX_DELAY 0
#define DISP_MODE_STATUS 0
#define DISP_MODE_GRAPH 1
#define DISP_MODE_INFO 2
static int s_state_mutex = 1, s_mode;
static bool s_therapy_safe_restart_reserving, s_therapy_safe_restart_committed;
static unsigned s_therapy_start_waiters, s_therapy_start_claims, s_as11_notifications_pending;
static bool s_therapy_safe_maintenance;
static int locked;
static void xSemaphoreTake(int m, int t) { assert(m && !locked); locked = 1; }
static void xSemaphoreGive(int m) { assert(m && locked); locked = 0; }
static void vTaskDelay(int t) { assert(!"unexpected blocking gate"); }
'''+ '\n'.join(function('bsp_display_'+n) for n in names) + '''
int main(void) {
    assert(bsp_display_try_reserve_therapy_safe_restart());
    bsp_display_note_as11_notification_queued();
    assert(!bsp_display_try_commit_therapy_safe_restart());
    bsp_display_cancel_therapy_safe_restart();
    assert(!bsp_display_try_reserve_therapy_safe_restart());
    bsp_display_note_as11_notification_processed();
    assert(bsp_display_reserve_therapy_start());
    assert(!bsp_display_try_reserve_therapy_safe_restart());
    bsp_display_release_therapy_start();
    assert(bsp_display_try_begin_therapy_safe_maintenance());
    s_mode = DISP_MODE_INFO;
    assert(bsp_display_therapy_safe_maintenance_should_abort());
    assert(!bsp_display_try_reserve_maintenance_commit());
    s_mode = DISP_MODE_GRAPH;
    assert(!bsp_display_try_reserve_maintenance_commit());
    s_mode = DISP_MODE_STATUS;
    assert(bsp_display_try_reserve_maintenance_commit());
    assert(bsp_display_try_commit_therapy_safe_restart());
    assert(!bsp_display_reserve_therapy_start());
    assert(!locked);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp); (p/'test.c').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=5)
print('Production therapy restart gates: pending RX, start claims, graph/info modes and final commit passed')
