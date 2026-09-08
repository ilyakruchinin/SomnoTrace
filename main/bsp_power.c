/*
 * SomnoTrace - Battery power latch and power button control
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */


#include "bsp_power.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "bsp_display.h"
#include "device_settings.h"
#include "as11_ble.h"
#include "esp_sleep.h"
#include "psram_task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_writer.h"
#include "therapy_alert.h"
#include "device_settings.h"

#define BSP_PIN_BAT_EN   2
#define BSP_PIN_KEY_PWR  5
#define BSP_PIN_BOOT     0
#define BSP_PIN_KEY_PLUS 4
#define BSP_PIN_BAT_ADC  1
#define BSP_PIN_CHG_STAT 3

static const char *TAG = "bsp_power";

void bsp_power_hold(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BSP_PIN_BAT_EN,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(BSP_PIN_BAT_EN, 1);
    ESP_LOGI(TAG, "battery power latched (BAT_EN=IO%d high)", BSP_PIN_BAT_EN);
}

void bsp_power_off(void)
{
    ESP_LOGW(TAG, "releasing battery latch (power off)");
    
    // Turn off screen backlight
    bsp_display_set_backlight(false);

    // Release battery enable latch
    gpio_set_level(BSP_PIN_BAT_EN, 0);

    // Wait until PWR button is released to prevent immediate wakeup if on USB
    while (gpio_get_level(BSP_PIN_KEY_PWR) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Wait 2 seconds for power to cut out (if on battery)
    vTaskDelay(pdMS_TO_TICKS(2000));

    // If still running, we are powered via USB-C. Enter deep sleep.
    ESP_LOGW(TAG, "Still powered. Entering deep sleep...");
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_5, 0); // wakeup on GPIO5 low
    esp_deep_sleep_start();
}

/* ── Consolidated button monitor task ────────────────────────────────
 * Polls PWR (IO5), BOOT (IO0), and PLUS (IO4) in a single task.
 * Replaces three separate tasks, saving ~5KB internal RAM stack + 2 TCBs.
 */

static struct {
    int  pwr_hold_ms;
    int  pwr_held_ms;
    volatile bool *softap_flag;
    int  boot_held_ms;
    int  plus_last_press_ms;
} s_btn;

