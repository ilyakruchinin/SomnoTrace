/* SomnoTrace Rev C native Connectivity, Alerts and Uploads. */
#pragma once
#include "lvgl.h"
#include "net_provision.h"
#include "therapy_alert.h"
#include "alert_history.h"
#include "uploader.h"
#include "timezone_catalog.h"

typedef enum { MC_WIFI, MC_TIME, MC_ALERTS, MC_UPLOADS } manage_config_page_t;
typedef enum {
    MC_REFRESH, MC_WIFI_SSID, MC_WIFI_PASSWORD, MC_WIFI_FORGET, MC_WIFI_MOVE,
    MC_WIFI_RECONNECT, MC_WIFI_SCAN, MC_IP_MODE, MC_IP_ADDRESS, MC_IP_MASK,
    MC_IP_GATEWAY, MC_IP_DNS, MC_TIMEZONE_SEARCH, MC_TIMEZONE_SET,
    MC_NTP, MC_HOSTNAME, MC_TIME_SYNC, MC_ALERT_ENABLE, MC_ALERT_WINDOW,
    MC_ALERT_DELAY1, MC_ALERT_DELAY2, MC_ALERT_PUSH, MC_ALERT_SERVER,
    MC_ALERT_TOPIC, MC_ALERT_GENERATE, MC_ALERT_PRIORITY, MC_ALERT_TEST,
    MC_ALERT_HISTORY, MC_SMB_ENABLE, MC_SMB_HOST, MC_SMB_SHARE, MC_SMB_PATH,
    MC_SMB_USER, MC_SMB_PASSWORD, MC_SHQ_ENABLE, MC_SHQ_ID, MC_SHQ_SECRET,
    MC_FTP_ENABLE, MC_FTP_ANON, MC_FTP_USER, MC_FTP_PASSWORD,
    MC_UPLOAD_DAYS, MC_UPLOAD_TEST, MC_UPLOAD_RETRY
} manage_config_command_t;
typedef struct {
    manage_config_command_t kind;
    int slot, index;
    char value[128];
} manage_config_request_t;
/* Secrets are redacted before publishing this value to the UI. */
typedef struct {
    bool ready, busy, simulated, recording, ftp_running;
    uint32_t generation, verified_at;
    char result[128];
    struct netprov_config wifi;
    netprov_link_t link;
    char mac[18], hostname[33], timezone[48], ntp[64], local_time[48];
    char sync_detail[112];
    netprov_scan_snapshot_t scan;
    timezone_catalog_entry_t zones[6];
    size_t zone_count;
    therapy_alert_config_t alerts;
    alert_history_page_t history;
    uploader_config_t uploads;
    uploader_progress_snapshot_t progress;
    uploader_test_snapshot_t test;
    bool progress_valid;
} manage_config_snapshot_t;

esp_err_t touch_manage_config_start(void);
esp_err_t touch_manage_config_submit(const manage_config_request_t *request);
bool touch_manage_config_snapshot(manage_config_snapshot_t *out);
/* UI operations only allocate while a destination is visible. Worker never
 * holds LVGL pointers and continues safely after navigation. */
esp_err_t touch_manage_config_show(lv_obj_t *parent, manage_config_page_t page);
void touch_manage_config_hide(void);
void touch_manage_config_refresh(void);

typedef struct {
    bool ready, alerts_enabled, push_configured, push_verified, history_storage_error;
    unsigned configured_uploads, failed_uploads;
} manage_config_health_t;
/* Tiny RAM-only snapshot for the shared rail; unstarted means unknown. */
void touch_manage_config_health(manage_config_health_t *out);
