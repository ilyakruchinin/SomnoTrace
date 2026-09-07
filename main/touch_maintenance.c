#include "touch_maintenance.h"
#include "as11_ble.h"
#include "bsp_display.h"
#include "cJSON.h"
#include "device_settings.h"
#include "edf_gen.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "firmware_target.h"
#include "maintenance_fs.h"
#include "maintenance_ota.h"
#include "maintenance_release.h"
#include "net_provision.h"
#include "oximeter.h"
#include "psram_task.h"
#include "sd_storage.h"
#include "uploader.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static maintenance_snapshot_t *s_snapshot;
static bool s_busy;
static uint32_t s_next_job_id;
static bool s_read_job_active, s_read_cancelled;
static bool s_airsense_ready, s_oxygen_ready;

void touch_maintenance_service_readiness(bool airsense, bool oxygen)
{
    portENTER_CRITICAL(&s_lock);
    s_airsense_ready = airsense;
    s_oxygen_ready = oxygen;
    portEXIT_CRITICAL(&s_lock);
}
typedef struct {
    maintenance_snapshot_t value;
    char argument[MAINTENANCE_NAME_MAX];
} job_t;
static void publish(maintenance_snapshot_t *value)
{
    portENTER_CRITICAL(&s_lock);
    value->revision = s_snapshot->revision + 1;
    *s_snapshot = *value;
    portEXIT_CRITICAL(&s_lock);
}
void touch_maintenance_snapshot(maintenance_snapshot_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    if (s_snapshot)
        *out = *s_snapshot;
    out->busy = s_busy;
    portEXIT_CRITICAL(&s_lock);
}
void touch_maintenance_diagnostics(maintenance_diagnostics_t *out)
{
    memset(out, 0, sizeof(*out));
    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(out->version, app->version, sizeof(out->version));
    snprintf(out->build, sizeof(out->build), "%s %s", app->date, app->time);
    snprintf(out->target, sizeof(out->target), "ESP32-S3 / %.15s",
             somnotrace_firmware_target.board);
    const unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    out->free_internal = heap_caps_get_free_size(internal);
    out->min_internal = heap_caps_get_minimum_free_size(internal);
    out->largest_internal = heap_caps_get_largest_free_block(internal);
    out->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    out->largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    out->tasks = uxTaskGetNumberOfTasks();
    out->uptime_s = esp_timer_get_time() / 1000000;
    out->card_ready = sd_storage_is_ready();
    out->sd_capacity_valid = sd_storage_get_cached_free(&out->sd_free, &out->sd_total);
    out->therapy = bsp_display_is_therapy_active();
    out->recording = sd_storage_recording_active();
    netprov_link_t link;
    netprov_get_link(&link);
    out->wifi = link.up;
    out->rssi = link.rssi;
    out->rssi_valid = link.rssi_valid;
    strlcpy(out->ssid, link.ssid, sizeof(out->ssid));
    strlcpy(out->ip, link.ip, sizeof(out->ip));
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    strlcpy(out->airsense, "Simulation: AirSense 11 paired", sizeof(out->airsense));
    strlcpy(out->oxygen, "Simulation: O2 Ring not paired", sizeof(out->oxygen));
#else
    portENTER_CRITICAL(&s_lock);
    bool airsense_ready = s_airsense_ready, oxygen_ready = s_oxygen_ready;
    portEXIT_CRITICAL(&s_lock);
    strlcpy(out->airsense,
            airsense_ready ? as11_ble_get_status() : "AirSense service not initialized",
            sizeof(out->airsense));
    if (airsense_ready && !strcmp(out->airsense, AS11_STATUS_ERROR))
        snprintf(out->airsense, sizeof(out->airsense), "Error: %.55s", as11_ble_get_error());
    strlcpy(out->oxygen, oxygen_ready ? oximeter_get_status() : "O2 service not initialized",
            sizeof(out->oxygen));
    if (oxygen_ready && !strcmp(out->oxygen, OX_STATUS_ERROR))
        snprintf(out->oxygen, sizeof(out->oxygen), "Error: %.55s", oximeter_get_error());
#endif
    uploader_progress_snapshot_t up;
    if (uploader_get_progress_snapshot(&up) == ESP_OK) {
        strlcpy(out->uploads, up.status, sizeof(out->uploads));
        for (size_t i = 0; i < up.backend_count; i++)
            if (up.backends[i].error_valid) {
                snprintf(out->uploads, sizeof(out->uploads), "%s: %s", up.backends[i].label,
                         up.backends[i].error);
                break;
            }
    } else
        strlcpy(out->uploads, "Upload service not initialized", sizeof(out->uploads));
    controller_diagnostics_get_snapshot(&out->controllers);
}
static bool cancelled(void *unused)
{
    (void)unused;
    return __atomic_load_n(&s_read_cancelled, __ATOMIC_RELAXED) ||
           bsp_display_is_therapy_active() || sd_storage_recording_active() ||
           sd_storage_recording_pending();
}

