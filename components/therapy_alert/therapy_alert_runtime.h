#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* App-owned task allocation/reaping, injected before alert initialization. */
typedef TaskHandle_t (*alert_task_create_fn_t)(TaskFunction_t, const char *,
    uint32_t, void *, UBaseType_t, BaseType_t, StackType_t **, StaticTask_t **);
typedef void (*alert_task_delete_fn_t)(TaskHandle_t);
void therapy_alert_set_task_fns(alert_task_create_fn_t create,
                                alert_task_delete_fn_t destroy);
