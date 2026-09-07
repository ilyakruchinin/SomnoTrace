#!/usr/bin/env python3
"""Count production pyramid stdio calls alongside the raw-sample differential oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='somno-history-flow-io-') as temp:
    path = Path(temp)
    (path/'io.h').write_text('''#include <stdio.h>
int history_test_fseek(FILE *, long, int);
size_t history_test_fread(void *, size_t, size_t, FILE *);
#define fseek history_test_fseek
#define fread history_test_fread
''')
    common = ['cc','-std=c11','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L','-D_DARWIN_C_SOURCE',
              '-I'+str(root/'main')]
    subprocess.run(common + ['-include',str(path/'io.h'),'-c',
        str(root/'main/history_flow_cache.c'),'-o',str(path/'flow.o')], check=True)
    subprocess.run(common + ['-DHISTORY_FLOW_IO_TEST',str(root/'scripts/history_flow_cache_test.c'),
        str(path/'flow.o'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
