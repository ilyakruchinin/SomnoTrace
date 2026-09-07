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
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp_display.h"

static const char *TAG = "sd_storage";

static bool s_mounted = false;
/* Invalidates RAM history snapshots even for same-length, same-FAT-timestamp
 * rewrites. Writers publish the epoch before releasing their exclusive claim. */
static uint32_t s_content_generation = 1;
uint32_t sd_storage_content_generation(void)
{
    return __atomic_load_n(&s_content_generation, __ATOMIC_ACQUIRE);
}
void sd_storage_content_changed(void)
{
    __atomic_add_fetch(&s_content_generation, 1U, __ATOMIC_RELEASE);
}
/* Optional FTP integration callback; keeps the third-party component independent
 * of main's headers and component dependency graph. */

static sdmmc_card_t *s_card = NULL;
/* Capacity is sampled by explicit storage work, never by a status request.
 * Keep the pair behind one critical section: uint64_t reads/writes can tear on
 * the ESP32-S3's 32-bit cores. */
static portMUX_TYPE s_capacity_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_cached_free_bytes;
static uint64_t s_cached_total_bytes;
static bool s_capacity_cache_valid;

static void capacity_cache_invalidate(void)
{
    portENTER_CRITICAL(&s_capacity_lock);
    s_capacity_cache_valid = false;
    portEXIT_CRITICAL(&s_capacity_lock);
}

static void capacity_cache_store(uint64_t free_bytes, uint64_t total_bytes)
{
    portENTER_CRITICAL(&s_capacity_lock);
    s_cached_free_bytes = free_bytes;
    s_cached_total_bytes = total_bytes;
    s_capacity_cache_valid = true;
    portEXIT_CRITICAL(&s_capacity_lock);
}

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
static volatile int s_recording_waiters = 0;
static uint32_t s_recording_intents = 0;
static volatile int s_uploading = 0;
static volatile int s_destructive = 0;

#define SD_RECORDING_PRIORITY_WAIT_MS 500U
#define SD_RECORDING_PRIORITY_POLL_MS 5U

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
    capacity_cache_invalidate();
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

static esp_err_t sdmmc_mount_with_fallback(
    sdmmc_host_t *host,
    sdmmc_slot_config_t *slot,
    const esp_vfs_fat_sdmmc_mount_config_t *mount_config,
    sdmmc_card_t **card)
{
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        SD_MOUNT_POINT, host, slot, mount_config, card);

    if (ret != ESP_OK && slot->width != 1) {
        ESP_LOGW(TAG, "4-bit mount failed (%s), trying 1-bit high speed",
                 esp_err_to_name(ret));
        slot->width = 1;
        ret = esp_vfs_fat_sdmmc_mount(
            SD_MOUNT_POINT, host, slot, mount_config, card);
    }

    if (ret != ESP_OK && host->max_freq_khz > SDMMC_FREQ_DEFAULT) {
        ESP_LOGW(TAG, "high-speed mount failed (%s), trying %d kHz",
                 esp_err_to_name(ret), SDMMC_FREQ_DEFAULT);
        host->max_freq_khz = SDMMC_FREQ_DEFAULT;
        ret = esp_vfs_fat_sdmmc_mount(
            SD_MOUNT_POINT, host, slot, mount_config, card);
    }
    return ret;
}

esp_err_t sd_storage_init(void)
{
    sd_storage_content_changed();
    capacity_cache_invalidate();
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
    esp_err_t ret = sdmmc_mount_with_fallback(
        &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "check: SD card inserted? pull-ups? GPIO pins 13-18?");

        bsp_display_set_sd_ready(false);
        return ret;
    }

    s_mounted = true;
    s_card = card;
    bsp_display_set_sd_ready(true);
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

    uint64_t initial_free = 0;
    uint64_t initial_total = 0;
    if (sd_storage_get_free(&initial_free, &initial_total) != ESP_OK)
        ESP_LOGW(TAG, "initial free-space query failed");

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
    uint64_t total = (uint64_t)(fs->n_fatent - 2) * fs->csize * fs->ssize;
    uint64_t free = (uint64_t)free_clst * fs->csize * fs->ssize;
    capacity_cache_store(free, total);
    if (total_bytes) *total_bytes = total;
    if (free_bytes) *free_bytes = free;
    return ESP_OK;
}

