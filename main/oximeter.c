/*
 * SomnoTrace - Oximeter multi-driver dispatcher
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
 *
 * The dispatcher loads the paired driver type from NVS at init and routes
 * public API calls to the appropriate driver (OxyII for Gen2, Legacy for
 * Gen1).  Both drivers are compiled in; only one is active at a time.
 * The scan function runs both drivers' scans and merges results so the
 * user can see all compatible rings in a single scan.
 */

#include "oximeter.h"
#include "oximeter_internal.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_writer.h"
#include "psram_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "oximeter";

#define OX_NVS_NS "oximeter"

static const ox_driver_ops_t *s_active = &oxyii_driver_ops;
static ox_driver_t s_driver_type = OX_DRIVER_OXYII;

/* Forward declarations for store functions */
bool ox_store_load_paired(char *serial, size_t serial_sz,
                          char *firmware, size_t fw_sz,
                          char *name_prefix, size_t prefix_sz,
                          char *last_addr, size_t addr_sz,
                          char *driver, size_t driver_sz,
                          char *ble_name, size_t ble_name_sz);

static void load_driver_type(void)
{
    /* Try NVS first */
    nvs_handle_t h;
    nvs_writer_lock();
    if (nvs_open(OX_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t drv;
        if (nvs_get_u8(h, "driver", &drv) == ESP_OK && drv <= OX_DRIVER_LEGACY)
            s_driver_type = (ox_driver_t)drv;
        nvs_close(h);
    }
    nvs_writer_unlock();

    /* Fall back to paired.json on SD */
    if (s_driver_type == OX_DRIVER_OXYII) {
        char drv[16] = {0};
        if (ox_store_load_paired(NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                                 drv, sizeof(drv), NULL, 0)) {
            if (strcmp(drv, "wellue_legacy") == 0)
                s_driver_type = OX_DRIVER_LEGACY;
        }
    }

    s_active = (s_driver_type == OX_DRIVER_LEGACY)
        ? &legacy_driver_ops
        : &oxyii_driver_ops;

    ESP_LOGI(TAG, "active driver: %s",
             s_driver_type == OX_DRIVER_LEGACY ? "legacy" : "oxyii");
}

/* ── Public API — delegates to active driver ──────────────────────── */

esp_err_t oximeter_init(void)
{
    load_driver_type();
    oxyii_driver_ops.init();
    legacy_driver_ops.init();
    return ESP_OK;
}

/* Scan runs both drivers sequentially because each owns its GAP callback and
 * result buffer.  Results are merged by address after both scans complete. */
esp_err_t oximeter_scan(int timeout_sec)
{
    int each = timeout_sec > 1 ? timeout_sec / 2 : 1;
    esp_err_t oxyii = oxyii_driver_ops.scan(each);
    esp_err_t legacy = legacy_driver_ops.scan(each);
    return oxyii == ESP_OK || legacy == ESP_OK ? ESP_OK : ESP_FAIL;
}

cJSON *oximeter_get_scan_results(void)
{
    cJSON *merged = cJSON_CreateArray();
    cJSON *oxyii = oxyii_driver_ops.get_scan_results();
    cJSON *legacy = legacy_driver_ops.get_scan_results();
    if (!merged || !oxyii || !legacy) {
        if (merged) cJSON_Delete(merged);
        if (oxyii) cJSON_Delete(oxyii);
        if (legacy) cJSON_Delete(legacy);
        return cJSON_CreateArray();
    }
    cJSON *item;
    /* Both Gen1 and Gen2 rings advertise both the legacy service UUID
     * (14839ac4-...) and manufacturer ID 0xF34E, so both drivers match
     * both generations.  Dedup by address — keep the first entry (legacy
     * driver's, which has the device name) and skip duplicates.  The
     * "type" field is kept for the API but the web UI ignores it: pairing
     * always uses auto-detection (OX_DRIVER_AUTO), which tries OxyII first
     * and falls back to Legacy.  The detected driver is persisted to NVS
     * by the winning driver's pair_task, so subsequent boots use the
     * correct protocol without user intervention. */
    cJSON_ArrayForEach(item, legacy) {
        cJSON *copy = cJSON_Duplicate(item, true);
        if (copy) cJSON_AddItemToArray(merged, copy);
    }
    cJSON_ArrayForEach(item, oxyii) {
        cJSON *addr = cJSON_GetObjectItem(item, "addr");
        bool dup = false;
        for (int i = 0; i < cJSON_GetArraySize(merged); i++) {
            cJSON *cand = cJSON_GetArrayItem(merged, i);
            cJSON *cand_addr = cJSON_GetObjectItem(cand, "addr");
            if (cJSON_IsString(addr) && cJSON_IsString(cand_addr) &&
                strcmp(addr->valuestring, cand_addr->valuestring) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            cJSON *copy = cJSON_Duplicate(item, true);
            if (copy) cJSON_AddItemToArray(merged, copy);
        }
    }
    cJSON_Delete(oxyii);
    cJSON_Delete(legacy);
    return merged;
}

/* ── Auto-detect pair task ─────────────────────────────────────────── */
/* Tries OxyII first (fast MTU=23 failure on Gen1), falls back to Legacy.
 * Each driver's pair_task is async, so we poll the state string to detect
 * completion.  Pairing is a rare, user-initiated event, so the polling
 * overhead (every 200 ms for ~30 s max) is negligible. */
struct auto_pair_arg {
    char addr[24];
};

/* Guard against concurrent pair calls (user clicks "Pair" twice). */
static bool s_pair_in_progress = false;

/* A pair attempt has "settled" only when the driver reports a definitive
 * outcome that we can attribute to *our* pair_task, not to the background
 * pull_task.  This is tricky because both tasks write the state string:
 *
 *  - pull_task sets PAIRED after each monitoring/pull cycle (line ~1638
 *    in oximeter_oxyii.c, multiple places in oximeter_legacy.c)
 *  - pair_task sets PAIRED after a successful pair (line ~1295/1301)
 *  - pair_task sets ERROR via set_error() on failure
 *
 * The key insight: s_paired (the bool, returned by is_paired()) is only
 * set to true by pair_task (or load_paired_from_nvs at boot).  The
 * pull_task NEVER sets s_paired = true — it only sets the state string.
 * So:
 *
 *  - PAIRED + is_paired()=true  → pair_task succeeded → settled
 *  - PAIRED + is_paired()=false → pull_task set PAIRED, pair_task
 *                                  hasn't run yet → NOT settled
 *  - ERROR                      → pair_task failed → settled
 *    (pull_task can briefly set ERROR via do_connect_and_discover, but
 *    it immediately overwrites it with PAIRED, so the window is ~0)
 *  - other states               → not settled */
static bool pair_settled(const char **state_out)
{
    const char *st = s_active->get_status();
    if (state_out) *state_out = st;
    if (strcmp(st, OX_STATUS_ERROR) == 0)
        return true;
    if (strcmp(st, OX_STATUS_PAIRED) == 0)
        return s_active->is_paired();
    return false;
}

static esp_err_t try_pair(const char *addr, ox_driver_t driver)
{
    if (driver != s_driver_type) {
        s_driver_type = driver;
        s_active = (driver == OX_DRIVER_LEGACY)
            ? &legacy_driver_ops
            : &oxyii_driver_ops;
        s_active->init();
        ESP_LOGI(TAG, "switched to driver: %s",
                 driver == OX_DRIVER_LEGACY ? "legacy" : "oxyii");
    }
    return s_active->pair(addr);
}

static void auto_pair_task(void *arg)
{
    struct auto_pair_arg *pa = (struct auto_pair_arg *)arg;
    const char *addr = pa->addr;

    /* Phase 1: try OxyII (Gen2).  Gen1 rings fail fast — the MTU=23
     * check aborts within ~2 s of connect.  Gen2 rings succeed in
     * ~5-10 s (connect + MTU + service discovery + GET_INFO). */
    ESP_LOGI(TAG, "auto-pair: trying OxyII (Gen2) for %s", addr);
    esp_err_t rc = try_pair(addr, OX_DRIVER_OXYII);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "auto-pair: OxyII pair start failed (%s) — trying Legacy",
                 esp_err_to_name(rc));
        /* pair() failed to start the task (BLE host not ready, OOM).
         * Try Legacy directly — it might succeed if the issue was
         * driver-specific (unlikely, but worth a shot). */
        rc = try_pair(addr, OX_DRIVER_LEGACY);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "auto-pair: Legacy pair start also failed (%s)",
                     esp_err_to_name(rc));
            s_pair_in_progress = false;
            free(pa);
            vTaskDelete(NULL);
            return;
        }
    }

    /* Wait for the pair_task to settle.  Timeout is 60 s to account for
     * the case where the background pull_task is holding s_ops_mtx in a
     * MONITORING loop (polls every 30 s) — the pair_task blocks on the
     * mutex until the pull_task releases it. */
    const char *st = NULL;
    for (int i = 0; i < 300 && !pair_settled(&st); i++)
        vTaskDelay(pdMS_TO_TICKS(200));

    if (st && strcmp(st, OX_STATUS_PAIRED) == 0) {
        ESP_LOGI(TAG, "auto-pair: succeeded — device is %s",
                 s_driver_type == OX_DRIVER_LEGACY ? "Gen1 (Legacy)" : "Gen2 (OxyII)");
        s_pair_in_progress = false;
        free(pa);
        vTaskDelete(NULL);
        return;
    }

    /* If we already fell back to Legacy (because OxyII pair start failed),
     * don't retry Legacy again. */
    if (s_driver_type == OX_DRIVER_LEGACY) {
        const char *lerr = s_active->get_error();
        ESP_LOGW(TAG, "auto-pair: Legacy failed (%s)",
                 lerr ? lerr : "unknown");
        s_pair_in_progress = false;
        free(pa);
        vTaskDelete(NULL);
        return;
    }

    /* OxyII failed.  Check if the failure indicates "wrong protocol"
     * (MTU=23 or service not found) rather than a transient error
     * (connect timeout, ring not present, etc.).  Only retry with
     * Legacy if the error is protocol-related.
     *
     * Only trust the error string if the state is actually ERROR —
     * otherwise we might see a stale error from a previous operation
     * (forget() doesn't clear s_error), or the poll timed out without
     * the pair_task ever running (pull_task was holding the mutex). */
    const char *err = (st && strcmp(st, OX_STATUS_ERROR) == 0)
        ? s_active->get_error() : NULL;
    bool protocol_mismatch = err && (
        strstr(err, "MTU=23") ||
        strstr(err, "OxyII service not found") ||
        strstr(err, "service range empty") ||
        strstr(err, "write/notify char not found"));

    if (!protocol_mismatch) {
        ESP_LOGI(TAG, "auto-pair: OxyII failed (state=%s, err=%s) — not a protocol mismatch, giving up",
                 st ? st : "null", err ? err : "none");
        s_pair_in_progress = false;
        free(pa);
        vTaskDelete(NULL);
        return;
    }

    /* Allow the BLE controller to finish disconnecting before starting Phase 2. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Phase 2: fall back to Legacy (Gen1). */
    ESP_LOGI(TAG, "auto-pair: OxyII protocol mismatch — retrying as Legacy (Gen1)");
    rc = try_pair(addr, OX_DRIVER_LEGACY);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "auto-pair: Legacy pair start failed (%s)",
                 esp_err_to_name(rc));
        s_pair_in_progress = false;
        free(pa);
        vTaskDelete(NULL);
        return;
    }

    st = NULL;
    for (int i = 0; i < 300 && !pair_settled(&st); i++)
        vTaskDelay(pdMS_TO_TICKS(200));

    if (st && strcmp(st, OX_STATUS_PAIRED) == 0) {
        ESP_LOGI(TAG, "auto-pair: Legacy succeeded — device is Gen1");
    } else {
        const char *lerr = s_active->get_error();
        ESP_LOGW(TAG, "auto-pair: Legacy also failed (%s)",
                 lerr ? lerr : "unknown");
    }

    s_pair_in_progress = false;
    free(pa);
    vTaskDelete(NULL);
}

