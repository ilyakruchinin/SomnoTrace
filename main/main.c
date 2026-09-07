/*
 * SomnoTrace - application entry point
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_power.h"
#include "bsp_display.h"
#include "net_provision.h"
#include "as11_ble.h"
#include "oximeter.h"
#include "sd_storage.h"
#include "session_writer.h"
#include "nvs_writer.h"
#include "psram_task.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "ftp.h"
#include "time_sync.h"
#include "uploader.h"
#include "log_stream.h"
#include "device_settings.h"
#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "bsp_touch.h"
#include "crash_diag.h"
#include "therapy_alert.h"
#include "therapy_alert_runtime.h"
#include "first_run_setup.h"
#include "nvs_flash.h"
#include "sdkconfig.h"


static const char *TAG = "somnotrace";
static volatile bool s_softap_requested = false;

#define SETUP_STEP_BIT(step) ((uint8_t)(1U << (unsigned)(step)))

/* Boot facts intentionally distinguish a usable fact from evidence that this
 * is an upgrade of a pre-setup-schema installation.  In particular, a card
 * can already be inserted in a brand-new device, so card_ready must never by
 * itself suppress first-run setup. */
typedef struct {
    bool setup_state_missing;
    bool wifi_configured;
    bool timezone_saved;
    bool drift_saved;
    bool airsense_paired;
    bool card_ready;
    bool device_settings_saved;
    bool alert_config_saved;
    bool upload_config_saved;
    bool upload_destination_configured;
} first_run_boot_facts_t;

static bool uploader_has_destination(const uploader_config_t *cfg)
{
    if (!cfg) return false;
    bool smb = cfg->smb_enabled && cfg->smb_host[0] != '\0' &&
               cfg->smb_share[0] != '\0';
    bool sleephq = cfg->shq_enabled && cfg->shq_client_id[0] != '\0' &&
                   cfg->shq_client_secret[0] != '\0';
    return smb || sleephq;
}

static bool boot_has_legacy_setup_evidence(const first_run_boot_facts_t *facts)
{
    if (!facts) return false;
    return facts->wifi_configured || facts->timezone_saved ||
           facts->drift_saved || facts->airsense_paired ||
           facts->device_settings_saved || facts->alert_config_saved ||
           facts->upload_config_saved;
}

static bool setup_reconcile_needed(
    const first_run_setup_snapshot_t *snapshot,
    const first_run_setup_observed_t *observed)
{
    if (!snapshot || !observed || !snapshot->schema_compatible) return false;
    if (!snapshot->persisted) return true;

    const bool present[FIRST_RUN_SETUP_STEP_COUNT] = {
        [FIRST_RUN_SETUP_STEP_WIFI] = observed->wifi_configured,
        [FIRST_RUN_SETUP_STEP_TIME] = observed->time_configured,
        [FIRST_RUN_SETUP_STEP_AIRSENSE] = observed->airsense_paired,
        [FIRST_RUN_SETUP_STEP_CARD] = observed->card_present,
        [FIRST_RUN_SETUP_STEP_ALERTS] = observed->alerts_configured,
        [FIRST_RUN_SETUP_STEP_UPLOADS] = observed->uploads_configured,
    };
    for (unsigned step = 0; step < FIRST_RUN_SETUP_STEP_COUNT; step++) {
        if (present[step] &&
            (snapshot->state.completed_mask & SETUP_STEP_BIT(step)) == 0) {
            return true;
        }
    }
    return false;
}

static void reconcile_first_run_setup(const first_run_boot_facts_t *facts)
{
    if (!facts) return;

    const first_run_setup_observed_t observed = {
        .established_installation = facts->setup_state_missing &&
                                    boot_has_legacy_setup_evidence(facts),
        .wifi_configured = facts->wifi_configured,
        .time_configured = facts->timezone_saved,
        .airsense_paired = facts->airsense_paired,
        .card_present = facts->card_ready,
        .alerts_configured = facts->alert_config_saved,
        .uploads_configured = facts->upload_destination_configured,
    };

    first_run_setup_snapshot_t snapshot;
    first_run_setup_snapshot(&snapshot);
    if (!snapshot.schema_compatible) {
        ESP_LOGE(TAG, "first-run setup state unavailable: %s",
                 esp_err_to_name(snapshot.last_storage_result));
        return;
    }

    if (setup_reconcile_needed(&snapshot, &observed)) {
        esp_err_t err = first_run_setup_reconcile(&observed);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "first-run setup reconcile failed: %s",
                     esp_err_to_name(err));
            return;
        }
        first_run_setup_snapshot(&snapshot);
    }

    ESP_LOGI(TAG,
             "first-run setup: current=%u finished=%u persisted=%u legacy=%u",
             (unsigned)snapshot.state.current_step,
             (unsigned)first_run_setup_is_finished(&snapshot.state),
             (unsigned)snapshot.persisted,
             (unsigned)observed.established_installation);
}

