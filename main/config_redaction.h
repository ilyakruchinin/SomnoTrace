/* Redact the entire fixed-size field, including bytes after the terminator. */
#pragma once
#include <string.h>
#include <stddef.h>
static inline void config_redact(char *value, size_t capacity,
                                  const char *present, const char *empty)
{
    if (!value || !capacity) return;
    const char *label = value[0] ? present : empty;
    size_t length = strlen(label);
    if (length >= capacity) length = capacity - 1;
    memset(value, 0, capacity);
    memcpy(value, label, length);
}
