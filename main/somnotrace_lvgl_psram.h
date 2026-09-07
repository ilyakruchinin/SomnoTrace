/* PSRAM-first allocator used only by LVGL on SomnoTrace's 7-inch targets. */
#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *somnotrace_lvgl_alloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline void *somnotrace_lvgl_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc_prefer(ptr, size, 2,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline void somnotrace_lvgl_free(void *ptr)
{
    heap_caps_free(ptr);
}
