/*
 * SomnoTrace - Append-only transaction log with CRC and sequence numbers
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

#include "tx_log.h"

#include "esp_log.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TX_LOG_HOST
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *TAG = "tx_log";

#define TX_MAGIC 0x54584c47u   /* "TXLG" */
#define TX_HDR_LEN 16          /* magic4 seq8 type1 id_len1 payload_len2 */
#define TX_MAX_RECORD (TX_HDR_LEN + TX_LOG_ID_MAX + TX_LOG_PAYLOAD_MAX + 4)

struct tx_log {
    char     path[300];
    uint64_t last_seq;
    uint64_t records;
};

#ifdef TX_LOG_FAULT_INJECT
static int s_fault_countdown;
static int s_alloc_fault_countdown;
void tx_log_fault_after(int writes_until_failure) { s_fault_countdown = writes_until_failure; }
void tx_log_alloc_fault_after(int allocations_until_failure) { s_alloc_fault_countdown = allocations_until_failure; }
static bool fault_hit(void)
{
    if (s_fault_countdown <= 0) return false;
    return --s_fault_countdown == 0;
}
#else
static bool fault_hit(void) { return false; }
#endif

static void *tx_alloc(size_t size)
{
#ifdef TX_LOG_FAULT_INJECT
    if (s_alloc_fault_countdown > 0 && --s_alloc_fault_countdown == 0)
        return NULL;
#endif
#ifdef TX_LOG_HOST
    return malloc(size);
#else
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static uint32_t crc32_bytes(const uint8_t *d, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

typedef struct {
    uint64_t seq;
    tx_type_t type;
    char id[TX_LOG_ID_MAX + 1];
    uint8_t payload[TX_LOG_PAYLOAD_MAX];
    size_t payload_len;
    size_t length;   /* whole record length on disk */
} tx_rec_t;

typedef enum { REC_OK, REC_INCOMPLETE, REC_BAD } rec_status_t;

/* Parse one record at buf[0..avail).  INCOMPLETE means the bytes end inside
 * the record (a torn append); BAD means a complete record that fails a check. */
static rec_status_t parse_record(const uint8_t *buf, size_t avail, tx_rec_t *r)
{
    if (avail < TX_HDR_LEN) return REC_INCOMPLETE;
    if (get_u32(buf) != TX_MAGIC) return REC_BAD;
    uint8_t id_len = buf[13];
    uint16_t plen = (uint16_t)(buf[14] | (buf[15] << 8));
    if (id_len == 0 || id_len > TX_LOG_ID_MAX || plen > TX_LOG_PAYLOAD_MAX) return REC_BAD;
    size_t total = TX_HDR_LEN + id_len + plen + 4;
    if (avail < total) return REC_INCOMPLETE;
    if (get_u32(buf + total - 4) != crc32_bytes(buf, total - 4)) return REC_BAD;
    uint8_t type = buf[12];
    if (type < TX_PREPARE || type > TX_ABORT) return REC_BAD;
    r->seq = get_u64(buf + 4);
    r->type = (tx_type_t)type;
    memcpy(r->id, buf + TX_HDR_LEN, id_len);
    r->id[id_len] = '\0';
    memcpy(r->payload, buf + TX_HDR_LEN + id_len, plen);
    r->payload_len = plen;
    r->length = total;
    return REC_OK;
}

static tx_state_t next_state(tx_state_t cur, tx_type_t type, bool *legal)
{
    *legal = true;
    switch (type) {
    case TX_PREPARE:
        if (cur == TX_STATE_NONE) return TX_STATE_PREPARED;
        break;
    case TX_COMMIT:
        if (cur == TX_STATE_PREPARED) return TX_STATE_COMMITTED;
        break;
    case TX_ABORT:
        if (cur == TX_STATE_PREPARED) return TX_STATE_ABORTED;
        break;
    }
    *legal = false;
    return cur;
}

/* Scan the whole file.  Returns the valid prefix length, the record count and
 * the last sequence, and reports the first problem.  The id table is searched
 * linearly: the log is small and this path runs only at open and on state
 * queries. */
typedef struct { char id[TX_LOG_ID_MAX + 1]; tx_state_t state; } id_state_t;

static esp_err_t scan(const char *path, uint8_t **data_out, size_t *len_out,
                      size_t *valid_out, uint64_t *records, uint64_t *last_seq,
                      bool *torn)
{
    *torn = false;
    *valid_out = 0;
    *records = 0;
    *last_seq = 0;
    *data_out = NULL;
    *len_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return ESP_OK;
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }
    uint8_t *buf = tx_alloc((size_t)n + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) { free(buf); return ESP_FAIL; }
    *data_out = buf;
    *len_out = (size_t)n;

    if (n == 0) return ESP_OK;

    /* Replay can run on the 8 KiB bridge task stack.  Keep the bounded
     * transaction table in PSRAM, with no internal-RAM fallback. */
    id_state_t *table = tx_alloc(256 * sizeof(*table));
    if (!table) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    size_t off = 0;
    size_t n_ids = 0;
    while (off < (size_t)n) {
        tx_rec_t r;
        rec_status_t st = parse_record(buf + off, (size_t)n - off, &r);
        if (st == REC_INCOMPLETE) { *torn = true; break; }
        if (st == REC_BAD || r.seq != *last_seq + 1) {
            result = ESP_ERR_INVALID_CRC;
            break;
        }
        size_t i = 0;
        while (i < n_ids && strcmp(table[i].id, r.id) != 0) i++;
        if (i == n_ids) {
            if (n_ids >= 256) {
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            snprintf(table[n_ids].id, sizeof(table[n_ids].id), "%s", r.id);
            table[n_ids].state = TX_STATE_NONE;
            n_ids++;
        }
        bool legal;
        table[i].state = next_state(table[i].state, r.type, &legal);
        if (!legal) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        off += r.length;
        *last_seq = r.seq;
        (*records)++;
        *valid_out = off;
    }
    free(table);
    return result;
}

/* Replace path with its valid prefix, keeping the old file until the new one
 * is complete.  A crash between the two renames leaves path.bak, which open
 * restores. */
static esp_err_t truncate_to(const char *path, const uint8_t *data, size_t valid)
{
    char tmp[320], bak[320];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    bool ok = valid == 0 || fwrite(data, 1, valid, f) == valid;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) { unlink(tmp); return ESP_FAIL; }
    if (rename(path, bak) != 0) { unlink(tmp); return ESP_FAIL; }
    if (rename(tmp, path) != 0) {
        rename(bak, path);
        return ESP_FAIL;
    }
    unlink(bak);
    return ESP_OK;
}

esp_err_t tx_log_open(const char *path, tx_log_t **out, bool *torn_tail)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    if (torn_tail) *torn_tail = false;
    *out = NULL;

    /* A crash during truncate_to leaves only the backup: restore it. */
    char bak[320];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    struct stat st;
    if (stat(path, &st) != 0 && stat(bak, &st) == 0)
        if (rename(bak, path) != 0) return ESP_FAIL;

    uint8_t *data = NULL;
    size_t len = 0, valid = 0;
    uint64_t records = 0, last_seq = 0;
    bool torn = false;
    esp_err_t r = scan(path, &data, &len, &valid, &records, &last_seq, &torn);
    if (r != ESP_OK) { free(data); return r; }
    if (torn) {
        r = truncate_to(path, data, valid);
        if (r != ESP_OK) { free(data); return r; }
        if (torn_tail) *torn_tail = true;
    }
    free(data);

    tx_log_t *log = tx_alloc(sizeof(*log));
    if (!log) return ESP_ERR_NO_MEM;
    memset(log, 0, sizeof(*log));
    snprintf(log->path, sizeof(log->path), "%s", path);
    log->last_seq = last_seq;
    log->records = records;
    *out = log;
    return ESP_OK;
}

