#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define LIVE_FLOW_MISSING INT16_MIN
#define LIVE_FLOW_SAMPLE_US 40000LL

/* The time budget follows the 25 Hz source, including missing positions.
 * Delayed display frames consume elapsed samples rather than slowing time. */
static inline unsigned live_flow_presentation_due(int64_t *presented_us,
                                                  int64_t now, unsigned pending)
{
    if (now < *presented_us || !pending) {
        *presented_us = now;
        return 0;
    }
    uint64_t due = (uint64_t)(now - *presented_us) / LIVE_FLOW_SAMPLE_US;
    unsigned count = due > pending ? pending : (unsigned)due;
    *presented_us += (int64_t)count * LIVE_FLOW_SAMPLE_US;
    if (count == pending) *presented_us = now;
    return count;
}

static inline bool live_flow_plot_contiguous(const int16_t *samples,
                                              size_t first, size_t last)
{
    if (!samples || last < first) return false;
    for (size_t i = first; i <= last; ++i)
        if (samples[i] == LIVE_FLOW_MISSING) return false;
    return true;
}

/* Keep both extrema of each contiguous bucket, in their original time order.
 * Uniformly skipping samples changes the apparent peak whenever the live
 * window advances by one sample. This uses the same bounded vertex budget,
 * retains the two endpoints, and never averages away a short excursion.
 * Indices are relative to the supplied valid samples (no startup padding). */
static inline size_t live_flow_plot_indices(const int16_t *samples, size_t count,
                                            uint16_t *indices, size_t capacity)
{
    if (!samples || !indices || !count || !capacity || count > UINT16_MAX)
        return 0;
    if (count <= capacity) {
        for (size_t i = 0; i < count; ++i) indices[i] = (uint16_t)i;
        return count;
    }
    if (capacity < 4) return 0;
    size_t used = 0;
    indices[used++] = 0;
    size_t buckets = (capacity - 2) / 2;
    for (size_t b = 0; b < buckets; ++b) {
        size_t first = 1 + b * (count - 2) / buckets;
        size_t end = 1 + (b + 1) * (count - 2) / buckets;
        size_t low = SIZE_MAX, high = SIZE_MAX;
        for (size_t i = first; i < end; ++i) {
            if (samples[i] == LIVE_FLOW_MISSING) continue;
            if (low == SIZE_MAX) { low = high = i; continue; }
            if (samples[i] < samples[low]) low = i;
            if (samples[i] > samples[high]) high = i;
        }
        if (low == SIZE_MAX) continue;
        if (low > high) {
            size_t swap = low;
            low = high;
            high = swap;
        }
        indices[used++] = (uint16_t)low;
        if (high != low) indices[used++] = (uint16_t)high;
    }
    indices[used++] = (uint16_t)(count - 1);
    return used;
}
