/*
 * SomnoTrace - Minimal FreeRTOS semaphore test shim for host unit tests
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3, or (at your option) any later version.
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

#include "freertos/FreeRTOS.h"

/* A non-NULL opaque token: callers only test for creation failure and pass
 * the handle back into take/give, which are no-ops on the host. */
typedef void *SemaphoreHandle_t;

#ifdef SOMNO_ML_WORKER_TEST
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t mutex);
#else
#define xSemaphoreCreateMutex()          ((SemaphoreHandle_t)0x1)
#define vSemaphoreDelete(m)              ((void)(m))
#endif
#define xSemaphoreTake(m, ticks)         do { (void)(m); (void)(ticks); } while (0)
#define xSemaphoreGive(m)                do { (void)(m); } while (0)
