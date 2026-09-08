/*
 * SomnoTrace - Therapy-stop alert system with ntfy push and buzzer escalation
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "therapy_alert.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "therapy_alert";

#define NVS_NAMESPACE  "alert"
#define NVS_KEY_CFG    "cfg"

/* ── Injected functions ─────────────────────────────────────────────── */
static alert_beep_fn_t          s_beep_fn  = NULL;
static alert_nvs_exec_fn_t       s_nvs_exec = NULL;
static alert_therapy_active_fn_t s_therapy_active_fn = NULL;

void therapy_alert_set_beep_fn(alert_beep_fn_t fn)    { s_beep_fn = fn; }
void therapy_alert_set_nvs_executor(alert_nvs_exec_fn_t fn) { s_nvs_exec = fn; }
void therapy_alert_set_therapy_active_fn(alert_therapy_active_fn_t fn) { s_therapy_active_fn = fn; }

/* ── Concurrency model ──────────────────────────────────────────────────
 *
 * This component is single-owner: exactly one task (`alert_monitor`) reads
 * and writes the state machine.  Every external event — TherapyStart/Stop,
 * BLE disconnect, button acknowledge, config change, and the escalation
 * routine's own progress — is delivered as a message on s_evt_q and applied
 * by that task.  Callers never mutate state and never block on the alert
 * subsystem, which matters because two of them are a NimBLE host callback
 * and a button monitor.
 *
 * Why this shape rather than a lock: the previous design guarded a single
 * enum with a heap-allocated mutex, which
 *   (a) did not make the read-decide-write sequences in reevaluate_state()
 *       atomic anyway — the lock was released between the read and the
 *       write, so it bought no real mutual exclusion; and
 *   (b) placed a FreeRTOS queue object (with its embedded per-queue
 *       spinlock) on the heap, where a neighbouring task-stack overflow
 *       could corrupt the lock word.  A corrupt spinlock owner value spins
 *       forever with interrupts disabled → INT_WDT reset, which is the
 *       2026-08-09 failure.
 *
 * The event queue, its storage, the config mutex and the owner task's stack
 * are all statically allocated in .bss for the same reason: nothing in this
 * component's synchronisation path is a heap neighbour of any task stack.
 * A single writer also means s_state needs no lock at all — readers take a
 * plain aligned 32-bit load and always observe a whole value. */

/* ── Config ─────────────────────────────────────────────────────────── */
/* s_cfg is written only by the owner task.  Writers stage into s_cfg_staged
 * under s_cfg_mtx and post EVT_CONFIG; the owner promotes it. */
static therapy_alert_config_t s_cfg = ALERT_DEFAULTS;
static therapy_alert_config_t s_cfg_staged = ALERT_DEFAULTS;
static bool s_cfg_loaded = false;

static StaticSemaphore_t s_cfg_mtx_buf;
static SemaphoreHandle_t s_cfg_mtx = NULL;

/* ── State machine (owner task only) ────────────────────────────────── */
static volatile alert_state_t s_state = ALERT_DISARMED;

/* Escalation-routine generation.  Cancelling is a counter bump, not a
 * handle-and-sleep handshake: any routine whose captured generation no
 * longer matches aborts at its next check and its state requests are
 * ignored.  This removes both the 50 ms sleep that used to run inside a BLE
 * callback and the dangling-handle race on s_alert_task_h. */
static volatile uint32_t s_routine_gen = 0;
static bool s_routine_active = false;      /* owner task only */

/* ── Event queue ────────────────────────────────────────────────────── */
typedef enum {
    EVT_TICK = 0,          /* periodic re-evaluation                     */
    EVT_THERAPY_START,
    EVT_THERAPY_STOP,
    EVT_BLE_DISCONNECT,
    EVT_ACK,
    EVT_CONFIG,            /* s_cfg_staged has a new config              */
    EVT_ROUTINE_STATE,     /* escalation routine reports a transition    */
    EVT_ROUTINE_EXIT,      /* escalation routine finished                */
} alert_evt_type_t;

typedef struct {
    uint8_t  type;
    uint8_t  state;        /* EVT_ROUTINE_STATE payload                  */
    uint32_t gen;          /* routine generation, for staleness checks   */
} alert_evt_t;

