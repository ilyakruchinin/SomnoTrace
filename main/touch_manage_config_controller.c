#include "touch_manage_config.h"
#include "config_redaction.h"
#include "time_sync.h"
#include "nvs_writer.h"
#include "nvs.h"
#include "psram_task.h"
#include "bsp_display.h"
#include "sd_storage.h"
#include "ftp.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct {
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    manage_config_snapshot_t *live;
} s_controller;
static bool number(const char *s, int min, int max, int *out)
{
    if (!s || !*s) return false;
    int n = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return false; n = n * 10 + *s - '0'; if (n > max) return false; }
    *out = n; return n >= min;
}
static void observe(manage_config_snapshot_t *s)
{
    netprov_load_config(&s->wifi);
    for (int i = 0; i < 4; i++) config_redact(s->wifi.wifi[i].pass, sizeof(s->wifi.wifi[i].pass), "Saved", "Open network");
    therapy_alert_load_config(&s->alerts);
    uploader_load_config(&s->uploads);
    config_redact(s->uploads.smb_pass, sizeof(s->uploads.smb_pass), "Saved", "Not set");
    config_redact(s->uploads.shq_client_secret, sizeof(s->uploads.shq_client_secret), "Saved", "Not set");
    config_redact(s->uploads.ftp_pass, sizeof(s->uploads.ftp_pass), "Saved", "Not set");
    time_sync_get_tz_name(s->timezone, sizeof(s->timezone));
    time_sync_get_ntp_server(s->ntp, sizeof(s->ntp));
    netprov_get_mdns_name(s->hostname, sizeof(s->hostname));
    time_t now = time(NULL); struct tm tm;
    if (now > 1609459200 && localtime_r(&now, &tm)) strftime(s->local_time, sizeof(s->local_time), "%d %b %Y %H:%M:%S %Z", &tm);
    else strlcpy(s->local_time, "Clock not established", sizeof(s->local_time));
    int64_t at = time_sync_last_success_epoch();
    if (at) {
        time_t when = at; char text[40]; localtime_r(&when, &tm); strftime(text, sizeof(text), "%d %b %H:%M:%S", &tm);
        snprintf(s->sync_detail, sizeof(s->sync_detail), "NTP: %s; %lld s ago (this boot)", text, (long long)(now >= at ? now - at : 0));
    } else snprintf(s->sync_detail, sizeof(s->sync_detail), "%s; no NTP sync recorded in this boot",
        time_source_get() == TIME_SRC_AS11_DRIFT ? "AS11 + saved drift" : "Clock source unavailable");
    s->history.write_error = alert_history_storage_error();
    s->verified_at = alert_history_verified_at(s->alerts.ntfy_srv, s->alerts.ntfy_topic);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    s->simulated = true;
    s->recording = false; /* explicit controller simulation, independent of Home demo */
    s->link.up = true; s->link.rssi_valid = true; s->link.rssi = -51;
    strlcpy(s->link.ssid, "QEMU-2.4GHz", sizeof(s->link.ssid));
    strlcpy(s->link.ip, "192.0.2.42", sizeof(s->link.ip));
    strlcpy(s->mac, "02:00:00:00:00:42", sizeof(s->mac));
#else
    s->recording = bsp_display_is_therapy_active() || sd_storage_recording_active();
    netprov_get_link(&s->link); netprov_get_mac(s->mac);
    netprov_scan_get_snapshot(&s->scan);
    s->progress_valid = uploader_get_progress_snapshot(&s->progress) == ESP_OK;
    uploader_test_snapshot(&s->test);
    s->ftp_running = ftp_isenabled();
#endif
    s->ready = true;
}
static esp_err_t apply_wifi(const manage_config_request_t *q)
{
    struct netprov_config cfg;
    netprov_load_config(&cfg);
    if (q->slot < 0 || q->slot >= 4) return ESP_ERR_INVALID_ARG;
    struct netprov_wifi_cred *w = &cfg.wifi[q->slot];
    switch (q->kind) {
    case MC_WIFI_SSID: if (strlen(q->value) > 32) return ESP_ERR_INVALID_ARG; strlcpy(w->ssid, q->value, sizeof(w->ssid)); break;
    case MC_WIFI_PASSWORD: if (strlen(q->value) > 63) return ESP_ERR_INVALID_ARG; if (*q->value) strlcpy(w->pass, q->value, sizeof(w->pass)); break;
    case MC_WIFI_FORGET: memset(w, 0, sizeof(*w)); break;
    case MC_WIFI_MOVE: {
        if (q->index < 0 || q->index >= 4) return ESP_ERR_INVALID_ARG;
        struct netprov_wifi_cred move = *w;
        if (q->index < q->slot) memmove(&cfg.wifi[q->index+1], &cfg.wifi[q->index], (q->slot-q->index)*sizeof(*w));
        else memmove(w, w+1, (q->index-q->slot)*sizeof(*w));
        cfg.wifi[q->index] = move; break;
    }
    case MC_IP_MODE: w->ipv4.manual = !w->ipv4.manual; break;
    case MC_IP_ADDRESS: strlcpy(w->ipv4.address, q->value, 16); break;
    case MC_IP_MASK: strlcpy(w->ipv4.netmask, q->value, 16); break;
    case MC_IP_GATEWAY: strlcpy(w->ipv4.gateway, q->value, 16); break;
    case MC_IP_DNS: strlcpy(w->ipv4.dns, q->value, 16); break;
    case MC_WIFI_RECONNECT: break;
    default: return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    esp_err_t result = netprov_save_config(&cfg);
#else
    esp_err_t result = netprov_apply_config(&cfg, q->kind == MC_WIFI_RECONNECT);
#endif
    memset(&cfg, 0, sizeof(cfg)); return result;
}
static esp_err_t apply_alert(const manage_config_request_t *q)
{
    therapy_alert_config_t a; therapy_alert_load_config(&a); int n;
    switch (q->kind) {
    case MC_ALERT_ENABLE: a.enabled = !a.enabled; break;
    case MC_ALERT_PUSH: a.push_en = !a.push_en; break;
    case MC_ALERT_SERVER: if (strlen(q->value) >= sizeof(a.ntfy_srv)) return ESP_ERR_INVALID_ARG; strlcpy(a.ntfy_srv, q->value, sizeof(a.ntfy_srv)); break;
    case MC_ALERT_TOPIC: if (strlen(q->value) >= sizeof(a.ntfy_topic)) return ESP_ERR_INVALID_ARG; strlcpy(a.ntfy_topic, q->value, sizeof(a.ntfy_topic)); break;
    case MC_ALERT_GENERATE: snprintf(a.ntfy_topic, sizeof(a.ntfy_topic), "somnotrace-%08lx%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random()); break;
    case MC_ALERT_PRIORITY: if (!number(q->value, 1, 5, &n)) return ESP_ERR_INVALID_ARG; a.ntfy_prio = n; break;
    case MC_ALERT_DELAY1: if (!number(q->value, 0, 180, &n)) return ESP_ERR_INVALID_ARG; a.delay1 = n; break;
    case MC_ALERT_DELAY2: if (!number(q->value, 0, 180, &n)) return ESP_ERR_INVALID_ARG; a.delay2 = n; break;
    case MC_ALERT_WINDOW: {
        int h1,m1,h2,m2; char tail;
        if (strlen(q->value) != 11 || sscanf(q->value, "%2d:%2d-%2d:%2d%c", &h1,&m1,&h2,&m2,&tail) != 4 ||
            h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23 || m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59) return ESP_ERR_INVALID_ARG;
        a.win_start = h1*60+m1; a.win_end = h2*60+m2; break;
    }
    default: return ESP_ERR_INVALID_ARG;
    }
    a.buzz_en = false; /* no speaker on 7B; second push uses delay2 */
    return therapy_alert_save_config(&a);
}
static esp_err_t apply_upload(const manage_config_request_t *q)
{
    uploader_config_t u; uploader_load_config(&u); char *dst = NULL; size_t size = 0; bool secret = false; int n;
    switch (q->kind) {
    case MC_SMB_ENABLE: u.smb_enabled = !u.smb_enabled; break;
    case MC_SHQ_ENABLE: u.shq_enabled = !u.shq_enabled; break;
    case MC_FTP_ENABLE: u.ftp_enabled = !u.ftp_enabled; break;
    case MC_FTP_ANON: u.ftp_anonymous = !u.ftp_anonymous; break;
    case MC_UPLOAD_DAYS: if (!number(q->value, 1, UPLOAD_MAX_DAYS_CAP, &n)) return ESP_ERR_INVALID_ARG; u.max_days = n; break;
#define FIELD(cmd, field, priv) case cmd: dst = u.field; size = sizeof(u.field); secret = priv; break
    FIELD(MC_SMB_HOST, smb_host, false); FIELD(MC_SMB_SHARE, smb_share, false);
    FIELD(MC_SMB_PATH, smb_path, false); FIELD(MC_SMB_USER, smb_user, false);
    FIELD(MC_SMB_PASSWORD, smb_pass, true); FIELD(MC_SHQ_ID, shq_client_id, false);
    FIELD(MC_SHQ_SECRET, shq_client_secret, true); FIELD(MC_FTP_USER, ftp_user, false);
    FIELD(MC_FTP_PASSWORD, ftp_pass, true);
#undef FIELD
    default: return ESP_ERR_INVALID_ARG;
    }
    if (dst) { if (strlen(q->value) >= size) return ESP_ERR_INVALID_ARG; if (*q->value || !secret) strlcpy(dst, q->value, size); }
    esp_err_t result = uploader_save_config(&u); memset(&u, 0, sizeof(u)); return result;
}
static esp_err_t execute(const manage_config_request_t *q, manage_config_snapshot_t *s)
{
    if ((q->kind >= MC_WIFI_SSID && q->kind <= MC_WIFI_RECONNECT) || (q->kind >= MC_IP_MODE && q->kind <= MC_IP_DNS)) return apply_wifi(q);
    if (q->kind >= MC_ALERT_ENABLE && q->kind <= MC_ALERT_PRIORITY) return apply_alert(q);
    if (q->kind >= MC_SMB_ENABLE && q->kind <= MC_UPLOAD_DAYS) return apply_upload(q);
    switch (q->kind) {
    case MC_REFRESH: return ESP_OK;
    case MC_TIMEZONE_SEARCH: s->zone_count = timezone_catalog_search(q->value, s->zones, 6); return ESP_OK;
    case MC_TIMEZONE_SET: {
        timezone_catalog_entry_t z; esp_err_t e = timezone_catalog_lookup(q->value, &z);
        return e == ESP_OK ? time_sync_set_timezone(z.posix, z.id) : e;
    }
    case MC_NTP: return time_sync_set_ntp_server(q->value);
    case MC_HOSTNAME: return netprov_set_mdns_name(q->value);
    case MC_ALERT_HISTORY: alert_history_read(q->index, &s->history); return s->history.storage_result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : s->history.storage_result;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    case MC_WIFI_SCAN:
        s->scan.state = NETPROV_SCAN_READY; s->scan.generation++; s->scan.count = 2;
        strlcpy(s->scan.aps[0].ssid, "QEMU-2.4GHz", 33); s->scan.aps[0].rssi = -51; s->scan.aps[0].secure = true;
        strlcpy(s->scan.aps[1].ssid, "QEMU-fallback", 33); s->scan.aps[1].rssi = -70; return ESP_OK;
    case MC_TIME_SYNC: return ESP_OK;
    case MC_ALERT_TEST: {
        uint32_t id = alert_history_begin(true);
        alert_history_result(id, q->index ? ALERT_DELIVERY_FAILED : ALERT_DELIVERY_ACCEPTED, false);
        return q->index ? ESP_FAIL : ESP_OK;
    }
    case MC_UPLOAD_TEST:
        memset(&s->test, 0, sizeof(s->test));
        strlcpy(s->test.backend, q->value, sizeof(s->test.backend));
        s->test.state = UPLOAD_TEST_RUNNING;
        for (int stage = 0; stage < UPLOAD_TEST_STAGE_COUNT; stage++) {
            bool relevant = !strcmp(q->value, "smb") || stage == UPLOAD_STAGE_CONNECT || stage == UPLOAD_STAGE_AUTH_MOUNT;
            if (!relevant) continue;
            s->test.stage = stage;
            strlcpy(s->test.detail, "SIMULATED: deterministic service probe; no network request", sizeof(s->test.detail));
            xSemaphoreTake(s_controller.lock, portMAX_DELAY);
            s->busy = true; *s_controller.live = *s;
            xSemaphoreGive(s_controller.lock);
            vTaskDelay(pdMS_TO_TICKS(450));
            if (q->index && stage == UPLOAD_STAGE_AUTH_MOUNT) { s->test.failed_mask |= 1U << stage; break; }
            s->test.completed_mask |= 1U << stage;
        }
        s->test.state = q->index ? UPLOAD_TEST_FAILED : UPLOAD_TEST_PASSED;
        strlcpy(s->test.detail, q->index ? "SIMULATED: credentials rejected" : "SIMULATED: connection probe passed; no network request", sizeof(s->test.detail));
        return ESP_OK;
    case MC_UPLOAD_RETRY: return ESP_OK;
#else
    case MC_WIFI_SCAN: return netprov_scan_request();
    case MC_TIME_SYNC: if (!netprov_is_link_up()) return ESP_ERR_INVALID_STATE; return time_sync_request_now();
    case MC_ALERT_TEST: if (s->recording || !netprov_is_link_up()) return ESP_ERR_INVALID_STATE; return therapy_alert_send_test_push(NULL);
    case MC_UPLOAD_TEST: if (!netprov_is_link_up()) return ESP_ERR_INVALID_STATE; return uploader_test_request(q->value, NULL);
    case MC_UPLOAD_RETRY: return uploader_retry(q->value[0] ? q->value : NULL);
#endif
    default: return ESP_ERR_INVALID_ARG;
    }
}
static void worker(void *arg)
{
    manage_config_snapshot_t *work = arg;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    uploader_set_nvs_executor(nvs_writer_run); therapy_alert_set_nvs_executor(nvs_writer_run);
    struct netprov_config demo;
    if (!netprov_load_config(&demo)) {
        strlcpy(demo.wifi[0].ssid, "QEMU-2.4GHz", 33);
        strlcpy(demo.wifi[1].ssid, "QEMU-fallback", 33);
        strlcpy(demo.wifi[2].ssid, "Saved-workshop", 33);
        netprov_save_config(&demo);
    }
#endif
    while (1) {
        manage_config_request_t q = {0};
        bool got = xQueueReceive(s_controller.queue, &q, pdMS_TO_TICKS(2000)) == pdTRUE;
        observe(work);
        if (got) {
            esp_err_t result = execute(&q, work);
            if (q.kind != MC_REFRESH && q.kind != MC_TIMEZONE_SEARCH && q.kind != MC_ALERT_HISTORY) {
                const char *ok = q.kind == MC_WIFI_RECONNECT ? "Connection attempt completed" :
                    (q.kind == MC_WIFI_SCAN || q.kind == MC_UPLOAD_TEST || q.kind == MC_TIME_SYNC || q.kind == MC_UPLOAD_RETRY) ? "Request accepted; check observed status" :
                    q.kind == MC_ALERT_TEST ? "Push service accepted the test; phone delivery is unverified" :
                    (q.kind >= MC_FTP_ENABLE && q.kind <= MC_FTP_PASSWORD) ? "Saved; FTP changes take effect after restart" :
                    (q.kind <= MC_IP_DNS) ? "Saved; current link stays until reconnect (blocked during recording)" : "Saved";
                snprintf(work->result, sizeof(work->result), "%s%s", work->simulated ? "SIMULATED SERVICES: " : "",
                         result == ESP_OK ? ok : result == ESP_ERR_INVALID_ARG ? "Invalid value; settings were not saved" : result == ESP_ERR_INVALID_STATE ? "Blocked by active recording, unavailable service or configuration" : "Operation failed; success was not confirmed");
            }
            observe(work);
        }
        xSemaphoreTake(s_controller.lock, portMAX_DELAY);
        work->busy = got ? false : s_controller.live->busy;
        work->generation = s_controller.live->generation + 1;
        *s_controller.live = *work;
        xSemaphoreGive(s_controller.lock);
        memset(&q, 0, sizeof(q));
    }
}
esp_err_t touch_manage_config_start(void)
{
    if (s_controller.queue) return ESP_OK;
    s_controller.live = heap_caps_calloc(1, sizeof(*s_controller.live), MALLOC_CAP_SPIRAM);
    s_controller.lock = xSemaphoreCreateMutex();
    s_controller.queue = xQueueCreate(1, sizeof(manage_config_request_t));
    manage_config_snapshot_t *work = heap_caps_calloc(1, sizeof(*work), MALLOC_CAP_SPIRAM);
    if (!s_controller.live || !s_controller.lock || !s_controller.queue || !work ||
        !psram_task_create(worker, "manage_cfg", 12288, work, 2, 0, NULL, NULL)) {
        if (s_controller.queue) vQueueDelete(s_controller.queue);
        if (s_controller.lock) vSemaphoreDelete(s_controller.lock);
        free(s_controller.live); free(work); memset(&s_controller,0,sizeof(s_controller));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t touch_manage_config_submit(const manage_config_request_t *request)
{
    if (!request || !memchr(request->value, 0, sizeof(request->value)) || !s_controller.queue) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_controller.lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_controller.live->busy) { xSemaphoreGive(s_controller.lock); return ESP_ERR_INVALID_STATE; }
    s_controller.live->busy = true;
    bool sent = xQueueSend(s_controller.queue, request, 0) == pdTRUE;
    if (!sent) s_controller.live->busy = false;
    xSemaphoreGive(s_controller.lock);
    return sent ? ESP_OK : ESP_ERR_TIMEOUT;
}
bool touch_manage_config_snapshot(manage_config_snapshot_t *out)
{
    if (!out || !s_controller.lock || xSemaphoreTake(s_controller.lock, 0) != pdTRUE) return false;
    *out = *s_controller.live;
    xSemaphoreGive(s_controller.lock); return true;
}

void touch_manage_config_health(manage_config_health_t *out)
{
    memset(out,0,sizeof(*out));
    if (!s_controller.lock || xSemaphoreTake(s_controller.lock,0) != pdTRUE) return;
    const manage_config_snapshot_t *s=s_controller.live;
    out->ready=s->ready;
    out->alerts_enabled=s->alerts.enabled;
    out->push_configured=s->alerts.push_en && s->alerts.ntfy_topic[0] && s->alerts.ntfy_srv[0];
    out->push_verified=s->verified_at != 0;
    out->history_storage_error=s->history.write_error != ESP_OK;
    for (size_t i=0;i<s->progress.backend_count;i++) {
        const uploader_backend_progress_t *p=&s->progress.backends[i];
        if (p->configured) { out->configured_uploads++; if (p->error_valid) out->failed_uploads++; }
    }
    xSemaphoreGive(s_controller.lock);
}
