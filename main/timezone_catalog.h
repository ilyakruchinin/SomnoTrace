/* Allocation-free search over SomnoTrace's embedded IANA-to-POSIX database. */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TIMEZONE_CATALOG_ID_MAX     48
#define TIMEZONE_CATALOG_POSIX_MAX  64
#define TIMEZONE_CATALOG_OFFSET_MAX 12
#define TIMEZONE_CATALOG_ABBREV_MAX 16

typedef struct {
    char id[TIMEZONE_CATALOG_ID_MAX];
    char posix[TIMEZONE_CATALOG_POSIX_MAX];
    char utc_offset[TIMEZONE_CATALOG_OFFSET_MAX];
    char abbreviation[TIMEZONE_CATALOG_ABBREV_MAX];
} timezone_catalog_entry_t;

/* Case-insensitive substring search. Spaces also match IANA underscores.
 * Empty queries intentionally return no arbitrary alphabetic prefix; callers
 * should present their own short, geographically relevant suggested list. */
size_t timezone_catalog_search(const char *query,
                               timezone_catalog_entry_t *entries,
                               size_t capacity);

/* Resolve one exact IANA identifier, including its POSIX rule used by libc. */
esp_err_t timezone_catalog_lookup(const char *iana_id,
                                  timezone_catalog_entry_t *entry);

/* Source-injected variants keep the parser host-testable without linking the
 * firmware's binary-data symbols. They perform no allocation. */
size_t timezone_catalog_search_source(const char *json, size_t json_size,
                                      const char *query,
                                      timezone_catalog_entry_t *entries,
                                      size_t capacity);
esp_err_t timezone_catalog_lookup_source(const char *json, size_t json_size,
                                         const char *iana_id,
                                         timezone_catalog_entry_t *entry);

#ifdef __cplusplus
}
#endif
