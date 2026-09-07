/* Timestamped touch observations; no RTOS, allocation or hardware ownership. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TOUCH_OBSERVATION_MAX_AGE_US 100000LL
#define TOUCH_OBSERVATION_FAILURE_LIMIT 3U

typedef struct {
    uint16_t x, y;
    bool pressed;
    bool observed;
    bool valid;
    bool recovering;
    bool preventive_recovery;
    uint8_t consecutive_errors;
    uint32_t errors;
    uint32_t recovery_attempts;
    uint32_t preventive_recovery_requests;
    uint32_t preventive_recovery_attempts;
    uint32_t visibility_requests;
    int64_t visibility_requested_us; /* Original demand time, retained over retries. */
    uint32_t continuity;
    int64_t observed_us;
    int64_t frame_us;
    int last_error;
} touch_observation_t;

/* A successful status read without new data may retain a point only while
 * observation is continuous. An error, reset or scheduling gap withdraws it
 * until a new complete controller frame arrives. */
void touch_observation_update(touch_observation_t *state, int64_t now_us,
                              int error, bool frame, bool pressed,
                              uint16_t x, uint16_t y);
void touch_observation_recovering(touch_observation_t *state);
void touch_observation_preventive_recovering(touch_observation_t *state);
bool touch_observation_healthy(const touch_observation_t *state, int64_t now_us);
bool touch_observation_pressed(const touch_observation_t *state, int64_t now_us);
