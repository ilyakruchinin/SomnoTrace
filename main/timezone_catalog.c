/* Allocation-free IANA timezone catalog for the native setup flow. */

#include "timezone_catalog.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef TIMEZONE_CATALOG_HOST_TEST
extern const char _binary_zones_json_start[];
extern const char _binary_zones_json_end[];
#endif

typedef struct {
    const char *cursor;
    const char *end;
} catalog_cursor_t;

static void copy_slice(char *destination, size_t capacity,
                       const char *start, size_t length)
{
    if (!destination || capacity == 0) return;
    if (length >= capacity) length = capacity - 1;
    if (length > 0) memcpy(destination, start, length);
    destination[length] = '\0';
}

static bool catalog_next(catalog_cursor_t *catalog,
                         timezone_catalog_entry_t *entry)
{
    if (!catalog || !entry) return false;
    const char *cursor = catalog->cursor;
    while (cursor < catalog->end && *cursor != '"') cursor++;
    if (cursor >= catalog->end) return false;
    const char *id_start = ++cursor;
    while (cursor < catalog->end && *cursor != '"') cursor++;
    if (cursor >= catalog->end) return false;
    const char *id_end = cursor++;
    while (cursor < catalog->end && (*cursor == ' ' || *cursor == ':'))
        cursor++;
    if (cursor >= catalog->end || *cursor != '"') return false;
    const char *posix_start = ++cursor;
    while (cursor < catalog->end && *cursor != '"') cursor++;
    if (cursor >= catalog->end) return false;
    const char *posix_end = cursor++;
    catalog->cursor = cursor;

    copy_slice(entry->id, sizeof(entry->id), id_start,
               (size_t)(id_end - id_start));
    copy_slice(entry->posix, sizeof(entry->posix), posix_start,
               (size_t)(posix_end - posix_start));
    entry->utc_offset[0] = '\0';
    entry->abbreviation[0] = '\0';
    return true;
}

static unsigned char folded(unsigned char character)
{
    if (character >= 'A' && character <= 'Z')
        return (unsigned char)(character - 'A' + 'a');
    if (character == ' ') return '_';
    return character;
}

static bool contains_folded(const char *text, const char *query)
{
    if (!text || !query || !query[0]) return false;
    size_t query_length = strlen(query);
    size_t text_length = strlen(text);
    if (query_length > text_length) return false;
    for (size_t offset = 0; offset + query_length <= text_length; offset++) {
        size_t index = 0;
        while (index < query_length &&
               folded((unsigned char)text[offset + index]) ==
               folded((unsigned char)query[index])) {
            index++;
        }
        if (index == query_length) return true;
    }
    return false;
}

static const char *parse_name(const char *cursor, char *name, size_t size)
{
    if (*cursor == '<') {
        const char *start = ++cursor;
        while (*cursor && *cursor != '>') cursor++;
        copy_slice(name, size, start, (size_t)(cursor - start));
        return *cursor == '>' ? cursor + 1 : cursor;
    }
    const char *start = cursor;
    while ((*cursor >= 'A' && *cursor <= 'Z') ||
           (*cursor >= 'a' && *cursor <= 'z')) {
        cursor++;
    }
    copy_slice(name, size, start, (size_t)(cursor - start));
    return cursor;
}

static const char *parse_posix_offset(const char *cursor, int *minutes)
{
    int sign = 1;
    if (*cursor == '-') {
        sign = -1;
        cursor++;
    } else if (*cursor == '+') {
        cursor++;
    }
    if (*cursor < '0' || *cursor > '9') return NULL;
    int hours = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        hours = hours * 10 + (*cursor++ - '0');
    }
    int minute_part = 0;
    if (*cursor == ':') {
        cursor++;
        if (*cursor < '0' || *cursor > '9') return NULL;
        while (*cursor >= '0' && *cursor <= '9') {
            minute_part = minute_part * 10 + (*cursor++ - '0');
        }
    }
    /* POSIX signs describe what is added to local time to obtain UTC. */
    *minutes = -(sign * (hours * 60 + minute_part));
    return cursor;
}

