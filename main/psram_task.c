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

#include "psram_task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "psram_task";

TaskHandle_t psram_task_create(TaskFunction_t task_func,
                               const char *name,
                               uint32_t stack_size,
                               void *arg,
                               UBaseType_t priority,
                               BaseType_t core_id,
                               StackType_t **out_stack,
                               StaticTask_t **out_tcb)
{
    StackType_t *stack = heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb  = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (!stack || !tcb) {
        ESP_LOGE(TAG, "failed to allocate %s: stack=%u PSRAM free=%u, tcb internal free=%u",
                 name, (unsigned)stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        free(stack);
        free(tcb);
        return NULL;
    }

    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        task_func, name, stack_size, arg, priority, stack, tcb, core_id);
    if (!h) {
        ESP_LOGE(TAG, "xTaskCreateStaticPinnedToCore failed for %s", name);
        free(stack);
        free(tcb);
        return NULL;
    }

    if (out_stack) *out_stack = stack;
    if (out_tcb)   *out_tcb   = tcb;
    return h;
}
