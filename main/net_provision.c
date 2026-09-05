/*
 * SomnoTrace - Wi-Fi provisioning, SoftAP captive portal, and NVS config
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


#include "net_provision.h"
#include "as11_ble.h"
#include "oximeter.h"
#include "time_sync.h"
#include "uploader.h"
#include "upload_sched.h"
#include "therapy_alert.h"
#include "edf_gen.h"
#include "as11_time.h"
#include "sd_storage.h"
#include "ff.h"
#include "log_stream.h"
#include "device_settings.h"
#include "session_graph.h"
#include "session_writer.h"
#include "oximetry_http.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "bsp_display.h"
#include "bsp_power.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "nvs_writer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "psram_task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "mdns.h"

static const char *TAG = "netprov";

#define NVS_NAMESPACE       "cfg"
#define NVS_KEY_HOSTNAME    "hostname"
#define NVS_KEY_SSID_FMT    "ssid%d"
#define NVS_KEY_PASS_FMT    "pass%d"
#define NVS_KEY_MDNS_NAME   "mdns_name"
#define MDNS_NAME_MAX       11   /* 10 chars + NUL */

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_STA_RETRY       3

/* Failed reconnects to the current SSID before falling back to a full
 * scan across every configured network.  Without this the driver retries
 * one dead SSID forever and never fails over. */
#define RECONNECT_TRIES_BEFORE_RESCAN  5

static EventGroupHandle_t s_wifi_events;
static int s_retry_num = 0;
static volatile bool s_connecting = false;
static volatile bool s_connected = false;
static char s_got_ip[16];
static httpd_handle_t s_httpd = NULL;
static bool s_portal_mode = false;
static char s_connected_ip[16] = "0.0.0.0";
static char s_ap_ssid[NETPROV_HOSTNAME_MAXLEN + 8];
static uint32_t s_ap_ip = 0;

/* Link state published to the LCD and /api/status. */
static char s_link_ssid[NETPROV_SSID_MAXLEN + 1] = "";
static SemaphoreHandle_t s_link_mutex = NULL;
static volatile int  s_reconnect_tries = 0;
static volatile bool s_rescan_requested = false;
/* Copy of the credentials kept for autonomous failover rescans. */
static struct netprov_config s_link_cfg;
static bool s_link_cfg_valid = false;

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;

/* ------------------------------------------------------------------ */
/*  NVS config storage                                                */
/* ------------------------------------------------------------------ */
static esp_err_t do_netprov_load(void *arg)
{
    struct netprov_config *out = arg;
    struct netprov_config local = {0};
    strlcpy(local.hostname, "SomnoTrace", sizeof(local.hostname));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;

    size_t len = sizeof(local.hostname);
    nvs_get_str(h, NVS_KEY_HOSTNAME, local.hostname, &len);
    bool any = false;
    for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_SSID_FMT, i + 1);
        size_t ssid_len = sizeof(local.wifi[i].ssid);
        if (nvs_get_str(h, key, local.wifi[i].ssid, &ssid_len) == ESP_OK &&
            local.wifi[i].ssid[0] != '\0') {
            any = true;
            snprintf(key, sizeof(key), NVS_KEY_PASS_FMT, i + 1);
            size_t pass_len = sizeof(local.wifi[i].pass);
            nvs_get_str(h, key, local.wifi[i].pass, &pass_len);
        }
    }
    nvs_close(h);
    memcpy(out, &local, sizeof(local));
    return any ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

bool netprov_load_config(struct netprov_config *cfg)
{
    if (!cfg) return false;
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->hostname, "SomnoTrace", sizeof(cfg->hostname));
    return nvs_writer_run(do_netprov_load, cfg) == ESP_OK;
}

/* Actual NVS write — runs on the internal-stack nvs_writer task. */
static esp_err_t do_netprov_save(void *arg)
{
    const struct netprov_config *cfg = (const struct netprov_config *)arg;
    struct netprov_config local = *cfg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, NVS_KEY_HOSTNAME, local.hostname);
    for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_SSID_FMT, i + 1);
        nvs_set_str(h, key, local.wifi[i].ssid);
        snprintf(key, sizeof(key), NVS_KEY_PASS_FMT, i + 1);
        nvs_set_str(h, key, local.wifi[i].pass);
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t netprov_save_config(const struct netprov_config *cfg)
{
    /* Delegate the flash write so callers on a PSRAM stack (httpd) are safe. */
    return nvs_writer_run(do_netprov_save, (void *)cfg);
}

/* ── mDNS custom name ──────────────────────────────────────────────── */
static char s_mdns_name[MDNS_NAME_MAX] = "somnotrace";

static esp_err_t do_save_mdns_name(void *arg)
{
    const char *name = (const char *)arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_MDNS_NAME, name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

typedef struct {
    char *out;
    size_t out_len;
    bool ok;
} mdns_read_args_t;

static esp_err_t do_load_mdns_name(void *arg)
{
    mdns_read_args_t *a = arg;
    char *out = a->out;
    size_t out_len = a->out_len;
    char value[MDNS_NAME_MAX] = {0};
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) { a->ok = false; return err; }
    size_t value_len = sizeof(value);
    err = nvs_get_str(h, NVS_KEY_MDNS_NAME, value, &value_len);
    nvs_close(h);
    a->ok = err == ESP_OK && value[0] != '\0';
    if (a->ok && out && out_len) strlcpy(out, value, out_len);
    return err;
}

void netprov_get_mdns_name(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    mdns_read_args_t args = { .out = out, .out_len = out_len, .ok = false };
    nvs_writer_run(do_load_mdns_name, &args);
    if (!args.ok || out[0] == '\0') strlcpy(out, "somnotrace", out_len);
    strlcpy(s_mdns_name, out, sizeof(s_mdns_name));
}

esp_err_t netprov_set_mdns_name(const char *name)
{
    if (!name || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_writer_run(do_save_mdns_name, (void *)name);
    if (err == ESP_OK) {
        strlcpy(s_mdns_name, name, sizeof(s_mdns_name));
    }
    return err;
}

const char *netprov_mdns_name_cached(void)
{
    return s_mdns_name;
}

/* ------------------------------------------------------------------ */
/*  WiFi events                                                       */
/* ------------------------------------------------------------------ */
/* Publish the "link is down" state.  Called on association loss: the IP we
 * were handed is no longer ours, so it must not be reported any more. */
static void link_mark_down(void)
{
    if (s_link_mutex) xSemaphoreTake(s_link_mutex, portMAX_DELAY);
    s_connected = false;
    s_link_ssid[0] = '\0';
    strlcpy(s_connected_ip, "0.0.0.0", sizeof(s_connected_ip));
    if (s_link_mutex) xSemaphoreGive(s_link_mutex);
}

/* Publish the "link is up" state, recording which AP we actually landed on
 * (which is not necessarily slot 1 — candidates are ranked by RSSI). */
static void link_mark_up(const char *ip)
{
    wifi_ap_record_t *ap = malloc(sizeof(wifi_ap_record_t));
    bool have_ap = ap && (esp_wifi_sta_get_ap_info(ap) == ESP_OK);

    if (s_link_mutex) xSemaphoreTake(s_link_mutex, portMAX_DELAY);
    s_connected = true;
    strlcpy(s_connected_ip, ip, sizeof(s_connected_ip));
    if (have_ap && ap) {
        strlcpy(s_link_ssid, (const char *)ap->ssid, sizeof(s_link_ssid));
    }
    if (s_link_mutex) xSemaphoreGive(s_link_mutex);
    if (ap) free(ap);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_connecting) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {
            /* Link genuinely lost while up.  Publish that immediately — the
             * old code left s_connected/s_connected_ip stale, so the LCD and
             * /api/status kept advertising a dead connection. */
            link_mark_down();
            bsp_display_set_wifi_connected(false);
            s_reconnect_tries = 0;
            ESP_LOGW(TAG, "Wi-Fi link lost, reconnecting...");
            esp_wifi_connect();
        } else if (s_connecting) {
            if (s_retry_num < MAX_STA_RETRY) {
                s_retry_num++;
                ESP_LOGI(TAG, "retry connect (%d/%d)", s_retry_num, MAX_STA_RETRY);
                esp_wifi_connect();
            } else if (s_wifi_events) {
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            }
        } else if (!s_portal_mode) {
            /* Reconnect attempt to the *current* SSID failed.  esp_wifi_connect()
             * only ever retries the single SSID in the driver config, so retrying
             * forever strands us on a network that has gone away while another
             * configured network sits available.  Escalate to a full rescan. */
            if (++s_reconnect_tries < RECONNECT_TRIES_BEFORE_RESCAN) {
                ESP_LOGI(TAG, "reconnect failed (%d/%d), retrying same SSID",
                         s_reconnect_tries, RECONNECT_TRIES_BEFORE_RESCAN);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "reconnect to '%s' failed %d times, "
                              "rescanning all configured networks",
                         s_link_ssid[0] ? s_link_ssid : "(unknown)",
                         s_reconnect_tries);
                s_rescan_requested = true;
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_got_ip, sizeof(s_got_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_reconnect_tries = 0;
        if (s_connecting && s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        } else if (!s_portal_mode) {
            /* Reconnect succeeded outside the boot-time connect path. */
            link_mark_up(s_got_ip);
            bsp_display_set_wifi_connected(true);
            ESP_LOGI(TAG, "Wi-Fi reconnected to '%s', ip=%s",
                     s_link_ssid[0] ? s_link_ssid : "?", s_got_ip);
        }
    }
}

void netprov_get_link(netprov_link_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    strlcpy(out->ip, "0.0.0.0", sizeof(out->ip));

    if (s_link_mutex) xSemaphoreTake(s_link_mutex, portMAX_DELAY);
    out->up = s_connected;
    if (s_connected) {
        strlcpy(out->ssid, s_link_ssid, sizeof(out->ssid));
        strlcpy(out->ip, s_connected_ip, sizeof(out->ip));
    }
    if (s_link_mutex) xSemaphoreGive(s_link_mutex);

    /* RSSI is only meaningful while associated, and the query can still
     * fail — report validity rather than a misleading default. */
    if (out->up) {
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
            out->rssi = rssi;
            out->rssi_valid = true;
        }
    }
}

bool netprov_is_link_up(void)
{
    return s_connected;
}

/* ------------------------------------------------------------------ */
/*  Link supervisor: autonomous failover between configured networks   */
/* ------------------------------------------------------------------ */
/* esp_wifi_connect() only ever retries the SSID currently programmed into
 * the driver, so a network that disappears permanently strands the device
 * even when another configured network is in range.  The event handler
 * raises s_rescan_requested after RECONNECT_TRIES_BEFORE_RESCAN failures;
 * this task performs the (blocking) scan-and-rank off the event loop. */
static void link_supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!s_rescan_requested || s_portal_mode || s_connected) continue;
        s_rescan_requested = false;

        if (!s_link_cfg_valid) {
            ESP_LOGW(TAG, "failover rescan requested but no cached config");
            continue;
        }

        ESP_LOGW(TAG, "failover: rescanning all configured networks");
        char ip[16] = "0.0.0.0";
        if (netprov_try_connect(&s_link_cfg, ip, 15000) == ESP_OK) {
            link_mark_up(ip);
            bsp_display_set_wifi_connected(true);
            strlcpy(s_connected_ip, ip, sizeof(s_connected_ip));
            ESP_LOGI(TAG, "failover: reconnected to '%s', ip=%s",
                     s_link_ssid[0] ? s_link_ssid : "?", ip);
        } else if (s_portal_mode) {
            /* Portal mode was activated while we were trying to connect.
             * Don't schedule another rescan — the AP is now up. */
            ESP_LOGI(TAG, "failover: portal mode active, suspending rescan");
        } else {
            /* Nothing reachable right now.  Back off and let the next
             * disconnect cycle raise another rescan. */
            ESP_LOGW(TAG, "failover: no configured network reachable, "
                          "retrying in 30s");
            vTaskDelay(pdMS_TO_TICKS(30000));
            s_rescan_requested = true;
        }
    }
}

