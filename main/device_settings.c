/*
 * SomnoTrace - Device hardware settings (brightness, LCD therapy mode)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "device_settings.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "nvs_writer.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "dev_settings";

#define NVS_NAMESPACE "device"
#define NVS_KEY_BRIGHTNESS   "bright"
#define NVS_KEY_THE_SCREEN   "th_scr"
#define NVS_KEY_BACKLIGHT    "bk_mod"
#define NVS_KEY_ALERT_VOL    "alrtvol"
#define NVS_KEY_LCD_ROTATION "lcd_rot"
#define NVS_KEY_BAT_ENABLED  "bat_en"
#define NVS_KEY_WAKE_TOUCH   "wake_tch"
#define NVS_KEY_WAKE_SEC     "wake_sec"
#define NVS_KEY_SCREEN_TIMEOUT "scr_tmo"
#define NVS_KEY_LEGACY_LCD_THERAPY "lcd_thr"

/* Brightness stored in tenth-percent units: 1=0.1%, 200=20.0%
 * Discrete steps: 0.1, 0.2, 0.5, 1, 2, 5, 10, 20 (roughly 2x each) */
#if CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B || \
    (CONFIG_SOMNOTRACE_BOARD_QEMU && !CONFIG_SOMNOTRACE_QEMU_DISPLAY_154)
#define DEFAULT_BRIGHTNESS       200 /* 100% on the 7-inch target. */
#define DEFAULT_SCREEN_TIMEOUT_S 300
#else
#define DEFAULT_BRIGHTNESS       100 /* 10.0% */
#define DEFAULT_SCREEN_TIMEOUT_S 0
#endif
#define MIN_BRIGHTNESS           1   /* 0.1% */
#define MAX_BRIGHTNESS           200 /* 20.0% */
#define DEFAULT_THE_SCREEN       THERAPY_SCREEN_INFO
#define DEFAULT_BACKLIGHT_MODE   BACKLIGHT_MODE_ON
#define DEFAULT_ALERT_VOLUME     65
#define MIN_ALERT_VOLUME         50
#define DEFAULT_LCD_ROTATION     LCD_ROTATION_0
#define DEFAULT_BAT_ENABLED      true
#define DEFAULT_WAKE_ON_TOUCH    true
#define DEFAULT_WAKE_TIMEOUT_SEC 10

static device_settings_t s_settings;
static device_settings_t s_save_work;
static uint32_t s_settings_revision;
static StaticSemaphore_t s_settings_mutex_storage;
static StaticSemaphore_t s_save_mutex_storage;
static SemaphoreHandle_t s_settings_mutex;
static SemaphoreHandle_t s_save_mutex;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

/* The display is initialized before NVS is available, so touchscreen code can
 * read or change defaults before device_settings_load(). Static, lazily-created
 * mutexes keep that early path allocation-free and make web/touch updates safe
 * once both tasks are running. */
static void ensure_settings_mutexes(void)
{
    if (s_settings_mutex && s_save_mutex) return;

    portENTER_CRITICAL(&s_mutex_init_lock);
    if (!s_settings_mutex) {
        s_settings_mutex = xSemaphoreCreateRecursiveMutexStatic(
            &s_settings_mutex_storage);
    }
    if (!s_save_mutex) {
        s_save_mutex = xSemaphoreCreateMutexStatic(&s_save_mutex_storage);
    }
    portEXIT_CRITICAL(&s_mutex_init_lock);
}

static void settings_lock(void)
{
    ensure_settings_mutexes();
    xSemaphoreTakeRecursive(s_settings_mutex, portMAX_DELAY);
}

static void settings_unlock(void)
{
    xSemaphoreGiveRecursive(s_settings_mutex);
}

static void copy_current_settings(device_settings_t *out, uint32_t *revision)
{
    settings_lock();
    *out = s_settings;
    if (revision) *revision = s_settings_revision;
    settings_unlock();
}

static bool lcd_rotation_is_valid(int degrees)
{
    switch (degrees) {
        case LCD_ROTATION_0:
        case LCD_ROTATION_90:
        case LCD_ROTATION_180:
        case LCD_ROTATION_270:
            return true;
        default:
            return false;
    }
}

static bool screen_timeout_is_valid(int seconds)
{
    switch (seconds) {
        case 0:
        case 60:
        case 120:
        case 300:
        case 900:
        case 1800:
            return true;
        default:
            return false;
    }
}