void tx_log_close(tx_log_t *log) { free(log); }

esp_err_t tx_log_append(tx_log_t *log, tx_type_t type, const char *id,
                        const void *payload, size_t payload_len)
{
    if (!log || !id || !id[0] || strlen(id) > TX_LOG_ID_MAX ||
        payload_len > TX_LOG_PAYLOAD_MAX || (payload_len && !payload))
        return ESP_ERR_INVALID_ARG;

    /* Legality is checked against the replayed state before anything is
     * written, so an illegal transition never reaches the file. */
    tx_state_t cur;
    esp_err_t sr = tx_log_state(log, id, &cur);
    if (sr != ESP_OK) return sr;
    bool legal;
    next_state(cur, type, &legal);
    if (!legal) return ESP_ERR_INVALID_STATE;

    uint8_t rec[TX_MAX_RECORD];
    size_t id_len = strlen(id);
    size_t total = TX_HDR_LEN + id_len + payload_len + 4;
    put_u32(rec, TX_MAGIC);
    put_u64(rec + 4, log->last_seq + 1);
    rec[12] = (uint8_t)type;
    rec[13] = (uint8_t)id_len;
    rec[14] = (uint8_t)(payload_len & 0xff);
    rec[15] = (uint8_t)(payload_len >> 8);
    memcpy(rec + TX_HDR_LEN, id, id_len);
    if (payload_len) memcpy(rec + TX_HDR_LEN + id_len, payload, payload_len);
    put_u32(rec + total - 4, crc32_bytes(rec, total - 4));

    if (fault_hit()) return ESP_FAIL;
    FILE *f = fopen(log->path, "ab");
    if (!f) {
        ESP_LOGW(TAG, "append: open %s failed (errno=%d)", log->path, errno);
        return ESP_FAIL;
    }
    bool ok = fwrite(rec, 1, total, f) == total;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) return ESP_FAIL;   /* a partial record is a torn tail on next open */
    log->last_seq++;
    log->records++;
    return ESP_OK;
}

