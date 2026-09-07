#!/usr/bin/env python3
"""Fault injection into production board recovery and LVGL input/power functions."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
from prepare_host_lvgl import prepare_lvgl
LVGL_INPUT = prepare_lvgl()
def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r'^(?:static )?(?:void|bool|uint8_t|esp_err_t) ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'
common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "touch_observation.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_INVALID_RESPONSE 4
#define ESP_ERR_NO_MEM 5
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define CONTROLLER_TOUCH_READ 0
#define CONTROLLER_TOUCH_RECOVERY 1
#define CONTROLLER_OUTPUT_MODE 2
#define CONTROLLER_LCD_POWER 3
#define CONTROLLER_BACKLIGHT_POWER 4
#define CONTROLLER_BACKLIGHT_BRIGHTNESS 5
static void controller_diagnostics_record(int a, int b) {(void)a;(void)b;}
'''
board = common + r'''
#define BOARD_I2C_TIMEOUT_MS 20
#define BOARD_LOCK_TIMEOUT_MS 25
#define BOARD_VISIBILITY_RETRY_US 250000LL
#define BOARD_TOUCH_OFF_RESET_DELAY_US 50000LL
#define IOX_REG_MODE 2
#define IOX_REG_OUTPUT 3
#define IOX_REG_PWM 5
#define IOX_TOUCH_RST 1
#define IOX_BACKLIGHT 2
#define IOX_LCD_POWER 6
#define GT911_STATUS_REG 0x814e
#define GT911_ID_REG 0x8140
#define GT911_ADDR 0x5d
#define I2C_HZ 400000
#define tskNO_AFFINITY (-1)
#define GPIO_NUM_4 4
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct {uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
static void *s_lock=(void *)1,*s_iox=(void *)1,*s_touch_device=(void *)2;
static void *s_i2c=(void *)3,*s_touch,*s_touch_task;
typedef struct {unsigned device_address,scl_speed_hz;} i2c_device_config_t;
static unsigned task_creates, device_creates, device_removes;
static bool task_oom, running_task;
static jmp_buf task_exit;
static touch_observation_t published;
static void (*created_task)(void *);
static void publish_touch(const touch_observation_t *state) {published=*state;}
static int64_t board_now=1000000, task_deadline;
static int64_t esp_timer_get_time(void) {return board_now;}
static const char *esp_err_to_name(int error) {(void)error;return "injected";}
static int i2c_master_bus_add_device(void *bus,const i2c_device_config_t *cfg,void **out) {
    assert(bus==s_i2c && cfg->device_address==0x5d);++device_creates;*out=(void *)2;return ESP_OK;
}
static void i2c_master_bus_rm_device(void *device) {assert(device==s_touch_device);++device_removes;}
static void *psram_task_create(void (*fn)(void *),const char *name,unsigned stack,void *arg,int priority,int core,void *a,void *b) {
    (void)name;(void)arg;(void)a;(void)b;assert(stack==4096 && priority==5 && core==tskNO_AFFINITY);
    ++task_creates;created_task=fn;return task_oom?NULL:(void *)4;
}
static uint8_t s_output, s_attenuation;
static bool s_backlight_on_acked,s_backlight_off_acked;
static uint32_t s_backlight_off_generation,s_touch_visibility_generation;
typedef enum {TOUCH_VISIBILITY_IDLE,TOUCH_VISIBILITY_CHECK,TOUCH_VISIBILITY_ASSERT} touch_visibility_work_t;
static touch_visibility_work_t s_touch_visibility_work;
static int64_t s_touch_visibility_retry_at,s_touch_visibility_requested_us;
static uint32_t s_touch_off_reset_generation,s_touch_off_reset_requests;
static int64_t s_touch_off_reset_due_us;
static bool s_touch_off_reset_pending,s_touch_off_reset_active,s_touch_off_reset_cancelled;
static unsigned writes, fail_at, lock_fail, modes, last_mode, last_pullup, status_byte;
static uint8_t regs[32], values[32];
static int read_failure, write_failure;
static bool read_point;
static bool held;
static int xSemaphoreTake(void *lock, unsigned timeout) {
    assert(lock && timeout==25 && !held);
    if(lock_fail)return 0;
    held=true;return 1;
}
static void xSemaphoreGive(void *lock) {assert(lock && held);held=false;}
static int i2c_master_transmit(void *device,const uint8_t *data,size_t n,int timeout) {
    assert(timeout==20);
    if(device==s_touch_device) {assert(n==3 && data[0]==0x81 && data[1]==0x4e);return write_failure;}
    assert(device==s_iox && n==2 && writes<32);
    regs[writes]=data[0];values[writes]=data[1];++writes;
    return writes==fail_at ? ESP_ERR_TIMEOUT : ESP_OK;
}
static int i2c_master_transmit_receive(void *device,const uint8_t *address,size_t an,void *data,size_t n,int timeout) {
    assert(device==s_touch_device && an==2 && timeout==20);
    if (read_failure) return read_failure;
    unsigned reg=(address[0]<<8)|address[1];
    memset(data,0,n);
    if(reg==GT911_ID_REG) {assert(n==4);memcpy(data,"911",3);}
    else if(reg==GT911_STATUS_REG) {assert(n==1);*(uint8_t *)data=status_byte;}
    else {assert(reg==GT911_STATUS_REG+1 && n==8);read_point=true;uint8_t *p=data;p[1]=0x34;p[2]=1;p[3]=0x56;p[4]=2;}
    return ESP_OK;
}
static int gpio_config(const gpio_config_t *g) {++modes;last_mode=g->mode;last_pullup=g->pull_up_en;return ESP_OK;}
static int gpio_set_level(int pin,int value) {assert(pin==4 && value==0);return ESP_OK;}
static void vTaskDelay(unsigned time) {
    board_now+=(int64_t)time*1000;
    if(running_task && (time==10 || time==250)) {
        if(!task_deadline || board_now>=task_deadline) longjmp(task_exit,1);
        return;
    }
    assert(time==100 || time==200);
}
'''
for name in ['iox_write','iox_output','waveshare_7b_recovery_brightness','reassert_visible_locked','waveshare_7b_reassert_visible','touch_read_register','touch_read_frame','assert_touch_reset','recover_touch','service_touch_visibility','request_visible','queue_touch_wake_check','service_touch_off_reset','touch_task','waveshare_7b_start_touch']:
    board += function('main/board_waveshare_7b.c', name)
board += r'''
int main(void) {
    for(unsigned failure=0;failure<=3;++failure) {
        s_output=0x99;writes=0;fail_at=failure;
        waveshare_7b_recovery_brightness(33);
        int result=waveshare_7b_reassert_visible();
        assert((result!=0)==(failure!=0));assert(writes==3);
        assert(regs[0]==2 && values[0]==255 && regs[1]==5 && values[1]==170);
        assert(regs[2]==3 && values[2]==0xdd); /* USB, SD and reset preserved */
    }
    lock_fail=1;writes=0;assert(waveshare_7b_reassert_visible()==ESP_ERR_TIMEOUT && !writes);lock_fail=0;
    bool frame,pressed;uint16_t x,y;
    status_byte=0;assert(!touch_read_frame(&frame,&pressed,&x,&y) && !frame);
    status_byte=0x81;assert(!touch_read_frame(&frame,&pressed,&x,&y));
    assert(frame && pressed && x==0x134 && y==0x256 && read_point);
    status_byte=0x80;assert(!touch_read_frame(&frame,&pressed,&x,&y) && frame && !pressed);
    status_byte=0x86;assert(touch_read_frame(&frame,&pressed,&x,&y)==ESP_ERR_INVALID_SIZE && !frame);
    status_byte=0x81;write_failure=ESP_ERR_TIMEOUT;
    assert(touch_read_frame(&frame,&pressed,&x,&y)==ESP_ERR_TIMEOUT && !frame);
    write_failure=0;read_failure=ESP_ERR_TIMEOUT;
    assert(touch_read_frame(&frame,&pressed,&x,&y)==ESP_ERR_TIMEOUT && !frame);
    for (unsigned failure=0;failure<=2;++failure) {
        read_failure=0;fail_at=failure;writes=0;modes=0;
        assert((recover_touch(false,NULL)!=ESP_OK)==(failure!=0));
        assert(writes==2 && (s_output&2));assert(last_mode==GPIO_MODE_INPUT);
        assert(last_pullup==GPIO_PULLUP_DISABLE);
    }
    /* No legacy vendor object after boot NACK: one retained worker repairs
     * reset/INT and publishes fresh input. Repeated start cannot duplicate it. */
    assert(!s_touch);task_oom=true;
    assert(waveshare_7b_start_touch()==ESP_ERR_NO_MEM && device_removes==1 && !s_touch_device);
    task_oom=false;assert(waveshare_7b_start_touch()==ESP_OK);
    assert(task_creates==2 && device_creates==2);
    assert(waveshare_7b_start_touch()==ESP_OK && task_creates==2 && device_creates==2);
    fail_at=0;writes=0;modes=0;status_byte=0x81;running_task=true;
    if(!setjmp(task_exit)) created_task(NULL);
    assert(published.recovery_attempts==1 && published.valid && published.pressed);
    assert(published.x==0x134 && last_mode==GPIO_MODE_INPUT);
    /* A physical wake retry remains pending until the controller acknowledges
     * it, and a newer OFF generation cancels stale work. */
    memset(&published,0,sizeof(published));writes=0;fail_at=3;held=false;
    s_backlight_off_generation=0;s_touch_visibility_work=TOUCH_VISIBILITY_CHECK;
    s_touch_visibility_generation=0;s_touch_visibility_requested_us=board_now;
    s_touch_visibility_retry_at=0;s_backlight_on_acked=false;
    service_touch_visibility(&published);
    assert(writes==3 && s_touch_visibility_work==TOUCH_VISIBILITY_ASSERT);
    board_now+=250000;fail_at=0;service_touch_visibility(&published);
    assert(writes==6 && s_backlight_on_acked && published.visibility_requests==1);
    s_touch_visibility_work=TOUCH_VISIBILITY_CHECK;s_touch_visibility_generation=0;
    assert(iox_output(IOX_BACKLIGHT,false)==ESP_OK);
    service_touch_visibility(&published);
    assert(s_touch_visibility_work==TOUCH_VISIBILITY_IDLE);
    puts("production board recovery: bounded I2C, reset cleanup, sticky wake retry and OFF ordering passed");
}
'''
bsp = common + r'''
#define CONFIG_SOMNOTRACE_BOARD_QEMU 0
#define WAVESHARE_7B_H_RES 1024
#define WAVESHARE_7B_V_RES 600
#define TOUCH_FAILURE_THRESHOLD 3
#define BACKLIGHT_RETRY_US 250000
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_INDEV_STATE_PRESSED 1
#define LV_INDEV_STATE_RELEASED 0
typedef struct {int unused;} lv_indev_drv_t;
typedef struct {struct {int x,y;} point;int state;} lv_indev_data_t;
typedef struct {int x,y;} lv_point_t;
typedef struct {int unused;} lv_obj_t;
typedef struct {
    unsigned wait_until_release,pr_timestamp,longpr_rep_timestamp,long_pr_sent;
    struct {struct {lv_obj_t *act_obj,*last_obj,*scroll_obj;lv_point_t scroll_throw_vect,scroll_throw_vect_ori;} pointer;} types;
} _lv_indev_proc_t;
typedef struct {_lv_indev_proc_t proc;} lv_indev_t;
static lv_indev_t active_indev;
static lv_indev_t *indev_act=&active_indev;
static lv_obj_t *indev_obj_act;
static lv_obj_t button;
static unsigned clicks,press_lost;
#define LV_EVENT_PRESS_LOST 1
#define LV_EVENT_RELEASED 2
#define LV_EVENT_SHORT_CLICKED 3
#define LV_EVENT_CLICKED 4
#define LV_IMG_ZOOM_NONE 256
#define LV_LOG_INFO(...) ((void)0)
static lv_indev_t *lv_indev_get_act(void) {return indev_act;}
static void lv_event_send(lv_obj_t *object,int event,void *input) {
    (void)input;if(!object)return;
    if(event==LV_EVENT_CLICKED)++clicks;
    if(event==LV_EVENT_PRESS_LOST) {++press_lost;}
}
static bool indev_reset_check(_lv_indev_proc_t *proc) {(void)proc;return false;}
static int lv_obj_get_style_transform_angle(lv_obj_t *o,int p) {(void)o;(void)p;return 0;}
static int lv_obj_get_style_transform_zoom(lv_obj_t *o,int p) {(void)o;(void)p;return 256;}
static lv_obj_t *lv_obj_get_parent(lv_obj_t *o) {(void)o;return NULL;}
static void lv_point_transform(lv_point_t *v,int a,int z,lv_point_t *p) {(void)v;(void)a;(void)z;(void)p;}
static void _lv_indev_scroll_throw_handler(_lv_indev_proc_t *p) {(void)p;}
static bool s_backlight=true,s_backlight_known=true,s_backlight_requested=true;
static bool s_wake_gesture_pending,s_touch_was_pressed;
static uint32_t s_touch_seen_visibility,s_touch_seen_continuity,s_backlight_revision,s_touch_read_errors,s_backlight_write_errors;
static uint8_t s_touch_consecutive_errors,s_brightness=66;
static uint16_t s_last_touch_x,s_last_touch_y;
static int64_t s_last_touch_activity_us,s_backlight_retry_after_us,now;
static int s_wake_overlay=1,power_error,brightness_error;
static unsigned power_calls,brightness_calls;static bool hidden=true;
static touch_observation_t observation;
static int64_t esp_timer_get_time(void) {return now;}
static void waveshare_7b_touch_snapshot(touch_observation_t *out) {*out=observation;}
static void bsp_display_set_notice(const char *s) {(void)s;}
static void lv_obj_add_flag(int o,int f) {(void)o;(void)f;hidden=true;}
static void lv_obj_clear_flag(int o,int f) {(void)o;(void)f;hidden=false;}
static void lv_obj_move_foreground(int o) {(void)o;}
static int waveshare_7b_reassert_visible(void) {++power_calls;return power_error;}
static int waveshare_7b_set_backlight(bool on) {assert(!on);++power_calls;return power_error;}
static int waveshare_7b_set_brightness(unsigned p) {assert(p==33);++brightness_calls;return brightness_error;}
'''
bsp += function(LVGL_INPUT, 'lv_indev_wait_release')
bsp += function(LVGL_INPUT, 'indev_proc_release')
for name in ['physical_brightness','bsp_display_set_backlight','touch_read_cb','apply_pending_backlight_locked']:
    bsp += function('main/bsp_display_7b.c', name)
bsp += r'''
int main(void) {
    apply_pending_backlight_locked();assert(!power_calls);
    bsp_display_set_backlight(true);apply_pending_backlight_locked();
    assert(power_calls==1 && brightness_calls==1 && s_backlight_known);
    bsp_display_set_backlight(true);power_error=ESP_ERR_TIMEOUT;apply_pending_backlight_locked();
    assert(power_calls==2 && !s_backlight_known);
    now=249999;apply_pending_backlight_locked();assert(power_calls==2);
    now=250000;power_error=0;brightness_error=ESP_ERR_TIMEOUT;apply_pending_backlight_locked();
    assert(power_calls==3 && !s_backlight_known);
    now=500000;brightness_error=0;apply_pending_backlight_locked();assert(s_backlight_known);
    bsp_display_set_backlight(false);power_error=ESP_ERR_TIMEOUT;apply_pending_backlight_locked();
    assert(s_backlight && s_backlight_requested && !s_backlight_known);
    power_error=0;apply_pending_backlight_locked();
    bsp_display_set_backlight(false);apply_pending_backlight_locked();assert(!s_backlight && !hidden);
    now+=10000;touch_observation_update(&observation,now,0,true,true,300,200);++observation.visibility_requests;
    lv_indev_drv_t driver={0};lv_indev_data_t input={0};
    touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_RELEASED && s_backlight_requested);
    apply_pending_backlight_locked();assert(s_backlight && hidden);
    touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_RELEASED);
    now+=10000;touch_observation_update(&observation,now,0,true,false,0,0);
    touch_read_cb(&driver,&input);assert(!s_wake_gesture_pending);
    now+=10000;touch_observation_update(&observation,now,0,true,true,300,200);
    touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_PRESSED && input.point.x==300);
    now+=100001;touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_RELEASED);
    /* A complete fresh down frame after a 120 ms gap still loses continuity. */
    now += 19000;touch_observation_update(&observation,now,0,true,true,300,200);
    touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_RELEASED && s_wake_gesture_pending);
    now += 10000;touch_observation_update(&observation,now,0,true,false,0,0);
    touch_read_cb(&driver,&input);assert(!s_wake_gesture_pending);
    ++observation.visibility_requests;apply_pending_backlight_locked();assert(s_backlight_known && s_wake_gesture_pending);
    /* Production LVGL release processing must send PRESS_LOST, never CLICKED,
     * for a 101-150 ms discontinuity, read error, expired point or recovery. */
    for(unsigned fault=0;fault<4;++fault) {
        memset(&observation,0,sizeof(observation));s_touch_seen_continuity=0;
        s_touch_seen_visibility=0;s_wake_gesture_pending=false;s_touch_was_pressed=false;
        memset(&active_indev,0,sizeof(active_indev));clicks=0;press_lost=0;now=1000000;

        for(;now<=3940000;now+=10000) {
            touch_observation_update(&observation,now,0,true,true,300,200);
            touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_PRESSED);
            active_indev.proc.types.pointer.act_obj=&button;
        }
        now=4050000;
        if(fault==0)touch_observation_update(&observation,now,0,true,true,300,200);
        if(fault==1)touch_observation_update(&observation,now,ESP_ERR_TIMEOUT,false,false,0,0);
        if(fault==3)touch_observation_recovering(&observation);
        touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_RELEASED);
        indev_proc_release(&active_indev.proc);
        assert(press_lost==1 && clicks==0);
        /* Recovery requires a fresh release, then a new uninterrupted hold. */
        now+=10000;touch_observation_update(&observation,now,0,true,false,0,0);
        touch_read_cb(&driver,&input);assert(!s_wake_gesture_pending);
        now+=10000;
        for(unsigned elapsed=0;elapsed<=3000;elapsed+=10,now+=10000) {
            touch_observation_update(&observation,now,0,true,true,300,200);
            touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_PRESSED);
        }
        active_indev.proc.types.pointer.act_obj=&button;
        touch_observation_update(&observation,now,0,true,false,0,0);
        touch_read_cb(&driver,&input);indev_proc_release(&active_indev.proc);
        assert(clicks==1); /* an ordinary fresh release still works */
    }
    /* Healthy idle polling may return no new zero-contact frame. It must not
     * consume the next ordinary tap as a recovery gesture. */
    for(unsigned idle=0;idle<500;++idle) {
        now+=10000;touch_observation_update(&observation,now,0,false,false,0,0);
        touch_read_cb(&driver,&input);assert(!s_wake_gesture_pending);
    }
    now+=10000;touch_observation_update(&observation,now,0,true,true,300,200);
    touch_read_cb(&driver,&input);assert(input.state==LV_INDEV_STATE_PRESSED);
    puts("production LVGL wake: cached-on reassertion, retry, input freshness and gesture suppression passed");
}
'''
assert 'esp_lcd_touch_read_data' not in function('main/bsp_display_7b.c','touch_read_cb')
with tempfile.TemporaryDirectory(prefix='somno-touch-recovery-') as directory:
    for name, source in [('board',board),('bsp',bsp)]:
        path=Path(directory)/f'{name}.c';path.write_text(source)
        output=Path(directory)/name
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(path),str(ROOT/'main/touch_observation.c'),'-o',str(output)],check=True)
        subprocess.run([str(output)],check=True)