#define ALERT_EVT_Q_LEN     8
#define ALERT_MONITOR_STACK 4096
#define ALERT_TICK_MS       30000

static StaticQueue_t s_evt_q_buf;
static uint8_t       s_evt_q_storage[ALERT_EVT_Q_LEN * sizeof(alert_evt_t)];
static QueueHandle_t s_evt_q = NULL;

static StaticTask_t  s_monitor_tcb;
static StackType_t  *s_monitor_stack;

/* Forward declarations */
static void cancel_alert_routine(void);
static void reevaluate_state(void);
static void set_state(alert_state_t st);
static void report_stack_headroom(const char *why);

/* Post an event.  Never blocks: callers include a NimBLE host callback and
 * the button monitor, where blocking is not acceptable.  A full queue means
 * the owner is already behind with equivalent work pending, and the periodic
 * tick will reconcile state regardless. */
static void post_evt(alert_evt_type_t type, alert_state_t st, uint32_t gen)
{
    if (!s_evt_q) return;
    alert_evt_t ev = { .type = (uint8_t)type, .state = (uint8_t)st, .gen = gen };
    if (xQueueSend(s_evt_q, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping event %d", (int)type);
    }
}

/* Snapshot the config for a reader outside the owner task. */
static void cfg_snapshot(therapy_alert_config_t *out)
{
    if (!out) return;
    if (s_cfg_mtx && xSemaphoreTake(s_cfg_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_cfg;
        xSemaphoreGive(s_cfg_mtx);
    } else {
        *out = s_cfg;
    }
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static bool time_in_window(int minutes_from_midnight, uint16_t start, uint16_t end)
{
    if (start == end) return true;  /* 24-hour window */
    if (start < end) {
        return minutes_from_midnight >= start && minutes_from_midnight < end;
    }
    /* Wraps midnight: e.g. 23:00 → 06:00 */
    return minutes_from_midnight >= start || minutes_from_midnight < end;
}

static int current_minutes_from_midnight(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

/* Check if the wall clock has been set (not still at epoch 0). */
static bool time_is_set(void)
{
    return time(NULL) > 1700000000;  /* > Nov 2023 */
}

/* Check if therapy is currently active via the injected checker. */
static bool therapy_is_active(void)
{
    return s_therapy_active_fn ? s_therapy_active_fn() : false;
}

/* Check if current time is inside a given config's alert window. */
static bool in_window_cfg(const therapy_alert_config_t *cfg)
{
    if (!time_is_set()) return false;
    int now_min = current_minutes_from_midnight();
    return time_in_window(now_min, cfg->win_start, cfg->win_end);
}

/* Check if current time is inside the configured alert window.
 * Owner-task helper: reads the live config. */
static bool currently_in_window(void)
{
    return in_window_cfg(&s_cfg);
}

const char *therapy_alert_state_str(alert_state_t st)
{
    switch (st) {
        case ALERT_DISARMED:  return "disarmed";
        case ALERT_ARMED:     return "armed";
        case ALERT_PENDING:   return "pending";
        case ALERT_PUSH_SENT: return "push_sent";
        case ALERT_BUZZING:   return "buzzing";
        case ALERT_ACKED:     return "acked";
        default:              return "unknown";
    }
}

/* Lock-free by construction: one writer (the owner task), aligned 32-bit
 * enum, so a reader never sees a partial value. */
alert_state_t therapy_alert_get_state(void)
{
    return s_state;
}

/* Owner task only. */
static void set_state(alert_state_t st)
{
    if (s_state == st) return;
    s_state = st;
    ESP_LOGI(TAG, "state → %s", therapy_alert_state_str(st));
}

/* ── NVS config persistence ─────────────────────────────────────────── */

static esp_err_t do_save_config(void *arg)
{
    const therapy_alert_config_t *cfg = (const therapy_alert_config_t *)arg;
    therapy_alert_config_t local = *cfg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_CFG, &local, sizeof(local));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t do_load_config(void *arg)
{
    therapy_alert_config_t *out = arg;
    therapy_alert_config_t local;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t required = sizeof(local);
    err = nvs_get_blob(h, NVS_KEY_CFG, &local, &required);
    nvs_close(h);
    if (err == ESP_OK) *out = local;
    return err;
}

esp_err_t therapy_alert_load_config(therapy_alert_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Use the injected NVS executor (nvs_writer_run) if available so the
     * read is serialized with all other NVS access.  Fall back to direct
     * access only before the executor is set (early boot, internal stack). */
    esp_err_t err;
    if (s_nvs_exec) {
        err = s_nvs_exec(do_load_config, cfg);
    } else {
        err = do_load_config(cfg);
    }

    if (err != ESP_OK) {
        *cfg = (therapy_alert_config_t)ALERT_DEFAULTS;
    } else {
        s_cfg_loaded = true;
    }
    return err;
}

esp_err_t therapy_alert_save_config_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return ESP_ERR_INVALID_ARG;

    therapy_alert_config_t cfg;
    cfg_snapshot(&cfg);
    cJSON *j;

    if ((j = cJSON_GetObjectItem(root, "enabled")))    cfg.enabled = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(root, "win_start")))  cfg.win_start = (uint16_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "win_end")))    cfg.win_end = (uint16_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "delay1")))     cfg.delay1 = (uint16_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "push_en")))    cfg.push_en = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(root, "ntfy_srv")))   { const char *s = cJSON_GetStringValue(j); if (s) strlcpy(cfg.ntfy_srv, s, sizeof(cfg.ntfy_srv)); }
    if ((j = cJSON_GetObjectItem(root, "ntfy_topic"))) { const char *s = cJSON_GetStringValue(j); if (s) strlcpy(cfg.ntfy_topic, s, sizeof(cfg.ntfy_topic)); }
    if ((j = cJSON_GetObjectItem(root, "ntfy_prio")))  cfg.ntfy_prio = (uint8_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "delay2")))     cfg.delay2 = (uint16_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "buzz_en")))    cfg.buzz_en = cJSON_IsTrue(j);

    cJSON_Delete(root);

    /* Persist via injected NVS executor (safe from PSRAM-stack httpd). */
    esp_err_t err = s_nvs_exec ? s_nvs_exec(do_save_config, &cfg)
                               : do_save_config(&cfg);
    if (err == ESP_OK) {
        /* Stage the new config and let the owner task promote it, so the
         * live config is never written from an httpd worker while the state
         * machine is deciding on it. */
        if (s_cfg_mtx && xSemaphoreTake(s_cfg_mtx, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_cfg_staged = cfg;
            xSemaphoreGive(s_cfg_mtx);
        } else {
            s_cfg_staged = cfg;
        }
        ESP_LOGI(TAG, "config saved: en=%d win=%d-%d d1=%d push=%d buzz=%d",
                 cfg.enabled, cfg.win_start, cfg.win_end, cfg.delay1,
                 cfg.push_en, cfg.buzz_en);
        /* Re-evaluate state on the owner task: a schedule change may
         * arm or disarm. */
        post_evt(EVT_CONFIG, 0, 0);
    }
    return err;
}

