#!/usr/bin/env python3
"""Production newest-day selection and pressure encoder reconciliation checks."""
from pathlib import Path
import datetime, subprocess, tempfile
from session_storage_behavior_test import COMMON, function
ROOT=Path(__file__).resolve().parents[1]
ox=(ROOT/'components/uploader/upload_ox.c').read_text()
edf=(ROOT/'main/edf_summary.c').read_text()
days=[(datetime.date(2026,1,1)+datetime.timedelta(days=i)).strftime('%Y%m%d') for i in range(500)]
order=[days[(i*137)%500] for i in range(500)]
code=COMMON+r'''
#include <dirent.h>
#include "upload_ox.h"
#define MALLOC_CAP_SPIRAM 1
#define OX_RECORDINGS_DIR "recordings"
static int root_reads, child_reads, close_count, allocations, fail_alloc, fail_child;
static int cancel_root_at, cancel_child_at;
static bool cancelled;
static const char *names[] = {'''+','.join('"'+d+'"' for d in order)+r'''};
static DIR *test_open(const char *path) { assert(!strcmp(path,OX_RECORDINGS_DIR)); return (DIR*)1; }
static struct dirent *test_read(DIR *d) {
    static struct dirent entry;
    assert(d==(DIR*)1);
    if(root_reads==500) return NULL;
    strcpy(entry.d_name,names[root_reads++]);
    return &entry;
}
static int test_close(DIR *d) { assert(d==(DIR*)1); ++close_count; return 0; }
static void *heap_caps_calloc(size_t n,size_t size,int caps) {
    if(fail_alloc) return NULL;
    void *p=calloc(n,size); assert(p); ++allocations; return p;
}
static void test_free(void *p) { assert(p && allocations==1); --allocations; free(p); }
static bool uploader_should_cancel(void) {
    return cancelled || (cancel_root_at>=0 && root_reads>=cancel_root_at);
}
esp_err_t upload_ox_init(void) { return ESP_OK; }
static int scan_day(const char *day,upload_ox_ref_t *out,int max_out) {
    assert(close_count==1 && max_out>0);
    ++child_reads;
    if(fail_child==child_reads) return -1;
    if(cancel_child_at==child_reads) cancelled=true;
    strcpy(out->day,day); return 1;
}
#define opendir test_open
#define readdir test_read
#define closedir test_close
#define free test_free
'''+function(ox,'valid_day')+function(ox,'upload_ox_scan')+r'''
#undef free
static void reset(void) {
    assert(!allocations);
    root_reads=child_reads=close_count=fail_alloc=fail_child=cancel_child_at=0;
    cancel_root_at=-1; cancelled=false;
}
'''+function(edf,'settings_x50').replace('static inline','static')+r'''
int main(void) {
    upload_ox_ref_t *out=calloc(64,sizeof(*out)); assert(out);
    reset(); assert(upload_ox_scan(out,64)==64 && close_count==1 && !allocations);
'''+''.join(f'    assert(!strcmp(out[{i}].day,"{d}"));\n' for i,d in enumerate(reversed(days[-64:])))+r'''
    reset(); cancel_root_at=13;
    assert(upload_ox_scan(out,64)==-1 && root_reads==13 && !child_reads && close_count==1 && !allocations);
    reset(); fail_child=3;
    assert(upload_ox_scan(out,64)==-1 && child_reads==3 && close_count==1 && !allocations);
    reset(); cancel_child_at=2;
    assert(upload_ox_scan(out,64)==-1 && child_reads==2 && close_count==1 && !allocations);
    reset(); fail_alloc=1;
    assert(upload_ox_scan(out,64)==0 && close_count==1 && !allocations);
    assert(settings_x50(4.6)==230);
    assert(settings_x50(4.2)==210 && settings_x50(0.6)==30);
    free(out);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-fsanitize=address,undefined','-I',str(ROOT/'scripts/test_include'),'-I',str(ROOT/'components/uploader'),str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=5)
print('Production O2 newest 64/500 unsorted days, root/child cancellation, negative scan and OOM cleanup; STR encoder rounding passed')