esp_err_t device_settings_load(device_settings_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    ensure_settings_mutexes();

    memset(cfg, 0, sizeof(*cfg));
    cfg->brightness = DEFAULT_BRIGHTNESS;
    cfg->therapy_screen = DEFAULT_THE_SCREEN;
    cfg->backlight_mode = DEFAULT_BACKLIGHT_MODE;
    cfg->alert_volume = DEFAULT_ALERT_VOLUME;
    cfg->lcd_rotation = DEFAULT_LCD_ROTATION;
    cfg->screen_timeout_s = DEFAULT_SCREEN_TIMEOUT_S;
    cfg->battery_enabled = DEFAULT_BAT_ENABLED;
    cfg->wake_on_touch = DEFAULT_WAKE_ON_TOUCH;
    cfg->wake_timeout_sec = DEFAULT_WAKE_TIMEOUT_SEC;
    /* Clamp stale NVS values to current valid range */

    nvs_handle_t h;
    nvs_writer_lock();
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        nvs_writer_unlock();
        ESP_LOGI(TAG, "no device settings in NVS — using defaults");
        settings_lock();
        s_settings = *cfg;
        s_settings_revision++;
        settings_unlock();
        return ESP_ERR_NVS_NOT_FOUND;
    }

    uint8_t u8val;
    if (nvs_get_u8(h, NVS_KEY_BRIGHTNESS, &u8val) == ESP_OK) {
        cfg->brightness = (u8val < MIN_BRIGHTNESS) ? MIN_BRIGHTNESS :
                          (u8val > MAX_BRIGHTNESS) ? MAX_BRIGHTNESS : u8val;
    }
    esp_err_t therapy_screen_err = nvs_get_u8(h, NVS_KEY_THE_SCREEN, &u8val);
    if (therapy_screen_err == ESP_OK) {
        if (u8val <= THERAPY_SCREEN_STATUS) {
            cfg->therapy_screen = (therapy_screen_t)u8val;
        }
    }
    esp_err_t backlight_mode_err = nvs_get_u8(h, NVS_KEY_BACKLIGHT, &u8val);
    if (backlight_mode_err == ESP_OK) {
        if (u8val <= BACKLIGHT_MODE_ALWAYS_OFF) {
            cfg->backlight_mode = (backlight_mode_t)u8val;
        }
    }
    if (therapy_screen_err == ESP_ERR_NVS_NOT_FOUND ||
        backlight_mode_err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t legacy_mode;
        if (nvs_get_u8(h, NVS_KEY_LEGACY_LCD_THERAPY, &legacy_mode) == ESP_OK &&
            legacy_mode <= 3) {
            if (therapy_screen_err == ESP_ERR_NVS_NOT_FOUND) {
                cfg->therapy_screen = legacy_mode == 3 ?
                                      THERAPY_SCREEN_INFO : THERAPY_SCREEN_GRAPH;
            }
            if (backlight_mode_err == ESP_ERR_NVS_NOT_FOUND) {
                cfg->backlight_mode = legacy_mode == 1 ? BACKLIGHT_MODE_OFF_THRP :
                                      legacy_mode == 2 ? BACKLIGHT_MODE_ALWAYS_OFF :
                                                         BACKLIGHT_MODE_ON;
            }
        }
    }
    if (nvs_get_u8(h, NVS_KEY_ALERT_VOL, &u8val) == ESP_OK) {
        cfg->alert_volume = (u8val < MIN_ALERT_VOLUME) ? MIN_ALERT_VOLUME : u8val;
    }
    if (nvs_get_u8(h, NVS_KEY_BAT_ENABLED, &u8val) == ESP_OK) {
        cfg->battery_enabled = (u8val != 0);
    }
    if (nvs_get_u8(h, NVS_KEY_WAKE_TOUCH, &u8val) == ESP_OK) {
        cfg->wake_on_touch = (u8val != 0);
    }
    if (nvs_get_u8(h, NVS_KEY_WAKE_SEC, &u8val) == ESP_OK) {
        cfg->wake_timeout_sec = (u8val > 60) ? DEFAULT_WAKE_TIMEOUT_SEC : u8val;
    }
    uint16_t rotation;
    esp_err_t rotation_err = nvs_get_u16(h, NVS_KEY_LCD_ROTATION, &rotation);
    if (rotation_err == ESP_ERR_NVS_TYPE_MISMATCH) {
        /* Firmware through v1.2.2 stored 0°/90° as uint8_t. Keep those
         * settings readable until the next save migrates the key to uint16_t. */
        uint8_t legacy_rotation;
        rotation_err = nvs_get_u8(h, NVS_KEY_LCD_ROTATION, &legacy_rotation);
        if (rotation_err == ESP_OK) rotation = legacy_rotation;
    }
    if (rotation_err == ESP_OK) {
        cfg->lcd_rotation = lcd_rotation_is_valid(rotation) ?
                            rotation : DEFAULT_LCD_ROTATION;
    }
    uint16_t screen_timeout_s;
    if (nvs_get_u16(h, NVS_KEY_SCREEN_TIMEOUT, &screen_timeout_s) == ESP_OK) {
        cfg->screen_timeout_s = screen_timeout_is_valid(screen_timeout_s) ?
                                screen_timeout_s : DEFAULT_SCREEN_TIMEOUT_S;
    }

    nvs_close(h);
    nvs_writer_unlock();
    settings_lock();
    s_settings = *cfg;
    s_settings_revision++;
    settings_unlock();
    ESP_LOGI(TAG, "loaded: brightness=%u (%.1f%%), thr_screen=%u, bkl_mode=%u, alert_vol=%u, lcd_rot=%u, screen_tmo=%us, bat_en=%d, wake_tch=%d, wake_sec=%u",
             cfg->brightness, cfg->brightness / 10.0,
             cfg->therapy_screen, cfg->backlight_mode, cfg->alert_volume,
             (unsigned)cfg->lcd_rotation, (unsigned)cfg->screen_timeout_s,
             cfg->battery_enabled,
             cfg->wake_on_touch, cfg->wake_timeout_sec);
    return ESP_OK;
}

