/*
 * SomnoTrace - Shared I2C master bus management
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

#include "bsp_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_i2c";

static i2c_master_bus_handle_t s_bus_handle = NULL;
static SemaphoreHandle_t s_init_lock = NULL;

esp_err_t bsp_i2c_init(void)
{
    if (s_bus_handle) return ESP_OK;

    if (!s_init_lock) {
        s_init_lock = xSemaphoreCreateMutex();
        if (!s_init_lock) return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_init_lock, portMAX_DELAY);
    if (s_bus_handle) {
        xSemaphoreGive(s_init_lock);
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA_PIN,
        .scl_io_num = BSP_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize shared I2C bus: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "shared I2C bus initialized (SCL=%d, SDA=%d, %d Hz)",
                 BSP_I2C_SCL_PIN, BSP_I2C_SDA_PIN, BSP_I2C_FREQ_HZ);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    xSemaphoreGive(s_init_lock);
    return ret;
}

i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void)
{
    if (!s_bus_handle) {
        bsp_i2c_init();
    }
    return s_bus_handle;
}