void touch_maintenance_cancel_reads(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_busy && s_read_job_active)
        __atomic_store_n(&s_read_cancelled, true, __ATOMIC_RELAXED);
    portEXIT_CRITICAL(&s_lock);
}
static bool exists_dir(const char *base, const char *name)
{
    char path[256];
    struct stat st;
    if (snprintf(path, sizeof(path), "%s/%s", base, name) >= (int)sizeof(path))
        return false;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
/* Bounded insertion sort keeps only one visible page, regardless of card size. */
static void insert(maintenance_snapshot_t *s, const char *name, uint64_t bytes, uint32_t files,
                   bool descending)
{
    int boundary = strcmp(name, s->cursor);
    if (s->cursor[0] && (descending ? boundary >= 0 : boundary <= 0))
        return;
    size_t i = 0;
    while (i < s->count && (descending ? strcmp(s->entries[i].name, name) > 0
                                       : strcmp(s->entries[i].name, name) < 0))
        i++;
    if (i < s->count && !strcmp(s->entries[i].name, name))
        return;
    if (s->count == MAINTENANCE_PAGE_SIZE)
        s->has_more = true;
    if (i == MAINTENANCE_PAGE_SIZE)
        return;
    size_t n = s->count < MAINTENANCE_PAGE_SIZE ? s->count++ : MAINTENANCE_PAGE_SIZE - 1;
    for (size_t j = n; j > i; j--)
        s->entries[j] = s->entries[j - 1];
    memset(&s->entries[i], 0, sizeof(s->entries[i]));
    strlcpy(s->entries[i].name, name, sizeof(s->entries[i].name));
    s->entries[i].bytes = bytes;
    s->entries[i].files = files;
}
static esp_err_t count_tree(const char *path, bool root, maintenance_fs_totals_t *out)
{
    int e = maintenance_fs_walk(path, MAINT_FS_COUNT, root, out, cancelled, NULL);
    return e == 0 ? ESP_OK : e == ECANCELED ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}
static esp_err_t completed_day(const char *day, bool *complete)
{
    *complete = false;
    if (cancelled(NULL))
        return ESP_ERR_INVALID_STATE;
    char path[256];
    snprintf(path, sizeof(path), "%s/%.8s", SD_STREAMS_DIR, day);
    DIR *dir = opendir(path);
    if (!dir)
        return ESP_OK;
    bool found = false, valid = true;
    esp_err_t result = ESP_OK;
    struct dirent *e;
    while (!cancelled(NULL) && (e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n < 14 || strcmp(e->d_name + n - 13, "_session.json"))
            continue;
        char file_path[384];
        snprintf(file_path, sizeof(file_path), "%s/%s", path, e->d_name);
        FILE *f = fopen(file_path, "r");
        if (!f) {
            valid = false;
            break;
        }
        char *buf = heap_caps_malloc(4097, MALLOC_CAP_SPIRAM);
        if (!buf) {
            fclose(f);
            valid = false;
            break;
        }
        size_t bytes = fread(buf, 1, 4096, f);
        bool ended = feof(f);
        fclose(f);
        if (cancelled(NULL)) {
            free(buf);
            break;
        }
        buf[bytes] = 0;
        cJSON *j = ended ? cJSON_Parse(buf) : NULL;
        free(buf);
        cJSON *start = cJSON_GetObjectItem(j, "start_epoch_ms"),
              *end = cJSON_GetObjectItem(j, "end_epoch_ms");
        valid =
            cJSON_IsNumber(start) && cJSON_IsNumber(end) && end->valuedouble > start->valuedouble;
        cJSON_Delete(j);
        found = true;
        if (!valid)
            break;
    }
    closedir(dir);
    if (cancelled(NULL))
        result = ESP_ERR_INVALID_STATE;
    *complete = result == ESP_OK && found && valid;
    return result;
}
static esp_err_t scan_card(maintenance_snapshot_t *s)
{
    s->count = 0;
    s->has_more = false;
    s->census_valid = false;
    s->capacity_valid = false;
    s->files = s->nights = s->generated_edfs = s->ox_files = s->largest_air_night = 0;
    s->estimate_samples = 0;
    if (!sd_storage_is_ready() || !sd_storage_lease_acquire(SD_LEASE_UPLOAD, 0))
        return ESP_ERR_INVALID_STATE;
    uint32_t generation = sd_storage_content_generation();
    esp_err_t result = sd_storage_get_free(&s->free_bytes, &s->total_bytes);
    s->capacity_valid = result == ESP_OK;
    maintenance_fs_totals_t raw = {0}, edf = {0}, ox = {0};
    if (result == ESP_OK)
        result = count_tree(SD_SESSIONS_DIR, false, &raw);
    if (result == ESP_OK)
        result = count_tree(SD_SDCARD_DIR, true, &edf);
    if (result == ESP_OK)
        result = count_tree(SD_OXYMETRY_DIR, false, &ox);
    s->files = raw.files + edf.files + ox.files;
    s->generated_edfs = edf.generated_edfs;
    s->ox_files = ox.files;
    const char *roots[] = {SD_STREAMS_DIR, SD_SDCARD_DATALOG};
    for (size_t r = 0; r < 2 && result == ESP_OK; r++) {
        DIR *dir = opendir(roots[r]);
        if (!dir) {
            if (errno != ENOENT)
                result = ESP_FAIL;
            continue;
        }
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (cancelled(NULL)) {
                result = ESP_ERR_INVALID_STATE;
                break;
            }
            if (!maintenance_day_valid(e->d_name) || !exists_dir(roots[r], e->d_name))
                continue;
            if (r && exists_dir(SD_STREAMS_DIR, e->d_name))
                continue;
            maintenance_fs_totals_t night = {0};
            char path[256];
            snprintf(path, sizeof(path), "%s/%.8s", SD_STREAMS_DIR, e->d_name);
            result = count_tree(path, false, &night);
            snprintf(path, sizeof(path), "%s/%.8s", SD_SDCARD_DATALOG, e->d_name);
            if (result == ESP_OK)
                result = count_tree(path, false, &night);
            if (result != ESP_OK)
                break;
            s->nights++;
            insert(s, e->d_name, night.bytes, (uint32_t)night.files, true);
            /* O2 sidecars are global; without reliable per-night attribution,
             * withhold the estimate for every card containing O2 files. */
            if (!ox.files && !r && exists_dir(SD_SDCARD_DATALOG, e->d_name)) {
                bool complete = false;
                result = completed_day(e->d_name, &complete);
                if (result != ESP_OK)
                    break;
                if (complete) {
                    s->estimate_samples++;
                    if (night.bytes > s->largest_air_night)
                        s->largest_air_night = night.bytes;
                }
            }
            vTaskDelay(1);
        }
        closedir(dir);
    }
    if (result == ESP_OK && (cancelled(NULL) || generation != sd_storage_content_generation()))
        result = ESP_ERR_INVALID_STATE;
    s->census_valid = result == ESP_OK;
    s->census_generation = generation;
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}
static esp_err_t list_files(maintenance_snapshot_t *s, bool images)
{
    if (!images && !maintenance_day_valid(s->day))
        return ESP_ERR_INVALID_ARG;
    if (!sd_storage_is_ready() || !sd_storage_lease_acquire(SD_LEASE_UPLOAD, 0))
        return ESP_ERR_INVALID_STATE;
    s->count = 0;
    s->has_more = false;
    esp_err_t result = ESP_OK;
    for (unsigned r = 0; r < (images ? 1 : 2); r++) {
        char path[256];
        if (images)
            strlcpy(path, SD_MOUNT_POINT, sizeof(path));
        else
            snprintf(path, sizeof(path), "%s/%s", r ? SD_SDCARD_DATALOG : SD_STREAMS_DIR, s->day);
        DIR *dir = opendir(path);
        if (!dir) {
            if (errno != ENOENT)
                result = ESP_FAIL;
            continue;
        }
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (cancelled(NULL)) {
                result = ESP_ERR_INVALID_STATE;
                break;
            }
            if (e->d_name[0] == '.')
                continue;
            if (images && !maintenance_sd_image_name_valid(e->d_name))
                continue;
            char child[512], name[MAINTENANCE_NAME_MAX];
            struct stat st;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (stat(child, &st)) {
                result = ESP_FAIL;
                break;
            }
            if (!S_ISREG(st.st_mode))
                continue;
            int n = snprintf(name, sizeof(name), "%s%s",
                             images ? ""
                             : r    ? "EDF/"
                                    : "Raw/",
                             e->d_name);
            if (n >= sizeof(name)) {
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            insert(s, name, st.st_size, 1, false);
        }
        closedir(dir);
        if (result != ESP_OK)
            break;
    }
    if (cancelled(NULL))
        result = ESP_ERR_INVALID_STATE;
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}
static esp_err_t recreate(maintenance_snapshot_t *s)
{
    char cursor[9] = "";
    for (;;) {
        if (cancelled(NULL))
            return ESP_ERR_INVALID_STATE;
        if (!sd_storage_lease_acquire(SD_LEASE_UPLOAD, 0))
            return ESP_ERR_INVALID_STATE;
        DIR *dir = opendir(SD_STREAMS_DIR);
        char next[9] = "";
        struct dirent *e;
        if (!dir) {
            int error = errno;
            sd_storage_lease_release(SD_LEASE_UPLOAD);
            return error == ENOENT ? ESP_OK : ESP_FAIL;
        }
        while ((e = readdir(dir)))
            if (maintenance_day_valid(e->d_name) && strcmp(e->d_name, cursor) > 0 &&
                (!next[0] || strcmp(e->d_name, next) < 0))
                strlcpy(next, e->d_name, sizeof(next));
        closedir(dir);
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        if (!next[0])
            return ESP_OK;
        if (cancelled(NULL))
            return ESP_ERR_INVALID_STATE;
        /* edf_gen_rebuild_day takes EXPORT itself and publishes transactionally.
         * Never delete the existing export before successful generation. */
        esp_err_t result = edf_gen_rebuild_day(next);
        if (result != ESP_OK)
            return result;
        uploader_on_day_invalidated(next);
        strlcpy(cursor, next, sizeof(cursor));
        if (s) {
            s->processed++;
            snprintf(s->message, sizeof(s->message),
                     "Rebuilt %lu nights; latest %s. Remaining time unknown.",
                     (unsigned long)s->processed, next);
            publish(s);
        }
    }
}
esp_err_t touch_maintenance_recreate(void) { return recreate(NULL); }
static esp_err_t destructive(maintenance_snapshot_t *s)
{
    /* Final admission happens in the worker, after hold completion. The
     * cancellable lifecycle gate allows therapy publication to preempt us. */
    if (!bsp_display_try_begin_therapy_safe_maintenance())
        return ESP_ERR_INVALID_STATE;
    if (!sd_storage_is_ready() || !sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE, 0)) {
        bsp_display_end_therapy_safe_maintenance();
        return ESP_ERR_INVALID_STATE;
    }
    maintenance_fs_totals_t totals = {0};
    int error = 0;
    if (bsp_display_therapy_safe_maintenance_should_abort())
        error = ECANCELED;
    else if (s->action == MAINT_DELETE_EDF)
        error = maintenance_fs_walk(SD_SDCARD_DIR, MAINT_FS_DELETE_GENERATED, true, &totals,
                                    cancelled, NULL);
    else {
        const char *roots[] = {SD_SDCARD_DIR, SD_SESSIONS_DIR, SD_OXYMETRY_DIR};
        for (size_t i = 0; i < 3 && !error; i++)
            error = maintenance_fs_walk(roots[i], MAINT_FS_DELETE_TREE, false, &totals, cancelled,
                                        NULL);
    }
    sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
    bsp_display_end_therapy_safe_maintenance();
    s->processed = (uint32_t)totals.files;
    s->census_valid = false;
    snprintf(s->message, sizeof(s->message),
             "%llu files deleted%s. Settings, pairing and device logs retained.",
             (unsigned long long)totals.files, error ? " before operation stopped" : "");
    if (!error && s->action == MAINT_RESET_RECORDINGS) {
        esp_err_t reset = uploader_reset_state();
        if (reset != ESP_OK) {
            strlcat(s->message, " Upload-state reset request failed.", sizeof(s->message));
            return reset;
        }
    }
    return error == 0 ? ESP_OK : error == ECANCELED ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}