static void button_monitor_task(void *arg)
{
    (void)arg;
    const int poll_ms = 50;
    const int double_click_window_ms = 400;
    const int pwr_short_min_ms = 100;
    const int pwr_short_max_ms = 1000;

    /* Configure all three button GPIOs at once */
    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BSP_PIN_KEY_PWR) |
                        (1ULL << BSP_PIN_BOOT)    |
                        (1ULL << BSP_PIN_KEY_PLUS),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* KEY_PWR may still be held from powering the board on.  Do not treat
     * that initial release as a short press; arm short-press handling only
     * after the monitor has observed the button released once. */
    bool pwr_armed = gpio_get_level(BSP_PIN_KEY_PWR) != 0;
    bool pwr_pressed = false;

    while (true) {
        /* --- PWR button: short press = optional backlight toggle,
         *                  5 s hold = power off --- */
        int pwr_level = gpio_get_level(BSP_PIN_KEY_PWR);
        if (!pwr_armed) {
            if (pwr_level != 0) pwr_armed = true;
            s_btn.pwr_held_ms = 0;
            pwr_pressed = false;
        } else if (pwr_level == 0) {
            pwr_pressed = true;
            s_btn.pwr_held_ms += poll_ms;
            if (s_btn.pwr_held_ms >= s_btn.pwr_hold_ms) {
                ESP_LOGW(TAG, "power button long-press: shutting down");
                const char *msg[] = { "Powering off..." };
                bsp_display_show_lines("SomnoTrace", msg, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                bsp_power_off();
                while (gpio_get_level(BSP_PIN_KEY_PWR) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(poll_ms));
                }
                s_btn.pwr_held_ms = 0;
            }
        } else {
            if (pwr_pressed && s_btn.pwr_held_ms >= pwr_short_min_ms &&
                s_btn.pwr_held_ms <= pwr_short_max_ms) {
                if (bsp_display_is_temporarily_awake()) {
                    bsp_display_cancel_temporary_wake();
                    ESP_LOGI(TAG, "POWER button: canceled temporary wake");
                } else {
                    bool on = bsp_display_toggle_backlight();
                    ESP_LOGI(TAG, "POWER button short-press: backlight %s",
                             on ? "on" : "off");
                }
            }
            s_btn.pwr_held_ms = 0;
            pwr_pressed = false;
        }

        /* --- BOOT button: 5 s hold = SoftAP entry --- */
        if (gpio_get_level(BSP_PIN_BOOT) == 0) {
            s_btn.boot_held_ms += poll_ms;
            if (s_btn.boot_held_ms >= 5000 && s_btn.softap_flag && !*s_btn.softap_flag) {
                ESP_LOGW(TAG, "BOOT long-press: flagging SoftAP entry");
                const char *msg[] = { "Entering Wi-Fi setup..." };
                bsp_display_show_lines("SomnoTrace", msg, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                *s_btn.softap_flag = true;
            }
        } else {
            s_btn.boot_held_ms = 0;
        }

        /* --- PLUS button: double-click = toggle therapy, single-click = alert ack --- */
        if (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
            int now_ms = (int)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            if (s_btn.plus_last_press_ms >= 0 &&
                (now_ms - s_btn.plus_last_press_ms) < double_click_window_ms) {
                ESP_LOGI(TAG, "PLUS button double-click");

                if (bsp_display_is_therapy_active()) {
                    ESP_LOGI(TAG, "stopping therapy via EnterStandby RPC");
                    esp_err_t ret = as11_ble_stop_therapy();
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "stop_therapy failed: %s",
                                 esp_err_to_name(ret));
                    }
                } else {
                    ESP_LOGI(TAG, "starting therapy via EnterTherapy RPC");
                    esp_err_t ret = as11_ble_start_therapy();
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "start_therapy failed: %s",
                                 esp_err_to_name(ret));
                    }
                }

                s_btn.plus_last_press_ms = -1;
            } else {
                /* First press: record timestamp and wait for release. */
                s_btn.plus_last_press_ms = now_ms;
            }

            /* Wait for button release. */
            while (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
                vTaskDelay(pdMS_TO_TICKS(poll_ms));
            }

            /* If this was a single press (not consumed by double-click),
             * poll for a second press during the double-click window.
             * If none arrives, fire single-click action (alert acknowledge). */
            if (s_btn.plus_last_press_ms >= 0) {
                int release_ms = (int)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                int deadline = release_ms + double_click_window_ms -
                               (release_ms - s_btn.plus_last_press_ms);
                while ((int)(xTaskGetTickCount() * portTICK_PERIOD_MS) < deadline) {
                    vTaskDelay(pdMS_TO_TICKS(poll_ms));
                    if (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
                        /* Second press arrived — handle as double-click */
                        ESP_LOGI(TAG, "PLUS button double-click");
                        if (bsp_display_is_therapy_active()) {
                            ESP_LOGI(TAG, "stopping therapy via EnterStandby RPC");
                            esp_err_t ret = as11_ble_stop_therapy();
                            if (ret != ESP_OK) {
                                ESP_LOGW(TAG, "stop_therapy failed: %s",
                                         esp_err_to_name(ret));
                            }
                        } else {
                            ESP_LOGI(TAG, "starting therapy via EnterTherapy RPC");
                            esp_err_t ret = as11_ble_start_therapy();
                            if (ret != ESP_OK) {
                                ESP_LOGW(TAG, "start_therapy failed: %s",
                                         esp_err_to_name(ret));
                            }
                        }
                        s_btn.plus_last_press_ms = -1;
                        /* Wait for release before resuming normal polling */
                        while (gpio_get_level(BSP_PIN_KEY_PLUS) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(poll_ms));
                        }
                        goto plus_done;
                    }
                }
                /* Window expired with no second press — resolve as single-click */
                if (s_btn.plus_last_press_ms >= 0) {
                    ESP_LOGI(TAG, "PLUS button single-click — alert acknowledge");
                    therapy_alert_acknowledge();
                    s_btn.plus_last_press_ms = -1;
                }
            }
        plus_done: ;
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