static void request_softap_from_display(void)
{
    s_softap_requested = true;
}

static void show_status(const char *title, const char *lines[], int n)
{
    bsp_display_show_lines(title, lines, n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %s", lines[i]);
    }
}

static void controlled_restart(void)
{
    if (!netprov_lifecycle_try_claim("main-reboot")) {
        ESP_LOGW(TAG, "restart deferred because another lifecycle operation is active");
        bsp_display_set_notice("Restart deferred: update or restart in progress");
        return;
    }
    if (bsp_display_is_therapy_active() || sd_storage_recording_active() ||
        !bsp_display_try_reserve_therapy_safe_restart()) {
        ESP_LOGE(TAG, "restart deferred because storage is recording or busy");
        bsp_display_set_notice("Restart deferred: therapy or microSD is busy");
        netprov_lifecycle_release();
        return;
    }
    if (!sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE, 0)) {
        bsp_display_cancel_therapy_safe_restart();
        ESP_LOGE(TAG, "restart deferred because storage is busy");
        bsp_display_set_notice("Restart deferred: microSD is busy");
        netprov_lifecycle_release();
        return;
    }
    if (bsp_display_is_therapy_active() || sd_storage_recording_active() ||
        !bsp_display_try_commit_therapy_safe_restart()) {
        sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
        bsp_display_cancel_therapy_safe_restart();
        ESP_LOGW(TAG, "restart deferred because therapy start won lifecycle gate");
        bsp_display_set_notice("Restart deferred: therapy recording started");
        netprov_lifecycle_release();
        return;
    }
    sd_storage_deinit();
    esp_restart();
}

