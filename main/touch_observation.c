#include "touch_observation.h"
#include <limits.h>

static void withdraw(touch_observation_t *s)
{
    if (s->valid || s->pressed) ++s->continuity;
    s->valid = false;
    s->pressed = false;
}

void touch_observation_update(touch_observation_t *s, int64_t now,
                              int error, bool frame, bool pressed,
                              uint16_t x, uint16_t y)
{
    if (!s) return;
    if ((s->observed && (now < s->observed_us ||
                        now - s->observed_us > TOUCH_OBSERVATION_MAX_AGE_US)) ||
        (s->valid && s->pressed && (now < s->frame_us ||
                      now - s->frame_us > TOUCH_OBSERVATION_MAX_AGE_US)))
        withdraw(s);
    s->observed = true;
    s->observed_us = now;
    s->last_error = error;
    if (error) {
        withdraw(s);
        if (s->errors != UINT32_MAX) ++s->errors;
        if (s->consecutive_errors != UINT8_MAX) ++s->consecutive_errors;
        return;
    }
    s->consecutive_errors = 0;
    s->recovering = false;
    s->preventive_recovery = false;
    if (frame) {
        s->frame_us = now;
        s->valid = true;
        s->pressed = pressed;
        s->x = x;
        s->y = y;
    }
    /* GT911 need not repeat zero-contact frames while idle. A known release
     * remains valid through successful status polls; only active points age
     * out. Bus errors or a missed polling interval still break continuity. */
}

void touch_observation_recovering(touch_observation_t *s)
{
    if (!s) return;
    withdraw(s);
    s->recovering = true;
    s->preventive_recovery = false;
    if (s->recovery_attempts != UINT32_MAX) ++s->recovery_attempts;
}

void touch_observation_preventive_recovering(touch_observation_t *s)
{
    if (!s) return;
    withdraw(s);
    s->recovering = true;
    s->preventive_recovery = true;
    if (s->recovery_attempts != UINT32_MAX) ++s->recovery_attempts;
    if (s->preventive_recovery_attempts != UINT32_MAX)
        ++s->preventive_recovery_attempts;
}

bool touch_observation_healthy(const touch_observation_t *s, int64_t now)
{
    return s && s->observed && !s->recovering &&
           s->consecutive_errors < TOUCH_OBSERVATION_FAILURE_LIMIT &&
           now >= s->observed_us &&
           now - s->observed_us <= TOUCH_OBSERVATION_MAX_AGE_US;
}

bool touch_observation_pressed(const touch_observation_t *s, int64_t now)
{
    return touch_observation_healthy(s, now) && !s->last_error &&
           s->valid && s->pressed && now >= s->frame_us &&
           now - s->frame_us <= TOUCH_OBSERVATION_MAX_AGE_US;
}
