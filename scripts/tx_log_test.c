/*
 * SomnoTrace - host tests for the append-only transaction log
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

#include "tx_log.h"

static int checks, fails;
static char dir[256];
static char path[320];

#define CHECK(cond, msg) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAILED: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static void fresh(void)
{
    snprintf(dir, sizeof(dir), "/tmp/tx_log_test_XXXXXX");
    if (!mkdtemp(dir)) { perror("mkdtemp"); exit(2); }
    snprintf(path, sizeof(path), "%s/tx.log", dir);
}

static void cleanup(void)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "warn: cleanup failed\n");
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void test_commit_and_state(void)
{
    fresh();
    tx_log_t *log = NULL;
    bool torn = true;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open new log");
    CHECK(!torn, "new log has no torn tail");
    tx_state_t st;
    CHECK(tx_log_state(log, "root-STR", &st) == ESP_OK && st == TX_STATE_NONE,
          "unknown id is NONE");
    CHECK(tx_log_append(log, TX_PREPARE, "root-STR", "p", 1) == ESP_OK, "prepare");
    CHECK(tx_log_state(log, "root-STR", &st) == ESP_OK && st == TX_STATE_PREPARED,
          "prepared state");
    CHECK(tx_log_append(log, TX_COMMIT, "root-STR", NULL, 0) == ESP_OK, "commit");
    CHECK(tx_log_state(log, "root-STR", &st) == ESP_OK && st == TX_STATE_COMMITTED,
          "committed state");
    uint64_t n = 0, seq = 0;
    CHECK(tx_log_count(log, &n, &seq) == ESP_OK && n == 2 && seq == 2,
          "two records, sequence 2");
    tx_log_close(log);

    log = NULL;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK && !torn,
          "reopen replays cleanly");
    CHECK(tx_log_state(log, "root-STR", &st) == ESP_OK && st == TX_STATE_COMMITTED,
          "committed state survives reopen");
    CHECK(tx_log_count(log, &n, &seq) == ESP_OK && n == 2 && seq == 2,
          "counts survive reopen");
    tx_log_close(log);
    cleanup();
}

static void test_illegal_transitions(void)
{
    fresh();
    tx_log_t *log = NULL;
    bool torn;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open");
    CHECK(tx_log_append(log, TX_COMMIT, "a", NULL, 0) == ESP_ERR_INVALID_STATE,
          "commit without prepare is refused");
    CHECK(tx_log_append(log, TX_PREPARE, "a", NULL, 0) == ESP_OK, "prepare a");
    CHECK(tx_log_append(log, TX_PREPARE, "a", NULL, 0) == ESP_ERR_INVALID_STATE,
          "second prepare is refused");
    CHECK(tx_log_append(log, TX_ABORT, "a", NULL, 0) == ESP_OK, "abort a");
    CHECK(tx_log_append(log, TX_COMMIT, "a", NULL, 0) == ESP_ERR_INVALID_STATE,
          "commit after abort is refused");
    uint64_t n = 0, seq = 0;
    CHECK(tx_log_count(log, &n, &seq) == ESP_OK && n == 2,
          "refused transitions write nothing");
    tx_log_close(log);
    cleanup();
}

static void test_torn_tail_is_truncated(void)
{
    fresh();
    tx_log_t *log = NULL;
    bool torn;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open");
    CHECK(tx_log_append(log, TX_PREPARE, "b", "payload", 7) == ESP_OK, "prepare b");
    CHECK(tx_log_append(log, TX_COMMIT, "b", NULL, 0) == ESP_OK, "commit b");
    tx_log_close(log);
    long good = file_size(path);

    FILE *f = fopen(path, "ab");
    CHECK(f != NULL, "open for torn append");
    if (f) {
        const unsigned char half[] = { 0x47, 0x4c, 0x58, 0x54, 0x01, 0x00 };
        fwrite(half, 1, sizeof(half), f);
        fclose(f);
    }
    CHECK(file_size(path) > good, "partial record present before recovery");

    log = NULL;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "reopen after torn append");
    CHECK(torn, "torn tail reported");
    CHECK(file_size(path) == good, "torn tail truncated to the valid prefix");
    tx_state_t st;
    CHECK(tx_log_state(log, "b", &st) == ESP_OK && st == TX_STATE_COMMITTED,
          "committed prefix intact after truncation");
    tx_log_close(log);
    cleanup();
}

static void test_middle_corruption_is_not_skipped(void)
{
    fresh();
    tx_log_t *log = NULL;
    bool torn;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open");
    CHECK(tx_log_append(log, TX_PREPARE, "c", "xxxx", 4) == ESP_OK, "prepare c");
    CHECK(tx_log_append(log, TX_COMMIT, "c", NULL, 0) == ESP_OK, "commit c");
    tx_log_close(log);

    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL, "open for corruption");
    if (f) {
        fseek(f, 20, SEEK_SET);   /* inside the first record's payload */
        int c = fgetc(f);
        fseek(f, 20, SEEK_SET);
        fputc(c ^ 0x5a, f);
        fclose(f);
    }
    long before = file_size(path);
    log = NULL;
    CHECK(tx_log_open(path, &log, &torn) == ESP_ERR_INVALID_CRC,
          "middle corruption is reported, not truncated");
    CHECK(file_size(path) == before, "corrupt file left untouched");
    CHECK(log == NULL, "no log handle returned on corruption");
    cleanup();
}