static bool enter_softap(const struct netprov_config *cfg)
{
    if (bsp_display_is_therapy_active() || sd_storage_recording_active()) {
        ESP_LOGW(TAG, "refusing SoftAP transition while therapy is recording");
        bsp_display_set_notice("Stop therapy before starting Wi-Fi setup");
        return false;
    }

    char ap_ip[16] = "0.0.0.0";
    esp_err_t err = netprov_start_portal(cfg, ap_ip);
    if (err != ESP_OK) {
        const char *lines[] = { "SoftAP failed" };
        show_status("Error", lines, 1);
        return false;
    }

    /* Only drop the CPAP link after the setup network is known to be live. */
    as11_ble_disconnect();
    bsp_display_set_wifi_connected(false);
    bsp_display_apply_backlight_policy(true);  /* always show display in AP mode */

    char ssid_line[48];
    snprintf(ssid_line, sizeof(ssid_line), "SSID: %s-setup", cfg->hostname);
    const char *lines[] = {
        "Wi-Fi Setup Mode",
        ssid_line,
        ap_ip,
        "Connect and configure",
    };
    show_status("Setup", lines, 4);
    return true;
}

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "SomnoTrace %s (IDF %s) starting up",
             app_desc ? app_desc->version : "unknown",
             app_desc ? app_desc->idf_ver : "?");

    /* 1. Power latch — must be first or device powers off on button release. */
    bsp_power_hold();

    /* 1b. Start log capture early so the ring buffer catches boot messages. */
    log_stream_init();

    /* 1c. Log reset reason and check for crash core dump from previous boot.
     * Must be after log_stream_init() so output is captured. */
    crash_diag_check();

    /* Create the static WithCaps reaper before finite PSRAM-backed workers can
     * start. Later task cleanup performs no internal-heap allocation. */
    ESP_ERROR_CHECK(psram_task_init());

    /* 2. Start button monitors. */
    bsp_power_start_button_monitor(5000);   /* PWR 5 s = power off */
    bsp_power_start_boot_monitor(&s_softap_requested, 5000);
    bsp_power_start_plus_monitor();         /* PLUS double-click = toggle therapy */

    /* 3. Initialise display. */
    bool display_ready = bsp_display_init() == ESP_OK;
    if (!display_ready) {
        ESP_LOGE(TAG, "display init failed");
    }
    bsp_display_set_setup_callback(request_softap_from_display);

    const char *boot_lines[] = { "Booting..." };
    show_status("SomnoTrace", boot_lines, 1);

    /* Initial battery reading for the status indicator */
    bsp_power_battery_monitor_start();

    /* 4. Initialise networking stack (includes NVS init). */
    ESP_ERROR_CHECK(netprov_init());

    /* 4-pre. The first-run record must be loaded before any setup-facing
     * service can accept input.  Start the internal-stack NVS proxy first and
     * inject it into components whose configuration probes run below. */
    nvs_writer_init();
    uploader_set_nvs_executor((uploader_nvs_exec_fn_t)nvs_writer_run);
    uploader_set_invalidation_hooks(session_writer_next_upload_invalidation,
                                    session_writer_ack_upload_invalidation);
    therapy_alert_set_nvs_executor((alert_nvs_exec_fn_t)nvs_writer_run);

    esp_err_t setup_load_ret = first_run_setup_load();
    if (setup_load_ret != ESP_OK &&
        setup_load_ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "first-run setup load failed: %s",
                 esp_err_to_name(setup_load_ret));
    }

    /* Capture durable configuration evidence before this boot can change it.
     * These reads distinguish an upgrade from a fresh device when the new
     * first-run namespace does not exist yet. */
    struct netprov_config cfg;
    bool has_creds = netprov_load_config(&cfg);

    /* 4a. Load device settings (brightness, LCD therapy mode) and apply.
     * Must be after netprov_init() which calls nvs_flash_init(). */
    device_settings_t dev_cfg;
    esp_err_t device_settings_ret = device_settings_load(&dev_cfg);
    bsp_display_set_brightness(dev_cfg.brightness);
    bsp_audio_set_volume(dev_cfg.alert_volume);
    bsp_display_set_rotation(dev_cfg.lcd_rotation);

    char saved_timezone[64];
    time_sync_get_timezone(saved_timezone, sizeof(saved_timezone));
    bool timezone_saved = saved_timezone[0] != '\0';
    bool drift_saved = time_sync_has_drift();

    therapy_alert_config_t alert_cfg_probe;
    bool alert_config_saved =
        therapy_alert_load_config(&alert_cfg_probe) == ESP_OK;

    uploader_config_t upload_cfg_probe;
    bool upload_config_saved =
        uploader_load_config(&upload_cfg_probe) == ESP_OK;
    bool upload_destination_configured =
        upload_config_saved && uploader_has_destination(&upload_cfg_probe);

    /* 4a-bis. Apply the saved timezone now, before BLE can reconnect.
     * as11_ble_init() may find therapy already running and start a session
     * immediately; without TZ applied the session id is generated in UTC
     * (e.g. 20260807_200019 for a session that really began 06:00 local). */
    time_sync_apply_saved_timezone();

    /* 4b. Initialise SD card storage and session writer BEFORE BLE.
     *
     * Ordering is load-bearing, not cosmetic.  as11_ble_init() starts
     * reconnect_task, which can find therapy already running and drive
     * session_writer_on_stream_data_raw() into session_writer_start() from
     * the first StreamData notification.  If that happened before
     * session_writer_init(), session_writer_start() would take a NULL
     * s_active_mutex; and session_writer_recover() — which treats "no
     * session.json" as "interrupted" — could stamp the *live* session as
     * interrupted.  Initialising storage and running recovery first removes
     * both races instead of relying on reconnect_task being slow. */
    esp_err_t sd_ret = sd_storage_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed; session storage unavailable");
        /* Distinguish "no card" from "card present but mount/format error".
         * ESP_ERR_NOT_FOUND = SDMMC host couldn't probe a card (none inserted).
         * Other errors (e.g. ESP_ERR_INVALID_STATE, FR_NO_FILESYSTEM) mean
         * the card is physically present but unusable. */
        const char *sd_title, *sd_lines[2];
        int sd_nlines;
        if (sd_ret == ESP_ERR_NOT_FOUND) {
            sd_title = "Warning";
            sd_lines[0] = "Insert microSD";
            sd_lines[1] = "Power off, insert microSD";
            sd_nlines = 2;
        } else {
            sd_title = "microSD error";
            sd_lines[0] = "microSD mount failed";
            sd_lines[1] = "Check or reformat microSD";
            sd_nlines = 2;
        }
        show_status(sd_title, sd_lines, sd_nlines);
        /* Hold the warning for 3 seconds before continuing boot */
        vTaskDelay(pdMS_TO_TICKS(3000));
    } else {
        if (session_writer_init() != ESP_OK) {
            /* The storage worker could not be created, so no session can be
             * durably recorded.  Say so now rather than discovering it at
             * TherapyStop, when the night is already lost. */
            ESP_LOGE(TAG, "session writer init failed; recording unavailable");
            bsp_display_set_notice("Recording OFF");
        } else {
        }
        session_writer_recover();
    }

    /* 4b-2. Initialise board audio before BLE. On the compact board, also
     * initialize its shared I2C bus and CST816 touch controller first because
     * BLE RF activity can make setup-time register writes NACK. The 7B display
     * owns its separate controller and GT911 initialization. */