static esp_err_t release_check(maintenance_snapshot_t *s)
{
    s->release_valid = false;
    s->compatible_asset = false;
    s->release_url[0] = 0;
    s->checked_epoch_s = time(NULL);
    s->checked_uptime_us = esp_timer_get_time();
    if (!netprov_is_link_up())
        return ESP_ERR_INVALID_STATE;
    esp_http_client_config_t cfg = {.url = MAINTENANCE_RELEASE_API,
                                    .crt_bundle_attach = esp_crt_bundle_attach,
                                    .timeout_ms = 10000,
                                    .buffer_size = 2048};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
        return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(client, "User-Agent", "SomnoTrace-7B");
    esp_err_t result = esp_http_client_open(client, 0);
    char *body = heap_caps_malloc(24577, MALLOC_CAP_SPIRAM);
    size_t used = 0;
    if (!body)
        result = ESP_ERR_NO_MEM;
    if (result == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200)
            result = ESP_FAIL;
    }
    while (result == ESP_OK && used < 24576) {
        int n = esp_http_client_read(client, body + used, 24576 - used);
        if (n < 0) {
            result = ESP_FAIL;
            break;
        }
        if (!n)
            break;
        used += n;
    }
    if (result == ESP_OK && (!esp_http_client_is_complete_data_received(client) || used == 24576))
        result = ESP_ERR_INVALID_SIZE;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result == ESP_OK) {
        /* TLS buffers are already freed. Bound cJSON's grammar/node allocation
         * and leave an internal reserve for therapy/runtime work. */
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 65536)
            result = ESP_ERR_NO_MEM;
        else {
            maintenance_release_t *release =
                heap_caps_calloc(1, sizeof(*release), MALLOC_CAP_SPIRAM);
            if (!release)
                result = ESP_ERR_NO_MEM;
            else {
                result = maintenance_release_parse(body, used, release);
                if (result == ESP_OK) {
                    s->release_valid = release->valid;
                    s->compatible_asset = release->compatible_asset;
                    strlcpy(s->release, release->version, sizeof(s->release));
                    strlcpy(s->published, release->published, sizeof(s->published));
                    strlcpy(s->release_url, release->url, sizeof(s->release_url));
                    strlcpy(s->notes, release->notes, sizeof(s->notes));
                }
                free(release);
            }
        }
    }
    free(body);
    return result;
}
static esp_err_t export_report(maintenance_snapshot_t *s)
{
    if (!sd_storage_is_ready() || !sd_storage_lease_acquire(SD_LEASE_UPLOAD, 0))
        return ESP_ERR_INVALID_STATE;
    maintenance_diagnostics_t *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
    if (!d) {
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        return ESP_ERR_NO_MEM;
    }
    touch_maintenance_diagnostics(d);
    snprintf(s->report_path, sizeof(s->report_path), "%s/diagnostics-%lld-%llu.txt", SD_LOG_DIR,
             (long long)time(NULL), (unsigned long long)d->uptime_s);
    FILE *f = fopen(s->report_path, "wx");
    bool ok = f != NULL;
    if (f) {
        char capacity[96], signal[40];
        if (d->sd_capacity_valid)
            snprintf(capacity, sizeof(capacity), "%llu/%llu bytes free",
                     (unsigned long long)d->sd_free, (unsigned long long)d->sd_total);
        else
            strlcpy(capacity, "unknown; no valid sample", sizeof(capacity));
        if (d->rssi_valid)
            snprintf(signal, sizeof(signal), "%d dBm", d->rssi);
        else
            strlcpy(signal, "unavailable", sizeof(signal));
        ok =
            fprintf(f,
                    "SomnoTrace diagnostic snapshot\nVersion: %s\nBuild: %s\nTarget: %s\nUptime: "
                    "%llu s\n"
                    "Internal free/min/largest: %lu/%lu/%lu bytes\nPSRAM free/min/largest: "
                    "%lu/%lu/%lu bytes\nTasks: %lu\n"
                    "Network: %s; IP %s; RSSI %s\nCard mounted: %d\nCapacity: %s\n"
                    "Therapy active: %d; recording active: %d\nAirSense: %s\nO2: %s\nUploads: %s\n"
                    "Controller history is %s; latest observed error episodes follow.\n",
                    d->version, d->build, d->target, (unsigned long long)d->uptime_s,
                    (unsigned long)d->free_internal, (unsigned long)d->min_internal,
                    (unsigned long)d->largest_internal, (unsigned long)d->free_psram,
                    (unsigned long)d->min_psram, (unsigned long)d->largest_psram,
                    (unsigned long)d->tasks, d->wifi ? "connected" : "offline",
                    d->wifi ? d->ip : "unavailable", signal, d->card_ready, capacity, d->therapy,
                    d->recording, d->airsense, d->oxygen, d->uploads,
                    d->controllers.simulated ? "simulated" : "measured") > 0;
        for (size_t i = 0; i < CONTROLLER_OPERATION_COUNT && ok; i++) {
            controller_operation_status_t *op = &d->controllers.operations[i];
            ok = !cancelled(NULL) &&
                 fprintf(f, "%s: %s; last=%ld; errors=%lu; latest error uptime=%lld us\n",
                         controller_diagnostics_operation_name(i),
                         op->observed ? "observed" : "unknown", (long)op->last_result,
                         (unsigned long)op->error_count, (long long)op->last_error_us) > 0;
        }
        for (size_t i = 0; i < d->controllers.history_count && ok; i++) {
            controller_error_episode_t *ep = &d->controllers.history[i];
            ok = !cancelled(NULL) &&
                 fprintf(f, "%s: %s x%lu; uptime=%lld..%lld us\n",
                         controller_diagnostics_operation_name(ep->operation),
                         esp_err_to_name(ep->result), (unsigned long)ep->occurrences,
                         (long long)ep->first_us, (long long)ep->last_us) > 0;
        }
        if (fflush(f))
            ok = false;
        if (fclose(f))
            ok = false;
    }
    if (!ok && f)
        remove(s->report_path);
    free(d);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return ok ? ESP_OK : ESP_FAIL;
}
static void worker(void *arg)
{
    job_t *job = arg;
    maintenance_snapshot_t *s = &job->value;
    esp_err_t result = ESP_OK;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    s->simulation = true;
    if (s->action == MAINT_SCAN) {
        s->capacity_valid = s->census_valid = true;
        s->free_bytes = 24600000000ULL;
        s->total_bytes = 29700000000ULL;
        s->nights = 3;
        s->files = 30;
        s->generated_edfs = 15;
        s->ox_files = 0;
        s->largest_air_night = 18000000;
        s->estimate_samples = 3;
        s->count = 0;
        s->has_more = false;
        insert(s, "20260902", 18000000, 10, true);
        insert(s, "20260901", 17100000, 10, true);
        insert(s, "20260831", 9600000, 10, true);
    } else if (s->action == MAINT_FILES) {
        s->count = 0;
        s->has_more = false;
        insert(s, "EDF/20260902_220000_BRP.edf", 9600000, 1, false);
        insert(s, "Raw/20260902_220000_flow.snt", 8400000, 1, false);
    } else if (s->action == MAINT_IMAGES) {
        s->count = 0;
        s->has_more = false;
        insert(s, "somnotrace-7b-demo.bin", 2097152, 1, false);
    } else if (s->action == MAINT_CHECK_FIRMWARE) {
        s->checked_epoch_s = time(NULL);
        s->checked_uptime_us = esp_timer_get_time();
        s->release_valid = s->compatible_asset = true;
        strlcpy(s->release, "QEMU fixture", sizeof(s->release));
        strlcpy(s->notes, "Deterministic release-check simulation. No network request was sent.",
                sizeof(s->notes));
        strlcpy(s->release_url, "https://example.invalid/somnotrace-7b-demo.bin",
                sizeof(s->release_url));
    } else if (s->action == MAINT_SAVE_DISPLAY)
        result = device_settings_save_current();
    else {
        result = ESP_ERR_NOT_SUPPORTED;
        strlcpy(s->message, "QEMU simulation: operation stopped without changing card data.",
                sizeof(s->message));
    }
#else
    switch (s->action) {
    case MAINT_SCAN:
        result = scan_card(s);
        break;
    case MAINT_FILES:
        result = list_files(s, false);
        break;
    case MAINT_IMAGES:
        result = list_files(s, true);
        break;
    case MAINT_CHECK_FIRMWARE:
        result = release_check(s);
        break;
    case MAINT_EXPORT_REPORT:
        result = export_report(s);
        break;
    case MAINT_RESET_UPLOAD:
        result = uploader_reset_state();
        strlcpy(s->message,
                "Upload-state reset requested; scheduler completion is not yet confirmed. "
                "Recording files retained.",
                sizeof(s->message));
        break;
    case MAINT_RECREATE_EDF:
        result = recreate(s);
        break;
    case MAINT_DELETE_EDF:
    case MAINT_RESET_RECORDINGS:
        result = destructive(s);
        break;
    case MAINT_FORMAT:
    case MAINT_FACTORY_RESET:
        result = s->action == MAINT_FACTORY_RESET ? maintenance_factory_reset_start()
                                                  : maintenance_format_start();
        if (result == ESP_OK) {
            bool active = true, done = false, ok = false;
            char error[96];
            while (active) {
                maintenance_format_snapshot(&active, &done, &ok, error, sizeof(error));
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            result = done && ok ? ESP_OK : ESP_FAIL;
            strlcpy(s->message, error, sizeof(s->message));
            s->census_valid = false;
        }
        break;
    case MAINT_SAVE_DISPLAY:
        result = device_settings_save_current();
        break;
    default:
        result = ESP_ERR_INVALID_ARG;
        break;
    }
#endif
    if ((s->action == MAINT_SCAN || s->action == MAINT_FILES || s->action == MAINT_IMAGES) &&
        cancelled(NULL))
        result = ESP_ERR_INVALID_STATE;
    if (result != ESP_OK &&
        (s->action == MAINT_SCAN || s->action == MAINT_FILES || s->action == MAINT_IMAGES)) {
        s->count = 0;
        s->has_more = false;
        if (s->action == MAINT_SCAN)
            s->census_valid = false;
    }
    s->result = result;
    s->busy = false;
    s->complete = true;
    if (!s->message[0]) {
        if (result != ESP_OK)
            snprintf(s->message, sizeof(s->message),
                     "Operation stopped: %s. Recording may have priority; review the current state.",
                     esp_err_to_name(result));
        else if (s->action == MAINT_EXPORT_REPORT)
            snprintf(s->message, sizeof(s->message), "Saved diagnostic report: %s", s->report_path);
        else
            strlcpy(s->message,
                    s->action == MAINT_SCAN ? "Card scan completed."
                    : s->action == MAINT_FILES ? "Night file list loaded."
                    : s->action == MAINT_IMAGES ? "Card image list loaded."
                    : s->action == MAINT_CHECK_FIRMWARE ? "Release check completed."
                    : s->action == MAINT_SAVE_DISPLAY ? "Display settings saved."
                    : "Operation completed.",
                    sizeof(s->message));
    }
    publish(s);
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    s_read_job_active = false;
    __atomic_store_n(&s_read_cancelled, false, __ATOMIC_RELAXED);
    portEXIT_CRITICAL(&s_lock);
    free(job);
    psram_task_delete(NULL);
}
esp_err_t touch_maintenance_request(maintenance_action_t action, const char *argument)
{
    return touch_maintenance_request_tracked(action, argument, NULL);
}
esp_err_t touch_maintenance_request_tracked(maintenance_action_t action, const char *argument,
                                           uint32_t *job_id)
{
    if (job_id) *job_id = 0;
    if (action <= MAINT_NONE || action > MAINT_SAVE_DISPLAY ||
        (argument && strlen(argument) >= MAINTENANCE_NAME_MAX))
        return ESP_ERR_INVALID_ARG;
    maintenance_snapshot_t *fresh = heap_caps_calloc(1, sizeof(*fresh), MALLOC_CAP_SPIRAM);
    job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    if (!job) {
        free(fresh);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    if (!s_snapshot && fresh) {
        s_snapshot = fresh;
        fresh = NULL;
    }
    bool admitted = s_snapshot && !s_busy;
    if (admitted) {
        s_busy = true;
        s_read_job_active = action == MAINT_SCAN || action == MAINT_FILES || action == MAINT_IMAGES;
        __atomic_store_n(&s_read_cancelled, false, __ATOMIC_RELAXED);
        job->value = *s_snapshot;
        if (++s_next_job_id == 0) ++s_next_job_id;
        job->value.job_id = s_next_job_id;
    }
    portEXIT_CRITICAL(&s_lock);
    free(fresh);
    if (!admitted) {
        free(job);
        return ESP_ERR_INVALID_STATE;
    }
    maintenance_snapshot_t *s = &job->value;
    if (action == MAINT_SCAN || action == MAINT_FILES || action == MAINT_IMAGES) {
        s->count = 0;
        s->has_more = false;
        if (action == MAINT_SCAN)
            s->census_valid = false;
    }
    s->action = action;
    s->busy = true;
    s->complete = false;
    s->processed = 0;
    s->message[0] = 0;
    if (action == MAINT_FILES) {
        /* day or day/cursor, both copied before returning to LVGL. */
        if (!argument || strlen(argument) < 8) {
            goto invalid;
        }
        memcpy(s->day, argument, 8);
        s->day[8] = 0;
        if (!maintenance_day_valid(s->day))
            goto invalid;
        strlcpy(s->cursor, argument[8] == '/' ? argument + 9 : "", sizeof(s->cursor));
    } else if (action == MAINT_SCAN || action == MAINT_IMAGES)
        strlcpy(s->cursor, argument ? argument : "", sizeof(s->cursor));
    publish(s);
    /* The worker can retire/free job before task creation returns. */
    if (job_id) *job_id = s->job_id;
    if (!psram_task_create(worker, "maintenance", 16384, job, 3, tskNO_AFFINITY, NULL, NULL)) {
        if (job_id) *job_id = 0;
        s->result = ESP_ERR_NO_MEM;
        s->busy = false;
        s->complete = true;
        strlcpy(s->message, "Maintenance worker allocation failed", sizeof(s->message));
        publish(s);
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        s_read_job_active = false;
        __atomic_store_n(&s_read_cancelled, false, __ATOMIC_RELAXED);
        portEXIT_CRITICAL(&s_lock);
        free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
invalid:
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    s_read_job_active = false;
    __atomic_store_n(&s_read_cancelled, false, __ATOMIC_RELAXED);
    portEXIT_CRITICAL(&s_lock);
    free(job);
    return ESP_ERR_INVALID_ARG;
}