static void test_append_failure_leaves_no_committed_record(void)
{
    fresh();
    tx_log_t *log = NULL;
    bool torn;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open");
    CHECK(tx_log_append(log, TX_PREPARE, "d", NULL, 0) == ESP_OK, "prepare d");
    tx_log_fault_after(1);
    CHECK(tx_log_append(log, TX_COMMIT, "d", NULL, 0) == ESP_FAIL,
          "injected write failure is reported");
    tx_state_t st;
    CHECK(tx_log_state(log, "d", &st) == ESP_OK && st == TX_STATE_PREPARED,
          "failed commit is not visible");
    CHECK(tx_log_append(log, TX_COMMIT, "d", NULL, 0) == ESP_OK,
          "retry after failure succeeds");
    CHECK(tx_log_state(log, "d", &st) == ESP_OK && st == TX_STATE_COMMITTED,
          "retried commit is durable");
    tx_log_close(log);
    cleanup();
}

static void test_recovers_from_interrupted_truncation(void)
{
    fresh();
    tx_log_t *log = NULL;
    bool torn;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open");
    CHECK(tx_log_append(log, TX_PREPARE, "e", NULL, 0) == ESP_OK, "prepare e");
    CHECK(tx_log_append(log, TX_COMMIT, "e", NULL, 0) == ESP_OK, "commit e");
    tx_log_close(log);

    /* Simulate a crash between the two renames: the valid log sits at .bak
     * and the live path is missing. */
    char bak[324];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    CHECK(rename(path, bak) == 0, "stage backup");
    log = NULL;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open restores backup");
    tx_state_t st;
    CHECK(tx_log_state(log, "e", &st) == ESP_OK && st == TX_STATE_COMMITTED,
          "backup restored with committed state");
    tx_log_close(log);
    cleanup();
}

static void test_replay_allocation_failure(void)
{
    fresh();
    tx_log_t *log = NULL;
    bool torn;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open");
    if (!log) { cleanup(); return; }
    CHECK(tx_log_append(log, TX_PREPARE, "alloc", NULL, 0) == ESP_OK,
          "prepare allocation test record");
    long before = file_size(path);

    tx_state_t st;
    tx_log_alloc_fault_after(2);
    CHECK(tx_log_state(log, "alloc", &st) == ESP_ERR_NO_MEM,
          "replay table allocation failure reported");
    tx_log_alloc_fault_after(2);
    CHECK(tx_log_append(log, TX_COMMIT, "alloc", NULL, 0) == ESP_ERR_NO_MEM,
          "append refuses to proceed when replay allocation fails");
    CHECK(file_size(path) == before, "failed replay does not change log");
    CHECK(tx_log_state(log, "alloc", &st) == ESP_OK && st == TX_STATE_PREPARED,
          "state remains intact after allocation failure");
    tx_log_close(log);

    for (int allocation = 1; allocation <= 3; allocation++) {
        log = NULL;
        tx_log_alloc_fault_after(allocation);
        CHECK(tx_log_open(path, &log, &torn) == ESP_ERR_NO_MEM && !log,
              "open reports allocation failure without returning a handle");
        CHECK(file_size(path) == before, "failed open leaves log untouched");
    }
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK && log,
          "open succeeds after allocation failures");
    if (log) {
        CHECK(tx_log_state(log, "alloc", &st) == ESP_OK && st == TX_STATE_PREPARED,
              "record survives allocation failures");
        tx_log_close(log);
    }
    cleanup();
}

static void test_full_replay_table(void)
{
    fresh();
    tx_log_t *log = NULL;
    bool torn;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK, "open");
    if (!log) { cleanup(); return; }
    bool ok = true;
    for (int i = 0; i < 256; i++) {
        char id[16];
        snprintf(id, sizeof(id), "id-%03d", i);
        if (tx_log_append(log, TX_PREPARE, id, NULL, 0) != ESP_OK) {
            ok = false;
            break;
        }
    }
    CHECK(ok, "populate all 256 replay table entries");
    tx_log_close(log);
    log = NULL;
    CHECK(tx_log_open(path, &log, &torn) == ESP_OK && log && !torn,
          "reopen full replay table");
    if (log) {
        tx_state_t st;
        CHECK(tx_log_state(log, "id-255", &st) == ESP_OK && st == TX_STATE_PREPARED,
              "last replay table entry is intact");
        tx_log_close(log);
    }
    cleanup();
}

int main(void)
{
    test_commit_and_state();
    test_illegal_transitions();
    test_torn_tail_is_truncated();
    test_middle_corruption_is_not_skipped();
    test_append_failure_leaves_no_committed_record();
    test_recovers_from_interrupted_truncation();
    test_replay_allocation_failure();
    test_full_replay_table();
    printf("tx_log_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
