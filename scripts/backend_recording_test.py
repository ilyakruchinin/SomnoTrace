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
