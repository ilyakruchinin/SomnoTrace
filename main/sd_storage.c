/*
 * SomnoTrace - SD card storage initialisation and FATFS mount
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

#include "sd_storage.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp_display.h"

static const char *TAG = "sd_storage";

static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

/* ── Space policy thresholds ──────────────────────────────────────────
 * A night of raw stream data is ~3-4 MB, plus derived EDFs.  The reserve
 * keeps enough room for the session about to start plus its recovery
 * metadata; the floor is the point at which recording is refused. */
#define SD_RESERVE_BYTES   (24ULL * 1024 * 1024)   /* warn below 24 MB  */
#define SD_FLOOR_BYTES     (8ULL * 1024 * 1024)    /* refuse below 8 MB */

/* Explicit cluster size for f_mkfs.  This MUST be set: passing 0 lets ESP-IDF
 * default it to the 512-byte sector size, and on a 32-64 GB card that means
 * ~60-130M FAT32 clusters and a FAT table hundreds of MB wide, built through a
 * 4 KB work buffer.  f_mkfs then either returns FR_MKFS_ABORTED or runs for
 * minutes and trips the task watchdog, so "Format SD" appears to do nothing.
 * 32 KB is the normal cluster size for this capacity and keeps the FAT small
 * enough to write in a few seconds. */
#define SD_FORMAT_ALLOC_UNIT  (32 * 1024)

/* ── Arbitration state ────────────────────────────────────────────── */
static SemaphoreHandle_t s_lease_mutex = NULL;   /* guards the counters  */
static SemaphoreHandle_t s_export_sem = NULL;    /* EXPORT/DESTRUCTIVE   */
static volatile int s_recording = 0;
static volatile int s_uploading = 0;

static void lease_init_once(void)
{
    if (!s_lease_mutex) s_lease_mutex = xSemaphoreCreateMutex();
    /* Recursive: a day rebuild holds the export lease across the whole
     * transaction and calls the per-session generator inside it, which takes
     * the same lease.  A plain mutex would self-deadlock. */
    if (!s_export_sem) s_export_sem = xSemaphoreCreateRecursiveMutex();
}

/* Shared SDMMC host/slot configuration for the Waveshare ESP32-S3-Touch-LCD-1.54.
 * Used by both sd_storage_init() and sd_storage_format() so pin assignments
 * stay in sync. */
static void sdmmc_config_default(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    sdmmc_host_t h = SDMMC_HOST_DEFAULT();
    h.slot = SDMMC_HOST_SLOT_1;
    h.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    *host = h;

    sdmmc_slot_config_t s = SDMMC_SLOT_CONFIG_DEFAULT();
    s.clk   = GPIO_NUM_16;
    s.cmd   = GPIO_NUM_15;
    s.d0    = GPIO_NUM_17;
    s.d1    = GPIO_NUM_18;
    s.d2    = GPIO_NUM_13;
    s.d3    = GPIO_NUM_14;
    s.width = 4;
    /* Internal pull-ups are often too weak for SD cards.
     * The Waveshare board should have external pull-ups on the SD lines. */
    s.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    *slot = s;
}

esp_err_t sd_storage_init(void)
{
    ESP_LOGI(TAG, "initialising SDMMC 4-bit mode...");

    sdmmc_host_t host;
    sdmmc_slot_config_t slot_config;
    sdmmc_config_default(&host, &slot_config);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 16,
        .allocation_unit_size = 0,
    };

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                             &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "4-bit mount failed (%s), trying 1-bit mode", esp_err_to_name(ret));
        slot_config.width = 1;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                       &mount_config, &card);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "check: SD card inserted? pull-ups? GPIO pins 13-18?");
        return ret;
    }

    s_mounted = true;
    s_card = card;
    lease_init_once();

    sdmmc_card_print_info(stdout, card);

    /* Create the SomnoTrace app-root and its subtrees. The parent .somnotrace/
     * must be created before its children (FATFS mkdir is non-recursive). */
    mkdir(SD_APP_DIR, 0775);
    mkdir(SD_SESSIONS_DIR, 0775);
    mkdir(SD_STREAMS_DIR, 0775);
    mkdir(SD_SUMMARIES_DIR, 0775);
    mkdir(SD_LOG_DIR, 0775);
    mkdir(SD_UPLOAD_STATE_DIR, 0775);

    /* Create oximetry directory tree */
    mkdir(SD_OXYMETRY_DIR, 0775);

    /* Create ResMed-compatible export directory tree */
    mkdir(SD_SDCARD_DIR, 0775);
    mkdir(SD_SDCARD_DATALOG, 0775);
    mkdir(SD_SDCARD_SETTINGS, 0775);

    ESP_LOGI(TAG, "SD mounted at %s, directory tree ready", SD_MOUNT_POINT);

    return ESP_OK;
}

bool sd_storage_is_ready(void)
{
    return s_mounted;
}

/* ── Free space ───────────────────────────────────────────────────── */

