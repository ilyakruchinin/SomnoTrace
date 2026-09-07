/*
 * SomnoTrace - first-run setup service controller
 */

#include "first_run_setup_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "as11_ble.h"
#include "bsp_display.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_provision.h"
#include "psram_task.h"
#include "sdkconfig.h"
#include "sd_storage.h"
#include "session_writer.h"
#include "therapy_alert.h"
#include "time_sync.h"
#include "timezone_catalog.h"
#include "uploader.h"

static const char *TAG = "setup_control";

#define SETUP_WORKER_STACK_BYTES 8192U
#define SETUP_CARD_FULL_BYTES    (8ULL * 1024ULL * 1024ULL)
#define SETUP_NIGHT_BYTES        (80ULL * 1024ULL * 1024ULL)
#define SETUP_WIFI_SCAN_POLLS    160U
#define SETUP_PAIR_POLLS         120U

typedef enum {
    SETUP_OP_NONE = 0,
    SETUP_OP_WIFI_SCAN,
    SETUP_OP_WIFI_CONNECT,
    SETUP_OP_TIMEZONE_SEARCH,
    SETUP_OP_TIMEZONE_SELECT,
    SETUP_OP_TIME_ADVANCED,
    SETUP_OP_AIRSENSE_SCAN,
    SETUP_OP_AIRSENSE_BEGIN,
    SETUP_OP_AIRSENSE_CONFIRM,
    SETUP_OP_CARD_RETRY,
    SETUP_OP_ALERTS_DEFAULT,
    SETUP_OP_UPLOADS_PRIVATE,
    SETUP_OP_STOP,
} setup_operation_kind_t;

typedef struct {
    setup_operation_kind_t kind;
    char first[65];
    char second[65];
} setup_operation_t;

typedef struct {
    SemaphoreHandle_t mutex;
    QueueHandle_t queue;
    TaskHandle_t worker;
    first_run_setup_ui_live_t live;
    uint32_t generation;
    bool active;
    bool operation_busy;
    bool finished_requested;
    uint32_t references;
    esp_err_t initial_card_result;
    first_run_setup_ui_controller_t callbacks;
} setup_controller_t;

static setup_controller_t *s_controller;
/* A detached controller reclaims itself on its worker after STOP is
 * observed. Keep restart closed until that reclamation has completed so two
 * setup service workers can never overlap. */
static bool s_controller_stopping;
static portMUX_TYPE s_controller_lock = portMUX_INITIALIZER_UNLOCKED;

static setup_controller_t *controller_acquire(void)
{
    portENTER_CRITICAL(&s_controller_lock);
    setup_controller_t *controller = s_controller;
    if (controller) controller->references++;
    portEXIT_CRITICAL(&s_controller_lock);
    return controller;
}

static void controller_release(setup_controller_t *controller)
{
    if (!controller) return;
    portENTER_CRITICAL(&s_controller_lock);
    if (controller->references > 0) controller->references--;
    portEXIT_CRITICAL(&s_controller_lock);
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0) return;
    strlcpy(destination, source ? source : "", capacity);
}

static void state_lock(setup_controller_t *controller)
{
    xSemaphoreTake(controller->mutex, portMAX_DELAY);
}

static void state_unlock(setup_controller_t *controller)
{
    xSemaphoreGive(controller->mutex);
}

static uint8_t configured_wifi_slots(const struct netprov_config *config)
{
    uint8_t used = 0;
    if (!config) return 0;
    for (unsigned i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
        if (config->wifi[i].ssid[0]) used++;
    }
    return used;
}

#if !CONFIG_SOMNOTRACE_BOARD_QEMU
static uint16_t estimated_card_nights(uint64_t free_bytes)
{
    uint64_t nights = free_bytes / SETUP_NIGHT_BYTES;
    if (nights >= 100) nights = ((nights + 5) / 10) * 10;
    else if (nights >= 20) nights = ((nights + 2) / 5) * 5;
    if (nights > UINT16_MAX) nights = UINT16_MAX;
    return (uint16_t)nights;
}
#endif

