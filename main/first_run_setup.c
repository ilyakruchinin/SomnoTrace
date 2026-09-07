/*
 * SomnoTrace - Persistent first-run setup service
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "first_run_setup.h"
#include "first_run_setup_internal.h"
#include "nvs_writer.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "first_run";

#define NVS_NAMESPACE   "first_run"
#define NVS_KEY_SCHEMA  "schema"
#define NVS_KEY_STATE   "state"

#define RECORD_FLAG_CONTINUE_WITHOUT_RECORDING (1U << 0)

/* The schema key is separate from the blob so future firmware can reject a
 * newer record before making any assumption about that record's size/layout. */
typedef struct {
    uint8_t current_step;
    uint8_t completed_mask;
    uint8_t skipped_mask;
    uint8_t flags;
} first_run_record_v1_t;

_Static_assert(sizeof(first_run_record_v1_t) == 4,
               "first-run v1 record layout changed");

static first_run_setup_state_t s_state;
/* Writer arguments live in static internal DRAM. A callback still copies this
 * value onto the nvs_writer task's internal stack before opening NVS. */
static first_run_setup_state_t s_nvs_work;
static bool s_loaded;
static bool s_persisted;
static bool s_schema_compatible;
static esp_err_t s_last_storage_result = ESP_ERR_INVALID_STATE;

static StaticSemaphore_t s_state_mutex_storage;
static StaticSemaphore_t s_operation_mutex_storage;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_operation_mutex;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_runtime_initialized;

static void ensure_runtime(void)
{
    if (s_state_mutex && s_operation_mutex && s_runtime_initialized) return;

    portENTER_CRITICAL(&s_mutex_init_lock);
    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_storage);
    }
    if (!s_operation_mutex) {
        s_operation_mutex = xSemaphoreCreateMutexStatic(
            &s_operation_mutex_storage);
    }
    if (!s_runtime_initialized) {
        first_run_setup_model_defaults(&s_state);
        s_loaded = false;
        s_persisted = false;
        s_schema_compatible = false;
        s_last_storage_result = ESP_ERR_INVALID_STATE;
        s_runtime_initialized = true;
    }
    portEXIT_CRITICAL(&s_mutex_init_lock);
}

static void state_lock(void)
{
    ensure_runtime();
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static void state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}

static void publish_state(const first_run_setup_state_t *state,
                          bool persisted,
                          bool schema_compatible,
                          esp_err_t storage_result)
{
    state_lock();
    s_state = *state;
    s_loaded = true;
    s_persisted = persisted;
    s_schema_compatible = schema_compatible;
    s_last_storage_result = storage_result;
    state_unlock();
}

static void publish_storage_result(esp_err_t storage_result)
{
    state_lock();
    s_last_storage_result = storage_result;
    state_unlock();
}

