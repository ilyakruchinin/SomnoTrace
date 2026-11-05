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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
void tx_log_fault_after(int writes_until_failure) { s_fault_countdown = writes_until_failure; }
static bool fault_hit(void)
{
    if (s_fault_countdown <= 0) return false;
    return --s_fault_countdown == 0;
}
#else
static bool fault_hit(void) { return false; }
#endif

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
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) { free(buf); return ESP_FAIL; }
    *data_out = buf;
    *len_out = (size_t)n;

    size_t off = 0;
    id_state_t table[256];
    size_t n_ids = 0;
    while (off < (size_t)n) {
        tx_rec_t r;
        rec_status_t st = parse_record(buf + off, (size_t)n - off, &r);
        if (st == REC_INCOMPLETE) { *torn = true; break; }
        if (st == REC_BAD) return ESP_ERR_INVALID_CRC;
        if (r.seq != *last_seq + 1) return ESP_ERR_INVALID_CRC;
        size_t i = 0;
        while (i < n_ids && strcmp(table[i].id, r.id) != 0) i++;
        if (i == n_ids) {
            if (n_ids >= 256) return ESP_ERR_INVALID_SIZE;
            snprintf(table[n_ids].id, sizeof(table[n_ids].id), "%s", r.id);
            table[n_ids].state = TX_STATE_NONE;
            n_ids++;
        }
        bool legal;
        table[i].state = next_state(table[i].state, r.type, &legal);
        if (!legal) return ESP_ERR_INVALID_STATE;
        off += r.length;
        *last_seq = r.seq;
        (*records)++;
        *valid_out = off;
    }
    return ESP_OK;
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

    tx_log_t *log = calloc(1, sizeof(*log));
    if (!log) return ESP_ERR_NO_MEM;
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
