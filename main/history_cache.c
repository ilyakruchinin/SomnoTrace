#include "history_cache.h"
#include <stdlib.h>
#include <string.h>
#ifdef HISTORY_CACHE_HOST_TEST
#include <pthread.h>
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_mutex)
#define UNLOCK() pthread_mutex_unlock(&s_mutex)
#define cache_alloc(n) malloc(n)
#else
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_mutex;
static bool cache_lock(void)
{
    SemaphoreHandle_t mutex = __atomic_load_n(&s_mutex, __ATOMIC_ACQUIRE);
    if (!mutex) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (!created) return false;
        SemaphoreHandle_t expected = NULL;
        if (!__atomic_compare_exchange_n(&s_mutex, &expected, created, false,
                                         __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
            vSemaphoreDelete(created);
        mutex = __atomic_load_n(&s_mutex, __ATOMIC_ACQUIRE);
    }
    return xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}
#define LOCK() if (!cache_lock()) return 0
#define UNLOCK() xSemaphoreGive(s_mutex)
#define cache_alloc(n) heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

typedef struct {
    history_cache_key_t key;
    void *data;
    size_t size;
    uint64_t used;
} cache_entry_t;
/* The entry table also belongs in PSRAM: internal heap is needed by RGB DMA,
 * BLE and OTA admission even when no History window is being read. */
static cache_entry_t *s_entries;
static size_t s_bytes;
static uint64_t s_clock;
static char s_selected_day[9];
static bool same_key(const history_cache_key_t *a, const history_cache_key_t *b)
{
    return a->generation == b->generation && a->kind == b->kind &&
        a->start_ms == b->start_ms && a->end_ms == b->end_ms &&
        a->signal == b->signal && a->therapy_only == b->therapy_only &&
        memcmp(a->day, b->day, sizeof(a->day)) == 0;
}
static void evict(size_t i)
{
    if (!s_entries) return;
    s_bytes -= s_entries[i].size;
    free(s_entries[i].data);
    memset(&s_entries[i], 0, sizeof(s_entries[i]));
}
size_t history_cache_get(const history_cache_key_t *key, void *out, size_t size)
{
    LOCK();
    if (!s_entries) { UNLOCK(); return 0; }
    size_t found = 0;
    for (size_t i = 0; i < HISTORY_CACHE_SLOTS; ++i) {
        cache_entry_t *entry = &s_entries[i];
        if (!entry->data || !same_key(key, &entry->key)) continue;
        if (!out || size == entry->size) {
            found = entry->size;
            if (out) memcpy(out, entry->data, size);
            entry->used = ++s_clock;
        }
        break;
    }
    UNLOCK();
    return found;
}
/* Return-valued internal helpers keep optional lock/allocation failures quiet. */
static int cache_store(const history_cache_key_t *key, const void *data, size_t size)
{
    if (!data || !size || size > HISTORY_CACHE_ITEM_BYTES) return 0;
    LOCK();
    if (s_selected_day[0] && memcmp(key->day, s_selected_day, 9)) {
        UNLOCK();
        return 0;
    }
    if (!s_entries) {
        s_entries = cache_alloc(HISTORY_CACHE_SLOTS * sizeof(*s_entries));
        if (!s_entries) { UNLOCK(); return 0; }
        memset(s_entries, 0, HISTORY_CACHE_SLOTS * sizeof(*s_entries));
    }
    size_t slot = HISTORY_CACHE_SLOTS;
    for (size_t i = 0; i < HISTORY_CACHE_SLOTS; ++i) {
        if (s_entries[i].data && (s_entries[i].key.generation != key->generation ||
            same_key(key, &s_entries[i].key))) evict(i);
        if (!s_entries[i].data) slot = i;
    }
    while (slot == HISTORY_CACHE_SLOTS || s_bytes + size > HISTORY_CACHE_BYTES) {
        size_t oldest = HISTORY_CACHE_SLOTS;
        for (size_t i = 0; i < HISTORY_CACHE_SLOTS; ++i)
            if (s_entries[i].data && (oldest == HISTORY_CACHE_SLOTS ||
                s_entries[i].used < s_entries[oldest].used)) oldest = i;
        if (oldest == HISTORY_CACHE_SLOTS) break;
        evict(oldest);
        slot = oldest;
    }
    void *copy = cache_alloc(size);
    if (copy && slot < HISTORY_CACHE_SLOTS) {
        memcpy(copy, data, size);
        s_entries[slot] = (cache_entry_t){.key = *key, .data = copy,
            .size = size, .used = ++s_clock};
        s_bytes += size;
    } else free(copy);
    UNLOCK();
    return 0;
}
void history_cache_put(const history_cache_key_t *key, const void *data, size_t size)
{
    (void)cache_store(key, data, size);
}
static int cache_reset(void)
{
    LOCK();
    for (size_t i = 0; i < HISTORY_CACHE_SLOTS; ++i) evict(i);
    free(s_entries);
    s_entries = NULL;
    s_selected_day[0] = '\0';
    UNLOCK();
    return 0;
}
void history_cache_clear(void) { (void)cache_reset(); }

static int cache_select(const char day[9])
{
    LOCK();
    if (memcmp(day, s_selected_day, 9)) {
        for (size_t i = 0; i < HISTORY_CACHE_SLOTS; ++i) evict(i);
        memcpy(s_selected_day, day, 9);
    }
    UNLOCK();
    return 0;
}
void history_cache_select_day(const char day[9]) { (void)cache_select(day); }