static bool valid_hostname(const char *hostname)
{
    if (!hostname) return false;
    size_t length = strlen(hostname);
    if (length == 0 || length > 10 || hostname[0] == '-' ||
        hostname[length - 1] == '-') return false;
    for (size_t i = 0; i < length; i++) {
        char c = hostname[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-')) return false;
    }
    return true;
}

static void publish_error(setup_controller_t *controller, const char *message)
{
    state_lock(controller);
    copy_text(controller->live.error_message,
              sizeof(controller->live.error_message), message);
    controller->generation++;
    state_unlock(controller);
}

static void publish_card_state(setup_controller_t *controller,
                               esp_err_t probe_result)
{
    first_run_setup_ui_card_state_t state;
    char summary[48] = {0};
    char error[96] = {0};
    uint16_t estimated_nights = 0;
    bool estimate_available = false;

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    (void)probe_result;
    state = FIRST_RUN_SETUP_UI_CARD_READY;
    copy_text(summary, sizeof(summary), "29.8 GB free · simulated");
    estimated_nights = 380;
    estimate_available = true;
#else
    if (probe_result != ESP_OK || !sd_storage_is_ready()) {
        state = probe_result == ESP_ERR_NOT_FOUND
                    ? FIRST_RUN_SETUP_UI_CARD_MISSING
                    : FIRST_RUN_SETUP_UI_CARD_UNREADABLE;
        if (state == FIRST_RUN_SETUP_UI_CARD_UNREADABLE) {
            snprintf(error, sizeof(error), "Card check failed: %s",
                     esp_err_to_name(probe_result));
        }
    } else {
        uint64_t free_bytes = 0;
        uint64_t total_bytes = 0;
        esp_err_t capacity = sd_storage_get_free(&free_bytes, &total_bytes);
        if (capacity != ESP_OK) {
            state = FIRST_RUN_SETUP_UI_CARD_UNREADABLE;
            snprintf(error, sizeof(error), "Capacity check failed: %s",
                     esp_err_to_name(capacity));
        } else if (free_bytes < SETUP_CARD_FULL_BYTES) {
            state = FIRST_RUN_SETUP_UI_CARD_FULL;
            snprintf(error, sizeof(error), "%llu MB free",
                     (unsigned long long)(free_bytes / (1024ULL * 1024ULL)));
        } else {
            state = FIRST_RUN_SETUP_UI_CARD_READY;
            estimated_nights = estimated_card_nights(free_bytes);
            estimate_available = true;
            if (free_bytes >= 1024ULL * 1024ULL * 1024ULL) {
                const uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
                snprintf(summary, sizeof(summary), "%llu.%llu GB free",
                         (unsigned long long)(free_bytes / gib),
                         (unsigned long long)(((free_bytes % gib) * 10ULL) /
                                              gib));
            } else {
                snprintf(summary, sizeof(summary), "%llu MB free",
                         (unsigned long long)(free_bytes /
                                              (1024ULL * 1024ULL)));
            }
        }
    }
#endif

    state_lock(controller);
    controller->live.card_state = state;
    copy_text(controller->live.card_summary,
              sizeof(controller->live.card_summary), summary);
    copy_text(controller->live.error_message,
              sizeof(controller->live.error_message), error);
    controller->live.card_estimated_nights = estimated_nights;
    controller->live.card_estimate_available = estimate_available;
    controller->generation++;
    state_unlock(controller);
}

static void publish_paired_info(setup_controller_t *controller)
{
    char name[32] = "AirSense 11";
    char address[24] = {0};
    char client_id[48] = {0};
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    copy_text(name, sizeof(name), "AirSense 11 · simulated");
    copy_text(address, sizeof(address), "AA:11:00:00:00:01");
    copy_text(client_id, sizeof(client_id), "simulated-client-id");
#else
    cJSON *paired = as11_ble_get_paired_info();
    if (paired) {
        const cJSON *json_name = cJSON_GetObjectItemCaseSensitive(paired,
                                                                  "name");
        const cJSON *json_address = cJSON_GetObjectItemCaseSensitive(paired,
                                                                     "addr");
        const cJSON *json_client_id = cJSON_GetObjectItemCaseSensitive(
            paired, "clientId");
        if (cJSON_IsString(json_name))
            copy_text(name, sizeof(name), json_name->valuestring);
        if (cJSON_IsString(json_address))
            copy_text(address, sizeof(address), json_address->valuestring);
        if (cJSON_IsString(json_client_id))
            copy_text(client_id, sizeof(client_id),
                      json_client_id->valuestring);
        cJSON_Delete(paired);
    }
#endif
    state_lock(controller);
    controller->live.airsense_paired = true;
    controller->live.airsense_state =
        FIRST_RUN_SETUP_UI_AIRSENSE_PAIRED;
    copy_text(controller->live.paired_name,
              sizeof(controller->live.paired_name), name);
    copy_text(controller->live.paired_address,
              sizeof(controller->live.paired_address), address);
    copy_text(controller->live.paired_client_id,
              sizeof(controller->live.paired_client_id), client_id);
    controller->live.paired_model_available = false;
    controller->live.paired_serial_available = false;
    controller->live.paired_firmware_available = false;
    controller->live.error_message[0] = '\0';
    controller->generation++;
    state_unlock(controller);
}

static void refresh_observed(setup_controller_t *controller)
{
    first_run_setup_ui_live_t observed;
    state_lock(controller);
    observed = controller->live;
    state_unlock(controller);

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    /* QEMU state changes are published by deterministic operations below. */
#else
    netprov_link_t link;
    netprov_get_link(&link);
    bool paired = as11_ble_is_paired();
    if (link.up) {
        observed.wifi_configured = true;
        observed.wifi_state = FIRST_RUN_SETUP_UI_WIFI_CONNECTED;
        copy_text(observed.connected_ssid, sizeof(observed.connected_ssid),
                  link.ssid);
    }
    if (paired) {
        observed.airsense_paired = true;
        observed.airsense_state = FIRST_RUN_SETUP_UI_AIRSENSE_PAIRED;
    }
    if (time_sync_is_synced()) {
        copy_text(observed.time_sync_status,
                  sizeof(observed.time_sync_status), "Synchronized by NTP");
    } else if (time_is_usable()) {
        copy_text(observed.time_sync_status,
                  sizeof(observed.time_sync_status),
                  "Clock available · NTP pending");
    } else {
        copy_text(observed.time_sync_status,
                  sizeof(observed.time_sync_status), "Waiting for time sync");
    }
#endif

    if (observed.time_configured) {
        time_t now = time(NULL);
        if (now > 1609459200) {
            struct tm local;
            if (localtime_r(&now, &local)) {
                strftime(observed.local_time, sizeof(observed.local_time),
                         "%H:%M local", &local);
            }
        } else {
            copy_text(observed.local_time, sizeof(observed.local_time),
                      "Time zone saved");
        }
    }

    state_lock(controller);
    if (memcmp(&controller->live, &observed, sizeof(observed)) != 0) {
        controller->live = observed;
        controller->generation++;
    }
    state_unlock(controller);

#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    if (paired && (!observed.paired_address[0] ||
                   !observed.paired_name[0])) {
        publish_paired_info(controller);
    }
#endif
}

