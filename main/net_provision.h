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
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define NETPROV_SSID_MAXLEN     32
#define NETPROV_PASS_MAXLEN     64
#define NETPROV_HOSTNAME_MAXLEN 32
#define NETPROV_MAX_SSID_SLOTS  4
#define NETPROV_SCAN_MAX_APS     20

/* IPv4 policy travels with its saved network, never with a display draft. */
struct netprov_ipv4 {
    bool manual;
    char address[16], netmask[16], gateway[16], dns[16];
};

/* One stored Wi-Fi credential pair. */
struct netprov_wifi_cred {
    char ssid[NETPROV_SSID_MAXLEN + 1];
    char pass[NETPROV_PASS_MAXLEN + 1];
    struct netprov_ipv4 ipv4;
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

/* Asynchronous, non-consuming nearby-network scan.  The returned snapshot is
 * a bounded value copy and remains valid until a later request changes it;
 * callers never own driver or heap memory.  `generation` changes once per
 * request which starts a scan or publishes a blocked/error outcome.
 *
 * Scans are rejected while therapy/storage is active or while another Wi-Fi
 * radio operation owns the driver.  Before netprov_init() (including the QEMU
 * UI build) request returns ESP_ERR_INVALID_STATE and the snapshot reports
 * NETPROV_SCAN_BLOCK_NOT_INITIALIZED. */
typedef enum {
    NETPROV_SCAN_IDLE = 0,
    NETPROV_SCAN_RUNNING,
    NETPROV_SCAN_READY,
    NETPROV_SCAN_ERROR,
    NETPROV_SCAN_BLOCKED,
} netprov_scan_state_t;

typedef enum {
    NETPROV_SCAN_BLOCK_NONE = 0,
    NETPROV_SCAN_BLOCK_NOT_INITIALIZED,
    NETPROV_SCAN_BLOCK_RECORDING,
    NETPROV_SCAN_BLOCK_RADIO_BUSY,
} netprov_scan_block_t;

typedef struct {
    char ssid[NETPROV_SSID_MAXLEN + 1];
    int8_t rssi;
    bool secure;
} netprov_scan_ap_t;

typedef struct {
    netprov_scan_state_t state;
    netprov_scan_block_t blocked_by;
    esp_err_t result;
    uint32_t generation;
    size_t count;
    netprov_scan_ap_t aps[NETPROV_SCAN_MAX_APS];
} netprov_scan_snapshot_t;

/* Reserve the Wi-Fi radio and start a background scan. */
esp_err_t netprov_scan_request(void);

/* Copy the latest scan state/result. Safe from any task and before init. */
void netprov_scan_get_snapshot(netprov_scan_snapshot_t *out);

/* Serialize firmware updates and every controlled restart.  Callers must
 * acquire this claim before publishing/scheduling an operation which can
 * reach esp_restart(), keep it until restart, and release it on every path
 * which returns without restarting.  owner must have static lifetime. */
bool netprov_lifecycle_try_claim(const char *owner);
void netprov_lifecycle_release(void);

/* Load the full config from NVS. Returns true if at least one SSID is stored. */
bool netprov_load_config(struct netprov_config *cfg);

/* Save the full config to NVS. */
esp_err_t netprov_save_config(const struct netprov_config *cfg);

/* Try to connect as a station using stored credentials.
 * Scans, picks matching SSIDs in saved slot order, tries up to 3 attempts
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

/* Worker-only: persist then update supervisor cache. Reorder/save never drops
 * an existing link. reconnect=true explicitly disconnects and tries slot order;
 * rejected while recording, scanning, portal mode, or lifecycle work is active. */
esp_err_t netprov_apply_config(const struct netprov_config *cfg, bool reconnect);
esp_err_t netprov_validate_config(const struct netprov_config *cfg);
void netprov_get_mac(char out[18]);
