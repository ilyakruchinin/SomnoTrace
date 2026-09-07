/* Optional, bounded PSRAM memoization. Callers hold a storage read lease and
 * key every result by the card mutation generation. No borrowed pointers. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define HISTORY_CACHE_BYTES (192U * 1024U)
#define HISTORY_CACHE_ITEM_BYTES (64U * 1024U)
#define HISTORY_CACHE_SLOTS 32U

typedef struct {
    int64_t start_ms, end_ms;
    uint32_t generation;
    uint16_t kind;
    uint8_t signal, therapy_only;
    char day[9];
} history_cache_key_t;
/* With out=NULL, returns the size. Otherwise copies only an exact size match. */
size_t history_cache_get(const history_cache_key_t *key, void *out, size_t size);
void history_cache_put(const history_cache_key_t *key, const void *data, size_t size);
void history_cache_clear(void);

/* Pin the active night so background calendar/index scans cannot evict it. */
void history_cache_select_day(const char day[9]);