void bsp_power_start_button_monitor(int hold_ms)
{
    s_btn.pwr_hold_ms = hold_ms;
    s_btn.pwr_held_ms = 0;
    s_btn.boot_held_ms = 0;
    s_btn.plus_last_press_ms = -1;
    psram_task_create(button_monitor_task, "btn_mon", 3072, NULL, 5, tskNO_AFFINITY, NULL, NULL);
}

void bsp_power_start_boot_monitor(volatile bool *softap_flag, int hold_ms)
{
    (void)hold_ms;
    s_btn.softap_flag = softap_flag;
}

void bsp_power_start_plus_monitor(void)
{
    /* No-op: PLUS button is handled by the consolidated button_monitor_task */
}

/* ── Battery monitoring ───────────────────────────────────────────────
 * GPIO1 (BAT_ADC) has a 200k/100k voltage divider: VBAT = VADC × 3.
 * The ESP32-S3 ADC1 channel 0 maps to GPIO1.  GPIO3 (CHG_STAT) is low
 * while charging.
 *
 * A single monitor task owns the ADC and publishes a snapshot; no
 * consumer ever triggers a conversion.  Three things matter for a
 * usable reading:
 *
 *   1. Window width, not sample count.  Wi-Fi/BLE TX bursts dip the
 *      rail for a few hundred microseconds.  Samples taken back-to-back
 *      all land inside one such dip and average to the same wrong
 *      answer, so the burst is deliberately spread over ~1 s.  A
 *      trimmed mean then discards the dip tails outright rather than
 *      averaging them in.
 *
 *   2. Per-chip calibration.  ADC_ATTEN_DB_12 is not a clean 0-3.3 V
 *      range and varies between chips, so we use the eFuse-backed
 *      calibration scheme where available.  Assuming a nominal
 *      full-scale reads several percent low, which is what forced the
 *      old code to fudge its 100% endpoint down to 3930 mV.
 *
 *   3. Li-ion voltage is not linear in charge.  The curve is flat from
 *      ~3.9 V to ~3.6 V (most of the capacity) then falls off a cliff,
 *      so percent comes from an OCV table, not a straight line.
 *
 * Charging raises the terminal voltage above the cell's true OCV, so
 * plugging in genuinely steps the reading upward — no filter can remove
 * that.  It is handled at the presentation layer instead: an IR-drop
 * offset while charging, a monotonic-while-charging rule, and a slew
 * limit on the published percentage that turns any residual step into a
 * smooth walk. */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali = NULL;
static bool s_adc_ready = false;
static const adc_channel_t s_bat_adc_channel = ADC_CHANNEL_0;  /* GPIO1 = ADC1_CH0 */

#define BAT_DIVIDER_RATIO   3     /* 200k + 100k divider → ×3 */

/* Sample burst: 256 reads spread over ~1 s (4 ms apart). */
#define BAT_BURST_SAMPLES   256
#define BAT_BURST_GAP_MS    4
/* Discard the lowest and highest eighth before averaging. */
#define BAT_TRIM_FRACTION   8

/* Cadence: sample ADC burst every 10 s for smooth filtering and responsive tracking. */
#define BAT_SAMPLE_PERIOD_S     10   /* base sampling cadence (10s) */
#define BAT_UNPLUG_SETTLE_S     10   /* electrochemical relaxation window (10s) */
#define BAT_DEBOUNCE_SEC        2    /* 2 s debounce to confirm physical plug/unplug edge */

/* Above this voltage or burst spread the reading cannot be a real Li-ion cell.
 * A real chemical cell has massive capacitance, so voltage spread during a
 * 1-second burst is <15 mV. Without a battery (open ~10uF cap), the charger
 * oscillates and spreads the voltage by >40 mV. */
