#!/usr/bin/env python3
"""Exercise production upload configuration validation and NVS failure handling."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'components/uploader/uploader.c').read_text()
a = source.index('static esp_err_t do_uploader_save_config(void *arg)')
b = source.index('bool uploader_is_smb_configured(void)', a)
fixture = r'''#include <assert.h>
#include <string.h>
#include "uploader.h"
#include "upload_scan.h"
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_NAMESPACE "upload"
#define ESP_LOGI(...) ((void)0)
static uploader_config_t s_config;
static uploader_nvs_exec_fn_t s_nvs_exec;
static int calls, fail_at, closed;
static int next(void) { return ++calls == fail_at ? ESP_FAIL : ESP_OK; }
static int nvs_open(const char *a, int b, nvs_handle_t *h) { (void)a; (void)b; *h=1; return next(); }
static int nvs_set_u8(nvs_handle_t h,const char *k,unsigned v) { (void)h; (void)k; (void)v; return next(); }
static int nvs_set_i32(nvs_handle_t h,const char *k,int v) { (void)h; (void)k; (void)v; return next(); }
static int nvs_set_str(nvs_handle_t h,const char *k,const char *v) { (void)h; (void)k; (void)v; return next(); }
static int nvs_commit(nvs_handle_t h) { (void)h; return next(); }
static void nvs_close(nvs_handle_t h) { (void)h; ++closed; }
''' + source[a:b] + r'''
int main(void) {
    uploader_config_t cfg = { .max_days=30, .smb_enabled=true };
    strcpy(cfg.smb_host,"nas"); strcpy(cfg.smb_share,"recordings");
    assert(uploader_save_config(NULL)==ESP_ERR_INVALID_ARG && calls==0);
    cfg.max_days=0; assert(uploader_save_config(&cfg)==ESP_ERR_INVALID_ARG && calls==0); cfg.max_days=30;
    strcpy(cfg.smb_path,"../outside"); assert(uploader_save_config(&cfg)==ESP_ERR_INVALID_ARG && calls==0); cfg.smb_path[0]=0;
    memset(cfg.smb_host,'a',sizeof(cfg.smb_host)); assert(uploader_save_config(&cfg)==ESP_ERR_INVALID_ARG && calls==0); strcpy(cfg.smb_host,"nas");
    for (int failure=1; failure<=16; ++failure) {
        calls=closed=0; fail_at=failure; memset(&s_config,0,sizeof(s_config));
        assert(uploader_save_config(&cfg)==ESP_FAIL);
        assert(calls==failure && closed==(failure>1));
        assert(s_config.max_days==0);
    }
    calls=closed=fail_at=0;
    assert(uploader_save_config(&cfg)==ESP_OK && calls==16 && closed==1);
    assert(s_config.max_days==30 && strcmp(s_config.smb_host,"nas")==0);
}
'''
with tempfile.TemporaryDirectory(prefix="somno-uploader-config-") as directory:
    tmp=Path(directory); (tmp/'test.c').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'scripts/test_include'),'-I'+str(root/'components/uploader'),str(tmp/'test.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
print('Production uploader config: invalid input has no writes; all16 NVS failures preserve runtime config; success publishes after commit')