static esp_err_t queue_operation(setup_controller_t *controller,
                                 setup_operation_kind_t kind,
                                 const char *first, const char *second)
{
    if (!controller || !controller->active) return ESP_ERR_INVALID_STATE;
    setup_operation_t operation = { .kind = kind };
    copy_text(operation.first, sizeof(operation.first), first);
    copy_text(operation.second, sizeof(operation.second), second);

    state_lock(controller);
    if (controller->operation_busy) {
        state_unlock(controller);
        return ESP_ERR_INVALID_STATE;
    }
    controller->operation_busy = true;
    controller->live.error_message[0] = '\0';
    switch (kind) {
        case SETUP_OP_WIFI_SCAN:
            controller->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_SCANNING;
            controller->live.wifi_result_count = 0;
            controller->live.wifi_scan_blocked = false;
            controller->live.wifi_scan_blocked_reason[0] = '\0';
            break;
        case SETUP_OP_WIFI_CONNECT:
            controller->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_CONNECTING;
            copy_text(controller->live.selected_ssid,
                      sizeof(controller->live.selected_ssid), first);
            break;
        case SETUP_OP_TIMEZONE_SEARCH:
            controller->live.time_state = FIRST_RUN_SETUP_UI_TIME_SEARCHING;
            controller->live.timezone_result_count = 0;
            break;
        case SETUP_OP_TIMEZONE_SELECT:
            controller->live.time_state = FIRST_RUN_SETUP_UI_TIME_APPLYING;
            copy_text(controller->live.timezone_id,
                      sizeof(controller->live.timezone_id), first);
            break;
        case SETUP_OP_TIME_ADVANCED:
            controller->live.time_state = FIRST_RUN_SETUP_UI_TIME_ADVANCED;
            break;
        case SETUP_OP_AIRSENSE_SCAN:
            controller->live.airsense_state =
                FIRST_RUN_SETUP_UI_AIRSENSE_SCANNING;
            controller->live.airsense_result_count = 0;
            break;
        case SETUP_OP_AIRSENSE_BEGIN:
            controller->live.airsense_state =
                FIRST_RUN_SETUP_UI_AIRSENSE_STARTING;
            copy_text(controller->live.selected_airsense_address,
                      sizeof(controller->live.selected_airsense_address),
                      first);
            break;
        case SETUP_OP_AIRSENSE_CONFIRM:
            controller->live.airsense_state =
                FIRST_RUN_SETUP_UI_AIRSENSE_CONFIRMING;
            break;
        case SETUP_OP_CARD_RETRY:
            controller->live.card_state = FIRST_RUN_SETUP_UI_CARD_CHECKING;
            break;
        default:
            break;
    }
    controller->generation++;
    state_unlock(controller);

    if (xQueueSend(controller->queue, &operation, 0) != pdTRUE) {
        state_lock(controller);
        controller->operation_busy = false;
        state_unlock(controller);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t wifi_scan_callback(void *context)
{
    return queue_operation(context, SETUP_OP_WIFI_SCAN, NULL, NULL);
}

static esp_err_t wifi_connect_callback(void *context, const char *ssid,
                                       const char *password)
{
    if (!ssid || !ssid[0] || !password) return ESP_ERR_INVALID_ARG;
    return queue_operation(context, SETUP_OP_WIFI_CONNECT, ssid, password);
}

static esp_err_t timezone_search_callback(void *context, const char *query)
{
    if (!query || !query[0]) return ESP_ERR_INVALID_ARG;
    return queue_operation(context, SETUP_OP_TIMEZONE_SEARCH, query, NULL);
}

static esp_err_t timezone_select_callback(void *context, const char *iana_id,
                                          const char *posix_tz)
{
    if (!iana_id || !iana_id[0] || !posix_tz || !posix_tz[0])
        return ESP_ERR_INVALID_ARG;
    return queue_operation(context, SETUP_OP_TIMEZONE_SELECT,
                           iana_id, posix_tz);
}

static esp_err_t time_advanced_callback(void *context, const char *ntp_server,
                                        const char *hostname)
{
    if (!ntp_server || strlen(ntp_server) >= 64 ||
        !valid_hostname(hostname)) return ESP_ERR_INVALID_ARG;
    return queue_operation(context, SETUP_OP_TIME_ADVANCED,
                           ntp_server, hostname);
}

static esp_err_t airsense_scan_callback(void *context)
{
    return queue_operation(context, SETUP_OP_AIRSENSE_SCAN, NULL, NULL);
}

static esp_err_t airsense_begin_callback(void *context, const char *address)
{
    if (!address || !address[0]) return ESP_ERR_INVALID_ARG;
    return queue_operation(context, SETUP_OP_AIRSENSE_BEGIN, address, NULL);
}

static esp_err_t airsense_confirm_callback(void *context,
                                           const char four_digit_code[5])
{
    if (!four_digit_code || strlen(four_digit_code) != 4)
        return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < 4; i++) {
        if (four_digit_code[i] < '0' || four_digit_code[i] > '9')
            return ESP_ERR_INVALID_ARG;
    }
    return queue_operation(context, SETUP_OP_AIRSENSE_CONFIRM,
                           four_digit_code, NULL);
}

static esp_err_t card_retry_callback(void *context)
{
    return queue_operation(context, SETUP_OP_CARD_RETRY, NULL, NULL);
}

static esp_err_t alerts_callback(void *context)
{
    return queue_operation(context, SETUP_OP_ALERTS_DEFAULT, NULL, NULL);
}

static esp_err_t uploads_callback(void *context)
{
    return queue_operation(context, SETUP_OP_UPLOADS_PRIVATE, NULL, NULL);
}

static esp_err_t finished_callback(void *context)
{
    setup_controller_t *controller = context;
    if (!controller || !controller->active) return ESP_ERR_INVALID_STATE;
    state_lock(controller);
    controller->finished_requested = true;
    state_unlock(controller);
    return ESP_OK;
}

static void do_wifi_scan(setup_controller_t *controller)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    static const first_run_setup_ui_wifi_result_t fixture[] = {
        { .ssid = "SomnoTrace Lab", .rssi_dbm = -42, .secure = true },
        { .ssid = "Guest 2.4 GHz", .rssi_dbm = -61, .secure = false },
        { .ssid = "Bedroom", .rssi_dbm = -70, .secure = true },
    };
    vTaskDelay(pdMS_TO_TICKS(250));
    state_lock(controller);
    memcpy(controller->live.wifi_results, fixture, sizeof(fixture));
    controller->live.wifi_result_count =
        (uint8_t)(sizeof(fixture) / sizeof(fixture[0]));
    controller->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_SELECT;
    controller->live.wifi_scan_blocked = false;
    controller->live.wifi_scan_blocked_reason[0] = '\0';
    controller->live.error_message[0] = '\0';
    controller->generation++;
    state_unlock(controller);
#else
    netprov_scan_snapshot_t before;
    netprov_scan_get_snapshot(&before);
    esp_err_t result = netprov_scan_request();
    if (result != ESP_OK) {
        netprov_scan_snapshot_t failed;
        netprov_scan_get_snapshot(&failed);
        const char *message = "Could not start a Wi-Fi scan.";
        if (failed.state == NETPROV_SCAN_BLOCKED &&
            failed.blocked_by == NETPROV_SCAN_BLOCK_RECORDING) {
            message = "Stop therapy before scanning for Wi-Fi.";
        } else if (failed.state == NETPROV_SCAN_BLOCKED &&
                   failed.blocked_by == NETPROV_SCAN_BLOCK_RADIO_BUSY) {
            message = "The Wi-Fi radio is busy. Try again shortly.";
        } else if (failed.state == NETPROV_SCAN_BLOCKED &&
                   failed.blocked_by == NETPROV_SCAN_BLOCK_NOT_INITIALIZED) {
            message = "Wi-Fi is still starting. Try again shortly.";
        }
        state_lock(controller);
        controller->live.wifi_scan_blocked =
            failed.state == NETPROV_SCAN_BLOCKED;
        controller->live.wifi_state = controller->live.wifi_scan_blocked
            ? FIRST_RUN_SETUP_UI_WIFI_SCAN_BLOCKED
            : FIRST_RUN_SETUP_UI_WIFI_ERROR;
        copy_text(controller->live.wifi_scan_blocked_reason,
                  sizeof(controller->live.wifi_scan_blocked_reason),
                  controller->live.wifi_scan_blocked ? message : "");
        copy_text(controller->live.error_message,
                  sizeof(controller->live.error_message), message);
        controller->generation++;
        state_unlock(controller);
        return;
    }

    netprov_scan_snapshot_t scan;
    memset(&scan, 0, sizeof(scan));
    for (unsigned poll = 0; poll < SETUP_WIFI_SCAN_POLLS; poll++) {
        netprov_scan_get_snapshot(&scan);
        if (scan.generation != before.generation &&
            scan.state != NETPROV_SCAN_RUNNING) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    state_lock(controller);
    controller->live.wifi_result_count = 0;
    controller->live.wifi_scan_blocked = false;
    controller->live.wifi_scan_blocked_reason[0] = '\0';
    controller->live.error_message[0] = '\0';
    if (scan.state == NETPROV_SCAN_READY) {
        size_t count = scan.count < FIRST_RUN_SETUP_UI_WIFI_RESULT_MAX
                           ? scan.count : FIRST_RUN_SETUP_UI_WIFI_RESULT_MAX;
        for (size_t i = 0; i < count; i++) {
            copy_text(controller->live.wifi_results[i].ssid,
                      sizeof(controller->live.wifi_results[i].ssid),
                      scan.aps[i].ssid);
            controller->live.wifi_results[i].rssi_dbm = scan.aps[i].rssi;
            controller->live.wifi_results[i].secure = scan.aps[i].secure;
        }
        controller->live.wifi_result_count = (uint8_t)count;
        controller->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_SELECT;
    } else {
        controller->live.wifi_scan_blocked =
            scan.state == NETPROV_SCAN_BLOCKED;
        controller->live.wifi_state = controller->live.wifi_scan_blocked
            ? FIRST_RUN_SETUP_UI_WIFI_SCAN_BLOCKED
            : FIRST_RUN_SETUP_UI_WIFI_ERROR;
        const char *message = "Wi-Fi scan did not finish.";
        if (scan.state == NETPROV_SCAN_BLOCKED &&
            scan.blocked_by == NETPROV_SCAN_BLOCK_RECORDING) {
            message = "Stop therapy before scanning for Wi-Fi.";
        } else if (scan.state == NETPROV_SCAN_BLOCKED &&
                   scan.blocked_by == NETPROV_SCAN_BLOCK_RADIO_BUSY) {
            message = "The Wi-Fi radio is busy. Try again shortly.";
        }
        copy_text(controller->live.wifi_scan_blocked_reason,
                  sizeof(controller->live.wifi_scan_blocked_reason),
                  controller->live.wifi_scan_blocked ? message : "");
        copy_text(controller->live.error_message,
                  sizeof(controller->live.error_message), message);
    }
    controller->generation++;
    state_unlock(controller);
#endif
}

#if !CONFIG_SOMNOTRACE_BOARD_QEMU
static void candidate_wifi_config(struct netprov_config *candidate,
                                  const char *ssid, const char *password)
{
    struct netprov_config previous;
    bool had_previous = netprov_load_config(&previous);
    memset(candidate, 0, sizeof(*candidate));
    copy_text(candidate->hostname, sizeof(candidate->hostname),
              had_previous ? previous.hostname : "SomnoTrace");
    copy_text(candidate->wifi[0].ssid, sizeof(candidate->wifi[0].ssid), ssid);
    copy_text(candidate->wifi[0].pass, sizeof(candidate->wifi[0].pass),
              password);
    unsigned destination = 1;
    for (unsigned i = 0;
         had_previous && i < NETPROV_MAX_SSID_SLOTS &&
         destination < NETPROV_MAX_SSID_SLOTS;
         i++) {
        if (!previous.wifi[i].ssid[0] ||
            strcmp(previous.wifi[i].ssid, ssid) == 0) continue;
        candidate->wifi[destination++] = previous.wifi[i];
    }
}
#endif

static void do_wifi_connect(setup_controller_t *controller,
                            const setup_operation_t *operation)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    vTaskDelay(pdMS_TO_TICKS(350));
    state_lock(controller);
    controller->live.wifi_configured = true;
    controller->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_CONNECTED;
    copy_text(controller->live.connected_ssid,
              sizeof(controller->live.connected_ssid), operation->first);
    controller->live.wifi_slots_used = 1;
    controller->live.error_message[0] = '\0';
    controller->generation++;
    state_unlock(controller);
#else
    struct netprov_config candidate;
    candidate_wifi_config(&candidate, operation->first, operation->second);
    char ip[16] = "0.0.0.0";
    esp_err_t result = netprov_try_connect(&candidate, ip, 8000);
    if (result == ESP_OK) result = netprov_save_config(&candidate);

    state_lock(controller);
    if (result == ESP_OK) {
        controller->live.wifi_configured = true;
        controller->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_CONNECTED;
        copy_text(controller->live.connected_ssid,
                  sizeof(controller->live.connected_ssid), operation->first);
        controller->live.wifi_slots_used = configured_wifi_slots(&candidate);
        controller->live.error_message[0] = '\0';
    } else {
        /* netprov currently reports join failures as one generic ESP_FAIL;
         * do not mislabel a lost AP as a known password rejection. */
        controller->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_ERROR;
        copy_text(controller->live.error_message,
                  sizeof(controller->live.error_message),
                  "Could not join this network. Check the password and signal.");
    }
    controller->generation++;
    state_unlock(controller);
#endif
}