#define BAT_NO_BATTERY_MV         4250
#define BAT_NO_BATTERY_SPREAD_MV  35

/* Published snapshot, guarded by a mutex. */
static SemaphoreHandle_t s_bat_mutex = NULL;
static bsp_battery_t s_bat_state = {
    .percent = -1, .millivolts = -1, .charging = false, .valid = false, .age_s = 0,
};
static TickType_t s_bat_last_ok_tick = 0;

/* Warm-reboot persistence: RTC Fast SRAM survives software resets, watchdog
 * resets, and flashing over USB without power loss. This prevents the battery
 * gauge from jumping to a false 100% when rebooted on an active charger. */
#define BAT_RTC_MAGIC   0x534E5442   /* 'SNTB' */

typedef struct {
    uint32_t magic;
    int16_t  shown_pct;
    int16_t  filtered_mv;
    uint32_t crc;
} rtc_bat_backup_t;

static RTC_NOINIT_ATTR rtc_bat_backup_t s_rtc_bat_backup;

static inline uint32_t bat_calc_backup_crc(const rtc_bat_backup_t *b)
{
    return b->magic ^ ((uint32_t)(uint16_t)b->shown_pct << 16) ^ (uint32_t)(uint16_t)b->filtered_mv;
}

/* Dynamic rate-limiting intervals:
 * Charging (CC/CV model):
 *   0%..69% : 35 s per 1%  (fast CC phase: full ~500 mA charge rate)
 *  70%..84% : 60 s per 1%  (early CV transition: current begins tapering)
 *  85%..94% : 120 s per 1% (deep CV absorption: ~150-200 mA)
 *  95%..99% : 180 s per 1% (trickle saturation: <100 mA)
 * Discharging:
 *  20%..100%: 30 s per 1%  (standard discharge slew guardrail against load spikes)
 *   0%..19% : 15 s per 1%  (responsive low-battery knee tracking before PMU cutoff)
 */
static inline int bat_charge_step_delay_s(int pct)
{
    if (pct < 70) return 35;
    if (pct < 85) return 60;
    if (pct < 95) return 120;
    return 180;
}

static inline int bat_discharge_step_delay_s(int pct)
{
    if (pct < 20) return 15;
    return 30;
}

/* Li-ion open-circuit voltage → percent.  Descending by mV; linear
 * interpolation between rows.  The 100% anchor is adaptive: it learns the
 * cell's charge-termination voltage so the display reaches 100% when the
 * charger IC stops, rather than requiring an unrealistic 4200 mV. */
static const struct { int mv; int pct; } s_ocv_curve[] = {
    { 4160, 100 },
    { 4100,  90 },
    { 3950,  75 },
    { 3850,  60 },
    { 3750,  45 },
    { 3650,  30 },
    { 3550,  15 },
    { 3450,   5 },
    { 3300,   0 },
};

/* Adaptive 100% anchor: updated to the observed charge-termination voltage.
 * Defaults to the OCV curve's 4160 mV; replaced with the real value once the
 * charger IC stops for the first time.  Persisted in NVS so it survives reboots. */
#define BAT_NVS_NS         "bat"
#define BAT_NVS_KEY_FULL   "full_mv"
#define BAT_ANCHOR_MIN_MV  4000
#define BAT_ANCHOR_MAX_MV  4160
static int s_full_charge_mv = 4160;