static void describe_posix(timezone_catalog_entry_t *entry)
{
    char standard[8] = {0};
    char daylight[8] = {0};
    const char *cursor = parse_name(entry->posix, standard, sizeof(standard));
    int utc_minutes = 0;
    cursor = parse_posix_offset(cursor, &utc_minutes);
    if (!cursor) {
        copy_slice(entry->utc_offset, sizeof(entry->utc_offset), "UTC", 3);
        copy_slice(entry->abbreviation, sizeof(entry->abbreviation),
                   standard, strlen(standard));
        return;
    }
    if ((*cursor >= 'A' && *cursor <= 'Z') ||
        (*cursor >= 'a' && *cursor <= 'z') || *cursor == '<') {
        (void)parse_name(cursor, daylight, sizeof(daylight));
    }

    char sign = utc_minutes < 0 ? '-' : '+';
    unsigned magnitude = (unsigned)(utc_minutes < 0
        ? -utc_minutes : utc_minutes);
    /* The IANA catalog never exceeds 24 hours. Keep malformed embedded input
     * bounded as well, both for the display field and for format analysis. */
    if (magnitude > 24U * 60U) {
        copy_slice(entry->utc_offset, sizeof(entry->utc_offset), "UTC", 3);
        copy_slice(entry->abbreviation, sizeof(entry->abbreviation),
                   standard, strlen(standard));
        return;
    }
    snprintf(entry->utc_offset, sizeof(entry->utc_offset), "UTC%c%02u:%02u",
             sign, magnitude / 60, magnitude % 60);
    if (daylight[0] && strcmp(standard, daylight)) {
        snprintf(entry->abbreviation, sizeof(entry->abbreviation), "%s / %s",
                 standard, daylight);
    } else {
        copy_slice(entry->abbreviation, sizeof(entry->abbreviation),
                   standard, strlen(standard));
    }
}

size_t timezone_catalog_search_source(const char *json, size_t json_size,
                                      const char *query,
                                      timezone_catalog_entry_t *entries,
                                      size_t capacity)
{
    if (!json || !query || !query[0] || (!entries && capacity > 0)) return 0;
    catalog_cursor_t catalog = { json, json + json_size };
    size_t count = 0;
    timezone_catalog_entry_t candidate;
    while (catalog_next(&catalog, &candidate)) {
        if (!contains_folded(candidate.id, query)) continue;
        if (count < capacity) {
            describe_posix(&candidate);
            entries[count] = candidate;
        }
        count++;
        if (count >= capacity) break;
    }
    return count;
}

esp_err_t timezone_catalog_lookup_source(const char *json, size_t json_size,
                                         const char *iana_id,
                                         timezone_catalog_entry_t *entry)
{
    if (!json || !iana_id || !iana_id[0] || !entry) return ESP_ERR_INVALID_ARG;
    catalog_cursor_t catalog = { json, json + json_size };
    timezone_catalog_entry_t candidate;
    while (catalog_next(&catalog, &candidate)) {
        if (strcmp(candidate.id, iana_id)) continue;
        describe_posix(&candidate);
        *entry = candidate;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

size_t timezone_catalog_search(const char *query,
                               timezone_catalog_entry_t *entries,
                               size_t capacity)
{
#ifdef TIMEZONE_CATALOG_HOST_TEST
    (void)query;
    (void)entries;
    (void)capacity;
    return 0;
#else
    return timezone_catalog_search_source(
        _binary_zones_json_start,
        (size_t)(_binary_zones_json_end - _binary_zones_json_start),
        query, entries, capacity);
#endif
}

esp_err_t timezone_catalog_lookup(const char *iana_id,
                                  timezone_catalog_entry_t *entry)
{
#ifdef TIMEZONE_CATALOG_HOST_TEST
    (void)iana_id;
    (void)entry;
    return ESP_ERR_NOT_FOUND;
#else
    return timezone_catalog_lookup_source(
        _binary_zones_json_start,
        (size_t)(_binary_zones_json_end - _binary_zones_json_start),
        iana_id, entry);
#endif
}