esp_err_t sd_storage_get_free(uint64_t *free_bytes, uint64_t *total_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FATFS *fs = NULL;
    DWORD free_clst = 0;
    /* Drive "0:" — the single mounted FATFS volume. */
    if (f_getfree("0:", &free_clst, &fs) != FR_OK || !fs) {
        return ESP_FAIL;
    }
    if (total_bytes)
        *total_bytes = (uint64_t)fs->n_fatent * fs->csize * fs->ssize;
    if (free_bytes)
        *free_bytes = (uint64_t)free_clst * fs->csize * fs->ssize;
    return ESP_OK;
}

/* Reclaim derived (regenerable) output so raw capture can proceed.
 * Only SDCARD/ is eligible: it is fully rebuildable from
 * .somnotrace/sessions/, which is the source of truth. */
static uint64_t reclaim_derived_output(void)
{
    /* Deliberately conservative: report what could be reclaimed and let the
     * user act.  Automatic deletion of derived data is only safe once the
     * per-day export/upload state machine can prove a day is reproducible
     * and already uploaded, so we do not delete here. */
    uint64_t free_bytes = 0;
    sd_storage_get_free(&free_bytes, NULL);
    return free_bytes;
}

bool sd_storage_reserve_for_recording(void)
{
    if (!s_mounted) return false;

    uint64_t free_bytes = 0, total = 0;
    if (sd_storage_get_free(&free_bytes, &total) != ESP_OK) {
        /* Cannot tell — do not block therapy recording on a failed query. */
        ESP_LOGW(TAG, "free-space query failed; allowing recording");
        return true;
    }

    if (free_bytes >= SD_RESERVE_BYTES) return true;

    ESP_LOGW(TAG, "low free space: %llu KB free of %llu KB",
             (unsigned long long)(free_bytes / 1024),
             (unsigned long long)(total / 1024));

    if (free_bytes < SD_FLOOR_BYTES) {
        free_bytes = reclaim_derived_output();
        if (free_bytes < SD_FLOOR_BYTES) {
            ESP_LOGE(TAG, "below hard floor (%llu KB) — refusing to record",
                     (unsigned long long)(free_bytes / 1024));
            bsp_display_set_notice("SD full");
            return false;
        }
    }

    bsp_display_set_notice("SD nearly full");
    return true;
}

/* ── Arbitration ──────────────────────────────────────────────────── */

void sd_storage_recording_begin(void)
{
    lease_init_once();
    if (!s_lease_mutex) { s_recording++; return; }
    xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    s_recording++;
    xSemaphoreGive(s_lease_mutex);
}

void sd_storage_recording_end(void)
{
    if (!s_lease_mutex) { if (s_recording > 0) s_recording--; return; }
    xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    if (s_recording > 0) s_recording--;
    xSemaphoreGive(s_lease_mutex);
}

bool sd_storage_recording_active(void)
{
    return s_recording > 0;
}

bool sd_storage_lease_acquire(sd_lease_t role, uint32_t timeout_ms)
{
    lease_init_once();
    if (!s_export_sem || !s_lease_mutex) return true;   /* pre-init: allow */

    TickType_t wait = pdMS_TO_TICKS(timeout_ms);

    switch (role) {
    case SD_LEASE_DESTRUCTIVE:
        /* Never destroy data while it is being produced or consumed. */
        if (s_recording > 0) {
            ESP_LOGW(TAG, "destructive op refused: recording in progress");
            return false;
        }
        if (s_uploading > 0) {
            ESP_LOGW(TAG, "destructive op refused: upload in progress");
            return false;
        }
        if (xSemaphoreTakeRecursive(s_export_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "destructive op refused: export in progress");
            return false;
        }
        if (s_recording > 0) {   /* re-check after acquiring */
            xSemaphoreGiveRecursive(s_export_sem);
            ESP_LOGW(TAG, "destructive op refused: recording started");
            return false;
        }
        return true;

    case SD_LEASE_EXPORT:
        /* Serialised against other exports and destructive work, but
         * permitted during recording: the previous session still needs
         * exporting while the next one records. */
        if (xSemaphoreTakeRecursive(s_export_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "export lease busy");
            return false;
        }
        return true;

    case SD_LEASE_UPLOAD:
        /* Held for the duration of the upload so a day can never be read
         * while it is being replaced by a rebuild. */
        if (xSemaphoreTakeRecursive(s_export_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "upload lease busy (export in progress)");
            return false;
        }
        xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
        s_uploading++;
        xSemaphoreGive(s_lease_mutex);
        return true;
    }
    return false;
}

void sd_storage_lease_release(sd_lease_t role)
{
    if (!s_export_sem || !s_lease_mutex) return;

    switch (role) {
    case SD_LEASE_EXPORT:
    case SD_LEASE_DESTRUCTIVE:
        xSemaphoreGiveRecursive(s_export_sem);
        break;
    case SD_LEASE_UPLOAD:
        xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
        if (s_uploading > 0) s_uploading--;
        xSemaphoreGive(s_lease_mutex);
        xSemaphoreGiveRecursive(s_export_sem);
        break;
    }
}