esp_err_t oximeter_pair(const char *addr_str, ox_driver_t driver)
{
    if (s_pair_in_progress) {
        ESP_LOGW(TAG, "pair already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (driver == OX_DRIVER_AUTO) {
        struct auto_pair_arg *pa = calloc(1, sizeof(*pa));
        if (!pa) return ESP_ERR_NO_MEM;
        strlcpy(pa->addr, addr_str, sizeof(pa->addr));
        s_pair_in_progress = true;
        TaskHandle_t h = psram_task_create(auto_pair_task, "ox_auto_pair",
                                           8192, pa, 5, tskNO_AFFINITY, NULL, NULL);
        if (!h) { s_pair_in_progress = false; free(pa); return ESP_ERR_NO_MEM; }
        return ESP_OK;
    }

    /* If pairing with a different driver type, switch active driver */
    if (driver != s_driver_type) {
        /* Switch driver */
        s_driver_type = driver;
        s_active = (driver == OX_DRIVER_LEGACY)
            ? &legacy_driver_ops
            : &oxyii_driver_ops;

        /* Initialize the new driver if not already done */
        s_active->init();

        ESP_LOGI(TAG, "switched to driver: %s",
                 driver == OX_DRIVER_LEGACY ? "legacy" : "oxyii");
    }

    return s_active->pair(addr_str);
}

esp_err_t oximeter_forget(void)
{
    s_active->forget();
    return ESP_OK;
}

const char *oximeter_get_status(void)
{
    return s_active->get_status();
}

const char *oximeter_get_error(void)
{
    return s_active->get_error();
}

bool oximeter_is_paired(void)
{
    return s_active->is_paired();
}

cJSON *oximeter_get_paired_info(void)
{
    return s_active->get_paired_info();
}

ox_driver_t oximeter_get_driver(void)
{
    return s_driver_type;
}

ox_probe_mode_t oximeter_get_probe_mode(void)
{
    return s_active->get_probe_mode();
}

esp_err_t oximeter_set_probe_mode(ox_probe_mode_t mode)
{
    return s_active->set_probe_mode(mode);
}