/* NVS write callback — runs on the internal-stack nvs_writer task. */
static esp_err_t do_save_bat_anchor(void *arg)
{
    int mv = *(const int *)arg;
    nvs_handle_t h;
    if (nvs_open(BAT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t err = nvs_set_i32(h, BAT_NVS_KEY_FULL, mv);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Load the persisted anchor at boot.  Called before the monitor task starts
 * (internal stack at that point), so a direct nvs_open is safe. */
static void bat_load_anchor(void)
{
    nvs_handle_t h;
    nvs_writer_lock();
    if (nvs_open(BAT_NVS_NS, NVS_READONLY, &h) != ESP_OK) { nvs_writer_unlock(); return; }
    int32_t mv = 4200;
    if (nvs_get_i32(h, BAT_NVS_KEY_FULL, &mv) == ESP_OK &&
        mv >= BAT_ANCHOR_MIN_MV && mv <= BAT_ANCHOR_MAX_MV) {
        s_full_charge_mv = mv;
        ESP_LOGI(TAG, "battery: loaded full-charge anchor = %dmV from NVS", mv);
    }
    nvs_close(h);
    nvs_writer_unlock();
}

static int bat_mv_to_percent(int mv)
{
    const int n = sizeof(s_ocv_curve) / sizeof(s_ocv_curve[0]);
    if (mv >= s_full_charge_mv) return 100;
    if (mv <= s_ocv_curve[n - 1].mv) return 0;
    for (int i = 0; i < n - 1; i++) {
        int hi_mv = s_ocv_curve[i].mv, lo_mv = s_ocv_curve[i + 1].mv;
        int hi_pct = s_ocv_curve[i].pct, lo_pct = s_ocv_curve[i + 1].pct;
        /* Stretch the top segment so its upper bound is the adaptive 100%
         * anchor rather than the hardcoded 4200 mV. */
        if (i == 0) hi_mv = s_full_charge_mv;
        if (mv <= hi_mv && mv > lo_mv) {
            return lo_pct + (mv - lo_mv) * (hi_pct - lo_pct) / (hi_mv - lo_mv);
        }
    }
    return 0;
}

static esp_err_t bat_adc_init(void)
{
    if (s_adc_ready) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery: ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_bat_adc_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery: ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Per-chip calibration from eFuse.  Without it every unit reads a few
     * percent off, which is the "heaps of variations" problem. */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_bat_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        ESP_LOGI(TAG, "battery: using eFuse ADC calibration");
    } else {
        s_adc_cali = NULL;
        ESP_LOGW(TAG, "battery: no eFuse ADC calibration, falling back to "
                      "nominal scaling (readings may be a few %% off)");
    }

    s_adc_ready = true;
    return ESP_OK;
}

static int bat_raw_to_mv(int raw)
{
    if (s_adc_cali) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
            return mv * BAT_DIVIDER_RATIO;
        }
    }
    /* Uncalibrated fallback: nominal 0-3100 mV full scale for DB_12. */
    return (raw * 3100 * BAT_DIVIDER_RATIO) / 4095;
}

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

/* Compensate charging IR rise.
 * Below 3900 mV (CC phase), full charge current causes ~60 mV IR rise.
 * Between 3900 mV and 4160 mV (CV phase), current tapers down to near zero,
 * so the IR rise tapers to 0 mV at 4160 mV. */
static int bat_calc_ocv(int mv, bool charging)
{
    if (!charging) return mv;
    int ir_offset = 0;
    if (mv < 3900) {
        ir_offset = 60;
    } else if (mv < 4160) {
        ir_offset = (60 * (4160 - mv)) / 260;
    } else {
        ir_offset = 0;
    }
    int ocv = mv - ir_offset;
    return (ocv > 0) ? ocv : 0;
}

/* One burst: BAT_BURST_SAMPLES reads spread over the sampling window,
 * combined with a trimmed mean.  Returns VBAT in mV, or -1 on failure.
 * If out_spread_mv is non-NULL, writes the trimmed voltage spread in mV. */
static int bat_sample_burst(int *out_spread_mv)
{
    static int samples[BAT_BURST_SAMPLES];
    int n = 0;

    for (int i = 0; i < BAT_BURST_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_bat_adc_channel, &raw) == ESP_OK) {
            samples[n++] = raw;
        }
        vTaskDelay(pdMS_TO_TICKS(BAT_BURST_GAP_MS));
    }
    if (n == 0) return -1;

    qsort(samples, n, sizeof(int), cmp_int);

    /* Trimmed mean: drop the tails where the TX dips and spikes live. */
    int trim = n / BAT_TRIM_FRACTION;
    int lo = trim, hi = n - trim;
    if (hi <= lo) { lo = 0; hi = n; }

    int64_t sum = 0;
    for (int i = lo; i < hi; i++) sum += samples[i];
    int raw_avg = (int)(sum / (hi - lo));

    if (out_spread_mv) {
        int lo_mv = bat_raw_to_mv(samples[lo]);
        int hi_mv = bat_raw_to_mv(samples[hi - 1]);
        *out_spread_mv = (hi_mv > lo_mv) ? (hi_mv - lo_mv) : 0;
    }

    return bat_raw_to_mv(raw_avg);
}

