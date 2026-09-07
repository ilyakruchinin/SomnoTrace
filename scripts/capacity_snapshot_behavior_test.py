#!/usr/bin/env python3
"""Compile the public capacity API with its production implementation."""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'main/sd_storage.c').read_text()
name='sd_storage_get_cached_free'
m=re.search(r'^bool '+name+r'\([^;]*?\)\s*\{',source,re.M)
assert m
cursor=m.end();depth=1
while depth:
    depth += (source[cursor]=='{')-(source[cursor]=='}');cursor+=1
body=source[m.start():cursor]
fixture='''#include <assert.h>
#include <stddef.h>
#include "sd_storage.h"
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int s_capacity_lock;
static bool s_capacity_cache_valid;
static uint64_t s_cached_free_bytes, s_cached_total_bytes;
'''+body+'''
int main(void) {
    uint64_t free_bytes=9, total_bytes=8;
    assert(!sd_storage_get_cached_free(&free_bytes, &total_bytes));
    s_capacity_cache_valid=true;
    s_cached_free_bytes=0x123456789ULL; s_cached_total_bytes=0xabcdef987ULL;
    assert(sd_storage_get_cached_free(&free_bytes, &total_bytes));
    assert(free_bytes==0x123456789ULL && total_bytes==0xabcdef987ULL);
    assert(sd_storage_get_cached_free(NULL, NULL));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'scripts/test_include'),'-I',str(ROOT/'main'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
net=(ROOT/'main/net_provision.c').read_text()
a=net.index('static void status_cache_refresh_sd(');b=net.index('\n}',a)
assert 'sd_storage_get_cached_free(' in net[a:b]
assert '== ESP_OK' not in net[a:b], 'bool capacity success must not be inverted'
print('Production capacity public API and bool success/failure convention passed')