bool sd_storage_get_cached_free(uint64_t *free_bytes, uint64_t *total_bytes)
{
    bool valid;
    portENTER_CRITICAL(&s_capacity_lock);
    valid = s_capacity_cache_valid;
    if (valid) {
        if (free_bytes) *free_bytes = s_cached_free_bytes;
        if (total_bytes) *total_bytes = s_cached_total_bytes;
    }
    portEXIT_CRITICAL(&s_capacity_lock);
    return valid;
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
    if (!sd_storage_get_cached_free(&free_bytes, &total)) {
        /* START is latency- and memory-sensitive: never allocate an SDMMC DMA
         * buffer here.  Mount and explicit capacity refreshes prime this
         * cache; an unavailable value must not block raw therapy capture. */
        ESP_LOGW(TAG, "free-space cache unavailable; allowing recording");
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
            bsp_display_set_critical_notice("microSD full");
            return false;
        }
    }

    bsp_display_set_notice("microSD nearly full");
    return true;
}

/* ── Arbitration ──────────────────────────────────────────────────── */

bool sd_storage_reserve_for_recording_cached(void)
{
    if (!s_mounted) return false;
    uint64_t free_bytes = 0;
    /* Match the existing missing-cache admission policy without probing FAT,
     * reclaiming output, or calling a display notice from an ingest callback. */
    return !sd_storage_get_cached_free(&free_bytes, NULL) ||
           free_bytes >= SD_FLOOR_BYTES;
}

bool sd_storage_recording_try_begin(void)
{
    /* Mount initializes arbitration. A notification retry must not allocate
     * mutexes, poll a reader, or wait behind another arbitration transition. */
    if (!s_export_sem || !s_lease_mutex ||
        xSemaphoreTake(s_lease_mutex, 0) != pdTRUE)
        return false;
    bool claimed = s_destructive == 0 && s_uploading == 0;
    if (claimed) __atomic_add_fetch(&s_recording, 1, __ATOMIC_RELEASE);
    xSemaphoreGive(s_lease_mutex);
    return claimed;
}

bool sd_storage_recording_begin(void)
{
    lease_init_once();
    if (!s_lease_mutex) return false;

    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)SD_RECORDING_PRIORITY_WAIT_MS * 1000;
    xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    __atomic_add_fetch(&s_recording_waiters, 1, __ATOMIC_RELEASE);
    bool claimed = false;
    while (s_destructive == 0) {
        if (s_uploading == 0) {
            __atomic_add_fetch(&s_recording, 1, __ATOMIC_RELEASE);
            claimed = true;
            break;
        }
        if (esp_timer_get_time() >= deadline_us)
            break;
        xSemaphoreGive(s_lease_mutex);
        vTaskDelay(pdMS_TO_TICKS(SD_RECORDING_PRIORITY_POLL_MS));
        xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    }
    __atomic_sub_fetch(&s_recording_waiters, 1, __ATOMIC_RELEASE);
    xSemaphoreGive(s_lease_mutex);
    if (!claimed) {
        ESP_LOGW(TAG, "recording claim timed out behind card maintenance");
    }
    return claimed;
}

void sd_storage_recording_end(void)
{
    sd_storage_content_changed();
    if (!s_lease_mutex) {
        if (s_recording > 0) __atomic_sub_fetch(&s_recording, 1, __ATOMIC_RELEASE);
        return;
    }
    xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    if (s_recording > 0) __atomic_sub_fetch(&s_recording, 1, __ATOMIC_RELEASE);
    xSemaphoreGive(s_lease_mutex);
}

bool sd_storage_recording_active(void)
{
    return __atomic_load_n(&s_recording, __ATOMIC_ACQUIRE) > 0;
}

void sd_storage_recording_intent_begin(void)
{
    lease_init_once();
    if (s_lease_mutex) xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    __atomic_add_fetch(&s_recording_intents, 1U, __ATOMIC_RELEASE);
    if (s_lease_mutex) xSemaphoreGive(s_lease_mutex);
}