static void bat_publish(int mv, int pct, bool charging, bool ok)
{
    if (!s_bat_mutex) return;
    xSemaphoreTake(s_bat_mutex, portMAX_DELAY);
    if (ok) {
        s_bat_state.millivolts = mv;
        s_bat_state.percent = pct;
        s_bat_state.valid = true;
        s_bat_last_ok_tick = xTaskGetTickCount();
    } else {
        s_bat_state.millivolts = mv;
        s_bat_state.percent = pct;
        s_bat_state.valid = false;
    }
    s_bat_state.charging = charging;
    xSemaphoreGive(s_bat_mutex);
}

static void battery_monitor_task(void *arg)
{
    (void)arg;

    if (bat_adc_init() != ESP_OK) {
        ESP_LOGE(TAG, "battery: monitor exiting, ADC unavailable");
        vTaskDelete(NULL);
        return;
    }

    /* Diagnostic: report stack high-water mark after init. */
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "battery: init done, stack high-water = %u bytes",
             (unsigned)(hwm * sizeof(StackType_t)));

    int  filtered_mv = -1;   /* IIR-filtered VBAT */
    int  shown_pct = -1;     /* slew-limited percentage actually published */
    bool debounced_charging = bsp_power_is_charging();
    bool settling = false;
    TickType_t settle_until = 0;
    TickType_t last_nvs_save = 0;
    TickType_t last_step_tick = 0;
    int  charging_duration_s = 0;
    bool is_calibrated = false;

    /* Check for reboot: restore shown_pct and filtered_mv from RTC Fast RAM.
     * RTC_NOINIT_ATTR survives software restarts, watchdog resets, and firmware
     * flashing over USB without power interruption. Magic and CRC protect against
     * cold-power uninitialized SRAM garbage. */
    esp_reset_reason_t rst_reason = esp_reset_reason();
    if (s_rtc_bat_backup.magic == BAT_RTC_MAGIC &&
        s_rtc_bat_backup.crc == bat_calc_backup_crc(&s_rtc_bat_backup) &&
        s_rtc_bat_backup.shown_pct >= 0 &&
        s_rtc_bat_backup.shown_pct <= 100) {
        shown_pct = s_rtc_bat_backup.shown_pct;
        if (s_rtc_bat_backup.filtered_mv > 0) {
            filtered_mv = s_rtc_bat_backup.filtered_mv;
        }
        is_calibrated = true;
        last_step_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "battery: restored shown_pct=%d%% filt=%dmV from RTC RAM across reboot (reason=%d)",
                 shown_pct, filtered_mv, (int)rst_reason);
    } else {
        memset(&s_rtc_bat_backup, 0, sizeof(s_rtc_bat_backup));
    }

    for (;;) {
        if (!device_settings_battery_enabled()) {
            bat_publish(-1, -1, debounced_charging, false);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int spread_mv = 0;
        int mv = bat_sample_burst(&spread_mv);

        if (mv <= 0) {
            ESP_LOGW(TAG, "battery: sample burst failed");
            bat_publish(0, 0, debounced_charging, false);
        } else {
            /* Valid battery reading */
            int ocv_mv = bat_calc_ocv(mv, debounced_charging);

            /* IIR low-pass across bursts.  Anything faster than this is
             * noise by definition at a 20-30 s cadence. */
            if (filtered_mv < 0) filtered_mv = ocv_mv;
            else filtered_mv += (ocv_mv - filtered_mv) / 4;

            int target_pct = bat_mv_to_percent(filtered_mv);

            /* Charge-termination clamp: if the charger has completed charging
             * (debounced_charging is false) and voltage sits in the full float
             * zone (>= 4140 mV), clamp target_pct to 100% so the gauge reliably
             * displays 100% after a charge cycle. */
            if (!debounced_charging && filtered_mv >= 4140) {
                target_pct = 100;
                shown_pct = 100;
                is_calibrated = true;
                last_step_tick = xTaskGetTickCount();
            }

            if (settling && xTaskGetTickCount() < settle_until) {
                /* Hold the displayed value (including uncalibrated "--%")
                 * while the cell relaxes after unplugging. */
            } else {
                settling = false;
                if (!is_calibrated) {
                    if (!debounced_charging) {
                        /* On battery without prior calibration (cold boot on battery,
                         * or after unplugging & settling): terminal voltage is genuine
                         * relaxed OCV. Snap directly to target_pct and engage guardrails. */
                        shown_pct = target_pct;
                        is_calibrated = true;
                        last_step_tick = xTaskGetTickCount();
                    } else {
                        /* On charger without prior calibration:
                         * If voltage is in the ambiguous CV float zone (>= 4100 mV),
                         * keep shown_pct as -1 (displays "--%") until either:
                         * 1) Charge completes (100%), or
                         * 2) Charger is unplugged (snaps to true relaxed OCV after settling).
                         * Below 4100 mV (e.g. 20-50% battery), terminal voltage is not
                         * float-clamped, so we can adopt target_pct directly. */
                        if (filtered_mv >= 4100) {
                            shown_pct = -1;
                        } else {
                            shown_pct = target_pct;
                            is_calibrated = true;
                            last_step_tick = xTaskGetTickCount();
                        }
                    }
                } else {
                    /* Direction lock and dynamic slew rate:
                     * While charging: strictly non-decreasing with staged CC/CV delay.
                     * While discharging: strictly non-increasing with load guardrail. */
                    TickType_t now = xTaskGetTickCount();
                    if (debounced_charging) {
                        if (target_pct > shown_pct) {
                            int delay_s = bat_charge_step_delay_s(shown_pct);
                            if (now - last_step_tick >= pdMS_TO_TICKS(delay_s * 1000)) {
                                shown_pct++;
                                last_step_tick = now;
                            }
                        }
                    } else {
                        if (target_pct < shown_pct) {
                            int delay_s = bat_discharge_step_delay_s(shown_pct);
                            if (now - last_step_tick >= pdMS_TO_TICKS(delay_s * 1000)) {
                                shown_pct--;
                                last_step_tick = now;
                            }
                        }
                    }
                }
            }

            bat_publish(filtered_mv, shown_pct, debounced_charging, true);

            /* Keep RTC Fast RAM snapshot up to date if calibrated */
            if (is_calibrated && shown_pct >= 0) {
                s_rtc_bat_backup.magic = BAT_RTC_MAGIC;
                s_rtc_bat_backup.shown_pct = (int16_t)shown_pct;
                s_rtc_bat_backup.filtered_mv = (int16_t)filtered_mv;
                s_rtc_bat_backup.crc = bat_calc_backup_crc(&s_rtc_bat_backup);
            }
            ESP_LOGD(TAG, "battery: vbat=%dmV filt=%dmV target=%d%% shown=%d%% chg=%d cal=%d spread=%dmV",
                     mv, filtered_mv, target_pct, shown_pct, debounced_charging, is_calibrated, spread_mv);
        }

        int period_s;
        if (settling && xTaskGetTickCount() < settle_until) {
            TickType_t rem = settle_until - xTaskGetTickCount();
            period_s = (int)((rem + pdMS_TO_TICKS(999)) / pdMS_TO_TICKS(1000));
            if (period_s < 1) period_s = 1;
        } else {
            period_s = BAT_SAMPLE_PERIOD_S;
        }
        int raw_same_count = 0;
        bool candidate_charging = debounced_charging;

        /* Sleep in 1 s slices while tracking and debouncing charger state transitions. */
        for (int s = 0; s < period_s; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (debounced_charging) charging_duration_s++;

            bool raw = bsp_power_is_charging();
            if (raw != candidate_charging) {
                candidate_charging = raw;
                raw_same_count = 1;
            } else {
                raw_same_count++;
            }

            /* Require BAT_DEBOUNCE_SEC consecutive seconds of new state before committing */
            if (candidate_charging != debounced_charging && raw_same_count >= BAT_DEBOUNCE_SEC) {
                debounced_charging = candidate_charging;
                ESP_LOGI(TAG, "battery: charger %s (debounced)",
                         debounced_charging ? "connected" : "disconnected");
                last_step_tick = xTaskGetTickCount();

                if (!debounced_charging) {
                    settling = true;
                    settle_until = xTaskGetTickCount() +
                                   pdMS_TO_TICKS(BAT_UNPLUG_SETTLE_S * 1000);

                    /* Only update 100% anchor if we had a sustained charge session (>5 min)
                     * and haven't written to NVS recently (rate-limited to 4h). */
                    TickType_t now = xTaskGetTickCount();
                    if (charging_duration_s >= 300 &&
                        (now - last_nvs_save > pdMS_TO_TICKS(4 * 3600 * 1000) || last_nvs_save == 0)) {
                        if (filtered_mv >= BAT_ANCHOR_MIN_MV && filtered_mv <= BAT_ANCHOR_MAX_MV) {
                            if (filtered_mv != s_full_charge_mv) {
                                s_full_charge_mv = filtered_mv;
                                ESP_LOGI(TAG, "battery: full-charge anchor = %dmV",
                                         s_full_charge_mv);
                                nvs_writer_run(do_save_bat_anchor, &s_full_charge_mv);
                                last_nvs_save = now;
                            }
                        }
                    }
                } else {
                    settling = false;
                }

                charging_duration_s = 0;
                /* Do NOT wipe shown_pct! Preserving shown_pct prevents sudden jumps on plug-in/unplug.
                 * Reset filtered_mv so the IIR filter starts fresh from the new OCV. */
                filtered_mv = -1;
                break;
            }
        }
    }
}