esp_err_t therapy_alert_get_config_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    therapy_alert_config_t cfg;
    cfg_snapshot(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    cJSON_AddNumberToObject(root, "win_start", cfg.win_start);
    cJSON_AddNumberToObject(root, "win_end", cfg.win_end);
    cJSON_AddNumberToObject(root, "delay1", cfg.delay1);
    cJSON_AddBoolToObject(root, "push_en", cfg.push_en);
    cJSON_AddStringToObject(root, "ntfy_srv", cfg.ntfy_srv);
    cJSON_AddStringToObject(root, "ntfy_topic", cfg.ntfy_topic);
    cJSON_AddNumberToObject(root, "ntfy_prio", cfg.ntfy_prio);
    cJSON_AddNumberToObject(root, "delay2", cfg.delay2);
    cJSON_AddBoolToObject(root, "buzz_en", cfg.buzz_en);

    cJSON *st = cJSON_AddObjectToObject(root, "state");
    cJSON_AddStringToObject(st, "alert", therapy_alert_state_str(therapy_alert_get_state()));

    *out_json = cJSON_Print(root);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── ntfy push notification ─────────────────────────────────────────── */

static esp_err_t send_ntfy_push(const char *srv, const char *topic,
                                const char *title, const char *body,
                                uint8_t priority)
{
    if (!srv || !topic || !topic[0]) {
        ESP_LOGW(TAG, "ntfy: topic empty, skipping push");
        return ESP_ERR_INVALID_ARG;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/%s", srv, topic);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    char prio_hdr[8];
    snprintf(prio_hdr, sizeof(prio_hdr), "%d", priority);
    esp_http_client_set_header(client, "Title", title);
    esp_http_client_set_header(client, "Priority", prio_hdr);
    esp_http_client_set_header(client, "Tags", "warning");

    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "ntfy push: HTTP %d", status);
        if (status < 200 || status >= 300) err = ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "ntfy push failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t therapy_alert_send_test_push(const char *json_override)
{
    therapy_alert_config_t cfg;
    cfg_snapshot(&cfg);

    if (json_override && json_override[0]) {
        cJSON *root = cJSON_Parse(json_override);
        if (root) {
            cJSON *j;
            if ((j = cJSON_GetObjectItem(root, "push_en")))    cfg.push_en = cJSON_IsTrue(j);
            if ((j = cJSON_GetObjectItem(root, "ntfy_srv")))   { const char *s = cJSON_GetStringValue(j); if (s) strlcpy(cfg.ntfy_srv, s, sizeof(cfg.ntfy_srv)); }
            if ((j = cJSON_GetObjectItem(root, "ntfy_topic"))) { const char *s = cJSON_GetStringValue(j); if (s) strlcpy(cfg.ntfy_topic, s, sizeof(cfg.ntfy_topic)); }
            if ((j = cJSON_GetObjectItem(root, "ntfy_prio")))  cfg.ntfy_prio = (uint8_t)j->valuedouble;
            cJSON_Delete(root);
        }
    }

    if (!cfg.push_en || !cfg.ntfy_topic[0]) {
        ESP_LOGW(TAG, "test push: push disabled or topic empty");
        return ESP_ERR_INVALID_STATE;
    }
    return send_ntfy_push(cfg.ntfy_srv, cfg.ntfy_topic,
                          "SomnoTrace test", "Test notification from SomnoTrace",
                          cfg.ntfy_prio);
}

/* ── Buzzer ─────────────────────────────────────────────────────────── */

/* True once this routine generation has been superseded or cancelled. */
static inline bool routine_stale(uint32_t gen)
{
    return s_routine_gen != gen;
}

static void run_buzzer(uint32_t gen)
{
    if (!s_beep_fn) {
        ESP_LOGW(TAG, "buzzer: no beep function injected");
        return;
    }
    for (int i = 0; i < 5; i++) {
        if (routine_stale(gen)) break;
        s_beep_fn(880, 1000, 100);
        if (routine_stale(gen)) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── Alert routine task ─────────────────────────────────────────────── */

/* The escalation routine never writes state directly: it requests
 * transitions from the owner task, tagged with its own generation so a
 * superseded routine cannot resurrect a stale state. */
static void routine_request_state(uint32_t gen, alert_state_t st)
{
    post_evt(EVT_ROUTINE_STATE, st, gen);
}

static void alert_routine_task(void *arg)
{
    const uint32_t gen = (uint32_t)(uintptr_t)arg;

    /* Work from a snapshot: the config can be replaced mid-routine by an
     * httpd worker, and re-reading it field by field across a multi-minute
     * escalation would mix old and new settings. */
    therapy_alert_config_t cfg;
    cfg_snapshot(&cfg);

    /* Phase 1: wait delay1 minutes, then send push (if enabled) */
    int delay1_ms = cfg.delay1 * 60 * 1000;
    ESP_LOGI(TAG, "alert routine: waiting %d ms before push/buzzer", delay1_ms);

    for (int waited = 0; waited < delay1_ms; waited += 1000) {
        if (routine_stale(gen)) goto done;
        if (!cfg.enabled || !in_window_cfg(&cfg)) {
            ESP_LOGI(TAG, "alert routine: disabled or outside window during delay1 — aborting");
            routine_request_state(gen, ALERT_DISARMED);
            goto done;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (routine_stale(gen)) goto done;

    /* Check if still in PENDING state (could have been ACKED or disarmed) */
    if (therapy_alert_get_state() != ALERT_PENDING) goto done;

    /* Safety: don't send push if we've exited the window during delay1 */
    if (!in_window_cfg(&cfg)) {
        ESP_LOGI(TAG, "alert routine: outside window after delay1 — aborting before push");
        routine_request_state(gen, ALERT_DISARMED);
        goto done;
    }

    /* Phase 2: send push notification (if enabled) */
    if (cfg.push_en && cfg.ntfy_topic[0]) {
        char body[64];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(body, sizeof(body), "Therapy stopped at %02d:%02d",
                 tm.tm_hour, tm.tm_min);

        routine_request_state(gen, ALERT_PUSH_SENT);
        esp_err_t err = send_ntfy_push(cfg.ntfy_srv, cfg.ntfy_topic,
                                       "SomnoTrace Alert", body,
                                       cfg.ntfy_prio);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "push failed, falling through to buzzer");
        }
    }

    if (routine_stale(gen)) goto done;

    /* Phase 3: wait delay2 minutes before buzzer (only if both push+buzz) */
    if (cfg.push_en && cfg.buzz_en && cfg.delay2 > 0) {
        int delay2_ms = cfg.delay2 * 60 * 1000;
        ESP_LOGI(TAG, "alert routine: waiting %d ms before buzzer", delay2_ms);
        for (int waited = 0; waited < delay2_ms; waited += 1000) {
            if (routine_stale(gen)) goto done;
            if (!cfg.enabled || !in_window_cfg(&cfg)) {
                ESP_LOGI(TAG, "alert routine: disabled or outside window during delay2 — aborting");
                routine_request_state(gen, ALERT_DISARMED);
                goto done;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (routine_stale(gen)) goto done;

    /* Safety: don't buzz if we've exited the window */
    if (!in_window_cfg(&cfg)) {
        ESP_LOGI(TAG, "alert routine: outside window before buzzer — aborting");
        routine_request_state(gen, ALERT_DISARMED);
        goto done;
    }

    /* Phase 4: buzzer (if enabled) */
    if (cfg.buzz_en) {
        routine_request_state(gen, ALERT_BUZZING);
        run_buzzer(gen);
    }

done:
    {
        UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        ESP_LOGI(TAG, "alert routine exiting (state=%s, stack headroom %u bytes)",
                 therapy_alert_state_str(therapy_alert_get_state()),
                 (unsigned)free_bytes);
        if (free_bytes < 1024) {
            ESP_LOGW(TAG, "alert routine: LOW STACK — %u bytes free at peak",
                     (unsigned)free_bytes);
        }
    }
    post_evt(EVT_ROUTINE_EXIT, 0, gen);
    vTaskDelete(NULL);
}

/* Owner task only. */
static void start_alert_routine(void)
{
    /* Bumping the generation cancels any routine still winding down; it will
     * observe the mismatch and exit on its own.  No handle is stored and no
     * sleep is needed, so there is nothing to dangle. */
    uint32_t gen = ++s_routine_gen;
    set_state(ALERT_PENDING);

    TaskHandle_t h = NULL;
    StackType_t *stack = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb  = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (stack && tcb) {
        h = xTaskCreateStaticPinnedToCore(alert_routine_task, "alert_routine",
                                          16384, (void *)(uintptr_t)gen, 5,
                                          stack, tcb, 0);
    }
    if (!h) {
        /* Without the routine there will be no push and no buzzer, so say so
         * rather than sitting silently in PENDING for ever. */
        ESP_LOGE(TAG, "failed to create alert routine task — disarming");
        set_state(ALERT_DISARMED);
        return;
    }
    s_routine_active = true;
}

/* Owner task only. */
static void cancel_alert_routine(void)
{
    s_routine_gen++;
    s_routine_active = false;
}

/* ── State re-evaluation ────────────────────────────────────────────── */

/* Re-evaluate the alert state based on current conditions.
 * Called by the periodic monitor task and after config changes.
 * Does NOT re-arm from ACKED — only a new TherapyStart can do that. */
static void reevaluate_state(void)
{
    if (!s_cfg.enabled || !time_is_set()) {
        alert_state_t st = therapy_alert_get_state();
        if (st != ALERT_DISARMED && st != ALERT_ACKED) {
            ESP_LOGI(TAG, "reevaluate: alerts disabled or clock unset — disarming");
            cancel_alert_routine();
            set_state(ALERT_DISARMED);
        }
        return;
    }

    bool in_win = currently_in_window();
    bool therapy_on = therapy_is_active();
    alert_state_t st = therapy_alert_get_state();

    if (in_win && therapy_on && st == ALERT_DISARMED) {
        /* Window open + therapy active + not yet armed → ARM */
        set_state(ALERT_ARMED);
        ESP_LOGI(TAG, "reevaluate: therapy active inside window — armed");
    } else if (!in_win && (st == ALERT_ARMED || st == ALERT_PENDING ||
                            st == ALERT_PUSH_SENT || st == ALERT_BUZZING)) {
        /* Window closed → disarm and cancel any running routine.
         * This is the deepest path in the component and the one implicated
         * in both INT_WDT events, so measure the stack here. */
        ESP_LOGI(TAG, "reevaluate: outside window — disarming (was %s)",
                 therapy_alert_state_str(st));
        cancel_alert_routine();
        set_state(ALERT_DISARMED);
        report_stack_headroom("window close");
    }
}

/* Report the owner task's own stack headroom.
 *
 * The 2026-08-09 investigation could not answer "was this stack too small?"
 * because nothing ever measured it.  Logging the high-water mark on the
 * window-close branch (the deepest path in this component) and once an hour
 * turns that into an observable number in ordinary field logs. */
static void report_stack_headroom(const char *why)
{
    UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    if (free_bytes < 1024) {
        ESP_LOGW(TAG, "alert_monitor: LOW STACK — %u bytes free at peak (%s)",
                 (unsigned)free_bytes, why);
    } else {
        ESP_LOGI(TAG, "alert_monitor: peak stack headroom %u bytes (%s)",
                 (unsigned)free_bytes, why);
    }
}

/* Promote a staged config written by an httpd worker. */
static void promote_staged_config(void)
{
    if (s_cfg_mtx && xSemaphoreTake(s_cfg_mtx, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_cfg = s_cfg_staged;
        xSemaphoreGive(s_cfg_mtx);
    } else {
        s_cfg = s_cfg_staged;
    }
}

/* ── Event handlers (owner task only) ───────────────────────────────── */

static void handle_therapy_start(void)
{
    if (!s_cfg.enabled) return;
    if (!time_is_set()) return;

    int now_min = current_minutes_from_midnight();
    if (time_in_window(now_min, s_cfg.win_start, s_cfg.win_end)) {
        cancel_alert_routine();
        set_state(ALERT_ARMED);
        ESP_LOGI(TAG, "therapy started inside window (%02d:%02d) — armed",
                 now_min / 60, now_min % 60);
    } else {
        ESP_LOGD(TAG, "therapy started outside window (%02d:%02d) — not armed",
                 now_min / 60, now_min % 60);
    }
}

static void handle_therapy_stop(void)
{
    if (!s_cfg.enabled) return;

    alert_state_t st = s_state;
    if (st == ALERT_ARMED) {
        if (!currently_in_window()) {
            ESP_LOGI(TAG, "therapy stop outside window — disarming (no alert)");
            set_state(ALERT_DISARMED);
            return;
        }
        ESP_LOGI(TAG, "therapy stop while armed inside window — starting alert routine");
        start_alert_routine();
    } else {
        ESP_LOGD(TAG, "therapy stop while %s — ignoring",
                 therapy_alert_state_str(st));
    }
}

static void handle_ble_disconnect(void)
{
    if (!s_cfg.enabled) return;

    alert_state_t st = s_state;
    if (st != ALERT_DISARMED) {
        ESP_LOGI(TAG, "BLE disconnect — disarming (was %s)",
                 therapy_alert_state_str(st));
        cancel_alert_routine();
        set_state(ALERT_DISARMED);
    }
}

static void handle_ack(void)
{
    alert_state_t st = s_state;
    if (st == ALERT_DISARMED || st == ALERT_ACKED) return;

    ESP_LOGI(TAG, "acknowledged (was %s)", therapy_alert_state_str(st));
    cancel_alert_routine();
    set_state(ALERT_ACKED);
}

/* ── Owner task ─────────────────────────────────────────────────────── */

/* The single writer of the state machine.  Blocks on the event queue with a
 * tick timeout, so it reacts immediately to events and still reconciles
 * against the wall clock every ALERT_TICK_MS. */
static void alert_owner_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "alert monitor started on core %d", xPortGetCoreID());

    /* Once an hour at a 30 s tick. */
    const int report_every = 3600000 / ALERT_TICK_MS;
    int ticks = 0;

    for (;;) {
        alert_evt_t ev;
        if (xQueueReceive(s_evt_q, &ev, pdMS_TO_TICKS(ALERT_TICK_MS)) != pdTRUE) {
            reevaluate_state();
            if (++ticks >= report_every) {
                ticks = 0;
                report_stack_headroom("hourly");
            }
            continue;
        }

        switch ((alert_evt_type_t)ev.type) {
        case EVT_TICK:
            reevaluate_state();
            break;
        case EVT_THERAPY_START:
            handle_therapy_start();
            break;
        case EVT_THERAPY_STOP:
            handle_therapy_stop();
            break;
        case EVT_BLE_DISCONNECT:
            handle_ble_disconnect();
            break;
        case EVT_ACK:
            handle_ack();
            break;
        case EVT_CONFIG:
            promote_staged_config();
            reevaluate_state();
            break;
        case EVT_ROUTINE_STATE:
            /* Ignore requests from a superseded routine. */
            if (ev.gen == s_routine_gen) set_state((alert_state_t)ev.state);
            break;
        case EVT_ROUTINE_EXIT:
            if (ev.gen == s_routine_gen) s_routine_active = false;
            break;
        default:
            break;
        }
    }
}

/* ── Event hooks ────────────────────────────────────────────────────── */

/* All four hooks are thin, non-blocking producers.  Two of them run in
 * contexts where blocking is unacceptable: on_ble_disconnect() is called
 * from a NimBLE host callback and acknowledge() from the button monitor.
 * The previous implementation called vTaskDelay() from both via
 * cancel_alert_routine(); now they only enqueue. */

void therapy_alert_on_therapy_start(void)
{
    post_evt(EVT_THERAPY_START, 0, 0);
}

void therapy_alert_on_therapy_stop(void)
{
    post_evt(EVT_THERAPY_STOP, 0, 0);
}

void therapy_alert_on_ble_disconnect(void)
{
    post_evt(EVT_BLE_DISCONNECT, 0, 0);
}

void therapy_alert_acknowledge(void)
{
    post_evt(EVT_ACK, 0, 0);
}

/* ── Init ───────────────────────────────────────────────────────────── */

esp_err_t therapy_alert_init(void)
{
    /* Load config from NVS */
    esp_err_t err = therapy_alert_load_config(&s_cfg);
    if (err == ESP_OK) {
        s_cfg_loaded = true;
        ESP_LOGI(TAG, "config loaded: en=%d win=%d-%d d1=%d push=%d buzz=%d",
                 s_cfg.enabled, s_cfg.win_start, s_cfg.win_end,
                 s_cfg.delay1, s_cfg.push_en, s_cfg.buzz_en);
    } else {
        ESP_LOGI(TAG, "no saved config, using defaults");
    }

    s_cfg_staged = s_cfg;
    s_state = ALERT_DISARMED;

    /* The owner task's stack lives in PSRAM to save internal RAM.  The event
     * queue and its storage remain in .bss, so no synchronisation object in
     * this component can be the heap neighbour of a task stack — the PSRAM
     * stack is in a completely separate memory region. */
    s_cfg_mtx = xSemaphoreCreateMutexStatic(&s_cfg_mtx_buf);
    if (!s_cfg_mtx) return ESP_ERR_NO_MEM;

    s_evt_q = xQueueCreateStatic(ALERT_EVT_Q_LEN, sizeof(alert_evt_t),
                                 s_evt_q_storage, &s_evt_q_buf);
    if (!s_evt_q) return ESP_ERR_NO_MEM;

    /* Start the owner task with a PSRAM-allocated stack.  A failure here
     * means no alerts at all, so it is reported instead of being discarded
     * as it was before. */
    s_monitor_stack = heap_caps_malloc(ALERT_MONITOR_STACK, MALLOC_CAP_SPIRAM);
    if (!s_monitor_stack) {
        ESP_LOGE(TAG, "failed to allocate alert monitor stack in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
            alert_owner_task, "alert_monitor",
            ALERT_MONITOR_STACK / sizeof(StackType_t), NULL, 2,
            s_monitor_stack, &s_monitor_tcb, 0);
    if (!h) {
        ESP_LOGE(TAG, "failed to create alert monitor task — alerts disabled");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
