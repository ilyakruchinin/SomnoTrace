/* Native maintenance services. All requests copy their input; no worker owns
 * an LVGL object. A single bounded PSRAM state survives navigation safely. */
#pragma once
#include "controller_diagnostics.h"
#include "esp_err.h"
#include "maintenance_model.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MAINT_NONE,
    MAINT_SCAN,
    MAINT_FILES,
    MAINT_IMAGES,
    MAINT_CHECK_FIRMWARE,
    MAINT_EXPORT_REPORT,
    MAINT_RESET_UPLOAD,
    MAINT_RECREATE_EDF,
    MAINT_DELETE_EDF,
    MAINT_RESET_RECORDINGS,
    MAINT_FORMAT,
    MAINT_FACTORY_RESET,
    MAINT_SAVE_DISPLAY
} maintenance_action_t;
typedef struct {
    char name[MAINTENANCE_NAME_MAX];
    uint64_t bytes;
    uint32_t files;
} maintenance_entry_t;
typedef struct {
    bool busy, complete, simulation;
    esp_err_t result;
    maintenance_action_t action;
    uint32_t revision, job_id, processed, total;
    uint32_t census_generation;
    char message[192], report_path[128];
    bool capacity_valid, census_valid, has_more;
    uint64_t free_bytes, total_bytes, files, generated_edfs, nights, ox_files;
    uint64_t largest_air_night;
    uint32_t estimate_samples;
    char cursor[MAINTENANCE_NAME_MAX], day[9];
    size_t count;
    maintenance_entry_t entries[MAINTENANCE_PAGE_SIZE];
    int64_t checked_epoch_s, checked_uptime_us;
    bool release_valid, compatible_asset;
    char release[48], published[40], release_url[512], notes[1024];
} maintenance_snapshot_t;
typedef struct {
    uint32_t free_internal, min_internal, largest_internal;
    uint32_t free_psram, min_psram, largest_psram, tasks;
    uint64_t uptime_s, sd_free, sd_total;
    bool sd_capacity_valid, card_ready, therapy, recording, wifi;
    int rssi;
    bool rssi_valid;
    char version[40], build[40], target[32], ip[16], ssid[33];
    char airsense[64], oxygen[64], uploads[128];
    controller_diagnostics_snapshot_t controllers;
} maintenance_diagnostics_t;
esp_err_t touch_maintenance_request(maintenance_action_t action, const char *argument);
/* Returns a nonzero identity for an admitted job, including jobs that finish
 * before this call returns. A failed admission leaves job_id zero. */
esp_err_t touch_maintenance_request_tracked(maintenance_action_t action, const char *argument,
                                           uint32_t *job_id);
/* Navigation only cancels read-only browsing. Mutations retain their owner. */
void touch_maintenance_cancel_reads(void);
void touch_maintenance_snapshot(maintenance_snapshot_t *out);
void touch_maintenance_diagnostics(maintenance_diagnostics_t *out);
/* Boot publishes actual initialization results before diagnostics call driver getters. */
void touch_maintenance_service_readiness(bool airsense, bool oxygen);
/* Same synchronous backend used by the native worker and browser rebuild
 * worker; caller must be an off-UI PSRAM task with >=16 KiB stack. */
esp_err_t touch_maintenance_recreate(void);
