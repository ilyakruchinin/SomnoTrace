#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* The custom descriptor follows esp_app_desc_t in ESP-IDF's first segment.
 * Images without board identity are deliberately rejected by native updates. */
typedef struct {
    char magic[16];
    char board[16];
} somnotrace_firmware_target_t;
extern const somnotrace_firmware_target_t somnotrace_firmware_target;
bool somnotrace_firmware_target_matches(const void *prefix, size_t size);
