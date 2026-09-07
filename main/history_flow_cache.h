/* Regenerable Flow pyramid. Portable implementation; caller owns the SD lease.
 * A cache miss/corruption never makes the recorded source unreadable. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*history_flow_cancel_fn)(void *context);
typedef struct { int16_t low, high; uint32_t count; } history_flow_bin_t;
/* 0 success, 1 absent/stale/unsupported/unreadable, -1 cancelled. Source is a
 * terminal v2 1 Hz, two-channel min/max .snt, never the live recorder. */
int history_flow_cache_build(const char *source, history_flow_cancel_fn cancel,
                             void *context);
int history_flow_cache_read(const char *source, int64_t start_ms, int64_t end_ms,
                            size_t bins, history_flow_bin_t *out,
                            bool source_verified, history_flow_cancel_fn cancel,
                            void *context, uint32_t *records_read);