esp_err_t netprov_init(void)
{
    s_link_mutex = xSemaphoreCreateMutex();
    if (!s_link_mutex) return ESP_ERR_NO_MEM;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();

    /* Set the DHCP hostname so routers show the friendly name instead of
     * the ESP-IDF default ("espressif").  Must be set before the interface
     * comes up for it to take effect on the first DHCP lease. */
    char dhname[MDNS_NAME_MAX];
    netprov_get_mdns_name(dhname, sizeof(dhname));
    esp_netif_set_hostname(s_netif_sta, dhname);
    esp_netif_set_hostname(s_netif_ap, dhname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  STA connect with scan + candidate selection                       */
/* ------------------------------------------------------------------ */
static esp_err_t try_single_ssid(const char *ssid, const char *pass,
                                 const wifi_ap_record_t *rec,
                                 char *ip_out, int timeout_ms)
{
    if (s_portal_mode) return ESP_FAIL;
    s_wifi_events = xEventGroupCreate();
    s_retry_num = 0;
    s_connecting = true;

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (rec) {
        memcpy(wc.sta.bssid, rec->bssid, sizeof(wc.sta.bssid));
        wc.sta.bssid_set = true;
        wc.sta.channel   = rec->primary;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    esp_err_t result;
    if (bits & WIFI_CONNECTED_BIT) {
        strlcpy(ip_out, s_got_ip, 16);
        /* Publish the link state, including which SSID we actually landed on. */
        if (s_link_mutex) xSemaphoreTake(s_link_mutex, portMAX_DELAY);
        strlcpy(s_connected_ip, s_got_ip, sizeof(s_connected_ip));
        strlcpy(s_link_ssid, ssid, sizeof(s_link_ssid));
        s_connected = true;
        if (s_link_mutex) xSemaphoreGive(s_link_mutex);
        s_reconnect_tries = 0;
        ESP_LOGI(TAG, "connected to '%s', ip=%s", ssid, ip_out);
        result = ESP_OK;
    } else {
        ESP_LOGW(TAG, "connect to '%s' failed", ssid);
        if (!s_portal_mode)
            esp_wifi_stop();
        result = ESP_FAIL;
    }

    s_connecting = false;
    vEventGroupDelete(s_wifi_events);
    s_wifi_events = NULL;
    return result;
}

esp_err_t netprov_try_connect(const struct netprov_config *cfg,
                               char *ip_out, int timeout_ms)
{
    if (s_portal_mode) return ESP_FAIL;
    link_mark_down();

    /* Cache the credentials so the link supervisor can rescan on its own
     * when the current network disappears. */
    if (cfg != &s_link_cfg) {
        memcpy(&s_link_cfg, cfg, sizeof(s_link_cfg));
        s_link_cfg_valid = true;
    }

    /* 1. Scan with retries */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Add a small delay for hardware initialization
    vTaskDelay(pdMS_TO_TICKS(100));

    uint16_t ap_count = 0;
    wifi_ap_record_t *records = NULL;
    int scan_retries = 3;
    int n_cands = 0;

    typedef struct { int slot; int rssi; wifi_ap_record_t rec; } cand_t;
    cand_t cands[NETPROV_MAX_SSID_SLOTS];

    for (int attempt = 1; attempt <= scan_retries; attempt++) {
        wifi_scan_config_t scan_cfg = { .show_hidden = false };
        esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
        if (scan_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi scan failed (err=0x%x), retrying scan (%d/%d)", scan_err, attempt, scan_retries);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 32) ap_count = 32;

        records = heap_caps_calloc(ap_count, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
        if (!records) records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (records && ap_count) {
            esp_wifi_scan_get_ap_records(&ap_count, records);
        }

        /* Build candidates: strongest matching SSID first */
        n_cands = 0;
        for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
            if (cfg->wifi[i].ssid[0] == '\0') continue;
            int best_rssi = -128;
            wifi_ap_record_t best_rec = {0};
            for (int j = 0; j < ap_count; j++) {
                if (records && strcmp((char *)records[j].ssid, cfg->wifi[i].ssid) == 0
                    && records[j].rssi > best_rssi) {
                    best_rssi = records[j].rssi;
                    best_rec = records[j];
                }
            }
            if (best_rssi > -128) {
                cands[n_cands].slot = i;
                cands[n_cands].rssi = best_rssi;
                cands[n_cands].rec = best_rec;
                n_cands++;
            }
        }

        if (records) {
            free(records);
            records = NULL;
        }

        if (n_cands > 0) {
            break; // Found candidate SSID(s)
        }

        if (attempt < scan_retries) {
            ESP_LOGI(TAG, "SSID candidates not found in scan, retrying scan in 1s (%d/%d)...", attempt, scan_retries);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (s_portal_mode) return ESP_FAIL;
    esp_wifi_stop();

    if (n_cands == 0) {
        ESP_LOGW(TAG, "no configured SSID visible after scan retries");
        return ESP_FAIL;
    }

    /* Sort by RSSI descending (bubble, small N) */
    for (int i = 0; i < n_cands - 1; i++) {
        for (int j = i + 1; j < n_cands; j++) {
            if (cands[j].rssi > cands[i].rssi) {
                cand_t t = cands[i]; cands[i] = cands[j]; cands[j] = t;
            }
        }
    }

    if (s_portal_mode) return ESP_FAIL;

    /* 3. Try each candidate: 3 attempts, 5 s between retries */
    for (int i = 0; i < n_cands; i++) {
        if (s_portal_mode) return ESP_FAIL;
        int slot = cands[i].slot;
        ESP_LOGI(TAG, "trying candidate %d: '%s' (%d dBm)",
                 i + 1, cfg->wifi[slot].ssid, cands[i].rssi);

        for (int attempt = 1; attempt <= MAX_STA_RETRY; attempt++) {
            esp_err_t err = try_single_ssid(cfg->wifi[slot].ssid,
                                            cfg->wifi[slot].pass,
                                            &cands[i].rec,
                                            ip_out, timeout_ms);
            if (err == ESP_OK) return ESP_OK;
            if (attempt < MAX_STA_RETRY) {
                ESP_LOGI(TAG, "waiting 5 s before retry %d/%d",
                         attempt + 1, MAX_STA_RETRY);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
    }

    ESP_LOGW(TAG, "all candidates exhausted");
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  HTTP helpers                                                      */
/* ------------------------------------------------------------------ */
static int url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dst_size; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = { src[si + 1], src[si + 2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return (int)di;
}

static bool form_get(const char *body, const char *key, char *out, size_t out_size)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    char raw[160];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(raw, out, out_size);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Web pages                                                         */
/* ------------------------------------------------------------------ */
extern const char _binary_portal_html_start[];
extern const char _binary_portal_html_end[];
#define PORTAL_HTML_START _binary_portal_html_start
#define PORTAL_HTML_LEN   ((size_t)(_binary_portal_html_end - _binary_portal_html_start))

extern const char _binary_zones_json_start[];
extern const char _binary_zones_json_end[];
#define ZONES_JSON_START _binary_zones_json_start
#define ZONES_JSON_LEN   ((size_t)(_binary_zones_json_end - _binary_zones_json_start))

extern const char _binary_uPlot_iife_min_js_start[];
extern const char _binary_uPlot_iife_min_js_end[];
#define UPLOT_JS_START _binary_uPlot_iife_min_js_start
#define UPLOT_JS_LEN   ((size_t)(_binary_uPlot_iife_min_js_end - _binary_uPlot_iife_min_js_start))

extern const char _binary_uPlot_min_css_start[];
extern const char _binary_uPlot_min_css_end[];
#define UPLOT_CSS_START _binary_uPlot_min_css_start
#define UPLOT_CSS_LEN   ((size_t)(_binary_uPlot_min_css_end - _binary_uPlot_min_css_start))

extern const char _binary_logo_full_svg_start[];
extern const char _binary_logo_full_svg_end[];
#define LOGO_FULL_SVG_START _binary_logo_full_svg_start

extern const char _binary_logo_small_svg_start[];
extern const char _binary_logo_small_svg_end[];
#define LOGO_SMALL_SVG_START _binary_logo_small_svg_start

/* portal.html and uPlot assets are embedded via CMakeLists.txt target_add_binary_data */


static esp_err_t redirect_to_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (s_portal_mode) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, PORTAL_HTML_START, PORTAL_HTML_LEN);
    return ESP_OK;
}

static esp_err_t tz_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    httpd_resp_send(req, ZONES_JSON_START, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t manifest_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    const char manifest[] =
        "{\n"
        "  \"short_name\": \"SomnoTrace\",\n"
        "  \"name\": \"SomnoTrace Web Portal\",\n"
        "  \"start_url\": \"/\",\n"
        "  \"background_color\": \"#0f172a\",\n"
        "  \"theme_color\": \"#0f172a\",\n"
        "  \"display\": \"standalone\",\n"
        "  \"orientation\": \"any\",\n"
        "  \"icons\": [\n"
        "    {\n"
        "      \"src\": \"/favicon.svg\",\n"
        "      \"sizes\": \"512x512\",\n"
        "      \"type\": \"image/svg+xml\"\n"
        "    }\n"
        "  ]\n"
        "}";
    httpd_resp_send(req, manifest, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t sw_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Connection", "close");
    const char sw[] =
        "const CACHE_NAME = 'somnotrace-v3';\n"
        "self.addEventListener('install', e => {\n"
        "  self.skipWaiting();\n"
        "  e.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(['/', '/manifest.json', '/uplot.js', '/uplot.css', '/logo.svg', '/favicon.svg'])));\n"
        "});\n"
        "self.addEventListener('activate', e => {\n"
        "  e.waitUntil(caches.keys().then(keys => Promise.all(\n"
        "    keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k))\n"
        "  )).then(() => self.clients.claim()));\n"
        "});\n"
        "self.addEventListener('fetch', e => {\n"
        "  if (e.request.url.includes('/api/') || e.request.url.includes('/scan') || e.request.url.includes('/save')) {\n"
        "    e.respondWith(fetch(e.request));\n"
        "  } else {\n"
        "    /* Network-first: always fetch fresh when the device is reachable,\n"
        "       fall back to cache only when offline. */\n"
        "    e.respondWith(\n"
        "      fetch(e.request).then(res => {\n"
        "        const copy = res.clone();\n"
        "        caches.open(CACHE_NAME).then(c => c.put(e.request, copy)).catch(() => {});\n"
        "        return res;\n"
        "      }).catch(() => caches.match(e.request))\n"
        "    );\n"
        "  }\n"
        "});\n";
    httpd_resp_send(req, sw, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t uplot_js_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    httpd_resp_set_hdr(req, "Connection", "close");
    /* Embedded as TEXT, which appends a NUL terminator. Use STRLEN so the
     * trailing NUL is not sent (a stray NUL breaks JS parsing). */
    httpd_resp_send(req, UPLOT_JS_START, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t uplot_css_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, UPLOT_CSS_START, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t logo_svg_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, LOGO_FULL_SVG_START, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, LOGO_SMALL_SVG_START, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Cached status data ────────────────────────────────────────────
 * These values rarely change but are expensive to query (SD I/O for
 * f_getfree, flash I/O for NVS).  Caching avoids contending with large
 * file downloads on the shared SD bus during 3-second status polls. */

#define STATUS_CACHE_SD_MS    600000  /* refresh SD free space every 10 min */
#define STATUS_CACHE_NVS_MS   120000  /* refresh NVS-backed settings every 2 min */

static struct {
    /* SD card */
    uint64_t sd_total;
    uint64_t sd_free;
    bool     sd_valid;
    TickType_t sd_tick;
    /* NVS config */
    struct netprov_config cfg;
    bool     cfg_valid;
    TickType_t cfg_tick;
    /* Timezone / NTP */
    char     tz_name[40];
    char     ntp_srv[64];
    TickType_t tz_tick;
} s_status_cache;

static void status_cache_refresh_sd(void)
{
    if (!sd_storage_is_ready()) {
        s_status_cache.sd_valid = false;
        return;
    }
    FATFS *fs = NULL;
    DWORD free_clst = 0;
    if (f_getfree("0:", &free_clst, &fs) == FR_OK && fs) {
        s_status_cache.sd_total = (uint64_t)fs->n_fatent * fs->csize * fs->ssize;
        s_status_cache.sd_free  = (uint64_t)free_clst * fs->csize * fs->ssize;
        s_status_cache.sd_valid = true;
    } else {
        s_status_cache.sd_valid = false;
    }
    s_status_cache.sd_tick = xTaskGetTickCount();
}

static void status_cache_refresh_nvs(void)
{
    s_status_cache.cfg_valid = netprov_load_config(&s_status_cache.cfg);
    time_sync_get_tz_name(s_status_cache.tz_name, sizeof(s_status_cache.tz_name));
    time_sync_get_ntp_server(s_status_cache.ntp_srv, sizeof(s_status_cache.ntp_srv));
    s_status_cache.tz_tick = xTaskGetTickCount();
}

cJSON *netprov_build_status_json(void)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t ms = portTICK_PERIOD_MS;

    /* Refresh SD free space at most once per STATUS_CACHE_SD_MS */
    if (!s_status_cache.sd_valid ||
        (uint32_t)((now - s_status_cache.sd_tick) * ms) >= STATUS_CACHE_SD_MS) {
        status_cache_refresh_sd();
    }

    /* Refresh NVS-backed settings at most once per STATUS_CACHE_NVS_MS */
    if (!s_status_cache.cfg_valid ||
        (uint32_t)((now - s_status_cache.tz_tick) * ms) >= STATUS_CACHE_NVS_MS) {
        status_cache_refresh_nvs();
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) return NULL;

    cJSON_AddStringToObject(resp, "mode", s_portal_mode ? "setup" : "connected");

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(resp, "fw_ver", app_desc ? app_desc->version : "unknown");

    if (!s_portal_mode) {
        /* Live link state: SSID, IP, RSSI — all derived from the event-driven
         * link state, not boot-time assumptions. */
        netprov_link_t link;
        netprov_get_link(&link);
        cJSON *wifi = cJSON_AddObjectToObject(resp, "wifi");
        cJSON_AddBoolToObject(wifi, "up", link.up);
        cJSON_AddStringToObject(wifi, "ssid", link.ssid);
        cJSON_AddStringToObject(wifi, "ip", link.ip);
        if (link.rssi_valid) {
            cJSON_AddNumberToObject(wifi, "rssi", link.rssi);
        } else {
            cJSON_AddNullToObject(wifi, "rssi");
        }
    }

    /* Configured SSIDs and password presence (from cache — no passwords sent) */
    if (s_status_cache.cfg_valid) {
        cJSON *ssids_arr = cJSON_AddArrayToObject(resp, "ssids");
        cJSON *has_pass_arr = cJSON_AddArrayToObject(resp, "has_pass");
        for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
            if (s_status_cache.cfg.wifi[i].ssid[0] != '\0') {
                cJSON_AddItemToArray(ssids_arr, cJSON_CreateString(s_status_cache.cfg.wifi[i].ssid));
                cJSON_AddItemToArray(has_pass_arr, cJSON_CreateBool(s_status_cache.cfg.wifi[i].pass[0] != '\0'));
            }
        }
    }

    /* Wi-Fi radio (channel is always available in STA mode) */
    uint8_t primary_chan = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_get_channel(&primary_chan, &second_chan);
    cJSON_AddNumberToObject(resp, "channel", primary_chan);

    /* Time / timezone / NTP (from cache) */
    cJSON_AddStringToObject(resp, "tz_name", s_status_cache.tz_name);
    cJSON_AddStringToObject(resp, "ntp_server", s_status_cache.ntp_srv);
    cJSON_AddStringToObject(resp, "mdns_name", netprov_mdns_name_cached());
    cJSON_AddBoolToObject(resp, "ntp_synced", time_sync_is_synced());
    const char *src_str = "none";
    switch (time_source_get()) {
        case TIME_SRC_NTP:        src_str = "ntp"; break;
        case TIME_SRC_AS11_DRIFT: src_str = "as11_drift"; break;
        default:                  src_str = "none"; break;
    }
    cJSON_AddStringToObject(resp, "time_source", src_str);
    time_t now_t = time(NULL);
    if (now_t > 1700000000) {
        struct tm tm_info;
        localtime_r(&now_t, &tm_info);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", &tm_info);
        cJSON_AddStringToObject(resp, "time", time_str);
    } else {
        cJSON_AddNullToObject(resp, "time");
    }

    /* Battery (from the background monitor — never blocks on the ADC) */
    {
        bsp_battery_t batt;
        bsp_power_battery_get(&batt);
        cJSON *batt_obj = cJSON_AddObjectToObject(resp, "battery");
        if (batt.valid) {
            cJSON_AddNumberToObject(batt_obj, "percent", batt.percent);
            cJSON_AddNumberToObject(batt_obj, "millivolts", batt.millivolts);
        } else {
            cJSON_AddNullToObject(batt_obj, "percent");
            cJSON_AddNullToObject(batt_obj, "millivolts");
        }
        cJSON_AddBoolToObject(batt_obj, "charging", batt.charging);
        cJSON_AddBoolToObject(batt_obj, "valid", batt.valid);
    }

    /* Uptime */
    uint32_t uptime_s = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    cJSON_AddNumberToObject(resp, "uptime", uptime_s);

    /* BLE status (only in connected mode) */
    if (!s_portal_mode) {
        cJSON *ble = cJSON_AddObjectToObject(resp, "ble");
        cJSON_AddStringToObject(ble, "state", as11_ble_get_status());
        cJSON_AddStringToObject(ble, "error", as11_ble_get_error());
        cJSON_AddBoolToObject(ble, "paired", as11_ble_is_paired());
        if (as11_ble_is_paired()) {
            cJSON *info = as11_ble_get_paired_info();
            if (info) {
                cJSON_AddItemToObject(ble, "device", info);
            }
        }
    }

    /* Oximeter (O2 Ring) status (only in connected mode) */
    if (!s_portal_mode) {
        cJSON *ox = cJSON_AddObjectToObject(resp, "oximeter");
        cJSON_AddStringToObject(ox, "state", oximeter_get_status());
        cJSON_AddStringToObject(ox, "error", oximeter_get_error());
        cJSON_AddBoolToObject(ox, "paired", oximeter_is_paired());
        cJSON_AddStringToObject(ox, "probe_mode",
                oximeter_get_probe_mode() == OX_PROBE_PERSISTENT
                    ? "persistent" : "legacy");
        if (oximeter_is_paired()) {
            cJSON *oinfo = oximeter_get_paired_info();
            if (oinfo) {
                cJSON_AddItemToObject(ox, "device", oinfo);
            }
        }
    }

    /* Upload summary (only in connected mode) */
    if (!s_portal_mode) {
        int pending = 0;
        const char *worst = "idle";
        uploader_get_summary(&pending, &worst);
        cJSON *up = cJSON_AddObjectToObject(resp, "uploads");
        cJSON_AddNumberToObject(up, "pending", pending);
        cJSON_AddStringToObject(up, "state", worst);
    }

    /* Heap stats */
    cJSON_AddNumberToObject(resp, "ih_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Therapy alert state */
    cJSON *alert = cJSON_AddObjectToObject(resp, "alert");
    cJSON_AddStringToObject(alert, "state", therapy_alert_state_str(therapy_alert_get_state()));

    {
        char *pending = NULL;
        if (session_writer_pending_export_json(&pending) == ESP_OK && pending) {
            cJSON *parsed = cJSON_Parse(pending);
            if (parsed) {
                cJSON_AddItemToObject(resp, "pending_export", parsed);
            }
            free(pending);
        }
    }
    cJSON_AddNumberToObject(resp, "ih_min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(resp, "ih_lfb", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(resp, "ps_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(resp, "ps_min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(resp, "ps_lfb", (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(resp, "tasks", (double)uxTaskGetNumberOfTasks());

    /* SD card free space (from cache — no SD I/O on every poll) */
    if (s_status_cache.sd_valid) {
        cJSON_AddNumberToObject(resp, "sd_total", (double)s_status_cache.sd_total);
        cJSON_AddNumberToObject(resp, "sd_free", (double)s_status_cache.sd_free);
    }

    return resp;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *resp = netprov_build_status_json();
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status failed");
        return ESP_FAIL;
    }
    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status serialization failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Async WiFi scan (non-blocking to avoid socket exhaustion)         */
/* ------------------------------------------------------------------ */
static volatile bool s_scan_running = false;
static volatile bool s_scan_done = false;
static char *s_scan_json = NULL;   /* cached JSON result */
static SemaphoreHandle_t s_scan_mutex = NULL;

static void wifi_scan_task(void *arg)
{
    ESP_LOGI(TAG, "wifi scan starting");
    if (s_portal_mode) {
        /* SoftAP: BLE is disconnected, so custom active scan params are safe.
         * ~20ms per channel × 13 channels ≈ 300ms total. */
        wifi_scan_config_t fast_cfg = {
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = 0,
            .scan_time.active.max = 20,
        };
        esp_wifi_scan_start(&fast_cfg, true);
    } else {
        /* STA: BLE may be active — pass NULL to let the driver use
         * BT-coexistence-safe defaults. */
        esp_wifi_scan_start(NULL, true);
    }
    ESP_LOGI(TAG, "wifi scan complete");

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *records = heap_caps_calloc(ap_count, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    if (!records) records = calloc(ap_count, sizeof(wifi_ap_record_t));
    cJSON *arr = cJSON_CreateArray();
    if (records && ap_count) {
        esp_wifi_scan_get_ap_records(&ap_count, records);
        for (int i = 0; i < ap_count; i++) {
            if (records[i].ssid[0] == '\0') continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", (char *)records[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", records[i].rssi);
            cJSON_AddBoolToObject(o, "lock", records[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, o);
        }
    }
    free(records);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (s_scan_mutex) xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    if (s_scan_json) cJSON_free(s_scan_json);
    s_scan_json = json;
    s_scan_done = true;
    s_scan_running = false;
    if (s_scan_mutex) xSemaphoreGive(s_scan_mutex);

    vTaskDelete(NULL);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!s_scan_mutex) s_scan_mutex = xSemaphoreCreateMutex();

    /* If a scan is running, tell the client to poll */
    if (s_scan_running) {
        httpd_resp_send(req, "{\"scanning\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* If we have cached results, return them */
    if (s_scan_done && s_scan_json) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
        char *cached = s_scan_json;
        s_scan_json = NULL;
        s_scan_done = false;
        xSemaphoreGive(s_scan_mutex);
        httpd_resp_send(req, cached, HTTPD_RESP_USE_STRLEN);
        cJSON_free(cached);
        return ESP_OK;
    }

    /* Start a new scan in a background task */
    s_scan_running = true;
    s_scan_done = false;
    BaseType_t ret = (psram_task_create(wifi_scan_task, "wifi_scan", 4096, NULL, 3, tskNO_AFFINITY, NULL, NULL) != NULL) ? pdPASS : pdFAIL;
    if (ret != pdPASS) {
        s_scan_running = false;
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, "{\"scanning\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  BLE (AirSense 11) pairing endpoints                               */
/* ------------------------------------------------------------------ */
static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    int total = req->content_len < (int)cap - 1 ? req->content_len : (int)cap - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) return ESP_FAIL;
        received += r;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t ble_scan_handler(httpd_req_t *req)
{
    if (as11_ble_scan(6) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ble not ready");
        return ESP_FAIL;
    }
    cJSON *arr = as11_ble_get_scan_results();
    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t ble_pair_handler(httpd_req_t *req)
{
    char body[128];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    cJSON *addr = j ? cJSON_GetObjectItem(j, "addr") : NULL;
    if (!cJSON_IsString(addr)) {
        if (j) cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr");
        return ESP_FAIL;
    }
    esp_err_t e = as11_ble_start_pair(addr->valuestring);
    cJSON_Delete(j);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pair start failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t ble_confirm_handler(httpd_req_t *req)
{
    char body[96];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    cJSON *pk = j ? cJSON_GetObjectItem(j, "passkey") : NULL;
    if (!cJSON_IsString(pk)) {
        if (j) cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing passkey");
        return ESP_FAIL;
    }
    esp_err_t e = as11_ble_confirm_pair(pk->valuestring);
    cJSON_Delete(j);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "confirm failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t ble_forget_handler(httpd_req_t *req)
{
    as11_ble_forget();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── Oximeter (O2 Ring) endpoints ──────────────────────────────────── */
static esp_err_t ox_scan_handler(httpd_req_t *req)
{
    if (oximeter_scan(6) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oximeter not ready");
        return ESP_FAIL;
    }
    cJSON *arr = oximeter_get_scan_results();
    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t ox_pair_handler(httpd_req_t *req)
{
    char body[128];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    cJSON *addr = j ? cJSON_GetObjectItem(j, "addr") : NULL;
    cJSON *type = j ? cJSON_GetObjectItem(j, "type") : NULL;
    if (!cJSON_IsString(addr)) {
        if (j) cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr");
        return ESP_FAIL;
    }
    ox_driver_t driver = OX_DRIVER_AUTO;
    if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "legacy") == 0)
            driver = OX_DRIVER_LEGACY;
        else if (strcmp(type->valuestring, "oxyii") == 0)
            driver = OX_DRIVER_OXYII;
        else if (strcmp(type->valuestring, "auto") == 0)
            driver = OX_DRIVER_AUTO;
    }
    esp_err_t e = oximeter_pair(addr->valuestring, driver);
    cJSON_Delete(j);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pair start failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t ox_forget_handler(httpd_req_t *req)
{
    oximeter_forget();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t ox_probe_mode_handler(httpd_req_t *req)
{
    char body[64];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    cJSON *mode = j ? cJSON_GetObjectItem(j, "mode") : NULL;
    if (!cJSON_IsString(mode)) {
        if (j) cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'mode'");
        return ESP_FAIL;
    }
    ox_probe_mode_t pm;
    if (strcmp(mode->valuestring, "persistent") == 0)
        pm = OX_PROBE_PERSISTENT;
    else if (strcmp(mode->valuestring, "legacy") == 0)
        pm = OX_PROBE_LEGACY;
    else {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode");
        return ESP_FAIL;
    }
    cJSON_Delete(j);
    esp_err_t e = oximeter_set_probe_mode(pm);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t ble_passthrough_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body (1..2048 bytes)");
        return ESP_FAIL;
    }

    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
    if (!body) body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, body, total);
    if (received < 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char *out_json = NULL;
    esp_err_t err = as11_ble_passthrough_rpc(body, &out_json, 10000);
    free(body);

    if (err != ESP_OK || !out_json) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        const char *errmsg = (err == ESP_ERR_INVALID_STATE) ? "BLE session not active/paired" :
                             (err == ESP_ERR_TIMEOUT) ? "BLE response timeout" : "BLE RPC failed";
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "{\"ok\":false,\"error\":\"%s\"}", errmsg);
        httpd_resp_sendstr(req, errbuf);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, out_json, HTTPD_RESP_USE_STRLEN);
    free(out_json);
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "rebooting to apply credentials");
    esp_restart();
}

static esp_err_t heap_stats_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();

    cJSON *internal = cJSON_AddObjectToObject(root, "internal");
    cJSON_AddNumberToObject(internal, "free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(internal, "min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(internal, "lfb", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    cJSON *psram = cJSON_AddObjectToObject(root, "psram");
    cJSON_AddNumberToObject(psram, "free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(psram, "min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(psram, "lfb", (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    cJSON *dma = cJSON_AddObjectToObject(root, "dma");
    cJSON_AddNumberToObject(dma, "free", (double)heap_caps_get_free_size(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(dma, "min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));

    cJSON_AddNumberToObject(root, "tasks", (double)uxTaskGetNumberOfTasks());

    cJSON *tasks = cJSON_AddArrayToObject(root, "task_list");
    TaskStatus_t *task_stats = heap_caps_malloc(uxTaskGetNumberOfTasks() * sizeof(TaskStatus_t),
                                                MALLOC_CAP_SPIRAM);
    if (task_stats) {
        UBaseType_t n = uxTaskGetSystemState(task_stats, uxTaskGetNumberOfTasks(), NULL);
        for (UBaseType_t i = 0; i < n; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", task_stats[i].pcTaskName);
            cJSON_AddNumberToObject(t, "stack_hwm", (double)(task_stats[i].usStackHighWaterMark * sizeof(StackType_t)));
            cJSON_AddNumberToObject(t, "prio", (double)task_stats[i].uxCurrentPriority);
            cJSON_AddNumberToObject(t, "state", (double)task_stats[i].eCurrentState);
            cJSON_AddItemToArray(tasks, t);
        }
        free(task_stats);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    psram_task_create(reboot_task, "reboot", 4096, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[768];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    /* Check if this is a timezone-only update */
    char tz_only[4] = { 0 };
    form_get(body, "tz_only", tz_only, sizeof(tz_only));
    bool is_tz_only = (tz_only[0] == '1');

    struct netprov_config old_cfg;
    netprov_load_config(&old_cfg);

    struct netprov_config cfg;
    memcpy(&cfg, &old_cfg, sizeof(cfg));

    int saved_count = 0;

    if (!is_tz_only) {
        memset(cfg.wifi, 0, sizeof(cfg.wifi));

        for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
            char ssid_key[16];
            char pass_key[16];
            snprintf(ssid_key, sizeof(ssid_key), "ssid%d", i + 1);
            snprintf(pass_key, sizeof(pass_key), "pass%d", i + 1);

            char ssid[NETPROV_SSID_MAXLEN + 1] = { 0 };
            char pass[NETPROV_PASS_MAXLEN + 1] = { 0 };

            if (form_get(body, ssid_key, ssid, sizeof(ssid)) && ssid[0] != '\0') {
                form_get(body, pass_key, pass, sizeof(pass));
                strlcpy(cfg.wifi[saved_count].ssid, ssid, sizeof(cfg.wifi[saved_count].ssid));
                if (strcmp(pass, "\xe2\x96\x88UNCHANGED\xe2\x96\x88") == 0) {
                    for (int j = 0; j < NETPROV_MAX_SSID_SLOTS; j++) {
                        if (strcmp(old_cfg.wifi[j].ssid, ssid) == 0) {
                            strlcpy(cfg.wifi[saved_count].pass, old_cfg.wifi[j].pass, sizeof(cfg.wifi[saved_count].pass));
                            break;
                        }
                    }
                } else {
                    strlcpy(cfg.wifi[saved_count].pass, pass, sizeof(cfg.wifi[saved_count].pass));
                }
                saved_count++;
            }
        }

        if (saved_count == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
            return ESP_FAIL;
        }

        if (netprov_save_config(&cfg) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
            return ESP_FAIL;
        }
        /* Invalidate cached config so /api/status returns the new SSID list */
        s_status_cache.cfg_valid = false;
        ESP_LOGI(TAG, "saved %d credentials", saved_count);
    }

    /* Save timezone if present */
    char tz_str_val[64] = { 0 };
    char tz_name_val[40] = { 0 };
    if (form_get(body, "tz_str", tz_str_val, sizeof(tz_str_val)) && tz_str_val[0] != '\0') {
        form_get(body, "tz_name", tz_name_val, sizeof(tz_name_val));
        time_sync_set_timezone(tz_str_val, tz_name_val);
        ESP_LOGI(TAG, "saved timezone %s (%s)", tz_name_val, tz_str_val);
    }

    /* Save custom NTP server if present (empty string = auto mode) */
    char ntp_srv_val[64] = { 0 };
    if (form_get(body, "ntp_srv", ntp_srv_val, sizeof(ntp_srv_val))) {
        time_sync_set_ntp_server(ntp_srv_val);
        ESP_LOGI(TAG, "saved NTP server: %s",
                 ntp_srv_val[0] ? ntp_srv_val : "(auto)");
    }

    /* Save mDNS name if present (requires reboot to take effect) */
    char mdns_val[MDNS_NAME_MAX] = { 0 };
    if (form_get(body, "mdns_name", mdns_val, sizeof(mdns_val))) {
        if (mdns_val[0] != '\0') {
            netprov_set_mdns_name(mdns_val);
            ESP_LOGI(TAG, "saved mDNS name: %s", mdns_val);
        }
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style=\"font-family:sans-serif\">Saved. Rebooting to connect...</body></html>");

    psram_task_create(reboot_task, "reboot", 4096, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  EZShare-compatible file server (/dir, /download)                  */
/* ------------------------------------------------------------------ */

#include <dirent.h>
#include <sys/stat.h>

#define SD_ROOT "/somnotrace"

/* Check if path contains ".." (traversal protection) */
static bool path_is_safe(const char *path)
{
    if (!path) return false;
    if (strstr(path, "..")) return false;
    return true;
}

/* URL-decode a query parameter value in-place */
static int fs_url_decode(char *dst, const char *src, int max_len)
{
    int i = 0;
    while (*src && i < max_len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = src[1] >= 'A' ? (src[1] | 0x20) - 'a' + 10 : src[1] - '0';
            int lo = src[2] >= 'A' ? (src[2] | 0x20) - 'a' + 10 : src[2] - '0';
            dst[i++] = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
    return i;
}

/* Extract a query parameter from the URI query string */
static bool get_query_param(httpd_req_t *req, const char *key, char *out, int out_len)
{
    char buf[512];
    int len = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (len <= 0) return false;

    char key_eq[32];
    snprintf(key_eq, sizeof(key_eq), "%s=", key);

    char *p = strstr(buf, key_eq);
    if (!p) return false;
    p += strlen(key_eq);

    char *end = strchr(p, '&');
    int val_len = end ? (int)(end - p) : (int)strlen(p);
    if (val_len <= 0) return false;

    char raw[256];
    if (val_len >= (int)sizeof(raw)) val_len = sizeof(raw) - 1;
    memcpy(raw, p, val_len);
    raw[val_len] = '\0';

    fs_url_decode(out, raw, out_len);
    return true;
}

static esp_err_t dir_get_handler(httpd_req_t *req)
{
    char dir_path[256];
    if (!get_query_param(req, "dir", dir_path, sizeof(dir_path)) || dir_path[0] == '\0') {
        strlcpy(dir_path, "/", sizeof(dir_path));
    }

    if (!path_is_safe(dir_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    DIR *d = opendir(dir_path);
    if (!d) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "dir not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    /* Build HTML <pre> listing — heap-allocated to avoid stack overflow */
    char *html = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!html) html = malloc(4096);
    if (!html) {
        closedir(d);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int pos = 0;
    pos += snprintf(html + pos, 4096 - pos, "<html><body><pre>\n");

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && pos < 4096 - 128) {
        if (ent->d_name[0] == '.') continue;

        char full_path[530];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        char timestr[32];
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);

        if (S_ISDIR(st.st_mode)) {
            pos += snprintf(html + pos, 4096 - pos,
                "%s    &lt;DIR&gt;    <a href=\"/dir?dir=%s/%s\">%s</a>\n",
                timestr, dir_path, ent->d_name, ent->d_name);
        } else {
            pos += snprintf(html + pos, 4096 - pos,
                "%s    %8ld    <a href=\"/download?path=%s/%s\">%s</a>\n",
                timestr, (long)st.st_size, dir_path, ent->d_name, ent->d_name);
        }
    }
    closedir(d);

    pos += snprintf(html + pos, 4096 - pos, "</pre></body></html>\n");
    httpd_resp_send(req, html, pos);
    free(html);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    char file_path[256];
    if (!get_query_param(req, "path", file_path, sizeof(file_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_FAIL;
    }

    if (!path_is_safe(file_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(file_path, &st) == 0) {
        char len_str[16];
        snprintf(len_str, sizeof(len_str), "%ld", (long)st.st_size);
        httpd_resp_set_hdr(req, "Content-Length", len_str);
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Connection", "close");

    /* Heap-allocate to avoid stack overflow */
    char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(2048);
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t n;
    while ((n = fread(buf, 1, 2048, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            free(buf);
            fclose(f);
            return ESP_FAIL;
        }
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── Upload config and status endpoints ────────────────────────────── */

/* Compact per-backend upload progress for the Uploads card. */
static esp_err_t upload_progress_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    if (uploader_get_progress_json(&json) != ESP_OK || !json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "progress failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* Debug: the parsed tracking state for one day, e.g.
 * /api/uploads/state?day=20260807 */
static esp_err_t upload_state_get_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char day[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "day", day, sizeof(day)) != ESP_OK ||
        strlen(day) != 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "day=YYYYMMDD required");
        return ESP_FAIL;
    }

    char *json = NULL;
    if (uploader_get_day_state_json(day, &json) != ESP_OK || !json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "state failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t upload_config_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    if (uploader_get_config_json(&json) != ESP_OK || !json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t upload_config_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }
    char *body = heap_caps_malloc((size_t)total + 1, MALLOC_CAP_SPIRAM);
    if (!body) body = malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, body, total);
    if (received < 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    if (uploader_save_config_json(body) != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid config");
        return ESP_FAIL;
    }
    free(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Upload "Test connection" ─────────────────────────────────────────
 * Probes one backend with the settings saved in NVS (not the form contents)
 * and answers {"ok":bool,"message":"..."} — 409 while an upload is running.
 * Blocks this worker for up to the backend's probe timeout (about 10 s). */
static esp_err_t upload_test_send(httpd_req_t *req, const char *backend_id)
{
    bool ok = false;
    char msg[192];
    esp_err_t err = uploader_test_connection(backend_id, &ok, msg, sizeof(msg));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown backend");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK && ok);
    cJSON_AddStringToObject(root, "message", msg);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (err == ESP_ERR_INVALID_STATE) httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t upload_test_smb_handler(httpd_req_t *req)
{
    return upload_test_send(req, "smb");
}

static esp_err_t upload_test_sleephq_handler(httpd_req_t *req)
{
    return upload_test_send(req, "sleephq");
}

/* ── Device settings endpoints ─────────────────────────────────────── */

/* ── Therapy alert config endpoints ─────────────────────────────────── */

static esp_err_t alert_config_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    if (therapy_alert_get_config_json(&json) != ESP_OK || !json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t alert_config_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }
    char *body = heap_caps_malloc((size_t)total + 1, MALLOC_CAP_SPIRAM);
    if (!body) body = malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, body, total);
    if (received < 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    if (therapy_alert_save_config_json(body) != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid config");
        return ESP_FAIL;
    }
    free(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t alert_test_push_handler(httpd_req_t *req)
{
    char *body = NULL;
    int total = req->content_len;
    if (total > 0 && total <= 2048) {
        body = heap_caps_malloc((size_t)total + 1, MALLOC_CAP_SPIRAM);
        if (!body) body = malloc((size_t)total + 1);
        if (body) {
            int received = httpd_req_recv(req, body, total);
            if (received < 0) {
                free(body);
                body = NULL;
            } else {
                body[received] = '\0';
            }
        }
    }

    esp_err_t err = therapy_alert_send_test_push(body);
    free(body);

    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "{\"ok\":false,\"error\":\"push failed\"}");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Device settings endpoints (cont'd) ────────────────────────────── */

static esp_err_t device_settings_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    if (device_settings_get_json(&json) != ESP_OK || !json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t settings_all_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *up_json = NULL;
    if (uploader_get_config_json(&up_json) == ESP_OK && up_json) {
        cJSON *parsed = cJSON_Parse(up_json);
        if (parsed) cJSON_AddItemToObject(root, "uploads", parsed);
        free(up_json);
    }

    char *dev_json = NULL;
    if (device_settings_get_json(&dev_json) == ESP_OK && dev_json) {
        cJSON *parsed = cJSON_Parse(dev_json);
        if (parsed) cJSON_AddItemToObject(root, "device", parsed);
        free(dev_json);
    }

    char *alert_json = NULL;
    if (therapy_alert_get_config_json(&alert_json) == ESP_OK && alert_json) {
        cJSON *parsed = cJSON_Parse(alert_json);
        if (parsed) cJSON_AddItemToObject(root, "alert", parsed);
        free(alert_json);
    }

    cJSON *st = netprov_build_status_json();
    if (st) cJSON_AddItemToObject(root, "status", st);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    return ESP_OK;
}

static esp_err_t device_settings_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }
    char *body = heap_caps_malloc((size_t)total + 1, MALLOC_CAP_SPIRAM);
    if (!body) body = malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, body, total);
    if (received < 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    if (device_settings_save_json(body) != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid settings");
        return ESP_FAIL;
    }
    free(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t audio_test_beep_handler(httpd_req_t *req)
{
    esp_err_t ret = bsp_audio_test_beep();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "audio unavailable");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Actions: Reset State, Delete EDFs, Reset All, Recreate EDFs ───── */

/* 409 Conflict for actions refused by storage arbitration.  ESP-IDF's
 * httpd_err_code_t has no 409, so the status is set explicitly and the
 * reason returned as JSON for the Web UI. */
static esp_err_t send_busy(httpd_req_t *req, const char *reason)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", reason);
    httpd_resp_sendstr(req, body);
    return ESP_FAIL;
}

/* Storage-lease adapters injected into the uploader component (which cannot
 * depend on the app's sd_storage). */
static bool uploader_lease_acquire(uint32_t timeout_ms)
{
    return sd_storage_lease_acquire(SD_LEASE_UPLOAD, timeout_ms);
}

static void uploader_lease_release(void)
{
    sd_storage_lease_release(SD_LEASE_UPLOAD);
}

/* Recursively delete a directory and all its contents. */
static void recursive_delete(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        remove(path);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (ent->d_type == DT_DIR) {
            recursive_delete(child);
        } else {
            remove(child);
        }
    }
    closedir(d);
    rmdir(path);
}

/* Background task for recreate EDFs (long-running, needs large stack). */
typedef struct {
    char session_dir[256];
    char session_id[32];
    int64_t start_epoch_ms;
    int64_t end_epoch_ms;
    int64_t clock_drift_ms;
} recreate_session_t;

static void recreate_edfs_task(void *arg)
{
    ESP_LOGI(TAG, "recreate_edfs_task: starting");

    int max_days = uploader_max_days();
    ESP_LOGI(TAG, "recreate_edfs_task: upload window = %d days (newest first)", max_days);

    /* 1. Per-day deletion happens inline during generation (below) so that
     * days outside the upload window are left untouched.  The shared root
     * artifacts (STR.edf, Identification.json, SETTINGS/) are overwritten
     * by edf_gen_generate via atomic rename — no pre-deletion needed. */

    /* 2. First pass: count total _session.json files so we can allocate the
     * exact-sized array (no arbitrary cap). */
    int total_sessions = 0;
    DIR *d = opendir(SD_STREAMS_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_type != DT_DIR) continue;
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char day_dir[300];
            snprintf(day_dir, sizeof(day_dir), "%s/%s", SD_STREAMS_DIR, ent->d_name);
            DIR *dd = opendir(day_dir);
            if (!dd) continue;
            struct dirent *fent;
            while ((fent = readdir(dd)) != NULL) {
                if (fent->d_type != DT_REG) continue;
                const char *suffix = "_session.json";
                int slen = strlen(suffix);
                int flen = strlen(fent->d_name);
                if (flen <= slen || strcmp(fent->d_name + flen - slen, suffix) != 0)
                    continue;
                total_sessions++;
            }
            closedir(dd);
        }
        closedir(d);
    }

    if (total_sessions == 0) {
        ESP_LOGI(TAG, "recreate_edfs_task: no sessions found");
        vTaskDelete(NULL);
        return;
    }

    /* 3. Allocate sessions array in PSRAM (each entry is ~312 bytes;
     * with many sessions this can exceed internal RAM free space). */
    recreate_session_t *sessions = heap_caps_calloc(total_sessions,
            sizeof(recreate_session_t), MALLOC_CAP_SPIRAM);
    if (!sessions) {
        ESP_LOGE(TAG, "recreate_edfs_task: failed to allocate %d sessions", total_sessions);
        vTaskDelete(NULL);
        return;
    }
    int n_sessions = 0;

    /* 4. Second pass: collect all sessions. */
    d = opendir(SD_STREAMS_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_type != DT_DIR) continue;
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char day_dir[300];
            snprintf(day_dir, sizeof(day_dir), "%s/%s", SD_STREAMS_DIR, ent->d_name);
            DIR *dd = opendir(day_dir);
            if (!dd) continue;
            struct dirent *fent;
            while ((fent = readdir(dd)) != NULL) {
                if (fent->d_type != DT_REG) continue;
                const char *suffix = "_session.json";
                int slen = strlen(suffix);
                int flen = strlen(fent->d_name);
                if (flen <= slen || strcmp(fent->d_name + flen - slen, suffix) != 0)
                    continue;
                char session_id[32];
                int prefix_len = flen - slen;
                if (prefix_len <= 0 || prefix_len >= (int)sizeof(session_id)) continue;
                memcpy(session_id, fent->d_name, prefix_len);
                session_id[prefix_len] = '\0';
                char json_path[600];
                snprintf(json_path, sizeof(json_path), "%s/%s", day_dir, fent->d_name);
                FILE *f = fopen(json_path, "r");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (fsize <= 0 || fsize > 4096) { fclose(f); continue; }
                char *buf = heap_caps_malloc((size_t)fsize + 1, MALLOC_CAP_SPIRAM);
                if (!buf) buf = malloc((size_t)fsize + 1);
                if (!buf) { fclose(f); continue; }
                fread(buf, 1, fsize, f);
                buf[fsize] = '\0';
                fclose(f);
                cJSON *j = cJSON_Parse(buf);
                free(buf);
                if (!j) continue;
                cJSON *j_start = cJSON_GetObjectItem(j, "start_epoch_ms");
                cJSON *j_end = cJSON_GetObjectItem(j, "end_epoch_ms");
                cJSON *j_drift = cJSON_GetObjectItem(j, "clock_drift_ms");
                if (j_start && cJSON_IsNumber(j_start)) {
                    int64_t epoch = (int64_t)j_start->valuedouble;
                    if (epoch < 946684800000LL) {
                        ESP_LOGW(TAG, "recreate_edfs: skipping %s (invalid start_epoch_ms=%lld)",
                                 session_id, (long long)epoch);
                        cJSON_Delete(j);
                        continue;
                    }
                    recreate_session_t *s = &sessions[n_sessions++];
                    strlcpy(s->session_dir, day_dir, sizeof(s->session_dir));
                    strlcpy(s->session_id, session_id, sizeof(s->session_id));
                    s->start_epoch_ms = (int64_t)j_start->valuedouble;
                    s->end_epoch_ms = (j_end && cJSON_IsNumber(j_end)) ? (int64_t)j_end->valuedouble : 0;
                    s->clock_drift_ms = (j_drift && cJSON_IsNumber(j_drift)) ? (int64_t)j_drift->valuedouble : 0;
                }
                cJSON_Delete(j);
            }
            closedir(dd);
        }
        closedir(d);
    }

    ESP_LOGI(TAG, "recreate_edfs_task: found %d sessions", n_sessions);

    /* 5. Sort sessions by start_epoch_ms descending (newest first). */
    for (int i = 1; i < n_sessions; i++) {
        recreate_session_t tmp = sessions[i];
        int j = i - 1;
        while (j >= 0 && sessions[j].start_epoch_ms < tmp.start_epoch_ms) {
            sessions[j + 1] = sessions[j];
            j--;
        }
        sessions[j + 1] = tmp;
    }

    /* 6. Walk newest-first, generating EDFs only for sessions within the
     * first max_days unique day folders.  Sessions are sorted descending so
     * once max_days distinct days have been seen, all remaining sessions are
     * older and outside the upload window. */
    char (*queued_days)[16] = heap_caps_calloc(max_days + 1,
            sizeof(*queued_days), MALLOC_CAP_SPIRAM);
    if (!queued_days) {
        ESP_LOGE(TAG, "recreate_edfs_task: failed to allocate queued_days");
        free(sessions);
        vTaskDelete(NULL);
        return;
    }
    int n_queued_days = 0;
    int n_processed = 0;

    for (int i = 0; i < n_sessions; i++) {
        char day_folder[32];
        as11_time_noon_day(sessions[i].start_epoch_ms - sessions[i].clock_drift_ms,
                           day_folder, sizeof(day_folder));

        bool dup = false;
        for (int j = 0; j < n_queued_days; j++) {
            if (strcmp(queued_days[j], day_folder) == 0) { dup = true; break; }
        }
        if (!dup) {
            if (n_queued_days >= max_days) {
                /* Exceeded the upload window — all remaining sessions are
                 * older; stop processing. */
                break;
            }
            /* Delete just this day's existing EDF folder before recreating
             * it, so stale files from a previous export don't survive. */
            char old_day_dir[300];
            snprintf(old_day_dir, sizeof(old_day_dir), "%s/%s",
                     SD_SDCARD_DATALOG, day_folder);
            recursive_delete(old_day_dir);
            strlcpy(queued_days[n_queued_days++], day_folder, sizeof(queued_days[0]));
        }

        n_processed++;
        ESP_LOGI(TAG, "recreate_edfs_task: generating EDFs for session %d: %s",
                 n_processed, sessions[i].session_id);
        edf_gen_generate(sessions[i].session_dir, sessions[i].session_id,
                         sessions[i].start_epoch_ms, sessions[i].end_epoch_ms,
                         sessions[i].clock_drift_ms);
    }

    /* 7. Queue unique day folders for upload. */
    for (int j = 0; j < n_queued_days; j++) {
        uploader_on_day_invalidated(queued_days[j]);
    }

    ESP_LOGI(TAG, "recreate_edfs_task: done (%d/%d sessions processed, %d unique days queued)",
             n_processed, n_sessions, n_queued_days);
    free(queued_days);
    free(sessions);
    vTaskDelete(NULL);
}

/* Scoped single-day rebuild.  Unlike recreate_edfs_task() this deletes
 * nothing up front: edf_gen_rebuild_day() stages the day and publishes it
 * only on full success, and the day is queued for upload only then. */
static void rebuild_day_task(void *arg)
{
    char *day = (char *)arg;
    if (!day) { vTaskDelete(NULL); return; }

    esp_err_t ret = edf_gen_rebuild_day(day);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "rebuild_day_task: %s rebuilt, queueing for upload", day);
        /* The day's files were replaced: discard what the backends were told
         * before, then re-offer the day. */
        uploader_on_day_invalidated(day);
    } else {
        ESP_LOGE(TAG, "rebuild_day_task: %s failed: %s — not queueing upload",
                 day, esp_err_to_name(ret));
    }
    free(day);
    vTaskDelete(NULL);
}

/* SD format progress, polled by the browser via /api/format-progress.  The
 * format runs tens of seconds on a large card, far longer than an HTTP request
 * should stay open, so actions_handler returns immediately and the UI polls.
 * actions_handler zeroes this and sets .active before starting the task. */
typedef struct {
    volatile bool active;   /* format task is running       */
    volatile bool done;     /* task finished — check .ok     */
    volatile bool ok;       /* format succeeded             */
    char error[64];         /* failure reason when !ok      */
} format_progress_t;
static format_progress_t s_format_progress;
static SemaphoreHandle_t s_format_mtx;  /* guards s_format_progress reads/writes across tasks */

/* Background task for the destructive SD format.  PSRAM-backed because the
 * format is slow and must not block the HTTP handler.  Holds the destructive
 * lease for the whole operation, including the reboot, so nothing writes to the
 * card in between.  The success path never returns (it reboots). */
static void format_sd_task(void *arg)
{
    ESP_LOGW(TAG, "format_sd_task: starting destructive format");

    if (!sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE, 5000)) {
        xSemaphoreTake(s_format_mtx, portMAX_DELAY);
        strlcpy(s_format_progress.error,
                "SD busy — a recording, export or upload is using the card",
                sizeof(s_format_progress.error));
        xSemaphoreGive(s_format_mtx);
    } else {
        /* No uploader_reset_state() here: on success the reboot re-inits the
         * uploader against the now-empty state dir, and on failure the old
         * state is still valid.  Calling it would also wake the upload
         * scheduler to rescan the card mid-format. */
        esp_err_t ret = sd_storage_format();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "format_sd_task: formatted OK, rebooting for clean remount");
            xSemaphoreTake(s_format_mtx, portMAX_DELAY);
            s_format_progress.ok = true;
            s_format_progress.done = true;
            s_format_progress.active = false;
            xSemaphoreGive(s_format_mtx);
            /* Give the browser poll (2 s interval) time to read the result,
             * then flush the fresh filesystem and reboot. */
            vTaskDelay(pdMS_TO_TICKS(2500));
            sd_storage_deinit();
            esp_restart();
        }
        ESP_LOGE(TAG, "format_sd_task: failed: %s", esp_err_to_name(ret));
        xSemaphoreTake(s_format_mtx, portMAX_DELAY);
        strlcpy(s_format_progress.error, esp_err_to_name(ret),
                sizeof(s_format_progress.error));
        xSemaphoreGive(s_format_mtx);
        sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
    }

    xSemaphoreTake(s_format_mtx, portMAX_DELAY);
    s_format_progress.done = true;
    s_format_progress.active = false;
    xSemaphoreGive(s_format_mtx);
    vTaskDelete(NULL);
}

static esp_err_t format_progress_handler(httpd_req_t *req)
{
    bool active, done, ok;
    char error[64];

    xSemaphoreTake(s_format_mtx, portMAX_DELAY);
    active = s_format_progress.active;
    done   = s_format_progress.done;
    ok     = s_format_progress.ok;
    strlcpy(error, s_format_progress.error, sizeof(error));
    xSemaphoreGive(s_format_mtx);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", active);
    cJSON_AddBoolToObject(root, "done", done);
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "error", error);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    cJSON_free(json_str);
    return ESP_OK;
}

/* ── OTA firmware upload ─────────────────────────────────────────── */

#define OTA_CHUNK_SIZE   4096
#define OTA_MAX_SIZE     (0x400000)  /* 4 MB — partition size */
#define OTA_BUF_SIZE     (OTA_CHUNK_SIZE * 2)  /* stream buffer: 8 KB */

/* OTA flash task context — passed from httpd handler to the internal-stack task. */
typedef struct {
    StreamBufferHandle_t sbuf;       /* data channel: httpd → flash task */
    int total_size;                  /* expected image size (0 = unknown/chunked) */
    volatile bool eof;               /* download side finished sending data */
    esp_err_t result;                /* result from flash task */
    bool done;                       /* flash task finished */
} ota_ctx_t;

/* OTA flash task — runs on an INTERNAL RAM stack because esp_ota_* functions
 * freeze the SPI cache, which asserts the task stack is not in PSRAM. */
static void ota_flash_task(void *arg)
{
    ota_ctx_t *ctx = (ota_ctx_t *)arg;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "OTA: no update partition");
        ctx->result = ESP_FAIL;
        ctx->done = true;
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t ota_hdl = 0;
    esp_err_t err = esp_ota_begin(part, ctx->total_size, &ota_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: begin failed: %s", esp_err_to_name(err));
        ctx->result = err;
        ctx->done = true;
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buf = malloc(OTA_CHUNK_SIZE);
    if (!buf) {
        esp_ota_abort(ota_hdl);
        ctx->result = ESP_ERR_NO_MEM;
        ctx->done = true;
        vTaskDelete(NULL);
        return;
    }

    int written = 0;
    while (ctx->total_size > 0 ? (written < ctx->total_size) : !ctx->eof || xStreamBufferBytesAvailable(ctx->sbuf) > 0) {
        size_t want;
        if (ctx->total_size > 0) {
            want = ctx->total_size - written;
        } else {
            want = OTA_CHUNK_SIZE;
        }
        if (want > OTA_CHUNK_SIZE) want = OTA_CHUNK_SIZE;
        /* When total_size is unknown, use a short timeout so we can re-check eof */
        size_t got = xStreamBufferReceive(ctx->sbuf, buf, want, pdMS_TO_TICKS(ctx->total_size > 0 ? 10000 : 500));
        if (got == 0) {
            if (ctx->total_size > 0) {
                ESP_LOGE(TAG, "OTA: stream buffer timeout at %d/%d", written, ctx->total_size);
                esp_ota_abort(ota_hdl);
                free(buf);
                ctx->result = ESP_ERR_TIMEOUT;
                ctx->done = true;
                vTaskDelete(NULL);
                return;
            }
            /* chunked mode: no data yet, but not EOF — keep waiting */
            continue;
        }
        err = esp_ota_write(ota_hdl, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_hdl);
            free(buf);
            ctx->result = err;
            ctx->done = true;
            vTaskDelete(NULL);
            return;
        }
        written += got;
    }
    free(buf);

    err = esp_ota_end(ota_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: end failed: %s", esp_err_to_name(err));
        ctx->result = err;
        ctx->done = true;
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set_boot_partition failed: %s", esp_err_to_name(err));
        ctx->result = err;
        ctx->done = true;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA: flash complete (%d bytes)", written);
    ctx->result = ESP_OK;
    ctx->done = true;
    vTaskDelete(NULL);
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    int total = req->content_len;
    bool chunked = (total <= 0);

    if (!chunked && total > OTA_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image too large");
        return ESP_FAIL;
    }

    if (chunked) {
        ESP_LOGI(TAG, "OTA: receiving chunked stream");
    } else {
        ESP_LOGI(TAG, "OTA: receiving %d bytes", total);
    }

    /* Set up stream buffer and context for the flash task. */
    ota_ctx_t ctx = {
        .sbuf = xStreamBufferCreate(OTA_BUF_SIZE, 1),
        .total_size = total,
        .eof = false,
        .result = ESP_FAIL,
        .done = false,
    };
    if (!ctx.sbuf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom (stream buffer)");
        return ESP_FAIL;
    }

    /* Launch flash task on an INTERNAL RAM stack (not PSRAM) — required for
     * cache-freeze safety during esp_ota_write. */
    TaskHandle_t flash_task = NULL;
    BaseType_t tr = xTaskCreate(ota_flash_task, "ota_flash", 8192, &ctx, 5, &flash_task);
    if (tr != pdPASS) {
        vStreamBufferDelete(ctx.sbuf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom (flash task)");
        return ESP_FAIL;
    }

    /* Feed data from the HTTP socket to the stream buffer. */
    uint8_t *recv_buf = heap_caps_malloc(OTA_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!recv_buf) {
        ctx.eof = true;
        vStreamBufferDelete(ctx.sbuf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    int received = 0;
    while (chunked || received < total) {
        int want = chunked ? OTA_CHUNK_SIZE : (total - received);
        if (want > OTA_CHUNK_SIZE) want = OTA_CHUNK_SIZE;
        int r = httpd_req_recv(req, (char *)recv_buf, want);
        if (r < 0) {
            ESP_LOGE(TAG, "OTA: recv error at %d bytes", received);
            break;
        }
        if (r == 0) {
            /* chunked mode: end of stream */
            break;
        }
        if (chunked && received + r > OTA_MAX_SIZE) {
            ESP_LOGE(TAG, "OTA: chunked stream exceeded max size (%d)", OTA_MAX_SIZE);
            break;
        }
        /* Wait for space in the stream buffer (flash task may be slow). */
        size_t sent = xStreamBufferSend(ctx.sbuf, recv_buf, r, pdMS_TO_TICKS(10000));
        if (sent != (size_t)r) {
            ESP_LOGE(TAG, "OTA: stream buffer full at %d bytes", received);
            break;
        }
        received += r;
    }
    free(recv_buf);
    ctx.eof = true;  /* signal flash task that no more data is coming */

    /* Wait for the flash task to finish.  Use a longer timeout for chunked
     * mode since the data arrives over a potentially slow stream. */
    int wait_ms = 0;
    while (!ctx.done && wait_ms < 120000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_ms += 100;
    }

    if (!ctx.done) {
        ESP_LOGE(TAG, "OTA: flash task timed out");
        vStreamBufferDelete(ctx.sbuf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash task timeout");
        return ESP_FAIL;
    }

    vStreamBufferDelete(ctx.sbuf);

    if (ctx.result != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ctx.result));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: upload complete (%d bytes), rebooting", received);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    /* Reboot after a short delay so the response is sent. */
    psram_task_create(reboot_task, "ota_reboot", 4096, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    return ESP_OK;
}

/* ── OTA firmware download from URL ─────────────────────────────────── */

#define OTA_URL_MAX_LEN  512

/* Progress tracking for URL-based OTA (polled by browser via /api/ota-progress). */
typedef struct {
    volatile int total;       /* total bytes to download (0 = unknown) */
    volatile int downloaded;  /* bytes downloaded so far */
    volatile int flashed;     /* bytes flashed so far */
    volatile bool active;     /* download in progress */
    volatile bool done;       /* finished (check result) */
    volatile bool ok;         /* true if flash succeeded */
    char error[64];           /* error message if failed */
} ota_progress_t;
static ota_progress_t s_ota_progress;

/* OTA URL download task — uses ESP-IDF's esp_https_ota, which handles
 * redirects, chunked/content-length bodies, and all esp_ota_* flashing
 * internally. Runs on an internal-RAM stack (esp_ota_* freezes the cache). */
static void ota_url_task(void *arg)
{
    char *url = (char *)arg;

    ESP_LOGI(TAG, "OTA URL: downloading %s", url);

    memset((void *)&s_ota_progress, 0, sizeof(s_ota_progress));
    s_ota_progress.active = true;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        /* GitHub's redirect Location URL is ~900 chars (signed Azure blob
         * URL + JWT); the 512-byte default overflows with "Out of buffer". */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA URL: begin failed: %s", esp_err_to_name(err));
        strlcpy(s_ota_progress.error, esp_err_to_name(err), sizeof(s_ota_progress.error));
        goto out;
    }

    s_ota_progress.total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "OTA URL: image size %d bytes", s_ota_progress.total);

    /* Pump the download/flash loop, updating progress as we go. */
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int read = esp_https_ota_get_image_len_read(handle);
        s_ota_progress.downloaded = read;
        s_ota_progress.flashed = read;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA URL: perform failed: %s", esp_err_to_name(err));
        strlcpy(s_ota_progress.error, esp_err_to_name(err), sizeof(s_ota_progress.error));
        esp_https_ota_abort(handle);
        goto out;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA URL: incomplete image received");
        strlcpy(s_ota_progress.error, "incomplete image", sizeof(s_ota_progress.error));
        esp_https_ota_abort(handle);
        goto out;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA URL: finish failed: %s", esp_err_to_name(err));
        strlcpy(s_ota_progress.error, esp_err_to_name(err), sizeof(s_ota_progress.error));
        goto out;
    }

    ESP_LOGI(TAG, "OTA URL: flash complete, rebooting");
    s_ota_progress.ok = true;
    s_ota_progress.active = false;
    s_ota_progress.done = true;
    free(url);
    psram_task_create(reboot_task, "ota_reboot", 4096, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    vTaskDelete(NULL);
    return;

out:
    s_ota_progress.active = false;
    s_ota_progress.done = true;
    free(url);
    vTaskDelete(NULL);
}

/* HTTP handler: POST /api/ota-url with JSON body {"url":"https://..."}.
 * Launches a background task that downloads and flashes the firmware. */
static esp_err_t ota_url_handler(httpd_req_t *req)
{
    char body[OTA_URL_MAX_LEN + 64];
    int total = req->content_len;
    if (total <= 0 || total > (int)sizeof(body) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    if (!url_item || !cJSON_IsString(url_item) || !url_item->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"url\"");
        return ESP_FAIL;
    }

    /* Check URL starts with http:// or https:// */
    const char *url = url_item->valuestring;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URL must start with http:// or https://");
        return ESP_FAIL;
    }

    /* Copy URL for the background task (it frees it). */
    char *url_copy = strdup(url);
    cJSON_Delete(root);
    if (!url_copy) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    /* Launch the download+flash task on an internal RAM stack. */
    TaskHandle_t task = NULL;
    BaseType_t tr = xTaskCreate(ota_url_task, "ota_url", 12288, url_copy, 5, &task);
    if (tr != pdPASS) {
        free(url_copy);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM (task)");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Download started. Device will reboot when complete.\"}");
    return ESP_OK;
}

/* GET /api/ota-progress — returns current OTA URL download/flash progress. */
static esp_err_t ota_progress_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"active\":%s,\"done\":%s,\"ok\":%s,\"total\":%d,\"downloaded\":%d,\"flashed\":%d,\"error\":\"%s\"}",
             s_ota_progress.active ? "true" : "false",
             s_ota_progress.done ? "true" : "false",
             s_ota_progress.ok ? "true" : "false",
             s_ota_progress.total, s_ota_progress.downloaded, s_ota_progress.flashed,
             s_ota_progress.error);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* HTTP handler for consolidated actions. Body: {"action":"reset-state|delete-edfs|reset-all|recreate-edfs|format-sd"} */

static esp_err_t actions_handler(httpd_req_t *req)
{
    char body[256];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    if (!action) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'action'");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    if (strcmp(action, "reset-state") == 0) {
        /* Clears tracking and re-uploads, bounded by the configured upload
         * window; the scheduler does the work asynchronously. */
        ESP_LOGI(TAG, "action: reset upload state");
        uploader_reset_state();
    } else if (strcmp(action, "delete-edfs") == 0) {
        /* Destructive: refused while recording, exporting or uploading. */
        if (!sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE, 0)) {
            cJSON_Delete(root);
            return send_busy(req, "recording, export or upload in progress");
        }
        ESP_LOGI(TAG, "action: delete all EDF files");
        recursive_delete(SD_SDCARD_DIR);
        sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
    } else if (strcmp(action, "reset-all") == 0) {
        if (!sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE, 0)) {
            cJSON_Delete(root);
            return send_busy(req, "recording, export or upload in progress");
        }
        ESP_LOGI(TAG, "action: reset all (state + SDCARD + .somnotrace)");
        uploader_reset_state();
        recursive_delete(SD_SDCARD_DIR);
        recursive_delete(SD_SESSIONS_DIR);
        sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
    } else if (strcmp(action, "recreate-edfs") == 0) {
        /* Explicitly destructive maintenance: deletes the whole export before
         * rebuilding.  Prefer "rebuild-day" for recovering a single night. */
        if (sd_storage_recording_active()) {
            cJSON_Delete(root);
            return send_busy(req, "therapy recording in progress");
        }
        ESP_LOGW(TAG, "action: recreate ALL EDFs (destructive)");
        TaskHandle_t h = psram_task_create(recreate_edfs_task, "recreate_edfs", 16384, NULL, 5, 1, NULL, NULL);
        if (!h) {
            err = ESP_ERR_NO_MEM;
        }
    } else if (strcmp(action, "rebuild-day") == 0) {
        /* Scoped, success-gated recovery: rebuilds one noon-day as a
         * transaction and queues it for upload only if it fully succeeded. */
        cJSON *day = cJSON_GetObjectItem(root, "day");
        if (!day || !cJSON_IsString(day) || strlen(day->valuestring) != 8) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "missing or invalid 'day' (YYYYMMDD)");
            return ESP_FAIL;
        }
        char *day_arg = strdup(day->valuestring);
        if (!day_arg) {
            err = ESP_ERR_NO_MEM;
        } else {
            ESP_LOGI(TAG, "action: rebuild day %s", day_arg);
            TaskHandle_t h = psram_task_create(rebuild_day_task, "rebuild_day",
                                               16384, day_arg, 5, 1, NULL, NULL);
            if (!h) {
                free(day_arg);
                err = ESP_ERR_NO_MEM;
            }
        }
    } else if (strcmp(action, "format-sd") == 0) {
        if (sd_storage_recording_active()) {
            cJSON_Delete(root);
            return send_busy(req, "therapy recording in progress");
        }
        xSemaphoreTake(s_format_mtx, portMAX_DELAY);
        if (s_format_progress.active) {
            xSemaphoreGive(s_format_mtx);
            cJSON_Delete(root);
            return send_busy(req, "format already in progress");
        }
        ESP_LOGI(TAG, "action: format SD card (destructive)");
        /* Clear any previous result so /api/format-progress reports this run. */
        memset((void *)&s_format_progress, 0, sizeof(s_format_progress));
        s_format_progress.active = true;
        xSemaphoreGive(s_format_mtx);
        TaskHandle_t h = psram_task_create(format_sd_task, "format_sd", 16384, NULL, 5, 1, NULL, NULL);
        if (!h) {
            xSemaphoreTake(s_format_mtx, portMAX_DELAY);
            s_format_progress.active = false;
            xSemaphoreGive(s_format_mtx);
            err = ESP_ERR_NO_MEM;
        }
    } else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown action");
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t start_webserver(void)
{
    if (s_httpd) {
        ESP_LOGI(TAG, "stopping existing webserver");
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    /* The httpd worker runs on a PSRAM stack (task_caps below). Its handlers
     * must therefore never call flash-write directly — all NVS writes are
     * routed through the internal-stack nvs_writer task, which MUST exist
     * before the server can accept a request. Init it (and wire the uploader's
     * NVS executor to it) here, before httpd_start. Both are idempotent. */
    nvs_writer_init();
    uploader_set_nvs_executor((uploader_nvs_exec_fn_t)nvs_writer_run);
    therapy_alert_set_nvs_executor((alert_nvs_exec_fn_t)nvs_writer_run);
    /* Let the uploader participate in storage arbitration so it never reads a
     * day folder while a rebuild is replacing it. */
    uploader_set_lease_fns(uploader_lease_acquire, uploader_lease_release);
    /* Periodic upload scans yield to a live therapy recording; event-driven
     * uploads still run, since they matter more than a housekeeping scan. */
    upload_sched_set_busy_fn(sd_storage_recording_active);
    /* Guards s_format_progress between format_sd_task and the progress handler.
     * Created here so it exists before any request can reach the handler. */
    if (!s_format_mtx) s_format_mtx = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 60;
    config.stack_size = 12288;
    config.max_open_sockets = 20;
    config.recv_wait_timeout = 1;       /* close idle keep-alive sockets fast */
    config.send_wait_timeout = 5;
    config.keep_alive_enable = true;    /* detect dead connections via TCP probes */
    config.keep_alive_idle = 2;         /* start probing after 2s idle */
    config.keep_alive_interval = 2;     /* probe every 2s */
    config.keep_alive_count = 2;        /* 2 failed probes = dead */
    /* Allocate the httpd worker task's stack from PSRAM to free internal RAM.
     * Safe because no handler performs a flash write on this task (see above). */
    config.task_caps = MALLOC_CAP_SPIRAM;

    /* Silence benign peer-reset (104 ECONNRESET) log noise on client disconnects. */
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    ESP_LOGI(TAG, "starting httpd: stack=%d (PSRAM), handlers=%d, internal free=%u",
             config.stack_size, config.max_uri_handlers,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    esp_err_t herr = httpd_start(&s_httpd, &config);
    if (herr != ESP_OK) {
        ESP_LOGE(TAG, "failed to start httpd: %s (internal free=%u)",
                 esp_err_to_name(herr),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return ESP_FAIL;
    }

    session_graph_init();
    oximetry_http_register_handlers(s_httpd);

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_httpd, &root);

    httpd_uri_t wifi_uri = { .uri = "/wifi", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_httpd, &wifi_uri);

    httpd_uri_t manifest = { .uri = "/manifest.json", .method = HTTP_GET, .handler = manifest_get_handler };
    httpd_register_uri_handler(s_httpd, &manifest);

    httpd_uri_t sw = { .uri = "/sw.js", .method = HTTP_GET, .handler = sw_get_handler };
    httpd_register_uri_handler(s_httpd, &sw);

    httpd_uri_t uplot_js = { .uri = "/uplot.js", .method = HTTP_GET, .handler = uplot_js_get_handler };
    httpd_register_uri_handler(s_httpd, &uplot_js);

    httpd_uri_t uplot_css = { .uri = "/uplot.css", .method = HTTP_GET, .handler = uplot_css_get_handler };
    httpd_register_uri_handler(s_httpd, &uplot_css);

    httpd_uri_t logo_svg = { .uri = "/logo.svg", .method = HTTP_GET, .handler = logo_svg_get_handler };
    httpd_register_uri_handler(s_httpd, &logo_svg);

    httpd_uri_t favicon = { .uri = "/favicon.svg", .method = HTTP_GET, .handler = favicon_get_handler };
    httpd_register_uri_handler(s_httpd, &favicon);

    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
    httpd_register_uri_handler(s_httpd, &status);

    httpd_uri_t tz_db = { .uri = "/api/tz", .method = HTTP_GET, .handler = tz_get_handler };
    httpd_register_uri_handler(s_httpd, &tz_db);

    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);

    /* AirSense 11 BLE pairing endpoints (status folded into /api/status) */
    httpd_uri_t ble_scan = { .uri = "/api/ble/scan", .method = HTTP_GET, .handler = ble_scan_handler };
    httpd_uri_t ble_pair = { .uri = "/api/ble/pair", .method = HTTP_POST, .handler = ble_pair_handler };
    httpd_uri_t ble_conf = { .uri = "/api/ble/confirm", .method = HTTP_POST, .handler = ble_confirm_handler };
    httpd_uri_t ble_forget = { .uri = "/api/ble/forget", .method = HTTP_POST, .handler = ble_forget_handler };
    httpd_uri_t ble_pass = { .uri = "/api/ble/passthrough", .method = HTTP_POST, .handler = ble_passthrough_handler };
    httpd_register_uri_handler(s_httpd, &ble_scan);
    httpd_register_uri_handler(s_httpd, &ble_pair);
    httpd_register_uri_handler(s_httpd, &ble_conf);
    httpd_register_uri_handler(s_httpd, &ble_forget);
    httpd_register_uri_handler(s_httpd, &ble_pass);

    /* Oximeter (O2 Ring) BLE pairing endpoints (status folded into /api/status) */
    httpd_uri_t ox_scan = { .uri = "/api/ox/scan", .method = HTTP_GET, .handler = ox_scan_handler };
    httpd_uri_t ox_pair = { .uri = "/api/ox/pair", .method = HTTP_POST, .handler = ox_pair_handler };
    httpd_uri_t ox_forget = { .uri = "/api/ox/forget", .method = HTTP_POST, .handler = ox_forget_handler };
    httpd_uri_t ox_pm = { .uri = "/api/ox/probe-mode", .method = HTTP_POST, .handler = ox_probe_mode_handler };
    httpd_register_uri_handler(s_httpd, &ox_scan);
    httpd_register_uri_handler(s_httpd, &ox_pair);
    httpd_register_uri_handler(s_httpd, &ox_forget);
    httpd_register_uri_handler(s_httpd, &ox_pm);

    /* EZShare-compatible file server endpoints */
    httpd_uri_t dir_hdl = { .uri = "/dir", .method = HTTP_GET, .handler = dir_get_handler };
    httpd_uri_t dl_hdl = { .uri = "/download", .method = HTTP_GET, .handler = download_get_handler };
    httpd_register_uri_handler(s_httpd, &dir_hdl);
    httpd_register_uri_handler(s_httpd, &dl_hdl);

    /* Upload configuration endpoints (status folded into /api/status) */
    httpd_uri_t up_prog_get = { .uri = "/api/uploads/progress", .method = HTTP_GET, .handler = upload_progress_get_handler };
    httpd_uri_t up_state_get = { .uri = "/api/uploads/state", .method = HTTP_GET, .handler = upload_state_get_handler };
    httpd_uri_t up_cfg_get = { .uri = "/api/uploads/config", .method = HTTP_GET, .handler = upload_config_get_handler };
    httpd_uri_t up_cfg_post = { .uri = "/api/uploads/config", .method = HTTP_POST, .handler = upload_config_post_handler };
    httpd_register_uri_handler(s_httpd, &up_cfg_get);
    httpd_register_uri_handler(s_httpd, &up_cfg_post);
    httpd_register_uri_handler(s_httpd, &up_prog_get);
    httpd_register_uri_handler(s_httpd, &up_state_get);

    /* "Test connection" buttons: probe a backend with the saved settings */
    httpd_uri_t up_test_smb = { .uri = "/api/uploads/test-smb", .method = HTTP_POST, .handler = upload_test_smb_handler };
    httpd_uri_t up_test_shq = { .uri = "/api/uploads/test-sleephq", .method = HTTP_POST, .handler = upload_test_sleephq_handler };
    httpd_register_uri_handler(s_httpd, &up_test_smb);
    httpd_register_uri_handler(s_httpd, &up_test_shq);

    /* Device settings endpoints (brightness, LCD therapy mode) */
    httpd_uri_t settings_all = { .uri = "/api/settings/all", .method = HTTP_GET, .handler = settings_all_get_handler };
    httpd_register_uri_handler(s_httpd, &settings_all);
    httpd_uri_t dev_get = { .uri = "/api/device/settings", .method = HTTP_GET, .handler = device_settings_get_handler };
    httpd_uri_t dev_post = { .uri = "/api/device/settings", .method = HTTP_POST, .handler = device_settings_post_handler };
    httpd_register_uri_handler(s_httpd, &dev_get);
    httpd_register_uri_handler(s_httpd, &dev_post);

    /* Audio test beep endpoint */
    httpd_uri_t beep_test = { .uri = "/api/device/test-beep", .method = HTTP_POST, .handler = audio_test_beep_handler };
    httpd_register_uri_handler(s_httpd, &beep_test);

    /* Therapy alert config endpoints */
    httpd_uri_t alert_cfg_get = { .uri = "/api/alert/config", .method = HTTP_GET, .handler = alert_config_get_handler };
    httpd_uri_t alert_cfg_post = { .uri = "/api/alert/config", .method = HTTP_POST, .handler = alert_config_post_handler };
    httpd_uri_t alert_test = { .uri = "/api/alert/test", .method = HTTP_POST, .handler = alert_test_push_handler };
    httpd_register_uri_handler(s_httpd, &alert_cfg_get);
    httpd_register_uri_handler(s_httpd, &alert_cfg_post);
    httpd_register_uri_handler(s_httpd, &alert_test);

    /* Reboot endpoint */
    httpd_uri_t reboot_post = { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler };
    httpd_register_uri_handler(s_httpd, &reboot_post);

    /* Heap stats endpoint (per-task stack HWM, internal/PSRAM/DMA breakdown) */
    httpd_uri_t heap_stats = { .uri = "/api/heap", .method = HTTP_GET, .handler = heap_stats_handler };
    httpd_register_uri_handler(s_httpd, &heap_stats);

    /* Consolidated actions endpoint */
    httpd_uri_t actions = { .uri = "/api/actions", .method = HTTP_POST, .handler = actions_handler };
    httpd_register_uri_handler(s_httpd, &actions);

    httpd_uri_t format_prog = { .uri = "/api/format-progress", .method = HTTP_GET, .handler = format_progress_handler };
    httpd_register_uri_handler(s_httpd, &format_prog);

    /* OTA firmware upload endpoint */
    httpd_uri_t ota_upload = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_upload_handler };
    httpd_register_uri_handler(s_httpd, &ota_upload);

    /* OTA firmware download from URL endpoint */
    httpd_uri_t ota_url = { .uri = "/api/ota-url", .method = HTTP_POST, .handler = ota_url_handler };
    httpd_register_uri_handler(s_httpd, &ota_url);

    /* OTA progress polling endpoint */
    httpd_uri_t ota_prog = { .uri = "/api/ota-progress", .method = HTTP_GET, .handler = ota_progress_handler };
    httpd_register_uri_handler(s_httpd, &ota_prog);

    /* Log stream endpoints (SSE, download, level control) */
    log_stream_register_handlers(s_httpd);

    /* Session graph data endpoints (dashboard charts) */
    httpd_uri_t sessions_list = { .uri = "/api/sessions", .method = HTTP_GET, .handler = sessions_list_handler };
    httpd_uri_t session_graph = { .uri = "/api/session/graph", .method = HTTP_GET, .handler = session_graph_handler };
    httpd_uri_t session_file  = { .uri = "/api/session/file", .method = HTTP_GET, .handler = session_file_handler };
    httpd_uri_t session_file_head = { .uri = "/api/session/file", .method = HTTP_HEAD, .handler = session_file_handler };
    httpd_uri_t days_list     = { .uri = "/api/days", .method = HTTP_GET, .handler = days_list_handler };
    httpd_uri_t summary_uri   = { .uri = "/api/summary", .method = HTTP_GET, .handler = summary_handler };
    httpd_uri_t sess_settings = { .uri = "/api/session/settings", .method = HTTP_GET, .handler = session_settings_handler };
    httpd_register_uri_handler(s_httpd, &sessions_list);
    httpd_register_uri_handler(s_httpd, &session_graph);
    httpd_register_uri_handler(s_httpd, &session_file);
    httpd_register_uri_handler(s_httpd, &session_file_head);
    httpd_register_uri_handler(s_httpd, &days_list);
    httpd_register_uri_handler(s_httpd, &summary_uri);
    httpd_register_uri_handler(s_httpd, &sess_settings);

    if (s_portal_mode) {
        /* Captive-portal probe intercepts (return 302 to trigger portal popup) */
        const char *probes[] = {
            "/hotspot-detect.html",
            "/generate_204",
            "/gen_204",
            "/connecttest.txt",
            "/ncsi.txt",
            "/success.txt",
            "/canonical.html",
            "/service/update2/json",
            NULL,
        };
        for (int i = 0; probes[i]; i++) {
            httpd_uri_t probe = {
                .uri = probes[i],
                .method = HTTP_GET,
                .handler = redirect_to_portal,
            };
            httpd_register_uri_handler(s_httpd, &probe);
        }
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Captive DNS server (wildcard hijack)                              */
/* ------------------------------------------------------------------ */
void netprov_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);

    ESP_LOGI(TAG, "captive DNS server listening on port 53");

    while (true) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&src_addr, &src_len);
        if (len < 12) continue;

        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if (qdcount != 1) continue;

        int qoff = 12;
        while (qoff < len && buf[qoff] != 0) {
            qoff += buf[qoff] + 1;
        }
        qoff++;
        if (qoff + 4 > len) continue;
        uint16_t qtype = (buf[qoff] << 8) | buf[qoff + 1];
        uint16_t qclass = (buf[qoff + 2] << 8) | buf[qoff + 3];
        qoff += 4;

        if (qtype != 1 || qclass != 1) continue;

        uint8_t resp[512];
        int rlen = 0;
        resp[rlen++] = buf[0]; resp[rlen++] = buf[1];
        resp[rlen++] = 0x81; resp[rlen++] = 0x80;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        memcpy(resp + rlen, buf + 12, qoff - 12);
        rlen += qoff - 12;

        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x04;
        resp[rlen++] = (s_ap_ip >> 0) & 0xFF;
        resp[rlen++] = (s_ap_ip >> 8) & 0xFF;
        resp[rlen++] = (s_ap_ip >> 16) & 0xFF;
        resp[rlen++] = (s_ap_ip >> 24) & 0xFF;

        sendto(sock, resp, rlen, 0, (struct sockaddr *)&src_addr, src_len);
    }
}

/* ------------------------------------------------------------------ */
/*  Public start functions                                            */
/* ------------------------------------------------------------------ */
esp_err_t netprov_start_portal(const struct netprov_config *cfg, char *ap_ip_out)
{
    s_portal_mode = true;
    s_connecting = false;
    link_mark_down();
    esp_wifi_disconnect();
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-setup", cfg->hostname);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "",
            .ssid_len = strlen(s_ap_ssid),
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1,
        },
    };
    memcpy(ap_cfg.ap.ssid, s_ap_ssid, ap_cfg.ap.ssid_len);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_netif_ap, &ip_info);
    if (ap_ip_out) {
        snprintf(ap_ip_out, 16, IPSTR, IP2STR(&ip_info.ip));
    }
    s_ap_ip = ip_info.ip.addr;

    ESP_LOGI(TAG, "SoftAP '%s' up at " IPSTR, s_ap_ssid, IP2STR(&ip_info.ip));

    psram_task_create(netprov_dns_task, "dns", 4096, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    return start_webserver();
}

void netprov_start_link_supervisor(void)
{
    static bool supervisor_started = false;
    if (supervisor_started) return;
    psram_task_create(link_supervisor_task, "link_sup", 4096,
                      NULL, 3, tskNO_AFFINITY, NULL, NULL);
    supervisor_started = true;
}

void netprov_request_rescan(void)
{
    s_rescan_requested = true;
}

esp_err_t netprov_start_connected_server(const char *ip)
{
    s_portal_mode = false;
    strlcpy(s_connected_ip, ip, sizeof(s_connected_ip));

    /* Start the link supervisor for autonomous failover.  The task checks
     * s_rescan_requested which is raised by the event handler after repeated
     * failed reconnects to the current SSID. */
    netprov_start_link_supervisor();

    /* Start mDNS so the device is reachable as <name>.local */
    char mdns_name[MDNS_NAME_MAX];
    netprov_get_mdns_name(mdns_name, sizeof(mdns_name));
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(mdns_name);
        mdns_service_add("SomnoTrace", "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS started: %s.local", mdns_name);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }

    return start_webserver();
}