static void do_timezone_search(setup_controller_t *controller,
                               const char *query)
{
    timezone_catalog_entry_t matches[FIRST_RUN_SETUP_UI_TIMEZONE_RESULT_MAX];
    size_t count = timezone_catalog_search(query, matches,
                                           FIRST_RUN_SETUP_UI_TIMEZONE_RESULT_MAX);
    state_lock(controller);
    memset(controller->live.timezone_results, 0,
           sizeof(controller->live.timezone_results));
    controller->live.timezone_result_count = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        first_run_setup_ui_timezone_result_t *destination =
            &controller->live.timezone_results[i];
        copy_text(destination->id, sizeof(destination->id), matches[i].id);
        copy_text(destination->posix_tz, sizeof(destination->posix_tz),
                  matches[i].posix);
        copy_text(destination->utc_offset, sizeof(destination->utc_offset),
                  matches[i].utc_offset);
        copy_text(destination->abbreviation,
                  sizeof(destination->abbreviation), matches[i].abbreviation);
    }
    controller->live.time_state = FIRST_RUN_SETUP_UI_TIME_IDLE;
    if (count == 0) {
        copy_text(controller->live.error_message,
                  sizeof(controller->live.error_message),
                  "No matching IANA time zones.");
    } else {
        controller->live.error_message[0] = '\0';
    }
    controller->generation++;
    state_unlock(controller);
}

