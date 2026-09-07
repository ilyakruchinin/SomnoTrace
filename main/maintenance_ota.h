#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MAINT_OTA_IDLE,
    MAINT_OTA_TRANSFER,
    MAINT_OTA_VERIFY,
    MAINT_OTA_COMMIT,
    MAINT_OTA_RESTART,
    MAINT_OTA_FAILED,
    MAINT_OTA_CANCELLED
} maintenance_ota_stage_t;
typedef struct {
    bool active, done, ok, cancellable, boot_selected;
    int total, transferred;
    maintenance_ota_stage_t stage, failed_stage;
    int64_t started_us;
    char error[96];
} maintenance_ota_snapshot_t;
/* No UI pointers cross this boundary. Workers retain their own arguments and
 * internal flash stacks; view teardown never deletes an updater task. */
esp_err_t maintenance_ota_start_url(const char *url);
esp_err_t maintenance_ota_start_sd(const char *root_filename);
bool maintenance_ota_cancel(void);
void maintenance_ota_snapshot(maintenance_ota_snapshot_t *out);
esp_err_t maintenance_format_start(void);
esp_err_t maintenance_factory_reset_start(void);
void maintenance_format_snapshot(bool *active, bool *done, bool *ok, char *error, size_t cap);
