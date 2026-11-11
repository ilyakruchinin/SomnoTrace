/*
 * SomnoTrace - host tests for the immutable source store
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "source_store.h"

static int checks, fails;
static char root[256];

#define CHECK(cond, msg) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAILED: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void fresh(void)
{
    snprintf(root, sizeof(root), "/tmp/source_store_test_XXXXXX");
    if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    CHECK(source_store_init(root) == ESP_OK, "store init");
}

static void cleanup(void)
{
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "warn: cleanup failed\n");
}

static void test_put_get_dedup(void)
{
    fresh();
    const char *text = "hello immutable source";
    source_store_put_result_t a, b;
    CHECK(source_store_put("str", (const uint8_t *)text, strlen(text), &a) == ESP_OK,
          "first put");
    CHECK(strlen(a.id) == SOURCE_STORE_ID_HEX, "content id length");
    CHECK(a.length == strlen(text) && !a.existed, "first put metadata");
    CHECK(source_store_put("str", (const uint8_t *)text, strlen(text), &b) == ESP_OK,
          "duplicate put");
    CHECK(b.existed && !strcmp(a.id, b.id), "duplicate reuses the object");

    uint8_t buf[64];
    size_t len = 0;
    CHECK(source_store_get(a.id, buf, sizeof(buf), &len) == ESP_OK,
          "get object");
    CHECK(len == strlen(text) && memcmp(buf, text, len) == 0, "get content");
    CHECK(source_store_verify(a.id, strlen(text)) == ESP_OK, "verify object");
    CHECK(source_store_verify(a.id, strlen(text) + 1) == ESP_ERR_INVALID_SIZE,
          "verify length mismatch");

    char src[420];
    snprintf(src, sizeof(src), "%s/input.bin", root);
    write_text(src, "file-backed object");
    source_store_put_result_t f;
    CHECK(source_store_put_path("root", src, &f) == ESP_OK,
          "put path");
    CHECK(source_store_get(f.id, buf, sizeof(buf), &len) == ESP_OK &&
          len == strlen("file-backed object") &&
          memcmp(buf, "file-backed object", len) == 0,
          "put path content");
    cleanup();
}

static void test_observations(void)
{
    fresh();
    const char *text = "native STR bytes";
    source_store_put_result_t p;
    CHECK(source_store_put("str", (const uint8_t *)text, strlen(text), &p) == ESP_OK,
          "put for observation");
    cJSON *obs = cJSON_Parse("{\"origin\":\"bridge\",\"serial\":\"fixture\"}");
    CHECK(obs != NULL, "observation JSON");
    CHECK(source_store_observe(p.id, "obs-1", "str", obs) == ESP_OK,
          "write observation");
    CHECK(source_store_observe(p.id, "obs-1", "str", obs) == ESP_OK,
          "idempotent observation");
    CHECK(source_store_has_observation(p.id, "obs-1", "str"),
          "committed observation can be found");
    CHECK(!source_store_has_observation(p.id, "obs-1", "other"),
          "observation kind must match");
    cJSON *other = cJSON_Parse("{\"origin\":\"bridge\",\"serial\":\"other\"}");
    CHECK(other != NULL, "second observation JSON");
    CHECK(source_store_observe(p.id, "obs-1", "str", other) == ESP_ERR_INVALID_STATE,
          "different observation under same id rejected");
    CHECK(source_store_observe(p.id, "../escape", "str", obs) == ESP_ERR_INVALID_ARG,
          "unsafe observation id rejected");
    char missing[SOURCE_STORE_ID_HEX + 1];
    memset(missing, '0', SOURCE_STORE_ID_HEX);
    missing[SOURCE_STORE_ID_HEX] = '\0';
    CHECK(source_store_observe(missing, "obs-2", "str", obs) == ESP_ERR_NOT_FOUND,
          "observation for missing object rejected");
    cJSON_Delete(other);
    cJSON_Delete(obs);
    cleanup();
}

static void test_corruption(void)
{
    fresh();
    const char *text = "bytes that will be corrupted";
    source_store_put_result_t p;
    CHECK(source_store_put("str", (const uint8_t *)text, strlen(text), &p) == ESP_OK,
          "put before corruption");
    char path[420];
    snprintf(path, sizeof(path), "%s/objects/%s", root, p.id);
    write_text(path, "bad");
    uint8_t buf[64];
    size_t len = 0;
    CHECK(source_store_get(p.id, buf, sizeof(buf), &len) == ESP_ERR_INVALID_CRC,
          "corrupt object rejected on get");
    esp_err_t vr = source_store_verify(p.id, strlen(text));
    CHECK(vr == ESP_ERR_INVALID_CRC || vr == ESP_ERR_INVALID_SIZE,
          "corrupt object rejected on verify");
    char missing[SOURCE_STORE_ID_HEX + 1];
    memset(missing, 'f', SOURCE_STORE_ID_HEX);
    missing[SOURCE_STORE_ID_HEX] = '\0';
    CHECK(source_store_get(missing, buf, sizeof(buf), &len) == ESP_ERR_NOT_FOUND,
          "missing object reported");
    cleanup();
}

static void test_prune(void)
{
    fresh();
    const char *kept = "referenced bytes";
    const char *orphan = "orphan bytes";
    source_store_put_result_t k, o;
    CHECK(source_store_put("str", (const uint8_t *)kept, strlen(kept), &k) == ESP_OK,
          "put referenced object");
    CHECK(source_store_put("str", (const uint8_t *)orphan, strlen(orphan), &o) == ESP_OK,
          "put orphan object");
    cJSON *obs = cJSON_Parse("{\"origin\":\"bridge\"}");
    CHECK(obs != NULL, "prune observation JSON");
    CHECK(source_store_observe(k.id, "obs-1", "str", obs) == ESP_OK,
          "reference kept object");
    cJSON_Delete(obs);

    int count = 0, removed = 0;
    CHECK(source_store_count_objects(&count) == ESP_OK && count == 2,
          "two objects before prune");
    CHECK(source_store_prune_orphans(&removed) == ESP_OK && removed == 1,
          "prune removes only the orphan");
    CHECK(source_store_count_objects(&count) == ESP_OK && count == 1,
          "one object after prune");
    CHECK(source_store_verify(k.id, strlen(kept)) == ESP_OK,
          "referenced object survives");
    CHECK(source_store_verify(o.id, strlen(orphan)) == ESP_ERR_NOT_FOUND,
          "orphan object removed");
    cleanup();
}

static void test_find_and_copy(void)
{
    fresh();
    const char *a = "first STR bytes";
    const char *b = "second STR bytes";
    source_store_put_result_t pa, pb;
    CHECK(source_store_put("str", (const uint8_t *)a, strlen(a), &pa) == ESP_OK,
          "put first find object");
    cJSON *oa = cJSON_Parse("{\"key\":\"STR.edf\",\"size\":\"1024\","
                            "\"ts\":\"t1\",\"observed_at_ms\":100}");
    CHECK(oa && source_store_observe(pa.id, "obs-a", "str", oa) == ESP_OK,
          "observe first find object");
    cJSON_Delete(oa);
    char id[SOURCE_STORE_ID_HEX + 1], obs[128];
    CHECK(source_store_find("str", "STR.edf", "1024", "t1", false,
                            id, sizeof(id), obs, sizeof(obs)) == ESP_OK,
          "find first tuple");
    CHECK(!strcmp(id, pa.id) && !strcmp(obs, "obs-a"), "find returns first object");

    CHECK(source_store_find("str", "STR.edf", "1024", "t2", false,
                            id, sizeof(id), obs, sizeof(obs)) == ESP_ERR_NOT_FOUND,
          "wrong timestamp is not found");
    CHECK(source_store_find("str", "other.edf", "1024", "t1", false,
                            id, sizeof(id), obs, sizeof(obs)) == ESP_ERR_NOT_FOUND,
          "wrong key is not found");

    CHECK(source_store_put("str", (const uint8_t *)b, strlen(b), &pb) == ESP_OK,
          "put second find object");
    cJSON *ob = cJSON_Parse("{\"key\":\"STR.edf\",\"size\":\"1024\","
                            "\"ts\":\"t1\",\"observed_at_ms\":200}");
    CHECK(ob && source_store_observe(pb.id, "obs-b", "str", ob) == ESP_OK,
          "observe second find object");
    cJSON_Delete(ob);
    CHECK(source_store_find("str", "STR.edf", "1024", "t1", false,
                            id, sizeof(id), obs, sizeof(obs)) == ESP_OK,
          "find newest tuple");
    CHECK(!strcmp(id, pb.id) && !strcmp(obs, "obs-b"), "newest object wins");

    const char *c = "third STR bytes";
    source_store_put_result_t pc;
    CHECK(source_store_put("str", (const uint8_t *)c, strlen(c), &pc) == ESP_OK,
          "put ambiguous find object");
    cJSON *oc = cJSON_Parse("{\"key\":\"STR.edf\",\"size\":\"1024\","
                            "\"ts\":\"t1\",\"observed_at_ms\":200}");
    CHECK(oc && source_store_observe(pc.id, "obs-c", "str", oc) == ESP_OK,
          "observe ambiguous find object");
    cJSON_Delete(oc);
    CHECK(source_store_find("str", "STR.edf", "1024", "t1", false,
                            id, sizeof(id), obs, sizeof(obs)) == ESP_ERR_INVALID_STATE,
          "equally-new disagreement is ambiguous");

    const char *d = "null timestamp bytes";
    source_store_put_result_t pd;
    CHECK(source_store_put("str", (const uint8_t *)d, strlen(d), &pd) == ESP_OK,
          "put null-ts object");
    cJSON *od = cJSON_Parse("{\"key\":\"CurrentSettings.json\",\"size\":\"3072\","
                            "\"ts\":null,\"observed_at_ms\":300}");
    CHECK(od && source_store_observe(pd.id, "obs-null", "str", od) == ESP_OK,
          "observe null-ts object");
    cJSON_Delete(od);
    CHECK(source_store_find("str", "CurrentSettings.json", "3072", NULL, true,
                            id, sizeof(id), obs, sizeof(obs)) == ESP_OK,
          "find null timestamp");
    CHECK(!strcmp(id, pd.id) && !strcmp(obs, "obs-null"), "null-ts object selected");

    char dest[420];
    snprintf(dest, sizeof(dest), "%s/staged-root.bin", root);
    CHECK(source_store_copy_to(pa.id, dest) == ESP_OK, "copy object to staging");
    FILE *f = fopen(dest, "rb");
    CHECK(f != NULL, "staged copy exists");
    if (f) {
        char buf[64] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(n == strlen(a) && memcmp(buf, a, n) == 0, "staged copy content");
    }
    cleanup();
}

static void test_budget(void)
{
    fresh();
    CHECK(source_store_set_budget(20) == ESP_OK, "set small budget");
    source_store_put_result_t a, b, c;
    const char *x = "twelve bytes";
    const char *y = "other 12 bty";
    CHECK(source_store_put("str", (const uint8_t *)x, strlen(x), &a) == ESP_OK,
          "first object fits budget");
    CHECK(source_store_put("str", (const uint8_t *)y, strlen(y), &b) == ESP_ERR_NO_MEM,
          "object over budget refused");
    CHECK(source_store_put("str", (const uint8_t *)x, strlen(x), &c) == ESP_OK &&
          c.existed, "existing object reuse allowed at budget");
    uint64_t total = 0;
    CHECK(source_store_total_bytes(&total) == ESP_OK && total == strlen(x),
          "total counts only stored objects");
    CHECK(source_store_verify(a.id, strlen(x)) == ESP_OK,
          "original object survives refusal");
    CHECK(source_store_set_budget(SOURCE_STORE_DEFAULT_BUDGET) == ESP_OK,
          "restore default budget");
    cleanup();
}

int main(void)
{
    test_put_get_dedup();
    test_observations();
    test_corruption();
    test_prune();
    test_find_and_copy();
    test_budget();
    printf("source_store_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