#if !CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B
    bsp_i2c_init();
#endif
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio codec init failed — buzzer will be unavailable");
    }
#if !CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B
    if (bsp_touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed — tap to wake unavailable");
    }
#endif

    /* 4c. Initialise BLE (AirSense 11 pairing). Non-fatal on failure.
     * Runs after storage init + crash recovery (see 4b). */
    bool as11_ready = as11_ble_init() == ESP_OK;
    if (!as11_ready) {
        ESP_LOGE(TAG, "BLE init failed; CPAP pairing unavailable");
    }
    ESP_LOGI(TAG, "[heap] after BLE init: internal free=%u min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    /* Publish one coherent, persisted setup snapshot before native touch
     * services become interactive.  A mounted card resolves the card check,
     * but is deliberately excluded from legacy-installation evidence. */
    const first_run_boot_facts_t setup_facts = {
        .setup_state_missing = setup_load_ret == ESP_ERR_NVS_NOT_FOUND,
        .wifi_configured = has_creds,
        .timezone_saved = timezone_saved,
        .drift_saved = drift_saved,
        .airsense_paired = as11_ble_is_paired(),
        .card_ready = sd_ret == ESP_OK && sd_storage_is_ready(),
        .device_settings_saved = device_settings_ret == ESP_OK,
        .alert_config_saved = alert_config_saved,
        .upload_config_saved = upload_config_saved,
        .upload_destination_configured = upload_destination_configured,
    };
    reconcile_first_run_setup(&setup_facts);

    /* 4c-ter. Initialise O2 Ring oximeter (shares NimBLE host with AS11). */
    bool oximeter_ready = oximeter_init() == ESP_OK;
    if (!oximeter_ready) {
        ESP_LOGE(TAG, "Oximeter init failed; O2 Ring sync unavailable");
    }
    /* Native touch controls can now safely query BLE driver state. */
    bsp_display_enable_touch_services(as11_ready, oximeter_ready);

    /* 4c-bis. BLE startup has begun, so reconnect can now establish whether
     * therapy is already running.  Only now is it safe to let the idle post
     * worker export days that boot recovery queued: doing it earlier could
     * run a multi-minute rebuild while a live session was trying to start. */
    session_writer_enable_deferred_export();

    /* 4d. Init therapy alert subsystem (loads config from NVS). */
    therapy_alert_set_task_fns(psram_task_create, psram_task_delete);
    therapy_alert_set_beep_fn(bsp_audio_beep);
    therapy_alert_set_therapy_active_fn(bsp_display_is_therapy_active);
    therapy_alert_init();

    first_run_setup_snapshot_t setup_snapshot;
    first_run_setup_snapshot(&setup_snapshot);
    bool setup_incomplete = setup_snapshot.schema_compatible &&
        !first_run_setup_is_finished(&setup_snapshot.state);
    bool native_setup_active = false;

#if CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B
    /* A fresh touch device is configured entirely on the panel.  Do not send
     * it into the old blocking captive-portal loop just because credentials
     * are absent; the native setup worker owns scan/join instead. */
    if (setup_incomplete) {
        esp_err_t setup_ui = bsp_display_start_first_run_setup(sd_ret);
        if (setup_ui != ESP_OK) {
            ESP_LOGE(TAG, "could not open native first-run setup: %s",
                     esp_err_to_name(setup_ui));
            bsp_display_set_critical_notice("Setup screen unavailable");
        } else {
            native_setup_active = bsp_display_first_run_setup_active();
        }
    }
#endif

    /* 6. If BOOT was held at boot, force SoftAP regardless. */
    if (s_softap_requested) {
        ESP_LOGW(TAG, "BOOT long-press detected: forcing SoftAP");
        while (!enter_softap(&cfg)) {
            ESP_LOGW(TAG, "SoftAP start failed; retrying in 5 seconds");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* 7. Try to connect to configured Wi-Fi. */
    char ip[16] = "0.0.0.0";
    esp_err_t err = ESP_FAIL;
    if (has_creds) {
        const char *lines[] = { "Connecting to Wi-Fi..." };
        show_status("SomnoTrace", lines, 1);
        err = netprov_try_connect(&cfg, ip, 15000);
    }

    bool in_softap = false;
    uint32_t softap_start_ticks = 0;
    bool wifi_connected = false;
    bool degraded_mode = false;
    bool ntp_ok = false;

    /* Always initialise time sync: loads the timezone from NVS (no network
     * needed) and starts SNTP (which will simply time out without Wi-Fi). */
    time_sync_init();

    if (err == ESP_OK) {
        wifi_connected = true;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=%s", ip);
        ESP_LOGI(TAG, "[heap] after WiFi: internal free=%u min=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        bsp_display_set_wifi_connected(true);
        netprov_start_connected_server(ip);

        /* ── Initial NTP sync with failure handling ─── */
        ntp_ok = time_sync_wait_initial();
        if (!ntp_ok) {
            ESP_LOGW(TAG, "NTP sync failed");
        }
        if (!ntp_ok && time_sync_has_drift() && as11_ble_is_paired()) {
            /* Drift is only useful if we can read the AS11 clock over BLE.
             * Wait for BLE to connect before entering degraded mode. */
            const char *wait_lines[] = { "Waiting for CPAP..." };
            show_status("SomnoTrace", wait_lines, 1);
            for (int i = 0; i < 30; i++) {
                if (strcmp(as11_ble_get_status(), AS11_STATUS_PAIRED) == 0) {
                    degraded_mode = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (!degraded_mode) {
                ESP_LOGW(TAG, "BLE not connected after 30s — drift unusable");
            }
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi connect failed");
        if (time_sync_has_drift() && as11_ble_is_paired()) {
            /* Wi-Fi failed but we might still use AS11 clock + drift.
             * BLE reconnect has been running since boot — by now the fast
             * retries are done.  Give it a short window to connect. */
            for (int i = 0; i < 30; i++) {
                if (strcmp(as11_ble_get_status(), AS11_STATUS_PAIRED) == 0) {
                    degraded_mode = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (!degraded_mode) {
                ESP_LOGW(TAG, "BLE not connected after 30s — drift unusable");
            }
        }
    }

    /* ── Handle degraded mode (no NTP, but drift available) ─── */
    if (degraded_mode) {
        ESP_LOGW(TAG, "Entering degraded mode: clock estimated from AS11 + stored drift");

        /* Persistent banner rather than a status screen: the status lines are
         * reused for Wi-Fi/SD messages and would otherwise hide this. */
        bsp_display_set_notice("Estimated time");

        if (bsp_audio_init() == ESP_OK) {
            bsp_audio_beep(660, 300, 100);
        }
        /* Proceed — time will be recovered from AS11 after BLE connects. */
    }

    /* ── Handle total time failure (no NTP, no drift) ─── */
    if (!degraded_mode) {
        /* No drift available. If Wi-Fi also failed or NTP failed, we need
         * to either reboot or enter SoftAP (reboot-loop guard). */
        bool time_failed = !wifi_connected || (wifi_connected && !ntp_ok);
        if (time_failed && !native_setup_active) {
            nvs_handle_t nvs_h;
            int boot_fail_count = 0;
            nvs_writer_lock();
            if (nvs_open("cfg", NVS_READWRITE, &nvs_h) == ESP_OK) {
                nvs_get_i32(nvs_h, "boot_fail", (int32_t *)&boot_fail_count);
                boot_fail_count++;
                nvs_set_i32(nvs_h, "boot_fail", boot_fail_count);
                nvs_commit(nvs_h);
                nvs_close(nvs_h);
            }
            nvs_writer_unlock();

            if (boot_fail_count >= 3) {
                ESP_LOGW(TAG, "3+ consecutive boot failures — entering SoftAP for user intervention");
                nvs_writer_lock();
                if (nvs_open("cfg", NVS_READWRITE, &nvs_h) == ESP_OK) {
                    nvs_set_i32(nvs_h, "boot_fail", 0);
                    nvs_commit(nvs_h);
                    nvs_close(nvs_h);
                }
                nvs_writer_unlock();
                if (enter_softap(&cfg)) {
                    in_softap = true;
                    softap_start_ticks = xTaskGetTickCount();
                }
            } else {
                ESP_LOGE(TAG, "No time source (attempt %d/3) — alarm + reboot",
                         boot_fail_count);

                const char *fail_lines[] = {
                    wifi_connected ? "NTP Sync Failed" : "No Wi-Fi / No NTP",
                    "Hold BOOT for setup",
                };
                show_status("Error", fail_lines, 2);

                if (bsp_audio_init() == ESP_OK) {
                    for (int i = 0; i < 5; i++) {
                        bsp_audio_beep(880, 1000, 100);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        if (s_softap_requested) break;
                    }
                }

                if (s_softap_requested) {
                    ESP_LOGW(TAG, "BOOT pressed during alarm: entering SoftAP");
                    if (enter_softap(&cfg)) {
                        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(500));
                controlled_restart();
            }
        } else if (time_failed) {
            /* Setup must remain usable offline.  It can establish Wi-Fi and
             * timezone asynchronously, after which the normal link loop will
             * start NTP and connected services without a reboot. */
            ESP_LOGW(TAG, "no time source yet; native first-run setup remains active");
        }
    } else {
        /* Degraded mode — reset boot failure counter. */
        nvs_handle_t nvs_h;
        nvs_writer_lock();
        if (nvs_open("cfg", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_i32(nvs_h, "boot_fail", 0);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
        nvs_writer_unlock();
    }

    /* ── Reset boot failure counter on a fully successful boot ─── */
    if (wifi_connected && ntp_ok && !degraded_mode) {
        nvs_handle_t nvs_h;
        nvs_writer_lock();
        if (nvs_open("cfg", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_i32(nvs_h, "boot_fail", 0);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
        nvs_writer_unlock();
    }

    /* ── Normal boot continuation (Wi-Fi connected or degraded mode) ─── */
    if (!in_softap) {
        if (wifi_connected) {
            if (sd_storage_is_ready()) {
                uploader_config_t upcfg;
                if (uploader_load_config(&upcfg) == ESP_OK && upcfg.ftp_enabled) {
                    ftp_anonymous_mode = upcfg.ftp_anonymous;
                    if (upcfg.ftp_anonymous) {
                        strlcpy(ftp_user, "anonymous", sizeof(ftp_user));
                        strlcpy(ftp_pass, "anonymous@", sizeof(ftp_pass));
                    } else {
                        strlcpy(ftp_user, upcfg.ftp_user, sizeof(ftp_user));
                        strlcpy(ftp_pass, upcfg.ftp_pass, sizeof(ftp_pass));
                    }
                    ftp_server_start();
                    ESP_LOGI(TAG, "FTP server started (%s mode)",
                             upcfg.ftp_anonymous ? "anonymous" : "authenticated");
                } else {
                    ESP_LOGI(TAG, "FTP server disabled in config");
                }
            }
            uploader_init();
            uploader_set_progress_notify_fn(log_stream_request_upload_push);

            char mdns_name[12];
            netprov_get_mdns_name(mdns_name, sizeof(mdns_name));
            char url_line[32];
            snprintf(url_line, sizeof(url_line), "http://%s.local", mdns_name);
            netprov_link_t link;
            netprov_get_link(&link);
            char ip_line[20];
            snprintf(ip_line, sizeof(ip_line), "%s", link.ip);
            const char *lines[] = {
                link.ssid[0] ? link.ssid : "Wi-Fi Connected",
                url_line,
                ip_line,
            };
            show_status("SomnoTrace", lines, 3);
        } else {
            /* Booted without Wi-Fi.  Recording still works (time comes from
             * the AS11), but keep hunting for a configured network in the
             * background so a router power blip heals itself. */
            ESP_LOGI(TAG, "starting link supervisor for background Wi-Fi retry");
            netprov_start_link_supervisor();
            netprov_request_rescan();

            const char *lines[] = {
                "Offline",
                "Retrying Wi-Fi...",
            };
            show_status("SomnoTrace", lines, 2);
        }

        bsp_display_apply_backlight_policy(false);
    }

    int refresh_counter = 0;
    static bool post_connect_init_done = false;
    if (wifi_connected) post_connect_init_done = true;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_softap_requested && !in_softap) {
            ESP_LOGW(TAG, "BOOT long-press detected at runtime: entering SoftAP");
            if (enter_softap(&cfg)) {
                in_softap = true;
                softap_start_ticks = xTaskGetTickCount();
            }
        }
        if (in_softap) {
            /* SoftAP idle timeout */
            if ((xTaskGetTickCount() - softap_start_ticks) * portTICK_PERIOD_MS
                 > 10 * 60 * 1000) {
                ESP_LOGW(TAG, "SoftAP 10-minute idle timeout: rebooting to retry connection");
                controlled_restart();
            }
            /* Update battery indicator in SoftAP mode */
            bsp_battery_t batt;
            bsp_power_battery_get(&batt);
            bsp_display_set_battery(batt.percent, batt.charging, batt.valid);
        } else {
            /* Update battery indicator every second (immediate redraw only on state change) */
            if (!bsp_display_is_therapy_active()) {
                bsp_battery_t batt;
                bsp_power_battery_get(&batt);
                bsp_display_set_battery(batt.percent, batt.charging, batt.valid);
            }

            /* Connected mode: refresh status display every 3 s.
             * Skipped during therapy (graph mode owns the display). */
            if (++refresh_counter >= 3 && !bsp_display_is_therapy_active()) {
                refresh_counter = 0;

                /* Use live link state instead of boot-time assumption. */
                netprov_link_t link;
                netprov_get_link(&link);

                /* Deferred init: if we booted offline and Wi-Fi just came up,
                 * start the web server and uploader now. */
                if (link.up && !post_connect_init_done) {
                    ESP_LOGI(TAG, "Wi-Fi recovered after offline boot — starting services");
                    bsp_display_set_wifi_connected(true);
                    netprov_start_connected_server(link.ip);
                    if (sd_storage_is_ready()) {
                        uploader_config_t upcfg;
                        if (uploader_load_config(&upcfg) == ESP_OK && upcfg.ftp_enabled) {
                            ftp_anonymous_mode = upcfg.ftp_anonymous;
                            if (upcfg.ftp_anonymous) {
                                strlcpy(ftp_user, "anonymous", sizeof(ftp_user));
                                strlcpy(ftp_pass, "anonymous@", sizeof(ftp_pass));
                            } else {
                                strlcpy(ftp_user, upcfg.ftp_user, sizeof(ftp_user));
                                strlcpy(ftp_pass, upcfg.ftp_pass, sizeof(ftp_pass));
                            }
                            ftp_server_start();
                        }
                    }
                    uploader_init();
                    uploader_set_progress_notify_fn(log_stream_request_upload_push);
                    post_connect_init_done = true;

                    /* Try NTP now that we have a network. */
                    if (!ntp_ok) {
                        ntp_ok = time_sync_wait_initial();
                        if (ntp_ok) {
                            /* NTP succeeded — clear the notice banner. */
                            bsp_display_set_notice(NULL);
                        }
                    }
                }

                if (!link.up) {
                    const char *lines[] = {
                        "Offline",
                        "Retrying Wi-Fi...",
                    };
                    bsp_display_show_lines("SomnoTrace", lines, 2);
                } else {
                    char mdns_name[12];
                    netprov_get_mdns_name(mdns_name, sizeof(mdns_name));
                    char url_line[32];
                    snprintf(url_line, sizeof(url_line), "http://%s.local", mdns_name);
                    const char *ssid_str = link.ssid[0] ? link.ssid : "Wi-Fi Connected";
                    char ip_line[20];
                    snprintf(ip_line, sizeof(ip_line), "%s", link.ip);
                    if (sd_storage_is_ready()) {
                        const char *lines[] = {
                            ssid_str,
                            url_line,
                            ip_line,
                        };
                        bsp_display_show_lines("SomnoTrace", lines, 3);
                    } else {
                        const char *lines[] = {
                            ssid_str,
                            url_line,
                            ip_line,
                            "microSD error",
                        };
                        bsp_display_show_lines("SomnoTrace", lines, 4);
                    }
                }
            }
        }
    }
}
