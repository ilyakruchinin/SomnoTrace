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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Records are appended as: magic, seq, type, id_len, payload_len, id, payload,
 * crc32.  A transaction is PREPARE then exactly one of COMMIT or ABORT under
 * the same id.  Only a COMMIT makes its transaction durable.
 *
 * Recovery rule: a damaged record at the very end of the file is a torn tail
 * from an interrupted append; it is truncated away.  A damaged record followed
 * by valid data is middle corruption; it is reported and never skipped. */

#define TX_LOG_ID_MAX      64
#define TX_LOG_PAYLOAD_MAX 512

typedef enum {
    TX_PREPARE = 1,
    TX_COMMIT  = 2,
    TX_ABORT   = 3,
} tx_type_t;

typedef enum {
    TX_STATE_NONE = 0,
    TX_STATE_PREPARED,
    TX_STATE_COMMITTED,
    TX_STATE_ABORTED,
} tx_state_t;

typedef struct tx_log tx_log_t;

/* Open (creating if absent) and recover.  *torn_tail reports whether an
 * incomplete final record was truncated.  Middle corruption returns
 * ESP_ERR_INVALID_CRC and leaves the file untouched. */
esp_err_t tx_log_open(const char *path, tx_log_t **out, bool *torn_tail);
void      tx_log_close(tx_log_t *log);

/* Append one record and make it durable (flush + fsync) before returning. */
esp_err_t tx_log_append(tx_log_t *log, tx_type_t type, const char *id,
                        const void *payload, size_t payload_len);

/* State of one transaction id by replaying the log. */
esp_err_t tx_log_state(tx_log_t *log, const char *id, tx_state_t *state);

/* Number of records currently in the log and the last sequence written. */
esp_err_t tx_log_count(tx_log_t *log, uint64_t *records, uint64_t *last_seq);

/* Test hooks: fail the Nth subsequent write or allocation (0 = never).
 * Only built when TX_LOG_FAULT_INJECT is defined. */
#ifdef TX_LOG_FAULT_INJECT
void tx_log_fault_after(int writes_until_failure);
void tx_log_alloc_fault_after(int allocations_until_failure);
#endif
