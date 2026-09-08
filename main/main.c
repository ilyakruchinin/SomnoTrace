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
#include "nvs_flash.h"


static const char *TAG = "somnotrace";
static volatile bool s_softap_requested = false;

static void show_status(const char *title, const char *lines[], int n)
{
    bsp_display_show_lines(title, lines, n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %s", lines[i]);
    }
}

static void enter_softap(const struct netprov_config *cfg)
{
    as11_ble_disconnect();
    bsp_display_set_wifi_connected(false);
    bsp_display_apply_backlight_policy(true);  /* always show display in AP mode */
    char ap_ip[16] = "0.0.0.0";
    esp_err_t err = netprov_start_portal(cfg, ap_ip);
    if (err != ESP_OK) {
        const char *lines[] = { "SoftAP failed" };
        show_status("Error", lines, 1);
        return;
    }

    char ssid_line[48];
    snprintf(ssid_line, sizeof(ssid_line), "SSID: %s-setup", cfg->hostname);
    const char *lines[] = {
        "Wi-Fi Setup Mode",
        ssid_line,
        ap_ip,
        "Connect and configure",
    };
    show_status("Setup", lines, 4);
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

    /* 2. Start button monitors. */
    bsp_power_start_button_monitor(5000);   /* PWR 5 s = power off */
    bsp_power_start_boot_monitor(&s_softap_requested, 5000);
    bsp_power_start_plus_monitor();         /* PLUS double-click = toggle therapy */

    /* 3. Initialise display. */
    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
    }

    const char *boot_lines[] = { "Booting..." };
    show_status("SomnoTrace", boot_lines, 1);

    /* Initial battery reading for the status indicator */
    bsp_power_battery_monitor_start();

    /* 4. Initialise networking stack (includes NVS init). */
    ESP_ERROR_CHECK(netprov_init());

    /* 4a. Load device settings (brightness, LCD therapy mode) and apply.
     * Must be after netprov_init() which calls nvs_flash_init(). */
    device_settings_t dev_cfg;
    device_settings_load(&dev_cfg);
    bsp_display_set_brightness(dev_cfg.brightness);
    bsp_audio_set_volume(dev_cfg.alert_volume);
    bsp_display_set_rotation(dev_cfg.lcd_rotation);

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
            sd_lines[0] = "Insert SD Card";
            sd_lines[1] = "Power off, insert card,";
            sd_nlines = 2;
        } else {
            sd_title = "SD Card Error";
            sd_lines[0] = "Card mount failed";
            sd_lines[1] = "Check or reformat card";
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
        }
        session_writer_recover();
    }

    /* 4b-2. Initialise audio codec and touch sensor BEFORE BLE —
     * BLE RF activity during connection causes I2C bus noise that makes
     * register writes NACK. The devices share the I2C bus on GPIO 41/42. */
    bsp_i2c_init();
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio codec init failed — buzzer will be unavailable");
    }
    if (bsp_touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed — tap to wake unavailable");
    }

    /* 4c. Initialise BLE (AirSense 11 pairing). Non-fatal on failure.
     * Runs after storage init + crash recovery (see 4b). */
    if (as11_ble_init() != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed; CPAP pairing unavailable");
    }
    ESP_LOGI(TAG, "[heap] after BLE init: internal free=%u min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    /* 4c-ter. Initialise O2 Ring oximeter (shares NimBLE host with AS11). */
    if (oximeter_init() != ESP_OK) {
        ESP_LOGE(TAG, "Oximeter init failed; O2 Ring sync unavailable");
    }

    /* 4c-bis. BLE startup has begun, so reconnect can now establish whether
     * therapy is already running.  Only now is it safe to let the idle post
     * worker export days that boot recovery queued: doing it earlier could
     * run a multi-minute rebuild while a live session was trying to start. */
    session_writer_enable_deferred_export();

    /* 4d. Init therapy alert subsystem (loads config from NVS). */
    therapy_alert_set_beep_fn(bsp_audio_beep);
    therapy_alert_set_therapy_active_fn(bsp_display_is_therapy_active);
    therapy_alert_init();

    /* 5. Load config from NVS. */
    struct netprov_config cfg;
    bool has_creds = netprov_load_config(&cfg);

    /* 6. If BOOT was held at boot, force SoftAP regardless. */
    if (s_softap_requested) {
        ESP_LOGW(TAG, "BOOT long-press detected: forcing SoftAP");
        enter_softap(&cfg);
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
        if (time_failed) {
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
                enter_softap(&cfg);
                in_softap = true;
                softap_start_ticks = xTaskGetTickCount();
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
                    enter_softap(&cfg);
                    while (true) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
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
            in_softap = true;
            enter_softap(&cfg);
            softap_start_ticks = xTaskGetTickCount();
        }
        if (in_softap) {
            /* SoftAP idle timeout */
            if ((xTaskGetTickCount() - softap_start_ticks) * portTICK_PERIOD_MS
                 > 10 * 60 * 1000) {
                ESP_LOGW(TAG, "SoftAP 10-minute idle timeout: rebooting to retry connection");
                esp_restart();
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
                            "SD Card Error",
                        };
                        bsp_display_show_lines("SomnoTrace", lines, 4);
                    }
                }
            }
        }
    }
}