static esp_err_t do_device_settings_save(void *arg)
{
    /* arg points at s_save_work in internal DRAM. Copy it to this writer
     * task's internal stack before any flash operation disables caches. */
    const device_settings_t cfg = *(const device_settings_t *)arg;
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_u8(h, NVS_KEY_BRIGHTNESS, cfg.brightness);
    if (ret == ESP_OK)
        ret = nvs_set_u8(h, NVS_KEY_THE_SCREEN,
                         (uint8_t)cfg.therapy_screen);
    if (ret == ESP_OK)
        ret = nvs_set_u8(h, NVS_KEY_BACKLIGHT,
                         (uint8_t)cfg.backlight_mode);
    if (ret == ESP_OK)
        ret = nvs_set_u8(h, NVS_KEY_ALERT_VOL, cfg.alert_volume);
    if (ret == ESP_OK)
        ret = nvs_set_u16(h, NVS_KEY_LCD_ROTATION, cfg.lcd_rotation);
    if (ret == ESP_OK)
        ret = nvs_set_u16(h, NVS_KEY_SCREEN_TIMEOUT, cfg.screen_timeout_s);
    if (ret == ESP_OK)
        ret = nvs_set_u8(h, NVS_KEY_BAT_ENABLED,
                         cfg.battery_enabled ? 1 : 0);
    if (ret == ESP_OK)
        ret = nvs_set_u8(h, NVS_KEY_WAKE_TOUCH,
                         cfg.wake_on_touch ? 1 : 0);
    if (ret == ESP_OK)
        ret = nvs_set_u8(h, NVS_KEY_WAKE_SEC, cfg.wake_timeout_sec);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

/* s_save_mutex must be held. If a setter runs during the flash commit, repeat
 * with its newer revision so an older snapshot can never be the last durable
 * value. Setters only take s_settings_mutex and therefore never block the UI
 * for the duration of an NVS write. */
static esp_err_t persist_current_locked(device_settings_t *persisted)
{
    for (;;) {
        uint32_t revision;
        copy_current_settings(&s_save_work, &revision);
        esp_err_t ret = nvs_writer_run(do_device_settings_save, &s_save_work);
        if (ret != ESP_OK) return ret;

        settings_lock();
        bool stable = revision == s_settings_revision;
        if (stable && persisted) *persisted = s_settings;
        settings_unlock();
        if (stable) return ESP_OK;
    }
}

esp_err_t device_settings_save_current(void)
{
    ensure_settings_mutexes();
    xSemaphoreTake(s_save_mutex, portMAX_DELAY);
    device_settings_t persisted;
    esp_err_t ret = persist_current_locked(&persisted);
    xSemaphoreGive(s_save_mutex);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "saved: brightness=%u (%.1f%%), thr_screen=%u, bkl_mode=%u, alert_vol=%u, lcd_rot=%u, screen_tmo=%us, bat_en=%d, wake_tch=%d, wake_sec=%u",
                 persisted.brightness, persisted.brightness / 10.0,
                 persisted.therapy_screen, persisted.backlight_mode,
                 persisted.alert_volume, (unsigned)persisted.lcd_rotation,
                 (unsigned)persisted.screen_timeout_s,
                 persisted.battery_enabled, persisted.wake_on_touch,
                 persisted.wake_timeout_sec);
    }
    return ret;
}

