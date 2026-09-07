#!/usr/bin/env python3
"""Fault every allocation in the checked ESP-IDF async begin/complete functions."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sdk_patches", ROOT / "scripts/idf-sdk-patches.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
struct sock_db {bool for_async_req;};
struct resp_hdr {const char *field,*value;};
struct httpd_req_aux {
    void *scratch;size_t scratch_cur_size,scratch_size_limit,remaining_len;
    struct resp_hdr *resp_hdrs;struct sock_db *sd;
};
struct httpd_data {struct {size_t max_resp_headers;} config;};
typedef struct {void *handle,*aux;} httpd_req_t;
static unsigned calls,fail_at,retained;
static void *fault_malloc(size_t bytes) {
    if(++calls==fail_at)return NULL;
    void *p=malloc(bytes);assert(p);++retained;return p;
}
static void *fault_calloc(size_t n,size_t bytes) {
    void *p=fault_malloc(n*bytes);if(p)memset(p,0,n*bytes);return p;
}
static void fault_free(void *p) {if(p){assert(retained);--retained;}free(p);}
#define malloc fault_malloc
#define calloc fault_calloc
#define free fault_free
'''
cases = r'''
int main(void) {
    char scratch[128]="request headers";
    struct resp_hdr headers[8]={{"Content-Type","application/octet-stream"}};
    struct httpd_data server={.config.max_resp_headers=8};
    for(unsigned has_scratch=0;has_scratch<2;++has_scratch) {
        for(unsigned failure=0;failure<=3+has_scratch;++failure) {
            struct sock_db socket={0};
            struct httpd_req_aux aux={.scratch=has_scratch?scratch:NULL,
                .scratch_cur_size=128,.remaining_len=99,.resp_hdrs=headers,.sd=&socket};
            httpd_req_t request={.handle=&server,.aux=&aux},*async=NULL;
            calls=0;fail_at=failure;retained=0;
            int result=httpd_req_async_handler_begin(&request,&async);
            if(failure) {
                assert(result==ESP_ERR_NO_MEM && !async && !retained);
                assert(!socket.for_async_req && aux.remaining_len==99);
            } else {
                assert(result==ESP_OK && async && retained==3+has_scratch);
                assert(socket.for_async_req && !aux.remaining_len);
                struct httpd_req_aux *copy=async->aux;
                assert(copy!=&aux && copy->resp_hdrs!=headers);
                if(has_scratch)assert(copy->scratch!=scratch && !memcmp(copy->scratch,scratch,128));
                assert(httpd_req_async_handler_complete(async)==ESP_OK);
                assert(!retained && !socket.for_async_req);
            }
        }
    }
    puts("checked SDK async request: every allocation failure unwinds with no retained bytes or ownership");
}
'''
with tempfile.TemporaryDirectory(prefix="somno-sdk-async-") as temp:
    directory = Path(temp)
    source = directory / "httpd_txrx.c"
    source.write_text(patch.REFERENCE.read_text())
    assert patch.apply(source)
    assert not patch.apply(source), "patch must be idempotent"
    patched = patch.functions(source.read_text())
    source.write_text(fixture + patched + cases)
    binary = directory / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    source.write_text(patch.REFERENCE.read_text().replace("r_aux->remaining_len = 0;", "r_aux->remaining_len = 1;"))
    try:
        patch.apply(source)
    except RuntimeError:
        pass
    else:
        raise AssertionError("unknown SDK implementation was accepted")