esp_err_t bsp_power_battery_monitor_start(void)
{
    if (s_bat_mutex) return ESP_OK;   /* already running */

    bat_load_anchor();
    nvs_writer_init();

    s_bat_mutex = xSemaphoreCreateMutex();
    if (!s_bat_mutex) return ESP_ERR_NO_MEM;

    TaskHandle_t h = psram_task_create(battery_monitor_task, "bat_mon", 8192,
                                       NULL, 3, tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        vSemaphoreDelete(s_bat_mutex);
        s_bat_mutex = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "battery monitor started (%ds sampling cadence, dynamic CC/CV slew rate)",
             BAT_SAMPLE_PERIOD_S);
    return ESP_OK;
}

void bsp_power_battery_get(bsp_battery_t *out)
{
    if (!out) return;
    if (!device_settings_battery_enabled() || !s_bat_mutex) {
        out->percent = -1;
        out->millivolts = -1;
        out->charging = false;
        out->valid = false;
        out->age_s = 0;
        return;
    }
    xSemaphoreTake(s_bat_mutex, portMAX_DELAY);
    *out = s_bat_state;
    if (s_bat_state.valid) {
        out->age_s = (uint32_t)((xTaskGetTickCount() - s_bat_last_ok_tick)
                                * portTICK_PERIOD_MS / 1000);
    }
    xSemaphoreGive(s_bat_mutex);
}

int bsp_power_battery_percent(void)
{
    bsp_battery_t b;
    bsp_power_battery_get(&b);
    return b.valid ? b.percent : -1;
}

bool bsp_power_is_charging(void)
{
    /* Configure CHG_STAT as input with pull-up if not already done.
     * The charger IC pulls CHG_STAT low when charging. */
    static bool chg_configured = false;
    if (!chg_configured) {
        gpio_config_t cfg = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1ULL << BSP_PIN_CHG_STAT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        chg_configured = true;
    }
    return (gpio_get_level(BSP_PIN_CHG_STAT) == 0);
}