esp_err_t device_settings_save(const device_settings_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!screen_timeout_is_valid(cfg->screen_timeout_s)) return ESP_ERR_INVALID_ARG;

    /* This is an explicit full-structure replacement. Interactive callers
     * that already used a field setter should call save_current() instead. */
    settings_lock();
    s_settings = *cfg;
    s_settings_revision++;
    settings_unlock();
    return device_settings_save_current();
}

void device_settings_snapshot(device_settings_t *out)
{
    if (!out) return;
    copy_current_settings(out, NULL);
}

const device_settings_t *device_settings_get(void)
{
    return &s_settings;
}

esp_err_t device_settings_set_brightness(uint8_t percent)
{
    if (percent < MIN_BRIGHTNESS) percent = MIN_BRIGHTNESS;
    if (percent > MAX_BRIGHTNESS) percent = MAX_BRIGHTNESS;

    settings_lock();
    s_settings.brightness = percent;
    s_settings_revision++;
    bsp_display_set_brightness(percent);
    settings_unlock();
    return ESP_OK;
}

esp_err_t device_settings_set_therapy_screen(therapy_screen_t screen)
{
    if (screen > THERAPY_SCREEN_STATUS) return ESP_ERR_INVALID_ARG;
    settings_lock();
    s_settings.therapy_screen = screen;
    s_settings_revision++;
    /* Re-evaluate display mode and backlight immediately */
    bsp_display_set_therapy_active(bsp_display_is_therapy_active());
    bsp_display_apply_backlight_policy(false);
    settings_unlock();
    return ESP_OK;
}

esp_err_t device_settings_set_backlight_mode(backlight_mode_t mode)
{
    if (mode > BACKLIGHT_MODE_ALWAYS_OFF) return ESP_ERR_INVALID_ARG;
    settings_lock();
    s_settings.backlight_mode = mode;
    s_settings_revision++;
    bsp_display_apply_backlight_policy(false);
    settings_unlock();
    return ESP_OK;
}

esp_err_t device_settings_set_alert_volume(uint8_t percent)
{
    if (percent < MIN_ALERT_VOLUME) percent = MIN_ALERT_VOLUME;
    if (percent > 100) percent = 100;
    settings_lock();
    s_settings.alert_volume = percent;
    s_settings_revision++;
    bsp_audio_set_volume(percent);
    settings_unlock();
    return ESP_OK;
}

esp_err_t device_settings_set_lcd_rotation(uint16_t degrees)
{
    if (!lcd_rotation_is_valid(degrees)) return ESP_ERR_INVALID_ARG;
    settings_lock();
    s_settings.lcd_rotation = degrees;
    s_settings_revision++;
    bsp_display_set_rotation(degrees);
    settings_unlock();
    return ESP_OK;
}

esp_err_t device_settings_set_screen_timeout_s(uint16_t seconds)
{
    if (!screen_timeout_is_valid(seconds)) return ESP_ERR_INVALID_ARG;
    settings_lock();
    s_settings.screen_timeout_s = seconds;
    s_settings_revision++;
    bsp_display_restart_idle_timeout();
    bsp_display_apply_backlight_policy(false);
    settings_unlock();
    return ESP_OK;
}

bool device_settings_battery_enabled(void)
{
    device_settings_t settings;
    device_settings_snapshot(&settings);
    return settings.battery_enabled;
}

