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

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_I2C_NUM         0
#define BSP_I2C_SCL_PIN     41
#define BSP_I2C_SDA_PIN     42
#define BSP_I2C_FREQ_HZ     100000

/* Initialize the shared I2C master bus (idempotent). */
esp_err_t bsp_i2c_init(void);

/* Get the shared I2C master bus handle, initializing it if not already done. */
i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void);

#ifdef __cplusplus
}
#endif