static void do_timezone_select(setup_controller_t *controller,
                               const setup_operation_t *operation)
{
    esp_err_t result = time_sync_set_timezone(operation->second,
                                              operation->first);
    state_lock(controller);
    if (result == ESP_OK) {
        controller->live.time_configured = true;
        controller->live.time_state = FIRST_RUN_SETUP_UI_TIME_SET;
        copy_text(controller->live.timezone_id,
                  sizeof(controller->live.timezone_id), operation->first);
        controller->live.error_message[0] = '\0';
    } else {
        controller->live.time_state = FIRST_RUN_SETUP_UI_TIME_ERROR;
        snprintf(controller->live.error_message,
                 sizeof(controller->live.error_message),
                 "Could not save time zone: %s", esp_err_to_name(result));
    }
    controller->generation++;
    state_unlock(controller);
    refresh_observed(controller);
}

static void do_time_advanced(setup_controller_t *controller,
                             const setup_operation_t *operation)
{
    esp_err_t ntp_result = ESP_OK;
    esp_err_t hostname_result = ESP_OK;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    vTaskDelay(pdMS_TO_TICKS(250));
#else
    ntp_result = time_sync_set_ntp_server(operation->first);
    if (ntp_result == ESP_OK)
        hostname_result = netprov_set_mdns_name(operation->second);
#endif

    state_lock(controller);
    controller->live.time_state = FIRST_RUN_SETUP_UI_TIME_ADVANCED;
    if (ntp_result == ESP_OK) {
        copy_text(controller->live.ntp_server,
                  sizeof(controller->live.ntp_server), operation->first);
    }
    if (hostname_result == ESP_OK) {
        copy_text(controller->live.hostname,
                  sizeof(controller->live.hostname), operation->second);
    }
    if (ntp_result == ESP_OK && hostname_result == ESP_OK) {
        copy_text(controller->live.time_sync_status,
                  sizeof(controller->live.time_sync_status),
#if CONFIG_SOMNOTRACE_BOARD_QEMU
                  "Advanced settings saved · simulated");
#else
                  time_sync_is_synced() ? "Synchronized by NTP"
                                        : "Settings saved · sync pending");
#endif
        controller->live.error_message[0] = '\0';
    } else if (ntp_result != ESP_OK) {
        snprintf(controller->live.error_message,
                 sizeof(controller->live.error_message),
                 "Could not save NTP server: %s",
                 esp_err_to_name(ntp_result));
    } else {
        snprintf(controller->live.error_message,
                 sizeof(controller->live.error_message),
                 "NTP saved; hostname failed: %s",
                 esp_err_to_name(hostname_result));
    }
    controller->generation++;
    state_unlock(controller);
}

static void do_airsense_scan(setup_controller_t *controller)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    vTaskDelay(pdMS_TO_TICKS(300));
    state_lock(controller);
    controller->live.airsense_result_count = 1;
    copy_text(controller->live.airsense_results[0].name,
              sizeof(controller->live.airsense_results[0].name),
              "AirSense 11 · simulated");
    copy_text(controller->live.airsense_results[0].address,
              sizeof(controller->live.airsense_results[0].address),
              "AA:11:00:00:00:01");
    controller->live.airsense_results[0].rssi_dbm = -47;
    controller->live.airsense_state = FIRST_RUN_SETUP_UI_AIRSENSE_SELECT;
    controller->live.error_message[0] = '\0';
    controller->generation++;
    state_unlock(controller);
