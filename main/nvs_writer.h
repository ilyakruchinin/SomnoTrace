/*
 * SomnoTrace - Centralised NVS/flash writer task
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

/*
 * A task with a PSRAM-backed stack cannot itself perform SPI-flash writes: the
 * flash routine runs with the cache disabled on its own core, and accessing
 * its PSRAM stack mid-operation faults. (Other PSRAM-stack tasks are protected
 * by CONFIG_SPI_FLASH_AUTO_SUSPEND, but the task *executing* the write is not.)
 *
 * To let the httpd worker and BLE reconnect workers run on PSRAM stacks while
 * they still access NVS, all NVS reads and writes are funnelled to this single
 * task, which is created with an ordinary internal-RAM stack. The submitting
 * task blocks until the operation completes and receives its result.
 */

/* Callback executed on the nvs_writer task. It may perform NVS reads/writes,
 * but must copy all input values into internal-stack locals before entering
 * flash and copy output values to caller memory only after the NVS operation
 * has completed. */
typedef esp_err_t (*nvs_writer_fn_t)(void *arg);

/* Create the writer task + queue. Idempotent. MUST be called before any task
 * with a PSRAM stack (e.g. the httpd worker) can invoke nvs_writer_run(). */
void nvs_writer_init(void);

/* Run fn(arg) on the internal-stack writer task and block until it finishes,
 * returning fn's result. If the writer has not yet been initialised during
 * early boot, fn runs inline on the calling task (which at that point has an
 * internal stack). If initialisation was attempted but failed, returns
 * ESP_ERR_INVALID_STATE rather than performing an unsafe inline flash access. */
esp_err_t nvs_writer_run(nvs_writer_fn_t fn, void *arg);

/* Global NVS access lock.  ALL NVS access — both proxy (nvs_writer_run) and
 * direct (nvs_open/nvs_commit from tasks with internal stacks) — must be
 * serialized to prevent concurrent flash erase/write operations from disabling
 * the cache while another task is mid-read from flash-mapped memory.  A
 * concurrent write+read causes an MMU entry fault (cache error) crash.
 *
 * Usage from direct NVS callers (internal-stack tasks only):
 *   nvs_writer_lock();
 *   nvs_open(...);
 *   nvs_get_str(...);
 *   nvs_close(h);
 *   nvs_writer_unlock();
 *
 * nvs_writer_run() acquires this lock internally; callers that use the proxy
 * do NOT need to call lock/unlock themselves. */
void nvs_writer_lock(void);
void nvs_writer_unlock(void);
