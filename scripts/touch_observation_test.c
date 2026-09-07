#include "touch_observation.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void)
{
    touch_observation_t s = {0};
    assert(!touch_observation_healthy(&s, 0));
    touch_observation_update(&s, 10000, 0, true, true, 123, 456);
    assert(touch_observation_pressed(&s, 10000));
    touch_observation_update(&s, 20000, 0, false, false, 0, 0);
    assert(touch_observation_pressed(&s, 20000));
    assert(s.x == 123 && s.y == 456);
    /* A responsive status register cannot refresh an old contact forever. */
    for (int64_t now = 30000; now <= 110000; now += 10000)
        touch_observation_update(&s, now, 0, false, false, 0, 0);
    assert(!touch_observation_pressed(&s, 110001));
    assert(!touch_observation_pressed(&s, 120001));
    touch_observation_update(&s, 130001, 0, false, false, 0, 0);
    assert(!touch_observation_pressed(&s, 130001));
    assert(s.continuity == 1 && !s.valid);
    touch_observation_update(&s, 140000, 0, true, true, 7, 8);
    touch_observation_update(&s, 150000, 9, false, false, 0, 0);
    assert(!touch_observation_pressed(&s, 150000));
    touch_observation_update(&s, 160000, 0, false, false, 0, 0);
    assert(!s.valid && !touch_observation_pressed(&s, 160000));
    touch_observation_update(&s, 170000, 0, true, false, 0, 0);
    assert(s.valid && !s.pressed);
    for (unsigned i = 0; i < 300; ++i)
        touch_observation_update(&s, 180000 + i, 9, false, false, 0, 0);
    assert(s.consecutive_errors == UINT8_MAX && s.errors == 301);
    assert(!touch_observation_healthy(&s, 180299));
    touch_observation_recovering(&s);
    assert(s.recovering && !s.preventive_recovery &&
           s.recovery_attempts == 1 && !s.valid);
    touch_observation_update(&s, 190000, 0, false, false, 0, 0);
    assert(touch_observation_healthy(&s, 190000) && !s.valid);
    touch_observation_preventive_recovering(&s);
    assert(s.recovering && s.preventive_recovery &&
           s.recovery_attempts == 2 && s.preventive_recovery_attempts == 1);
    touch_observation_update(&s, 195000, 0, false, false, 0, 0);
    assert(!s.recovering && !s.preventive_recovery);
    touch_observation_update(&s, 200000, 0, true, true, 7, 8);
    touch_observation_update(&s, 199999, 0, false, false, 0, 0);
    assert(!s.valid); /* a clock discontinuity also releases input */
    s.errors = UINT32_MAX;
    touch_observation_update(&s, 210000, 1, false, false, 0, 0);
    assert(s.errors == UINT32_MAX);
    puts("touch observation freshness, continuity and recovery passed");
}