esp_err_t tx_log_state(tx_log_t *log, const char *id, tx_state_t *state)
{
    if (!log || !id || !state) return ESP_ERR_INVALID_ARG;
    *state = TX_STATE_NONE;
    uint8_t *data = NULL;
    size_t len = 0, valid = 0;
    uint64_t records = 0, last_seq = 0;
    bool torn = false;
    esp_err_t r = scan(log->path, &data, &len, &valid, &records, &last_seq, &torn);
    if (r != ESP_OK) { free(data); return r; }
    size_t off = 0;
    tx_state_t cur = TX_STATE_NONE;
    while (off < valid) {
        tx_rec_t rec;
        if (parse_record(data + off, valid - off, &rec) != REC_OK) break;
        if (strcmp(rec.id, id) == 0) {
            bool legal;
            cur = next_state(cur, rec.type, &legal);
        }
        off += rec.length;
    }
    free(data);
    *state = cur;
    return ESP_OK;
}

esp_err_t tx_log_count(tx_log_t *log, uint64_t *records, uint64_t *last_seq)
{
    if (!log || !records || !last_seq) return ESP_ERR_INVALID_ARG;
    *records = log->records;
    *last_seq = log->last_seq;
    return ESP_OK;
}

/* The bridge's segmented journal deliberately has a separate API from the
 * legacy single-file log. It never truncates or deletes archived evidence. */
#define JOURNAL_IDS             192u
#define JOURNAL_PENDING          16u
#define JOURNAL_SEGMENT_BYTES   (32u * 1024u)
#define JOURNAL_ARCHIVE_BYTES   (1024u * 1024u)
#define JOURNAL_SCAN_BUDGET     (256u * 1024u * 1024u)

typedef struct {
    uint8_t bytes[TX_MAX_RECORD];
    tx_rec_t record;
} journal_scratch_t;