#else
    esp_err_t result = as11_ble_scan(8);
    cJSON *devices = result == ESP_OK ? as11_ble_get_scan_results() : NULL;
    state_lock(controller);
    controller->live.airsense_result_count = 0;
    if (devices && cJSON_IsArray(devices)) {
        const cJSON *device = NULL;
        cJSON_ArrayForEach(device, devices) {
            if (controller->live.airsense_result_count >=
                FIRST_RUN_SETUP_UI_AIRSENSE_RESULT_MAX) break;
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(device,
                                                                  "name");
            const cJSON *address = cJSON_GetObjectItemCaseSensitive(device,
                                                                     "addr");
            const cJSON *rssi = cJSON_GetObjectItemCaseSensitive(device,
                                                                  "rssi");
            if (!cJSON_IsString(address)) continue;
            first_run_setup_ui_airsense_result_t *destination =
                &controller->live.airsense_results[
                    controller->live.airsense_result_count++];
            copy_text(destination->name, sizeof(destination->name),
                      cJSON_IsString(name) ? name->valuestring : "AirSense 11");
            copy_text(destination->address, sizeof(destination->address),
                      address->valuestring);
            destination->rssi_dbm = cJSON_IsNumber(rssi)
                                        ? (int8_t)rssi->valueint : -127;
        }
    }
    if (result == ESP_OK && controller->live.airsense_result_count > 0) {
        controller->live.airsense_state =
            FIRST_RUN_SETUP_UI_AIRSENSE_SELECT;
        controller->live.error_message[0] = '\0';
    } else if (result == ESP_OK) {
        controller->live.airsense_state =
            FIRST_RUN_SETUP_UI_AIRSENSE_NOT_FOUND;
        copy_text(controller->live.error_message,
                  sizeof(controller->live.error_message),
                  "No machine is advertising. Reopen More > myAir App.");
    } else {
        controller->live.airsense_state =
            FIRST_RUN_SETUP_UI_AIRSENSE_ERROR;
        snprintf(controller->live.error_message,
                 sizeof(controller->live.error_message),
                 "Bluetooth scan failed: %s", esp_err_to_name(result));
    }
    controller->generation++;
    state_unlock(controller);
    if (devices) cJSON_Delete(devices);
#endif
}

static bool wait_for_airsense(setup_controller_t *controller,
                              bool confirmation)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    vTaskDelay(pdMS_TO_TICKS(350));
    if (confirmation) publish_paired_info(controller);
    else {
        state_lock(controller);
        controller->live.airsense_state =
            FIRST_RUN_SETUP_UI_AIRSENSE_WAIT_CODE;
        controller->live.error_message[0] = '\0';
        controller->generation++;
        state_unlock(controller);
    }
    return true;
#else
    for (unsigned poll = 0; poll < SETUP_PAIR_POLLS; poll++) {
        const char *status = as11_ble_get_status();
        if (!strcmp(status, AS11_STATUS_PAIRED)) {
            publish_paired_info(controller);
            return true;
        }
        if (!confirmation && !strcmp(status, AS11_STATUS_WAIT_PASSKEY)) {
            state_lock(controller);
            controller->live.airsense_state =
                FIRST_RUN_SETUP_UI_AIRSENSE_WAIT_CODE;
            controller->live.error_message[0] = '\0';
            controller->generation++;
            state_unlock(controller);
            return true;
        }
        if (!strcmp(status, AS11_STATUS_ERROR)) {
            state_lock(controller);
            controller->live.airsense_state = confirmation
                ? FIRST_RUN_SETUP_UI_AIRSENSE_CODE_REJECTED
                : FIRST_RUN_SETUP_UI_AIRSENSE_ERROR;
            copy_text(controller->live.error_message,
                      sizeof(controller->live.error_message),
                      as11_ble_get_error());
            controller->generation++;
            state_unlock(controller);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    state_lock(controller);
    controller->live.airsense_state =
        FIRST_RUN_SETUP_UI_AIRSENSE_ERROR;
    copy_text(controller->live.error_message,
              sizeof(controller->live.error_message),
              confirmation ? "Pairing confirmation timed out."
                           : "The AirSense did not show a code in time.");
    controller->generation++;
    state_unlock(controller);
    return false;
#endif
}

static void do_airsense_begin(setup_controller_t *controller,
                              const char *address)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    (void)address;
    wait_for_airsense(controller, false);
#else
    esp_err_t result = as11_ble_start_pair(address);
    if (result == ESP_OK) {
        wait_for_airsense(controller, false);
        return;
    }
    state_lock(controller);
    controller->live.airsense_state = FIRST_RUN_SETUP_UI_AIRSENSE_ERROR;
    snprintf(controller->live.error_message,
             sizeof(controller->live.error_message),
             "Could not start pairing: %s", esp_err_to_name(result));
    controller->generation++;
    state_unlock(controller);
#endif
}

static void do_airsense_confirm(setup_controller_t *controller,
                                const char *code)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    (void)code;
    wait_for_airsense(controller, true);
#else
    esp_err_t result = as11_ble_confirm_pair(code);
    if (result == ESP_OK) {
        wait_for_airsense(controller, true);
        return;
    }
    state_lock(controller);
    controller->live.airsense_state =
        FIRST_RUN_SETUP_UI_AIRSENSE_CODE_REJECTED;
    snprintf(controller->live.error_message,
             sizeof(controller->live.error_message),
             "Could not confirm code: %s", esp_err_to_name(result));
    controller->generation++;
    state_unlock(controller);
#endif
}

static void do_card_retry(setup_controller_t *controller)
{
    esp_err_t result = ESP_OK;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    if (bsp_display_is_therapy_active() ||
        !sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE, 1000)) {
        state_lock(controller);
        controller->live.card_state = sd_storage_is_ready()
            ? FIRST_RUN_SETUP_UI_CARD_FULL
            : (controller->initial_card_result == ESP_ERR_NOT_FOUND
                ? FIRST_RUN_SETUP_UI_CARD_MISSING
                : FIRST_RUN_SETUP_UI_CARD_UNREADABLE);
        copy_text(controller->live.error_message,
                  sizeof(controller->live.error_message),
                  "Stop therapy before checking the card again.");
        controller->generation++;
        state_unlock(controller);
        return;
    }
    /* The destructive claim is also a recording-start barrier. It spans the
     * mount and full interrupted-session recovery, closing the former race in
     * which therapy could start immediately after the one-time check above. */
    if (!sd_storage_is_ready()) result = sd_storage_init();
    if (result == ESP_OK && sd_storage_is_ready()) {
        /* Keep the writer unable to open a live session until recovery has
         * completely repaired any interrupted files on the newly mounted
         * card. This preserves boot's recovery-before-BLE ordering even
         * though the physical card arrived later. */
        session_writer_recover();
        esp_err_t writer = session_writer_init();
        if (writer == ESP_OK) {
            session_writer_enable_deferred_export();
        } else {
            result = writer;
        }
    }
    sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
#endif
    if (result == ESP_OK) controller->initial_card_result = ESP_OK;
    publish_card_state(controller, result);
}

