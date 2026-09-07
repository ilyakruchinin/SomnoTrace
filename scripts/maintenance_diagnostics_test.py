#!/usr/bin/env python3
"""Run the production diagnostics getter against unavailable service fixtures."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'main/touch_maintenance.c').read_text()
match=re.search(r'void touch_maintenance_diagnostics\([^)]*\)\s*\{',source)
assert match
cursor=match.end();depth=1
while depth:
    if source[cursor]=='{':depth+=1
    elif source[cursor]=='}':depth-=1
    cursor+=1
function=source[match.start():cursor]
stubs=r'''
#include "touch_maintenance.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define AS11_STATUS_ERROR "error"
#define OX_STATUS_ERROR "error"
#define portENTER_CRITICAL(p) (void)(p)
#define portEXIT_CRITICAL(p) (void)(p)
static int s_lock,as_calls,ox_calls;
static bool s_airsense_ready,s_oxygen_ready;
#undef strlcpy
#define strlcpy test_strlcpy
static size_t strlcpy(char *out,const char *s,size_t n){size_t len=strlen(s);if(n)snprintf(out,n,"%s",s);return len;}
typedef struct {char version[32],date[16],time[16];} esp_app_desc_t;
static const esp_app_desc_t *esp_app_get_description(void){static const esp_app_desc_t a={"test-version","Sep 5 2026","12:00"};return &a;}
static struct {char board[16];} somnotrace_firmware_target={"waveshare-7b"};
static unsigned heap_caps_get_free_size(unsigned cap){return 100000+cap;}
static unsigned heap_caps_get_minimum_free_size(unsigned cap){return 50000+cap;}
static unsigned heap_caps_get_largest_free_block(unsigned cap){return 20000+cap;}
static unsigned uxTaskGetNumberOfTasks(void){return 7;}
static long long esp_timer_get_time(void){return 123000000;}
static bool sd_storage_is_ready(void){return false;}
static bool sd_storage_get_cached_free(uint64_t *a,uint64_t *b){*a=*b=0;return false;}
static bool bsp_display_is_therapy_active(void){return false;}
static bool sd_storage_recording_active(void){return false;}
typedef struct {bool up;int rssi;bool rssi_valid;char ssid[33],ip[16];} netprov_link_t;
static void netprov_get_link(netprov_link_t *out){memset(out,0,sizeof(*out));}
static const char *as11_ble_get_status(void){assert(s_airsense_ready);as_calls++;return "paired";}
static const char *as11_ble_get_error(void){assert(s_airsense_ready);return "test error";}
static const char *oximeter_get_status(void){assert(s_oxygen_ready);ox_calls++;return "unpaired";}
static const char *oximeter_get_error(void){assert(s_oxygen_ready);return "test error";}
typedef struct {char status[64];size_t backend_count;struct {bool error_valid;char label[32],error[72];} backends[4];} uploader_progress_snapshot_t;
static int uploader_get_progress_snapshot(uploader_progress_snapshot_t *out){(void)out;return ESP_ERR_INVALID_STATE;}
void controller_diagnostics_get_snapshot(controller_diagnostics_snapshot_t *out){memset(out,0,sizeof(*out));}
'''
test=r'''
int main(void){
    maintenance_diagnostics_t d;assert(s_lock==0);
    touch_maintenance_diagnostics(&d);
    assert(as_calls==0&&ox_calls==0);assert(d.tasks==7&&d.uptime_s==123);
    assert(d.free_internal==100003&&d.free_psram==100004);
    assert(!d.card_ready&&!d.sd_capacity_valid&&!d.recording&&!d.wifi);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    assert(strstr(d.airsense,"Simulation"));
#else
    assert(strstr(d.airsense,"not initialized"));assert(strstr(d.oxygen,"not initialized"));
    s_airsense_ready=true;touch_maintenance_diagnostics(&d);assert(as_calls==1&&ox_calls==0);
    s_oxygen_ready=true;touch_maintenance_diagnostics(&d);assert(as_calls==2&&ox_calls==1);
#endif
    puts("production diagnostics: startup, partial initialization, measured resources and simulation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='somno-diagnostics-') as path:
    c=Path(path)/'diagnostics.c';c.write_text(stubs+'\n'+function+'\n'+test)
    for mode in (0,1):
        binary=Path(path)/f'run-{mode}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',f'-DCONFIG_SOMNOTRACE_BOARD_QEMU={mode}','-I',str(ROOT/'scripts/test_include'),'-I',str(ROOT/'main'),str(c),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