static esp_err_t do_load_nvs(void *arg)
{
    first_run_setup_state_t decoded;
    first_run_setup_model_defaults(&decoded);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    uint16_t schema = 0;
    err = nvs_get_u16(h, NVS_KEY_SCHEMA, &schema);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    switch (schema) {
        case 1: {
            first_run_record_v1_t record = {0};
            size_t record_size = sizeof(record);
            err = nvs_get_blob(h, NVS_KEY_STATE, &record, &record_size);
            if (err == ESP_OK && record_size != sizeof(record)) {
                err = ESP_ERR_INVALID_SIZE;
            }
            if (err == ESP_OK) {
                decoded.schema_version = FIRST_RUN_SETUP_SCHEMA_VERSION;
                decoded.current_step =
                    (first_run_setup_step_t)record.current_step;
                decoded.completed_mask = record.completed_mask;
                decoded.skipped_mask = record.skipped_mask;
                decoded.continue_without_recording =
                    (record.flags &
                     RECORD_FLAG_CONTINUE_WITHOUT_RECORDING) != 0;
            }
            break;
        }

        /* Add explicit migrations here when schema v2 is introduced. */
        default:
            err = ESP_ERR_INVALID_VERSION;
            break;
    }

    nvs_close(h);
    if (err != ESP_OK) return err;
    if (!first_run_setup_model_is_valid(&decoded)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* NVS is closed before caller-owned/static output memory is touched. */
    *(first_run_setup_state_t *)arg = decoded;
    return ESP_OK;
}

static esp_err_t do_save_nvs(void *arg)
{
    /* Copy before nvs_open: arg can never be dereferenced while flash has the
     * cache disabled, even if this callback's caller used a PSRAM stack. */
    const first_run_setup_state_t state =
        *(const first_run_setup_state_t *)arg;
    if (!first_run_setup_model_is_valid(&state)) {
        return ESP_ERR_INVALID_ARG;
    }

    const first_run_record_v1_t record = {
        .current_step = (uint8_t)state.current_step,
        .completed_mask = state.completed_mask,
        .skipped_mask = state.skipped_mask,
        .flags = state.continue_without_recording
                     ? RECORD_FLAG_CONTINUE_WITHOUT_RECORDING
                     : 0,
    };

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u16(h, NVS_KEY_SCHEMA,
                      FIRST_RUN_SETUP_SCHEMA_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_STATE, &record, sizeof(record));
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t do_reset_nvs(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t persist_candidate(const first_run_setup_state_t *candidate)
{
    /* s_operation_mutex serializes this static work area. It is deliberately
     * not caller stack memory, because a touchscreen/UI caller may use PSRAM. */
    s_nvs_work = *candidate;
    return nvs_writer_run(do_save_nvs, &s_nvs_work);
}

esp_err_t first_run_setup_load(void)
{
    ensure_runtime();
    /* Prevent nvs_writer_run() from taking its early-boot inline fallback when
     * this API is first reached from a PSRAM-stack UI or HTTP task. */
    nvs_writer_init();
    xSemaphoreTake(s_operation_mutex, portMAX_DELAY);

    first_run_setup_model_defaults(&s_nvs_work);
    esp_err_t err = nvs_writer_run(do_load_nvs, &s_nvs_work);
    if (err == ESP_OK) {
        publish_state(&s_nvs_work, true, true, ESP_OK);
        ESP_LOGI(TAG, "loaded schema=%u current=%u complete=0x%02x skip=0x%02x no-record=%u",
                 (unsigned)s_nvs_work.schema_version,
                 (unsigned)s_nvs_work.current_step,
                 s_nvs_work.completed_mask, s_nvs_work.skipped_mask,
                 s_nvs_work.continue_without_recording);
    } else {
        first_run_setup_state_t defaults;
        first_run_setup_model_defaults(&defaults);
        bool missing = err == ESP_ERR_NVS_NOT_FOUND;
        bool record_present = err == ESP_ERR_INVALID_VERSION ||
                              err == ESP_ERR_INVALID_SIZE ||
                              err == ESP_ERR_INVALID_STATE;
        publish_state(&defaults, record_present, missing, err);
        if (missing) {
            ESP_LOGI(TAG, "no setup state in NVS - starting at Wi-Fi");
        } else {
            ESP_LOGE(TAG, "setup state unavailable: %s", esp_err_to_name(err));
        }
    }

    xSemaphoreGive(s_operation_mutex);
    return err;
}

void first_run_setup_snapshot(first_run_setup_snapshot_t *out)
{
    if (!out) return;
    state_lock();
    out->state = s_state;
    out->persisted = s_persisted;
    out->schema_compatible = s_schema_compatible;
    out->last_storage_result = s_last_storage_result;
    state_unlock();
}

static esp_err_t copy_mutable_state(first_run_setup_state_t *out,
                                    bool *persisted)
{
    state_lock();
    if (!s_loaded) {
        state_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_schema_compatible) {
        esp_err_t err = s_last_storage_result;
        state_unlock();
        return err == ESP_OK ? ESP_ERR_INVALID_VERSION : err;
    }
    *out = s_state;
    if (persisted) *persisted = s_persisted;
    state_unlock();
    return ESP_OK;
}

esp_err_t first_run_setup_update(first_run_setup_step_t step,
                                 first_run_setup_update_t update)
{
    ensure_runtime();
    nvs_writer_init();
    xSemaphoreTake(s_operation_mutex, portMAX_DELAY);

    first_run_setup_state_t candidate;
    esp_err_t err = copy_mutable_state(&candidate, NULL);
    if (err == ESP_OK &&
        !first_run_setup_model_apply(&candidate, step, update)) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) err = persist_candidate(&candidate);
    if (err == ESP_OK) {
        publish_state(&candidate, true, true, ESP_OK);
    } else {
        publish_storage_result(err);
    }

    xSemaphoreGive(s_operation_mutex);
    return err;
}

esp_err_t first_run_setup_reconcile(
    const first_run_setup_observed_t *observed)
{
    if (!observed) return ESP_ERR_INVALID_ARG;
    ensure_runtime();
    nvs_writer_init();
    xSemaphoreTake(s_operation_mutex, portMAX_DELAY);

    first_run_setup_state_t candidate;
    bool had_persisted_state = false;
    esp_err_t err = copy_mutable_state(&candidate, &had_persisted_state);
    if (err == ESP_OK &&
        !first_run_setup_model_reconcile(&candidate, observed,
                                         had_persisted_state)) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) err = persist_candidate(&candidate);
    if (err == ESP_OK) {
        publish_state(&candidate, true, true, ESP_OK);
    } else {
        publish_storage_result(err);
    }

    xSemaphoreGive(s_operation_mutex);
    return err;
}

esp_err_t first_run_setup_reset(void)
{
    ensure_runtime();
    nvs_writer_init();
    xSemaphoreTake(s_operation_mutex, portMAX_DELAY);

    esp_err_t err = nvs_writer_run(do_reset_nvs, NULL);
    if (err == ESP_OK) {
        first_run_setup_state_t defaults;
        first_run_setup_model_defaults(&defaults);
        publish_state(&defaults, false, true, ESP_OK);
        ESP_LOGI(TAG, "setup progress reset");
    } else {
        publish_storage_result(err);
    }

    xSemaphoreGive(s_operation_mutex);
    return err;
}