static void do_alert_defaults(setup_controller_t *controller)
{
    esp_err_t result = ESP_OK;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    /* Pressing Configure is an explicit opt-in to the conservative bedside
     * preset: visible stop alerts after five minutes, no network push until a
     * topic is supplied later.  This is persisted, not merely painted. */
    result = therapy_alert_save_config_json(
        "{\"enabled\":true,\"win_start\":1380,\"win_end\":360,"
        "\"delay1\":5,\"push_en\":false,\"ntfy_srv\":\"https://ntfy.sh\","
        "\"ntfy_topic\":\"\",\"ntfy_prio\":5,\"delay2\":5,"
        "\"buzz_en\":true}");
#endif
    state_lock(controller);
    controller->live.alerts_configured = result == ESP_OK;
    if (result == ESP_OK) controller->live.error_message[0] = '\0';
    else snprintf(controller->live.error_message,
                  sizeof(controller->live.error_message),
                  "Could not save alert preset: %s", esp_err_to_name(result));
    controller->generation++;
    state_unlock(controller);
}

static void do_upload_private_default(setup_controller_t *controller)
{
    esp_err_t result = ESP_OK;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    /* Explicit private default: no remote or FTP service is enabled.  The
     * user can add a destination later in Manage/browser; setup never invents
     * credentials or claims an upload target exists. */
    uploader_config_t config;
    memset(&config, 0, sizeof(config));
    config.ftp_anonymous = true;
    config.max_days = 30;
    result = uploader_save_config(&config);
#endif
    state_lock(controller);
    controller->live.uploads_configured = result == ESP_OK;
    if (result == ESP_OK) controller->live.error_message[0] = '\0';
    else snprintf(controller->live.error_message,
                  sizeof(controller->live.error_message),
                  "Could not save private upload choice: %s",
                  esp_err_to_name(result));
    controller->generation++;
    state_unlock(controller);
}

static void setup_worker(void *argument)
{
    setup_controller_t *controller = argument;
    setup_operation_t operation;
    while (true) {
        if (xQueueReceive(controller->queue, &operation,
                          pdMS_TO_TICKS(500)) != pdTRUE) {
            refresh_observed(controller);
            continue;
        }
        if (operation.kind == SETUP_OP_STOP) break;

        switch (operation.kind) {
            case SETUP_OP_WIFI_SCAN:
                do_wifi_scan(controller);
                break;
            case SETUP_OP_WIFI_CONNECT:
                do_wifi_connect(controller, &operation);
                break;
            case SETUP_OP_TIMEZONE_SEARCH:
                do_timezone_search(controller, operation.first);
                break;
            case SETUP_OP_TIMEZONE_SELECT:
                do_timezone_select(controller, &operation);
                break;
            case SETUP_OP_TIME_ADVANCED:
                do_time_advanced(controller, &operation);
                break;
            case SETUP_OP_AIRSENSE_SCAN:
                do_airsense_scan(controller);
                break;
            case SETUP_OP_AIRSENSE_BEGIN:
                do_airsense_begin(controller, operation.first);
                break;
            case SETUP_OP_AIRSENSE_CONFIRM:
                do_airsense_confirm(controller, operation.first);
                break;
            case SETUP_OP_CARD_RETRY:
                do_card_retry(controller);
                break;
            case SETUP_OP_ALERTS_DEFAULT:
                do_alert_defaults(controller);
                break;
            case SETUP_OP_UPLOADS_PRIVATE:
                do_upload_private_default(controller);
                break;
            default:
                publish_error(controller, "Unknown setup operation.");
                break;
        }
        state_lock(controller);
        controller->operation_busy = false;
        state_unlock(controller);
        refresh_observed(controller);
    }

    ESP_LOGI(TAG, "setup worker stopped");

    /* stop() detaches the singleton before posting STOP, so no new snapshot
     * can acquire this pointer. The worker is the final owner and reclaims
     * all controller resources immediately before deleting its own
     * PSRAM-backed task. */
    portENTER_CRITICAL(&s_controller_lock);
    if (s_controller == controller) s_controller = NULL;
    portEXIT_CRITICAL(&s_controller_lock);
    while (true) {
        portENTER_CRITICAL(&s_controller_lock);
        bool referenced = controller->references != 0;
        portEXIT_CRITICAL(&s_controller_lock);
        if (!referenced) break;
        vTaskDelay(1);
    }
    QueueHandle_t queue = controller->queue;
    SemaphoreHandle_t mutex = controller->mutex;
    controller->queue = NULL;
    controller->mutex = NULL;
    vQueueDelete(queue);
    vSemaphoreDelete(mutex);
    free(controller);
    portENTER_CRITICAL(&s_controller_lock);
    s_controller_stopping = false;
    portEXIT_CRITICAL(&s_controller_lock);
    psram_task_delete(NULL);
}