void sd_storage_recording_intent_end(void)
{
    if (s_lease_mutex) xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    uint32_t count = __atomic_load_n(&s_recording_intents, __ATOMIC_ACQUIRE);
    /* Also safe before the arbitration mutex exists; never underflow. */
    while (count && !__atomic_compare_exchange_n(&s_recording_intents,
            &count, count - 1, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    if (s_lease_mutex) xSemaphoreGive(s_lease_mutex);
}

bool sd_storage_recording_pending(void)
{
    return __atomic_load_n(&s_recording_intents, __ATOMIC_ACQUIRE) > 0 ||
           __atomic_load_n(&s_recording_waiters, __ATOMIC_ACQUIRE) > 0;
}

bool sd_storage_lease_acquire(sd_lease_t role, uint32_t timeout_ms)
{
    lease_init_once();
    if (!s_export_sem || !s_lease_mutex) return true;   /* pre-init: allow */

    TickType_t wait = pdMS_TO_TICKS(timeout_ms);

    switch (role) {
    case SD_LEASE_DESTRUCTIVE:
        /* Take the file-operation gate first, then publish the destructive
         * claim under the same mutex used by recording_begin(). This closes
         * the old check-then-act window where recording could begin after the
         * final check but before destructive I/O started. */
        if (xSemaphoreTakeRecursive(s_export_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "destructive op refused: export in progress");
            return false;
        }
        xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
        if (s_recording > 0 || sd_storage_recording_pending() ||
            s_uploading > 0 || s_destructive > 0) {
            xSemaphoreGive(s_lease_mutex);
            xSemaphoreGiveRecursive(s_export_sem);
            ESP_LOGW(TAG, "destructive op refused: card owner active");
            return false;
        }
        s_destructive++;
        xSemaphoreGive(s_lease_mutex);
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
        /* The reader claim and recording claim are mutually exclusive under
         * s_lease_mutex. Whichever publishes first wins; there is no gap in
         * which a session can start after History's readiness check. */
        if (xSemaphoreTakeRecursive(s_export_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "upload lease busy (export in progress)");
            return false;
        }
        xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
        if (s_recording > 0 || sd_storage_recording_pending() ||
            s_destructive > 0) {
            xSemaphoreGive(s_lease_mutex);
            xSemaphoreGiveRecursive(s_export_sem);
            ESP_LOGW(TAG, "upload lease refused: recording in progress");
            return false;
        }
        s_uploading++;
        xSemaphoreGive(s_lease_mutex);
        return true;
    }
    return false;
}

void sd_storage_lease_release_unchanged(sd_lease_t role)
{
    if (!s_export_sem || !s_lease_mutex) return;

    switch (role) {
    case SD_LEASE_EXPORT:
        xSemaphoreGiveRecursive(s_export_sem);
        break;
    case SD_LEASE_DESTRUCTIVE:
        xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
        if (s_destructive > 0) s_destructive--;
        xSemaphoreGive(s_lease_mutex);
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

void sd_storage_lease_release(sd_lease_t role)
{
    /* Preserve conservative invalidation for existing writer call sites.
     * Publish before giving the file-operation gate to the next reader. */
    if (role != SD_LEASE_UPLOAD) sd_storage_content_changed();
    sd_storage_lease_release_unchanged(role);
}

esp_err_t sd_storage_format(void)
{
    ESP_LOGW(TAG, "format: formatting SD card — ALL DATA WILL BE LOST");
    capacity_cache_invalidate();

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

        /* The IDF 5.5 mount helper unregisters FATFS and deinitialises the host
         * on failure, so the same width/frequency fallback used at boot is safe
         * here too. This matters for marginal cards that initialise at 20 MHz
         * but not at the preferred 40 MHz. */
        sdmmc_card_t *card = NULL;
        ret = sdmmc_mount_with_fallback(&host, &slot, &cfg, &card);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "format: mount+format failed: %s", esp_err_to_name(ret));
            bsp_display_set_sd_ready(false);
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

    uint64_t formatted_free = 0;
    uint64_t formatted_total = 0;
    if (sd_storage_get_free(&formatted_free, &formatted_total) != ESP_OK)
        ESP_LOGW(TAG, "format: initial free-space query failed");

    ESP_LOGI(TAG, "format: SD card formatted and directory tree recreated");
    bsp_display_set_sd_ready(true);
    return ESP_OK;
}

void sd_storage_deinit(void)
{
    sd_storage_content_changed();
    capacity_cache_invalidate();
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
    bsp_display_set_sd_ready(false);
}
