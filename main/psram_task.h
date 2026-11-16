/*
 * SomnoTrace - PSRAM-backed FreeRTOS task creation helper
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Create a FreeRTOS task with its stack in PSRAM and TCB in internal RAM.
 *
 * This saves internal SRAM (~290 KB total, shared with Wi-Fi/BLE DMA buffers)
 * by moving the stack to the 8 MB PSRAM.  Requires
 * CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y (already enabled).
 *
 * The StaticTask_t (TCB, ~92 bytes) stays in internal RAM (FreeRTOS
 * requirement).  The stack is allocated with MALLOC_CAP_SPIRAM.
 *
 * For self-deleting tasks: if out_stack and out_tcb are NULL (default),
 * the task is automatically tracked and its stack and TCB are reaped
 * once the task has exited and been processed by the FreeRTOS idle task.
 * Alternatively, pass non-NULL pointers to manage memory explicitly.
 *
 * @param task_func   Task function
 * @param name        FreeRTOS task name
 * @param stack_size  Stack size in bytes
 * @param arg         Task argument
 * @param priority    Task priority
 * @param core_id     Core affinity (0, 1, or tskNO_AFFINITY)
 * @param out_stack   If non-NULL, receives the PSRAM stack pointer (caller frees).
 *                    If NULL, memory is automatically reclaimed upon task exit.
 * @param out_tcb     If non-NULL, receives the internal TCB pointer (caller frees).
 *                    If NULL, memory is automatically reclaimed upon task exit.
 * @return Task handle, or NULL on failure
 */
TaskHandle_t psram_task_create(TaskFunction_t task_func,
                               const char *name,
                               uint32_t stack_size,
                               void *arg,
                               UBaseType_t priority,
                               BaseType_t core_id,
                               StackType_t **out_stack,
                               StaticTask_t **out_tcb);

/**
 * Reaps any terminated self-deleting tasks that were created with
 * psram_task_create(..., NULL, NULL).  Called automatically by a periodic timer
 * and on every psram_task_create() call; exposed here for explicit sweeps.
 */
void psram_task_reap(void);

/* Bounded phase snapshots for the memory-intensive workers.  The stack
 * watermark is a lifetime minimum, not proof that this phase caused it. */
void psram_work_begin(const char *phase, uint32_t stack_bytes);
void psram_work_phase(const char *phase, uint32_t stack_bytes);
void psram_work_end(const char *phase, uint32_t stack_bytes);
