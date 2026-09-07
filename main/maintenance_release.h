#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#define MAINTENANCE_RELEASE_SOURCE "dmitrif/SomnoTrace"
#define MAINTENANCE_RELEASE_API                                                                    \
    "https://api.github.com/repos/" MAINTENANCE_RELEASE_SOURCE "/releases?per_page=1"
#define MAINTENANCE_RELEASE_RESPONSE_MAX 24576U
typedef struct {
    bool valid, compatible_asset, notes_truncated;
    char version[48], published[40], url[512], notes[1024];
} maintenance_release_t;
/* Bounded JSON grammar budget limits cJSON node memory before parsing. Input
 * need not be NUL-terminated; malformed/oversized data never yields an asset. */
esp_err_t maintenance_release_parse(const char *json, size_t length, maintenance_release_t *out);
