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


#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define NETPROV_SSID_MAXLEN     32
#define NETPROV_PASS_MAXLEN     64
#define NETPROV_HOSTNAME_MAXLEN 32
#define NETPROV_MAX_SSID_SLOTS  4

/* One stored Wi-Fi credential pair. */
struct netprov_wifi_cred {
    char ssid[NETPROV_SSID_MAXLEN + 1];
    char pass[NETPROV_PASS_MAXLEN + 1];
};

/* Full configuration loaded from NVS. */
struct netprov_config {
    char hostname[NETPROV_HOSTNAME_MAXLEN + 1];
    struct netprov_wifi_cred wifi[NETPROV_MAX_SSID_SLOTS];
};

#include "cJSON.h"

/* Initialise NVS, netif, event loop and Wi-Fi driver. Call once at boot. */
esp_err_t netprov_init(void);

/* Build full system status JSON object (used by /api/status and /api/ws). */
cJSON *netprov_build_status_json(void);

/* Live station link state, maintained from Wi-Fi/IP events.
 *
 * This is observed state, not a boot-time assumption: `up` goes false the
 * moment the AP disappears, so callers must never cache "connected". */
typedef struct {
    bool up;                              /* associated AND holding an IP */
    char ssid[NETPROV_SSID_MAXLEN + 1];   /* AP actually in use ("" if down) */
    char ip[16];                          /* current IP ("0.0.0.0" if down) */
    int  rssi;                            /* dBm; only valid if rssi_valid */
    bool rssi_valid;                      /* false when down or query failed */
} netprov_link_t;

/* Snapshot the current station link state. Non-blocking, safe from any task. */
void netprov_get_link(netprov_link_t *out);

/* True while the station is associated and holds an IP. */
bool netprov_is_link_up(void);

/* Load the full config from NVS. Returns true if at least one SSID is stored. */
bool netprov_load_config(struct netprov_config *cfg);

/* Save the full config to NVS. */
esp_err_t netprov_save_config(const struct netprov_config *cfg);

/* Try to connect as a station using stored credentials.
 * Scans, picks the strongest matching SSID, tries up to 3 attempts
 * with 5 s spacing per candidate. On success writes IP into ip_out
 * (>= 16 bytes) and returns ESP_OK. */
esp_err_t netprov_try_connect(const struct netprov_config *cfg,
                              char *ip_out, int timeout_ms);

/* Start the SoftAP provisioning portal and captive DNS/HTTP server.
 * SSID is "${hostname}-setup". ap_ip_out (>= 16 bytes) receives the AP IP.
 * After credentials are saved the device reboots. */
esp_err_t netprov_start_portal(const struct netprov_config *cfg, char *ap_ip_out);

/* Start the web server in connected (STA) mode showing the device IP. */
esp_err_t netprov_start_connected_server(const char *ip);

/* Start the autonomous link supervisor without a web server. Used when the
 * boot-time connect failed so the device keeps trying to reach a configured
 * network in the background (e.g. after a router power blip). Idempotent. */
void netprov_start_link_supervisor(void);

/* Ask the link supervisor to attempt a full scan-and-connect cycle now. */
void netprov_request_rescan(void);

/* Task entry for the captive DNS server (wildcard hijack). 
 * arg is ignored; starts automatically inside netprov_start_portal. */
void netprov_dns_task(void *arg);

/* mDNS custom name (stored in NVS, defaults to "somnotrace"). */
void netprov_get_mdns_name(char *out, size_t out_len);
esp_err_t netprov_set_mdns_name(const char *name);
const char *netprov_mdns_name_cached(void);