esp_err_t device_settings_set_battery_enabled(bool enabled)
{
    settings_lock();
    s_settings.battery_enabled = enabled;
    s_settings_revision++;
    if (!enabled) {
        bsp_display_set_battery(-1, false, false);
    }
    settings_unlock();
    return ESP_OK;
}

esp_err_t device_settings_get_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    device_settings_t settings;
    device_settings_snapshot(&settings);
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "brightness", settings.brightness);
    cJSON_AddNumberToObject(root, "therapy_screen", (int)settings.therapy_screen);
    cJSON_AddNumberToObject(root, "backlight_mode", (int)settings.backlight_mode);
    cJSON_AddNumberToObject(root, "alert_volume", settings.alert_volume);
    cJSON_AddNumberToObject(root, "lcd_rotation", settings.lcd_rotation);
    cJSON_AddNumberToObject(root, "screen_timeout_s", settings.screen_timeout_s);
    cJSON_AddBoolToObject(root, "battery_enabled", settings.battery_enabled);
    cJSON_AddBoolToObject(root, "wake_on_touch", settings.wake_on_touch);
    cJSON_AddNumberToObject(root, "wake_timeout_sec", settings.wake_timeout_sec);

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t device_settings_save_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "failed to parse settings JSON");
        return ESP_ERR_INVALID_STATE;
    }

    settings_lock();
    device_settings_t cfg = s_settings;
    bool screen_timeout_present = false;

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "brightness")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        if (val < MIN_BRIGHTNESS) val = MIN_BRIGHTNESS;
        if (val > MAX_BRIGHTNESS) val = MAX_BRIGHTNESS;
        cfg.brightness = (uint8_t)val;
    }
    if ((v = cJSON_GetObjectItem(root, "therapy_screen")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        if (val >= 0 && val <= THERAPY_SCREEN_STATUS) {
            cfg.therapy_screen = (therapy_screen_t)val;
        }
    }
    if ((v = cJSON_GetObjectItem(root, "backlight_mode")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        if (val >= 0 && val <= BACKLIGHT_MODE_ALWAYS_OFF) {
            cfg.backlight_mode = (backlight_mode_t)val;
        }
    }
    if ((v = cJSON_GetObjectItem(root, "alert_volume")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        if (val < MIN_ALERT_VOLUME) val = MIN_ALERT_VOLUME;
        if (val > 100) val = 100;
        cfg.alert_volume = (uint8_t)val;
    }
    if ((v = cJSON_GetObjectItem(root, "lcd_rotation")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        cfg.lcd_rotation = lcd_rotation_is_valid(val) ?
                           (uint16_t)val : DEFAULT_LCD_ROTATION;
    }
    v = cJSON_GetObjectItem(root, "screen_timeout_s");
    if (v) {
        int val = v->valueint;
        if (!cJSON_IsNumber(v) || v->valuedouble != (double)val ||
            !screen_timeout_is_valid(val)) {
            settings_unlock();
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cfg.screen_timeout_s = (uint16_t)val;
        screen_timeout_present = true;
    }
    if ((v = cJSON_GetObjectItem(root, "battery_enabled")) && cJSON_IsBool(v)) {
        cfg.battery_enabled = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(root, "wake_on_touch")) && cJSON_IsBool(v)) {
        cfg.wake_on_touch = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(root, "wake_timeout_sec")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        if (val < 0) val = 0;
        if (val > 60) val = 60;
        cfg.wake_timeout_sec = (uint8_t)val;
    }

    s_settings = cfg;
    s_settings_revision++;
    /* Keep state and hardware application in one ordered section. A newer
     * touchscreen update blocks here briefly and then applies after this one. */
    bsp_display_set_brightness(cfg.brightness);
    bsp_audio_set_volume(cfg.alert_volume);
    bsp_display_set_rotation(cfg.lcd_rotation);
    if (screen_timeout_present) bsp_display_restart_idle_timeout();
    if (!cfg.battery_enabled) {
        bsp_display_set_battery(-1, false, false);
    }
    bsp_display_set_therapy_active(bsp_display_is_therapy_active());
    bsp_display_apply_backlight_policy(false);
    settings_unlock();
    cJSON_Delete(root);

    /* If a touchscreen setter races this flash commit, save_current() repeats
     * with the newer revision instead of leaving the stale web snapshot in NVS. */
    return device_settings_save_current();
}