struct tx_journal {
    char path[300];
    char archive_dir[300];
    id_state_t *seen;
    journal_scratch_t *scratch;
    journal_scratch_t *prior_scratch;
    size_t seen_count;
    size_t distinct;
    size_t pending;
    uint64_t bytes;
    uint64_t records;
    uint64_t last_seq;
    uint64_t archive_bytes;
    uint64_t nonce;
    uint32_t counter;
    bool nonce_set;
    bool counter_exhausted;
    bool allow_writes;
    bool poisoned;
};

typedef enum { JR_RECORD, JR_END, JR_TORN, JR_BAD, JR_IO } journal_read_t;

static journal_read_t journal_read(FILE *f, journal_scratch_t *s,
                                   uint64_t *work)
{
    size_t n = fread(s->bytes, 1, TX_HDR_LEN, f);
    *work += n;
    if (n != TX_HDR_LEN) {
        if (ferror(f)) return JR_IO;
        return n ? JR_TORN : JR_END;
    }
    if (get_u32(s->bytes) != TX_MAGIC) return JR_BAD;
    unsigned id_len = s->bytes[13];
    unsigned plen = (unsigned)s->bytes[14] | ((unsigned)s->bytes[15] << 8);
    if (!id_len || id_len > TX_LOG_ID_MAX || plen > TX_LOG_PAYLOAD_MAX)
        return JR_BAD;
    size_t remain = id_len + plen + 4;
    n = fread(s->bytes + TX_HDR_LEN, 1, remain, f);
    *work += n;
    if (n != remain) return ferror(f) ? JR_IO : JR_TORN;
    if (parse_record(s->bytes, TX_HDR_LEN + remain, &s->record) != REC_OK ||
        memchr(s->bytes + TX_HDR_LEN, '\0', id_len) != NULL)
        return JR_BAD;
    return JR_RECORD;
}

/* Validate an ID's preceding transitions without retaining an unbounded
 * lifetime-ID table. This slow path runs only on oversized legacy segments. */
