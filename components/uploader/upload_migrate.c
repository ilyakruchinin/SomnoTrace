/*
 * SomnoTrace - One-shot migration from the legacy day-level upload state
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

/* DECOMMISSION AFTER v0.7.x — see upload_migrate.h */

#include "upload_migrate.h"
#include "upload_index.h"
#include "upload_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "up_migrate";

#define LEGACY_PATH  UPLOAD_STATE_DIR "/upload_state.json"
#define LEGACY_DONE  UPLOAD_STATE_DIR "/upload_state.json.migrated"

/* Mark every group currently present in `day` as OK for `slot`.
 *
 * This is exact in the normal case: the legacy tracker only set a day to "ok"
 * after uploading the whole day, so every group then on the card had been
 * transferred.  A group added after that point is not in the legacy state
 * either way, and the scan will pick it up as new. */
static int adopt_day(uint32_t day, int slot, uint32_t last_try_s)
{
    char daystr[12];
    snprintf(daystr, sizeof(daystr), "%08u", (unsigned)day);

    upload_group_ref_t *refs = heap_caps_calloc(UPLOAD_MAX_GROUPS_PER_DAY, sizeof(*refs), MALLOC_CAP_SPIRAM);
    if (!refs) refs = calloc(UPLOAD_MAX_GROUPS_PER_DAY, sizeof(*refs));
    if (!refs) return 0;

    int n = upload_scan_day_groups(daystr, refs, UPLOAD_MAX_GROUPS_PER_DAY);
    if (n == 0) { free(refs); return 0; }

    upload_day_t *d = upload_index_day(day, true);
    if (!d) { free(refs); return 0; }

    int adopted = 0;
    for (int i = 0; i < n; i++) {
        upload_group_t *g = upload_index_group(d, refs[i].prefix_sec, true);
        if (!g) continue;
        g->kinds = refs[i].kinds;
        g->n_files = (uint8_t)refs[i].n_files;
        g->be[slot].status = UG_OK;
        if (g->be[slot].attempts == 0) g->be[slot].attempts = 1;
        g->be[slot].last_try_s = last_try_s;
        adopted++;
    }
    d->dirty = true;
    upload_index_save_day(d);

    free(refs);
    return adopted;
}

bool upload_migrate_legacy_state(void)
{
    struct stat st;
    if (stat(LEGACY_PATH, &st) != 0) return false;      /* nothing to do */
    if (st.st_size <= 0 || st.st_size > 64 * 1024) {
        ESP_LOGW(TAG, "legacy state file has implausible size — ignoring");
        rename(LEGACY_PATH, LEGACY_DONE);
        return false;
    }

    FILE *f = fopen(LEGACY_PATH, "r");
    if (!f) return false;
    char *buf = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, st.st_size, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "legacy state file is not valid JSON — ignoring");
        rename(LEGACY_PATH, LEGACY_DONE);
        return false;
    }

    ESP_LOGI(TAG, "migrating legacy day-level upload state");

    int n_days = 0, n_groups = 0;
    cJSON *days = cJSON_GetObjectItem(root, "days");
    if (days && cJSON_IsArray(days)) {
        int nd = cJSON_GetArraySize(days);
        for (int i = 0; i < nd; i++) {
            cJSON *dy = cJSON_GetArrayItem(days, i);
            if (!dy) continue;
            cJSON *dayj = cJSON_GetObjectItem(dy, "day");
            if (!dayj || !cJSON_IsString(dayj)) continue;
            if (strlen(dayj->valuestring) != 8) continue;
            uint32_t day = (uint32_t)strtoul(dayj->valuestring, NULL, 10);
            if (day < 20000101u) continue;

            cJSON *bes = cJSON_GetObjectItem(dy, "backends");
            if (!bes || !cJSON_IsArray(bes)) continue;

            bool touched = false;
            int nb = cJSON_GetArraySize(bes);
            for (int b = 0; b < nb; b++) {
                cJSON *be = cJSON_GetArrayItem(bes, b);
                if (!be) continue;
                cJSON *name = cJSON_GetObjectItem(be, "name");
                cJSON *status = cJSON_GetObjectItem(be, "status");
                if (!name || !cJSON_IsString(name)) continue;
                if (!status || !cJSON_IsString(status)) continue;

                /* Only "ok" is carried over.  pending/failed days simply stay
                 * untracked, which the scan reads as "needs upload". */
                if (strcmp(status->valuestring, "ok") != 0) continue;

                int slot = upload_index_backend_slot(name->valuestring);
                if (slot < 0) continue;

                cJSON *lt = cJSON_GetObjectItem(be, "last_try");
                uint32_t last_s = 0;
                if (lt && cJSON_IsNumber(lt)) {
                    last_s = (uint32_t)(((int64_t)lt->valuedouble) / 1000);
                }

                int adopted = adopt_day(day, slot, last_s);
                if (adopted > 0) {
                    n_groups += adopted;
                    touched = true;
                    ESP_LOGI(TAG, "  %.8s %s: adopted %d group(s) as uploaded",
                             dayj->valuestring, name->valuestring, adopted);
                }
            }
            if (touched) n_days++;
        }
    }
    cJSON_Delete(root);

    /* Renamed rather than deleted: the step is then reversible while the
     * change is being validated in the field. */
    if (rename(LEGACY_PATH, LEGACY_DONE) != 0) {
        unlink(LEGACY_PATH);
    }

    ESP_LOGI(TAG, "migration done: %d day(s), %d group(s) adopted as uploaded",
             n_days, n_groups);
    return true;
}
