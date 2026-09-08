/*
 * SomnoTrace - Touch controller (CST816) driver for tap-to-wake events
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
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

#include "bsp_touch.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "device_settings.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_touch";

/* ── Hardware Pins (Waveshare ESP32-S3-Touch-LCD-1.54) ────────────── */
#define BSP_PIN_TP_RST      47
#define BSP_PIN_TP_INT      48

/* ── I2C Address ──────────────────────────────────────────────────── */
#define CST816_ADDR         0x15

#define NOTIF_TOUCH   (1 << 0)

static bool s_has_touch = false;
static TaskHandle_t s_touch_task = NULL;
static i2c_master_dev_handle_t s_cst816_dev = NULL;

static void IRAM_ATTR touch_gpio_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t hp_woken = pdFALSE;
    if (s_touch_task) {
        xTaskNotifyFromISR(s_touch_task, NOTIF_TOUCH, eSetBits, &hp_woken);
    }
    if (hp_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void cst816_reset_pulse(void)
{
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << BSP_PIN_TP_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);

    gpio_set_level(BSP_PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BSP_PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void touch_worker_task(void *arg)
{
    (void)arg;
    uint32_t notif = 0;
    int64_t last_wake_ms = 0;

    ESP_LOGI(TAG, "touch worker task running");

    while (1) {
        if (xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY) == pdTRUE) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            const device_settings_t *dev = device_settings_get();

            /* Acknowledge CST816 by reading touch coordinates */
            uint8_t reg = 0x00;
            uint8_t data[7] = {0};
            if (s_cst816_dev) {
                i2c_master_transmit_receive(s_cst816_dev, &reg, 1, data, sizeof(data), 50);
            }

            if (dev->wake_on_touch && dev->wake_timeout_sec > 0) {
                if (now_ms - last_wake_ms >= 500) {
                    last_wake_ms = now_ms;
                    ESP_LOGD(TAG, "touch tap wake event");
                    bsp_display_wake_temporary(dev->wake_timeout_sec);
                }
            }
        }
    }
}

esp_err_t bsp_touch_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_bus_handle();
    if (!bus) {
        ESP_LOGE(TAG, "cannot initialize touch: I2C bus unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_err));
        return isr_err;
    }

    cst816_reset_pulse();
    esp_err_t probe_tp = i2c_master_probe(bus, CST816_ADDR, 100);
    if (probe_tp == ESP_OK) {
        i2c_device_config_t tp_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CST816_ADDR,
            .scl_speed_hz = BSP_I2C_FREQ_HZ,
        };
        if (i2c_master_bus_add_device(bus, &tp_cfg, &s_cst816_dev) == ESP_OK) {
            gpio_config_t int_cfg = {
                .pin_bit_mask = (1ULL << BSP_PIN_TP_INT),
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_NEGEDGE,
            };
            gpio_config(&int_cfg);
            gpio_isr_handler_add(BSP_PIN_TP_INT, touch_gpio_isr_handler, NULL);
            s_has_touch = true;
            ESP_LOGI(TAG, "CST816 touch controller detected on I2C 0x%02X", CST816_ADDR);
            xTaskCreate(touch_worker_task, "bsp_touch", 3072, NULL, 5, &s_touch_task);
        }
    } else {
        ESP_LOGW(TAG, "CST816 touch controller not detected (%s)", esp_err_to_name(probe_tp));
    }

    return ESP_OK;
}

bool bsp_touch_has_touch(void)
{
    return s_has_touch;
}