static esp_err_t journal_prior_state(tx_journal_t *j, long before,
                                     const char *id, tx_state_t *out,
                                     uint64_t *work)
{
    FILE *f = fopen(j->path, "rb");
    if (!f) return ESP_FAIL;
    tx_state_t state = TX_STATE_NONE;
    esp_err_t ret = ESP_OK;
    while (ftell(f) < before) {
        if (*work > JOURNAL_SCAN_BUDGET) { ret = ESP_ERR_INVALID_SIZE; break; }
        journal_read_t r = journal_read(f, j->prior_scratch, work);
        if (r != JR_RECORD) { ret = ESP_ERR_INVALID_CRC; break; }
        if (!strcmp(j->prior_scratch->record.id, id)) {
            bool legal;
            state = next_state(state, j->prior_scratch->record.type, &legal);
            if (!legal) { ret = ESP_ERR_INVALID_STATE; break; }
        }
#ifndef TX_LOG_HOST
        if (((*work >> 16) & 7u) == 0) taskYIELD();
#endif
    }
    if (fclose(f) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    *out = state;
    return ret;
}

/* An incomplete-looking record can also be a corrupt length in the middle
 * of a file. A subsequent complete CRC-valid record makes it corruption,
 * not a tail that can be moved aside as an interrupted append. */
static esp_err_t journal_later_record(tx_journal_t *j, long offset,
                                      uint64_t *work, bool *found)
{
    *found = false;
    FILE *f = fopen(j->path, "rb");
    if (!f) return ESP_FAIL;
    esp_err_t ret = ESP_OK;
    if (fseek(f, offset + 1, SEEK_SET) != 0) ret = ESP_FAIL;
    uint32_t window = 0;
    unsigned have = 0;
    int ch;
    while (ret == ESP_OK && (ch = fgetc(f)) != EOF) {
        (*work)++;
        if (*work > JOURNAL_SCAN_BUDGET) { ret = ESP_ERR_INVALID_SIZE; break; }
#ifndef TX_LOG_HOST
        if ((*work & 4095u) == 0) taskYIELD();
#endif
        window = (window >> 8) | ((uint32_t)(uint8_t)ch << 24);
        if (have < 4) have++;
        if (have < 4 || window != TX_MAGIC) continue;
        long candidate = ftell(f) - 4;
        if (candidate < 0 || fseek(f, candidate, SEEK_SET) != 0) {
            ret = ESP_FAIL;
            break;
        }
        journal_read_t r = journal_read(f, j->prior_scratch, work);
        if (r == JR_IO) { ret = ESP_FAIL; break; }
        if (r == JR_RECORD) { *found = true; break; }
        if (fseek(f, candidate + 4, SEEK_SET) != 0) { ret = ESP_FAIL; break; }
#ifndef TX_LOG_HOST
        taskYIELD();
#endif
    }
    if (ferror(f) && ret == ESP_OK) ret = ESP_FAIL;
    if (fclose(f) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    return ret;
}

static esp_err_t journal_scan_active(tx_journal_t *j, bool *torn)
{
    FILE *f = fopen(j->path, "rb");
    if (!f) return ESP_FAIL;
    j->seen_count = j->distinct = j->pending = 0;
    j->bytes = j->records = j->last_seq = 0;
    *torn = false;
    uint64_t work = 0;
    esp_err_t ret = ESP_OK;
    for (;;) {
        long offset = ftell(f);
        if (offset < 0 || work > JOURNAL_SCAN_BUDGET) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        journal_read_t r = journal_read(f, j->scratch, &work);
        if (r == JR_END) break;
        if (r == JR_TORN) {
            bool later = false;
            ret = journal_later_record(j, offset, &work, &later);
            if (ret == ESP_OK && later) ret = ESP_ERR_INVALID_CRC;
            if (ret == ESP_OK) *torn = true;
            break;
        }
        if (r != JR_RECORD) {
            ret = r == JR_IO ? ESP_FAIL : ESP_ERR_INVALID_CRC;
            break;
        }
        tx_rec_t *rec = &j->scratch->record;
        if (rec->seq != j->last_seq + 1) {
            ret = ESP_ERR_INVALID_CRC;
            break;
        }
        size_t i = 0;
        while (i < j->seen_count && strcmp(j->seen[i].id, rec->id)) i++;
        tx_state_t state = TX_STATE_NONE;
        if (i < j->seen_count) {
            state = j->seen[i].state;
        } else if (j->seen_count == JOURNAL_IDS) {
            ret = journal_prior_state(j, offset, rec->id, &state, &work);
            if (ret != ESP_OK) break;
        }
        bool legal;
        tx_state_t next = next_state(state, rec->type, &legal);
        if (!legal) { ret = ESP_ERR_INVALID_STATE; break; }
        if (state == TX_STATE_NONE) {
            j->distinct++;
            if (j->seen_count < JOURNAL_IDS) {
                i = j->seen_count++;
                snprintf(j->seen[i].id, sizeof(j->seen[i].id), "%s", rec->id);
            }
        }
        if (i < j->seen_count) j->seen[i].state = next;
        if (rec->type == TX_PREPARE) j->pending++;
        else j->pending--;
        j->last_seq = rec->seq;
        j->records++;
        j->bytes += rec->length;
    }
    if (fclose(f) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    return ret;
}

static esp_err_t journal_inventory(tx_journal_t *j)
{
    DIR *dir = opendir(j->archive_dir);
    if (!dir) return ESP_FAIL;
    uint64_t total = 0;
    esp_err_t ret = ESP_OK;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        unsigned long number;
        char tail;
        if (sscanf(ent->d_name, "seg-%8lx.log%c", &number, &tail) != 1 ||
            strlen(ent->d_name) != 16) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        char path[340];
        struct stat st;
        if (snprintf(path, sizeof(path), "%s/%s", j->archive_dir,
                     ent->d_name) >= (int)sizeof(path) ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
            ret = ESP_FAIL;
            break;
        }
        total += (uint64_t)st.st_size;
        errno = 0;
    }
    if (errno && ret == ESP_OK) ret = ESP_FAIL;
    if (closedir(dir) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    if (ret == ESP_OK) j->archive_bytes = total;
    return ret;
}

static esp_err_t journal_create_active(tx_journal_t *j)
{
    if (!j->allow_writes) return ESP_ERR_INVALID_STATE;
    int fd = open(j->path, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd < 0) return ESP_FAIL;
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t journal_rotate(tx_journal_t *j)
{
    if (!j->allow_writes || j->poisoned) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = journal_inventory(j);
    if (ret != ESP_OK) return ret;
    struct stat st;
    if (stat(j->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
        return ESP_FAIL;
    uint64_t size = (uint64_t)st.st_size;
    if (j->archive_bytes > JOURNAL_ARCHIVE_BYTES ||
        size > JOURNAL_ARCHIVE_BYTES - j->archive_bytes)
        return ESP_ERR_INVALID_SIZE;
    char archive[340];
    bool found = false;
    for (uint64_t i = 0; i <= UINT32_MAX; i++) {
        if (snprintf(archive, sizeof(archive), "%s/seg-%08lx.log",
                     j->archive_dir, (unsigned long)i) >= (int)sizeof(archive))
            return ESP_ERR_INVALID_SIZE;
        if (stat(archive, &st) != 0) {
            if (errno != ENOENT) return ESP_FAIL;
            found = true;
            break;
        }
    }
    if (!found) return ESP_ERR_INVALID_SIZE;
    /* The single journal owner checks the unused destination immediately
     * before rename; never intentionally replace an existing archive. */
    if (fault_hit()) return ESP_FAIL;
    j->poisoned = true;
    if (rename(j->path, archive) != 0) return ESP_FAIL;
    if (fault_hit()) return ESP_FAIL;
    ret = journal_create_active(j);
    if (ret != ESP_OK) return ret;
    bool torn;
    ret = journal_scan_active(j, &torn);
    if (ret != ESP_OK || torn || j->records != 0)
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    j->archive_bytes += size;
    j->poisoned = false;
    return ESP_OK;
}

void tx_journal_close(tx_journal_t *j)
{
    if (!j) return;
    free(j->seen);
    free(j->scratch);
    free(j->prior_scratch);
    free(j);
}

esp_err_t tx_journal_open(const char *path, const char *archive_dir,
                          bool allow_writes, tx_journal_t **out,
                          bool *archived_unfinished)
{
    if (!path || !archive_dir || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (archived_unfinished) *archived_unfinished = false;
    tx_journal_t *j = tx_alloc(sizeof(*j));
    if (!j) return ESP_ERR_NO_MEM;
    memset(j, 0, sizeof(*j));
    j->seen = tx_alloc(JOURNAL_IDS * sizeof(*j->seen));
    j->scratch = tx_alloc(sizeof(*j->scratch));
    j->prior_scratch = tx_alloc(sizeof(*j->prior_scratch));
    if (!j->seen || !j->scratch || !j->prior_scratch) {
        tx_journal_close(j);
        return ESP_ERR_NO_MEM;
    }
    if (snprintf(j->path, sizeof(j->path), "%s", path) >= (int)sizeof(j->path) ||
        snprintf(j->archive_dir, sizeof(j->archive_dir), "%s", archive_dir) >=
            (int)sizeof(j->archive_dir)) {
        tx_journal_close(j);
        return ESP_ERR_INVALID_SIZE;
    }
    j->allow_writes = allow_writes;
    struct stat st;
    if (stat(j->archive_dir, &st) != 0) {
        if (errno != ENOENT || !allow_writes || mkdir(j->archive_dir, 0775) != 0) {
            tx_journal_close(j);
            return ESP_FAIL;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        tx_journal_close(j);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = journal_inventory(j);
    if (ret != ESP_OK) { tx_journal_close(j); return ret; }
    if (j->archive_bytes >= JOURNAL_ARCHIVE_BYTES) {
        tx_journal_close(j);
        return ESP_ERR_INVALID_SIZE;
    }

    char bak[320], tmp[320];
    snprintf(bak, sizeof(bak), "%s.bak", j->path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", j->path);
    bool active = stat(j->path, &st) == 0;
    if (!active && errno != ENOENT) { tx_journal_close(j); return ESP_FAIL; }
    bool backup = stat(bak, &st) == 0;
    if (!backup && errno != ENOENT) { tx_journal_close(j); return ESP_FAIL; }
    bool temporary = stat(tmp, &st) == 0;
    if (!temporary && errno != ENOENT) { tx_journal_close(j); return ESP_FAIL; }
    if (active && (backup || temporary)) {
        /* Do not guess which evidence survived a previous replacement. */
        tx_journal_close(j);
        return ESP_ERR_INVALID_STATE;
    }
    if (!active && backup && temporary) {
        tx_journal_close(j);
        return ESP_ERR_INVALID_STATE;
    }
    if (!active && backup) {
        if (!allow_writes || rename(bak, j->path) != 0) {
            tx_journal_close(j);
            return ESP_FAIL;
        }
        active = true;
    } else if (!active && temporary) {
        tx_journal_close(j);
        return ESP_ERR_INVALID_STATE;
    }
    if (!active && (ret = journal_create_active(j)) != ESP_OK) {
        tx_journal_close(j);
        return ret;
    }

    bool torn;
    ret = journal_scan_active(j, &torn);
    if (ret != ESP_OK) { tx_journal_close(j); return ret; }
    bool unfinished = j->pending != 0;
    if (torn || unfinished || j->distinct > JOURNAL_IDS ||
        j->bytes > JOURNAL_SEGMENT_BYTES) {
        ret = journal_rotate(j);
        if (ret != ESP_OK) { tx_journal_close(j); return ret; }
        if (archived_unfinished) *archived_unfinished = torn || unfinished;
    }
    *out = j;
    return ESP_OK;
}

void tx_journal_allow_writes(tx_journal_t *j, bool allow)
{
    if (j) j->allow_writes = allow;
}

bool tx_journal_healthy(const tx_journal_t *j)
{
    return j && !j->poisoned && j->allow_writes && j->nonce_set;
}

static esp_err_t journal_file_has_nonce(tx_journal_t *j, const char *path,
                                        const char *needle, bool *found)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    uint64_t seq = 0, work = 0;
    esp_err_t ret = ESP_OK;
    for (;;) {
        journal_read_t r = journal_read(f, j->scratch, &work);
        if (r == JR_END || r == JR_TORN) break;
        if (r != JR_RECORD || j->scratch->record.seq != seq + 1) {
            ret = r == JR_IO ? ESP_FAIL : ESP_ERR_INVALID_CRC;
            break;
        }
        seq++;
        if (strstr(j->scratch->record.id, needle)) *found = true;
        if (work > JOURNAL_SCAN_BUDGET) { ret = ESP_ERR_INVALID_SIZE; break; }
    }
    if (fclose(f) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    return ret;
}

esp_err_t tx_journal_set_nonce(tx_journal_t *j, uint64_t nonce)
{
    if (!j || j->poisoned || j->nonce_set) return ESP_ERR_INVALID_STATE;
    char needle[20];
    snprintf(needle, sizeof(needle), "@%016" PRIx64 "-", nonce);
    bool found = false;
    esp_err_t ret = journal_file_has_nonce(j, j->path, needle, &found);
    if (ret != ESP_OK) return ret;
    DIR *dir = opendir(j->archive_dir);
    if (!dir) return ESP_FAIL;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(dir)) != NULL && ret == ESP_OK) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char path[340];
        if (snprintf(path, sizeof(path), "%s/%s", j->archive_dir, ent->d_name) >=
            (int)sizeof(path)) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        ret = journal_file_has_nonce(j, path, needle, &found);
        errno = 0;
    }
    if (errno && ret == ESP_OK) ret = ESP_FAIL;
    if (closedir(dir) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    if (ret != ESP_OK) return ret;
    if (found) return ESP_ERR_INVALID_STATE;
    j->nonce = nonce;
    j->counter = 0;
    j->counter_exhausted = false;
    j->nonce_set = true;
    return ESP_OK;
}

esp_err_t tx_journal_next_id(tx_journal_t *j, const char *key,
                             char *id, size_t cap)
{
    if (!j || !key || !key[0] || !id || !cap) return ESP_ERR_INVALID_ARG;
    if (!tx_journal_healthy(j) || j->counter_exhausted)
        return ESP_ERR_INVALID_STATE;
    uint32_t counter = j->counter;
    if (counter == UINT32_MAX) j->counter_exhausted = true;
    else j->counter++;
    int n = snprintf(id, cap, "%s@%016" PRIx64 "-%08" PRIx32,
                     key, j->nonce, counter);
    if (n < 0 || (size_t)n >= cap || n > TX_LOG_ID_MAX) {
        id[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t tx_journal_append(tx_journal_t *j, tx_type_t type,
                            const char *id, const void *payload, size_t payload_len)
{
    if (!j || !id || !id[0] || strlen(id) > TX_LOG_ID_MAX ||
        payload_len > TX_LOG_PAYLOAD_MAX || (payload_len && !payload))
        return ESP_ERR_INVALID_ARG;
    if (!tx_journal_healthy(j)) return ESP_ERR_INVALID_STATE;
    size_t i = 0;
    while (i < j->seen_count && strcmp(j->seen[i].id, id)) i++;
    tx_state_t state = i < j->seen_count ? j->seen[i].state : TX_STATE_NONE;
    bool legal;
    tx_state_t next = next_state(state, type, &legal);
    if (!legal) return ESP_ERR_INVALID_STATE;
    size_t id_len = strlen(id);
    size_t total = TX_HDR_LEN + id_len + payload_len + 4;
    if (type == TX_PREPARE) {
        if (j->pending >= JOURNAL_PENDING) return ESP_ERR_INVALID_SIZE;
        uint64_t reserved = (j->pending + 1) * TX_MAX_RECORD;
        if (j->distinct >= JOURNAL_IDS ||
            j->bytes + total + reserved > JOURNAL_SEGMENT_BYTES) {
            if (j->pending) return ESP_ERR_INVALID_SIZE;
            esp_err_t r = journal_rotate(j);
            if (r != ESP_OK) return r;
            i = 0;
        }
    } else if (j->bytes + total > JOURNAL_SEGMENT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *rec = j->scratch->bytes;
    put_u32(rec, TX_MAGIC);
    put_u64(rec + 4, j->last_seq + 1);
    rec[12] = (uint8_t)type;
    rec[13] = (uint8_t)id_len;
    rec[14] = (uint8_t)payload_len;
    rec[15] = (uint8_t)(payload_len >> 8);
    memcpy(rec + TX_HDR_LEN, id, id_len);
    if (payload_len) memcpy(rec + TX_HDR_LEN + id_len, payload, payload_len);
    put_u32(rec + total - 4, crc32_bytes(rec, total - 4));
    if (fault_hit()) return ESP_FAIL;
    int fd = open(j->path, O_WRONLY | O_APPEND);
    if (fd < 0) return ESP_FAIL;
    ssize_t written = write(fd, rec, total);
    bool ok = written == (ssize_t)total;
    if (ok && fault_hit()) ok = false;
    if (ok) ok = fsync(fd) == 0;
    if (ok && fault_hit()) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) {
        j->poisoned = true;
        return ESP_FAIL;
    }
    if (type == TX_PREPARE) {
        snprintf(j->seen[i].id, sizeof(j->seen[i].id), "%s", id);
        j->seen_count++;
        j->distinct++;
        j->pending++;
    } else {
        j->pending--;
    }
    j->seen[i].state = next;
    j->bytes += total;
    j->records++;
    j->last_seq++;
    return ESP_OK;
}
