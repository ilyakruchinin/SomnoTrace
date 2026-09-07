/* Bounded, allocation-free 7B controller observations since boot. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define CONTROLLER_DIAGNOSTICS_HISTORY_MAX 8U

typedef enum {
    CONTROLLER_PANEL_INIT = 0,
    CONTROLLER_PANEL_SUBMIT,
    CONTROLLER_PANEL_HANDOFF,
    CONTROLLER_TOUCH_INIT,
    CONTROLLER_TOUCH_READ,
    CONTROLLER_BACKLIGHT_POWER,
    CONTROLLER_BACKLIGHT_BRIGHTNESS,
    CONTROLLER_TOUCH_RECOVERY,
    CONTROLLER_OUTPUT_MODE,
    CONTROLLER_LCD_POWER,
    CONTROLLER_OPERATION_COUNT,
} controller_operation_t;

typedef struct {
    bool observed;
    esp_err_t last_result;
    uint32_t error_count;
    int64_t last_observed_us;
    int64_t last_error_us;
} controller_operation_status_t;

typedef struct {
    controller_operation_t operation;
    esp_err_t result;
    uint32_t occurrences;
    int64_t first_us;
    int64_t last_us;
} controller_error_episode_t;

typedef struct {
    bool initialized;
    bool simulated;
    controller_operation_status_t operations[CONTROLLER_OPERATION_COUNT];
    /* Most recently observed failures first. Repeated errors before recovery
     * coalesce into one episode, even when another operation also fails. */
    controller_error_episode_t history[CONTROLLER_DIAGNOSTICS_HISTORY_MAX];
    size_t history_count;
} controller_diagnostics_snapshot_t;

/* Initialize once before controller startup. No heap, card, or log writes.
 * Task-context calls only; do not call from an ISR or another critical section.
 * Timestamps are monotonic microseconds since boot, never wall-clock dates.
 * Counters saturate at UINT32_MAX. Unobserved operations remain unknown. */
void controller_diagnostics_init(bool simulated);
void controller_diagnostics_record(controller_operation_t operation,
                                   esp_err_t result);
void controller_diagnostics_get_snapshot(controller_diagnostics_snapshot_t *out);
const char *controller_diagnostics_operation_name(controller_operation_t operation);
