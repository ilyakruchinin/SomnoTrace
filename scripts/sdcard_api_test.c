/*
 * SomnoTrace - Host unit tests for read-only SD export path guard & walker
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdcard_api_core.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char paths[32][SDCARD_API_MAX_RELPATH];
    uint64_t sizes[32];
    int count;
    int abort_after;
    int abort_rc;
} walk_collect_t;

static int collect_cb(const char *relpath, uint64_t size, int64_t mtime, void *ctx)
{
    (void)mtime;
    walk_collect_t *c = (walk_collect_t *)ctx;
    assert(c->count < 32);
    snprintf(c->paths[c->count], sizeof(c->paths[c->count]), "%s", relpath);
    c->sizes[c->count] = size;
    c->count++;
    if (c->abort_after > 0 && c->count >= c->abort_after) {
        return c->abort_rc;
    }
    return 0;
}

static int has_path(const walk_collect_t *c, const char *p)
{
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->paths[i], p) == 0) return i;
    }
    return -1;
}

static void write_file_bytes(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (len > 0) assert(fwrite(data, 1, len, f) == len);
    assert(fclose(f) == 0);
}

static void test_relpath_valid(void)
{
    assert(sdcard_api_relpath_valid("STR.edf"));
    assert(sdcard_api_relpath_valid("Identification.json"));
    assert(sdcard_api_relpath_valid("SETTINGS/20261124.tgt"));
    assert(sdcard_api_relpath_valid("DATALOG/20261124/20261124_220000_BRP.edf"));

    assert(!sdcard_api_relpath_valid(NULL));
    assert(!sdcard_api_relpath_valid(""));
    assert(!sdcard_api_relpath_valid("/"));
    assert(!sdcard_api_relpath_valid("/STR.edf"));
    assert(!sdcard_api_relpath_valid("STR.edf/"));
    assert(!sdcard_api_relpath_valid("."));
    assert(!sdcard_api_relpath_valid(".."));
    assert(!sdcard_api_relpath_valid("../secret"));
    assert(!sdcard_api_relpath_valid("DATALOG/../STR.edf"));
    assert(!sdcard_api_relpath_valid("DATALOG/.."));
    assert(!sdcard_api_relpath_valid("DATALOG//STR.edf"));
    assert(!sdcard_api_relpath_valid("DATALOG/./STR.edf"));
    assert(!sdcard_api_relpath_valid(".somnotrace/logs"));
    assert(!sdcard_api_relpath_valid("DATALOG/.hidden"));
    assert(!sdcard_api_relpath_valid("DATALOG\\STR.edf"));
    assert(!sdcard_api_relpath_valid("DATALOG\nSTR.edf"));
    assert(!sdcard_api_relpath_valid("DATALOG\x1fSTR.edf"));
    assert(!sdcard_api_relpath_valid("DATALOG\x7fSTR.edf"));

    char too_long[SDCARD_API_MAX_RELPATH + 8];
    memset(too_long, 'a', SDCARD_API_MAX_RELPATH);
    too_long[SDCARD_API_MAX_RELPATH] = '\0';
    assert(!sdcard_api_relpath_valid(too_long));
    too_long[SDCARD_API_MAX_RELPATH - 1] = '\0';
    assert(sdcard_api_relpath_valid(too_long));
}

static void test_build_path(void)
{
    char out[256];
    assert(sdcard_api_build_path("/somnotrace/SDCARD", "STR.edf", out, sizeof(out)));
    assert(strcmp(out, "/somnotrace/SDCARD/STR.edf") == 0);

    assert(sdcard_api_build_path("/somnotrace/SDCARD/", "DATALOG/20261124/a.edf", out, sizeof(out)));
    assert(strcmp(out, "/somnotrace/SDCARD/DATALOG/20261124/a.edf") == 0);

    assert(!sdcard_api_build_path("/somnotrace/SDCARD", "../evil", out, sizeof(out)));
    assert(!sdcard_api_build_path("", "STR.edf", out, sizeof(out)));
    assert(!sdcard_api_build_path(NULL, "STR.edf", out, sizeof(out)));

    char tiny[12];
    assert(!sdcard_api_build_path("/somnotrace/SDCARD", "STR.edf", tiny, sizeof(tiny)));
}

static void test_json_escape(void)
{
    char dst[64];
    int n = sdcard_api_json_escape("DATALOG/20261124/a.edf", dst, sizeof(dst));
    assert(n == 22);
    assert(strcmp(dst, "DATALOG/20261124/a.edf") == 0);

    n = sdcard_api_json_escape("a\"b\\c\n\t\x01", dst, sizeof(dst));
    assert(n > 0);
    assert(strcmp(dst, "a\\\"b\\\\c\\n\\t\\u0001") == 0);

    char small[4];
    assert(sdcard_api_json_escape("abcd", small, sizeof(small)) == -1);
    assert(sdcard_api_json_escape("a\"", small, sizeof(small)) == 3);
    assert(sdcard_api_json_escape("ab\"", small, sizeof(small)) == -1);
    assert(sdcard_api_json_escape("ab", small, sizeof(small)) == 2);
}

static void test_walk_tree(void)
{
    char tmpl[] = "/tmp/st_sdcard_api_XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root != NULL);

    char p[512];
    snprintf(p, sizeof(p), "%s/STR.edf", root);
    write_file_bytes(p, "STRDATA", 7);

    snprintf(p, sizeof(p), "%s/.hidden", root);
    write_file_bytes(p, "SECRET", 6);

    snprintf(p, sizeof(p), "%s/.somnotrace", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/.somnotrace/token.txt", root);
    write_file_bytes(p, "PRIVATE", 7);

    snprintf(p, sizeof(p), "%s/DATALOG", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/DATALOG/20261124", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/DATALOG/20261124/BRP.edf", root);
    write_file_bytes(p, "BRP123", 6);

    /* Create nested levels up to depth 5: depth 4 is enumerated, depth 5 skipped */
    snprintf(p, sizeof(p), "%s/d1", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/d1/d2", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/d1/d2/d3", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/d1/d2/d3/d4", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/d1/d2/d3/d4/ok.txt", root);
    write_file_bytes(p, "OK", 2);
    snprintf(p, sizeof(p), "%s/d1/d2/d3/d4/d5", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/d1/d2/d3/d4/d5/too_deep.txt", root);
    write_file_bytes(p, "NOPE", 4);

    walk_collect_t *all = calloc(1, sizeof(*all));
    walk_collect_t *sub = calloc(1, sizeof(*sub));
    walk_collect_t *abort_ctx = calloc(1, sizeof(*abort_ctx));
    assert(all && sub && abort_ctx);

    assert(sdcard_api_walk(root, NULL, collect_cb, all) == 0);
    int idx_str = has_path(all, "STR.edf");
    int idx_brp = has_path(all, "DATALOG/20261124/BRP.edf");
    int idx_ok = has_path(all, "d1/d2/d3/d4/ok.txt");
    assert(idx_str >= 0 && all->sizes[idx_str] == 7);
    assert(idx_brp >= 0 && all->sizes[idx_brp] == 6);
    assert(idx_ok >= 0 && all->sizes[idx_ok] == 2);
    assert(has_path(all, ".hidden") == -1);
    assert(has_path(all, ".somnotrace/token.txt") == -1);
    assert(has_path(all, "d1/d2/d3/d4/d5/too_deep.txt") == -1);
    assert(all->count == 3);

    assert(sdcard_api_walk(root, "DATALOG/20261124", collect_cb, sub) == 0);
    assert(sub->count == 1);
    assert(strcmp(sub->paths[0], "DATALOG/20261124/BRP.edf") == 0);

    abort_ctx->abort_after = 1;
    abort_ctx->abort_rc = -EIO;
    assert(sdcard_api_walk(root, NULL, collect_cb, abort_ctx) == -EIO);
    assert(abort_ctx->count == 1);

    assert(sdcard_api_walk(root, "../evil", collect_cb, sub) == -EINVAL);
    assert(sdcard_api_walk(root, "MISSING_DIR", collect_cb, sub) == -ENOENT);

    free(abort_ctx);
    free(sub);
    free(all);

    /* Cleanup temp tree */
    const char *files[] = {
        "STR.edf", ".hidden", ".somnotrace/token.txt",
        "DATALOG/20261124/BRP.edf", "d1/d2/d3/d4/ok.txt", "d1/d2/d3/d4/d5/too_deep.txt"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", root, files[i]);
        unlink(p);
    }
    const char *dirs[] = {
        "d1/d2/d3/d4/d5", "d1/d2/d3/d4", "d1/d2/d3", "d1/d2", "d1",
        "DATALOG/20261124", "DATALOG", ".somnotrace"
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", root, dirs[i]);
        rmdir(p);
    }
    rmdir(root);
}

int main(void)
{
    test_relpath_valid();
    test_build_path();
    test_json_escape();
    test_walk_tree();
    puts("sdcard_api_test: all tests passed");
    return 0;
}