static void initialise_live_state(setup_controller_t *controller)
{
    first_run_setup_ui_live_t *live = &controller->live;
    memset(live, 0, sizeof(*live));
    live->wifi_state = FIRST_RUN_SETUP_UI_WIFI_IDLE;
    live->time_state = FIRST_RUN_SETUP_UI_TIME_IDLE;
    live->airsense_state = FIRST_RUN_SETUP_UI_AIRSENSE_INSTRUCTIONS;
    live->card_state = FIRST_RUN_SETUP_UI_CARD_CHECKING;

    struct netprov_config network = {0};
    live->wifi_configured = netprov_load_config(&network);
    live->wifi_slots_max = NETPROV_MAX_SSID_SLOTS;
    live->wifi_slots_used = configured_wifi_slots(&network);
    live->static_ipv4_supported = false;
    live->static_ipv4_active = false;
    netprov_link_t link;
    netprov_get_link(&link);
    if (link.up) {
        live->wifi_state = FIRST_RUN_SETUP_UI_WIFI_CONNECTED;
        copy_text(live->connected_ssid, sizeof(live->connected_ssid),
                  link.ssid);
    }

    char timezone[64];
    time_sync_get_timezone(timezone, sizeof(timezone));
    live->time_configured = timezone[0] != '\0';
    time_sync_get_tz_name(live->timezone_id, sizeof(live->timezone_id));
    time_sync_get_ntp_server(live->ntp_server, sizeof(live->ntp_server));
    netprov_get_mdns_name(live->hostname, sizeof(live->hostname));
    copy_text(live->time_sync_status, sizeof(live->time_sync_status),
              time_sync_is_synced() ? "Synchronized by NTP"
                                    : "Waiting for time sync");
    if (live->time_configured) {
        live->time_state = FIRST_RUN_SETUP_UI_TIME_SET;
        copy_text(live->local_time, sizeof(live->local_time),
                  "Time zone saved");
    }

    live->airsense_paired = as11_ble_is_paired();
    if (live->airsense_paired)
        live->airsense_state = FIRST_RUN_SETUP_UI_AIRSENSE_PAIRED;

    therapy_alert_config_t alerts;
    live->alerts_configured = therapy_alert_load_config(&alerts) == ESP_OK;
    uploader_config_t uploads;
    live->uploads_configured = uploader_load_config(&uploads) == ESP_OK;

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    /* The selectable setup preview begins unresolved even though the normal
     * QEMU boot seeds a finished record.  Operations remain deterministic. */
    live->wifi_configured = false;
    live->time_configured = false;
    live->airsense_paired = false;
    live->alerts_configured = false;
    live->uploads_configured = false;
    live->wifi_state = FIRST_RUN_SETUP_UI_WIFI_IDLE;
    live->time_state = FIRST_RUN_SETUP_UI_TIME_IDLE;
    live->airsense_state = FIRST_RUN_SETUP_UI_AIRSENSE_INSTRUCTIONS;
    live->wifi_slots_used = 0;
    live->wifi_slots_max = NETPROV_MAX_SSID_SLOTS;
    copy_text(live->ntp_server, sizeof(live->ntp_server), "pool.ntp.org");
    copy_text(live->hostname, sizeof(live->hostname), "somnotrace");
    copy_text(live->time_sync_status, sizeof(live->time_sync_status),
              "Not synchronized · simulated");
#endif
}

esp_err_t first_run_setup_controller_start(esp_err_t initial_card_result)
{
    portENTER_CRITICAL(&s_controller_lock);
    bool already_started = s_controller != NULL || s_controller_stopping;
    portEXIT_CRITICAL(&s_controller_lock);
    if (already_started) return ESP_ERR_INVALID_STATE;

    setup_controller_t *controller = heap_caps_calloc(
        1, sizeof(*controller), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!controller) return ESP_ERR_NO_MEM;
    controller->mutex = xSemaphoreCreateMutex();
    controller->queue = xQueueCreate(1, sizeof(setup_operation_t));
    if (!controller->mutex || !controller->queue) {
        if (controller->mutex) vSemaphoreDelete(controller->mutex);
        if (controller->queue) vQueueDelete(controller->queue);
        free(controller);
        return ESP_ERR_NO_MEM;
    }
    controller->active = true;
    controller->generation = 1;
    controller->initial_card_result = initial_card_result;
    controller->callbacks = (first_run_setup_ui_controller_t) {
        .context = controller,
        .wifi_scan = wifi_scan_callback,
        .wifi_connect = wifi_connect_callback,
        .timezone_search = timezone_search_callback,
        .timezone_select = timezone_select_callback,
        .time_advanced_set = time_advanced_callback,
        .airsense_scan = airsense_scan_callback,
        .airsense_begin_pairing = airsense_begin_callback,
        .airsense_confirm_code = airsense_confirm_callback,
        .card_retry = card_retry_callback,
        .configure_alerts = alerts_callback,
        .configure_uploads = uploads_callback,
        .finished = finished_callback,
    };
    initialise_live_state(controller);

    portENTER_CRITICAL(&s_controller_lock);
    s_controller = controller;
    portEXIT_CRITICAL(&s_controller_lock);

    publish_card_state(controller, initial_card_result);
    if (controller->live.airsense_paired) publish_paired_info(controller);
    controller->worker = psram_task_create(
        setup_worker, "setup_worker", SETUP_WORKER_STACK_BYTES,
        controller, 3, tskNO_AFFINITY, NULL, NULL);
    if (!controller->worker) {
        controller->active = false;
        portENTER_CRITICAL(&s_controller_lock);
        s_controller = NULL;
        portEXIT_CRITICAL(&s_controller_lock);
        vQueueDelete(controller->queue);
        vSemaphoreDelete(controller->mutex);
        free(controller);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "setup controller started; card=%s",
             esp_err_to_name(initial_card_result));
    return ESP_OK;
}

void first_run_setup_controller_stop(void)
{
    setup_controller_t *controller;
    portENTER_CRITICAL(&s_controller_lock);
    controller = s_controller;
    if (controller) {
        /* Detach first: after this point callbacks/snapshots cannot discover a
         * controller that its worker is preparing to free. */
        s_controller = NULL;
        s_controller_stopping = true;
    }
    portEXIT_CRITICAL(&s_controller_lock);
    if (!controller) return;

    state_lock(controller);
    controller->active = false;
    state_unlock(controller);
    setup_operation_t stop = { .kind = SETUP_OP_STOP };
    /* The queue has length one. Overwrite guarantees a stop is retained even
     * if a final UI press was queued but not yet dispatched. */
    xQueueOverwrite(controller->queue, &stop);
}

const first_run_setup_ui_controller_t *
first_run_setup_controller_callbacks(void)
{
    setup_controller_t *controller;
    portENTER_CRITICAL(&s_controller_lock);
    controller = s_controller;
    portEXIT_CRITICAL(&s_controller_lock);
    return controller ? &controller->callbacks : NULL;
}

bool first_run_setup_controller_snapshot(first_run_setup_ui_live_t *out,
                                         uint32_t *generation)
{
    if (!out) return false;
    setup_controller_t *controller = controller_acquire();
    if (!controller) return false;
    state_lock(controller);
    *out = controller->live;
    if (generation) *generation = controller->generation;
    state_unlock(controller);
    controller_release(controller);
    return true;
}

bool first_run_setup_controller_take_finished(void)
{
    setup_controller_t *controller = controller_acquire();
    if (!controller) return false;
    state_lock(controller);
    bool requested = controller->finished_requested;
    controller->finished_requested = false;
    state_unlock(controller);
    controller_release(controller);
    return requested;
}
