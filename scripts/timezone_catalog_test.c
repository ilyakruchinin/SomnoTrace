#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "timezone_catalog.h"

static const char FIXTURE[] =
    "{\n"
    "\"America/St_Johns\":\"NST3:30NDT,M3.2.0,M11.1.0\",\n"
    "\"America/Toronto\":\"EST5EDT,M3.2.0,M11.1.0\",\n"
    "\"Asia/Kathmandu\":\"<+0545>-5:45\",\n"
    "\"Australia/Lord_Howe\":\"<+1030>-10:30<+11>-11,M10.1.0,M4.1.0\",\n"
    "\"Etc/UTC\":\"UTC0\"\n"
    "}";

int main(void)
{
    timezone_catalog_entry_t results[3];
    assert(timezone_catalog_search_source(
               FIXTURE, strlen(FIXTURE), "america", results, 3) == 2);
    assert(!strcmp(results[0].id, "America/St_Johns"));
    assert(!strcmp(results[0].utc_offset, "UTC-03:30"));
    assert(!strcmp(results[0].abbreviation, "NST / NDT"));
    assert(!strcmp(results[1].id, "America/Toronto"));
    assert(!strcmp(results[1].utc_offset, "UTC-05:00"));

    assert(timezone_catalog_search_source(
               FIXTURE, strlen(FIXTURE), "lord howe", results, 3) == 1);
    assert(!strcmp(results[0].utc_offset, "UTC+10:30"));
    assert(!strcmp(results[0].abbreviation, "+1030 / +11"));

    timezone_catalog_entry_t exact;
    assert(timezone_catalog_lookup_source(
               FIXTURE, strlen(FIXTURE), "Asia/Kathmandu", &exact) == 0);
    assert(!strcmp(exact.posix, "<+0545>-5:45"));
    assert(!strcmp(exact.utc_offset, "UTC+05:45"));
    assert(!strcmp(exact.abbreviation, "+0545"));
    assert(timezone_catalog_lookup_source(
               FIXTURE, strlen(FIXTURE), "Missing/Zone", &exact) != 0);
    assert(timezone_catalog_search_source(
               FIXTURE, strlen(FIXTURE), "", results, 3) == 0);

    puts("timezone catalog tests passed");
    return 0;
}
