/* Bounded, platform-independent maintenance policy. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAINTENANCE_HOLD_MS 3000U
#define MAINTENANCE_HOLD_OBSERVATION_MS 150U
#define MAINTENANCE_PAGE_SIZE 4
#define MAINTENANCE_NAME_MAX 96

typedef struct {
    bool pressed;
    uint32_t started_ms;
    uint32_t observed_ms;
} maintenance_hold_t;
void maintenance_hold_reset(maintenance_hold_t *hold);
void maintenance_hold_press(maintenance_hold_t *hold, uint32_t now);
/* Call only from a current input PRESSING event. A missed observation window
 * cancels the gesture, even when a later sample again reports pressed. */
bool maintenance_hold_ready(maintenance_hold_t *hold, uint32_t now, bool allowed);
uint32_t maintenance_hold_elapsed(const maintenance_hold_t *hold, uint32_t now);
bool maintenance_day_valid(const char *name);
bool maintenance_sd_image_name_valid(const char *name);
bool maintenance_generated_edf_name(const char *name, bool root);
/* Conservative empirical estimate uses the largest complete observed night,
 * never a made-up bytes/night constant. Zero means evidence unavailable. */
uint64_t maintenance_estimated_nights(uint64_t free_bytes, uint64_t largest_night, size_t samples);
