#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "live_flow_plot.h"

static void verify(const int16_t *samples, size_t count, size_t capacity)
{
    uint16_t indices[302];
    for (size_t i = 0; i < 302; ++i) indices[i] = UINT16_MAX;
    size_t n = live_flow_plot_indices(samples, count, indices + 1, capacity);
    assert(n > 0 && n <= capacity && n <= count);
    assert(indices[0] == UINT16_MAX && indices[capacity + 1] == UINT16_MAX);
    assert(indices[1] == 0 && indices[n] == count - 1);
    int16_t source_min = INT16_MAX, source_max = INT16_MIN;
    int16_t drawn_min = INT16_MAX, drawn_max = INT16_MIN;
    for (size_t i = 0; i < count; ++i) {
        if (samples[i] < source_min) source_min = samples[i];
        if (samples[i] > source_max) source_max = samples[i];
    }
    for (size_t i = 1; i <= n; ++i) {
        assert(indices[i] < count);
        if (i > 1) assert(indices[i] > indices[i - 1]);
        if (samples[indices[i]] < drawn_min) drawn_min = samples[indices[i]];
        if (samples[indices[i]] > drawn_max) drawn_max = samples[indices[i]];
    }
    assert(source_min == drawn_min && source_max == drawn_max);
    if (count <= capacity) {
        assert(n == count);
        for (size_t i = 1; i <= n; ++i) assert(indices[i] == i - 1);
    }
}

int main(void)
{
    int16_t samples[300] = {0};
    /* Move narrow positive/negative peaks through every sampling phase. The
     * previous every-other-point renderer dropped one or both at many offsets. */
    for (size_t peak = 0; peak < 300; ++peak) {
        memset(samples, 0, sizeof(samples));
        samples[peak] = 900;
        samples[(peak + 1) % 300] = -900;
        verify(samples, 300, 150);
        verify(samples, 300, 300);
    }
    for (size_t n = 1; n <= 300; ++n) {
        for (size_t i = 0; i < n; ++i)
            samples[i] = (int16_t)((int)i * 6 - 900);
        verify(samples, n, 150);
        verify(samples, n, 149);
        verify(samples, n, 4);
        memset(samples, 0, sizeof(samples));
        verify(samples, n, 150);
    }
    /* Partial startup windows must not include the preceding zero padding. */
    for (size_t i = 0; i < 300; ++i) samples[i] = -1000;
    for (size_t i = 290; i < 300; ++i) samples[i] = 100;
    verify(samples + 290, 10, 150);
    uint16_t indices[150];
    assert(!live_flow_plot_indices(NULL, 300, indices, 150));
    assert(!live_flow_plot_indices(samples, 0, indices, 150));
    for (size_t i = 0; i < 300; ++i) samples[i] = 100;
    samples[147] = LIVE_FLOW_MISSING;
    samples[148] = -900;
    samples[149] = 900;
    size_t used = live_flow_plot_indices(samples, 300, indices, 150);
    bool low = false, high = false, gap = false;
    for (size_t i = 0; i < used; ++i) {
        low |= samples[indices[i]] == -900;
        high |= samples[indices[i]] == 900;
        if (i && indices[i - 1] < 147 && indices[i] >= 147) {
            assert(!live_flow_plot_contiguous(samples, indices[i - 1], indices[i]));
            gap = true;
        }
    }
    assert(low && high && gap);
    assert(live_flow_plot_contiguous(samples, 148, 299));
    int64_t presented = 0;
    unsigned consumed = 0;
    const int64_t irregular_frames[] = {10000, 65000, 80000, 180000, 200000};
    for (size_t i = 0; i < 5; ++i)
        consumed += live_flow_presentation_due(&presented, irregular_frames[i], 5 - consumed);
    assert(consumed == 5 && presented == 200000);
    assert(live_flow_presentation_due(&presented, 360000, 5) == 4);
    assert(!live_flow_presentation_due(&presented, 350000, 5));
    puts("live waveform extrema, time order, budgets and startup windows passed");
}
