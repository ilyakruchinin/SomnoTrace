/*
 * SomnoTrace - Minimal FreeRTOS task shim for host unit tests
 * Copyright (C) 2026 Dmitri Farkov
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include "FreeRTOS.h"

static inline TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (TaskHandle_t)(uintptr_t)1;
}