esp_err_t sd_storage_format(void)
{
    ESP_LOGW(TAG, "format: formatting SD card — ALL DATA WILL BE LOST");

    /* Report the card as unavailable for the whole operation: the volume is
     * unmounted while f_mkfs runs, and other subsystems (log flush, oximetry
     * dir creation) gate their SD writes on sd_storage_is_ready().  Restored
     * to true on success below. */
    bool was_mounted = s_mounted;
    s_mounted = false;

    esp_err_t ret;

    if (was_mounted) {
        /* Card is already mounted — pre-erase the first 8 sectors so any
         * residual exFAT boot-sector signature in sector 0 cannot confuse
         * f_mkfs on a future re-flash, then format in-place. */
        uint8_t *zeros = calloc(8 * 512, 1);
        if (zeros) {
            if (sdmmc_write_sectors(s_card, zeros, 0, 8) == ESP_OK)
                ESP_LOGI(TAG, "format: pre-erased first 8 sectors");
            else
                ESP_LOGW(TAG, "format: pre-erase failed, proceeding");
            free(zeros);
        }

        /* Unmounts, reformats, and remounts at the same point; the card handle
         * stays valid.  The _cfg variant is required: the plain
         * esp_vfs_fat_sdcard_format() reuses sd_storage_init()'s mount config,
         * whose allocation_unit_size is 0 (see SD_FORMAT_ALLOC_UNIT). */
        esp_vfs_fat_mount_config_t fmt_cfg = {
            .format_if_mount_failed = false,
            .max_files              = 16,
            .allocation_unit_size   = SD_FORMAT_ALLOC_UNIT,
        };
        ret = esp_vfs_fat_sdcard_format_cfg(SD_MOUNT_POINT, s_card, &fmt_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "format: esp_vfs_fat_sdcard_format_cfg failed: %s",
                     esp_err_to_name(ret));
            /* esp_vfs_fat_sdcard_format_cfg() unmounts, runs f_mkfs, then tries
             * to remount.  On f_mkfs failure the remount may still have
             * succeeded (card usable) or failed (driver has recycled s_card).
             * opendir() on the mount root distinguishes the two: it only
             * succeeds when the VFS is registered and the volume is mounted,
             * which is exactly the case where s_card is still valid.  Otherwise
             * leave s_mounted=false and the user reboots. */
            DIR *d = opendir(SD_MOUNT_POINT);
            if (d) { closedir(d); s_mounted = true; }
            return ret;
        }
        s_mounted = true;
    } else {
        /* Card not mounted — common cause: factory 64 GB SDXC cards ship with
         * exFAT.  ESP-IDF 5.5 builds FATFS with FF_FS_EXFAT=0, so the mount
         * in sd_storage_init() returns FR_NO_FILESYSTEM and s_card stays NULL.
         * Mount with format_if_mount_failed=true so the driver formats
         * (FM_ANY → FAT32, 32 KB clusters) and remounts in one step. */
        sdmmc_host_t host;
        sdmmc_slot_config_t slot;
        sdmmc_config_default(&host, &slot);

        esp_vfs_fat_sdmmc_mount_config_t cfg = {
            .format_if_mount_failed = true,
            .max_files              = 16,
            .allocation_unit_size   = SD_FORMAT_ALLOC_UNIT,
        };

        /* Single 4-bit attempt.  A 1-bit retry after this fails is a no-op:
         * esp_vfs_fat_sdmmc_mount() leaves the VFS path registered on failure,
         * so the retry just returns ESP_ERR_INVALID_STATE and hides the real
         * error.  sd_storage_init() already handles 4->1-bit for normal mounts. */
        sdmmc_card_t *card = NULL;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &cfg, &card);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "format: mount+format failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_card    = card;
        s_mounted = true;
        lease_init_once();
    }

    /* Recreate the SomnoTrace directory tree (format wipes everything). */
    mkdir(SD_APP_DIR, 0775);
    mkdir(SD_SESSIONS_DIR, 0775);
    mkdir(SD_STREAMS_DIR, 0775);
    mkdir(SD_SUMMARIES_DIR, 0775);
    mkdir(SD_LOG_DIR, 0775);
    mkdir(SD_UPLOAD_STATE_DIR, 0775);
    mkdir(SD_OXYMETRY_DIR, 0775);
    mkdir(SD_SDCARD_DIR, 0775);
    mkdir(SD_SDCARD_DATALOG, 0775);
    mkdir(SD_SDCARD_SETTINGS, 0775);

    ESP_LOGI(TAG, "format: SD card formatted and directory tree recreated");
    return ESP_OK;
}

void sd_storage_deinit(void)
{
    if (!s_mounted || !s_card) return;

    /* The VFS unmount runs f_unmount, which syncs the FAT window and issues a
     * final CTRL_SYNC so the card commits its own write buffer.  Without it a
     * reboot right after a format or a write burst can beat the flush. */
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "deinit: unmount failed: %s", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "deinit: SD flushed and unmounted");

    s_mounted = false;
    s_card = NULL;
}
