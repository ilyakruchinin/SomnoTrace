/* Deterministic service inputs for the same native retained Logs controller. */
#include "touch_logs_qemu.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_disconnected;

void touch_logs_qemu_disconnect(void)
{
    __atomic_store_n(&s_disconnected, true, __ATOMIC_RELAXED);
    ESP_LOGI("qemu_logs", "simulated retained-view disconnection");
}

bool touch_logs_qemu_disconnected(void)
{
    return __atomic_load_n(&s_disconnected, __ATOMIC_RELAXED);
}

void touch_logs_qemu_reconnect(void)
{
    __atomic_store_n(&s_disconnected, false, __ATOMIC_RELAXED);
    ESP_LOGI("qemu_logs", "simulated retained-view recovered");
}

void touch_logs_qemu_tick(void)
{
    static unsigned step;
    if (step == 0) esp_log_level_set("qemu", ESP_LOG_DEBUG);
    static const char *const tags[] = {"qemu", "smb", "qemu_wifi", "smb", "qemu"};
    static const char levels[] = {'I', 'E', 'W', 'I', 'D'};
    static const esp_log_level_t severity[] = {
        ESP_LOG_INFO, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG,
    };
    unsigned index = step++ % 5;
    /* Explicitly simulated outcomes, not claimed radio or upload activity.
     * esp_log_write feeds the real sanitizer and bounded retained ring. */
    esp_log_write(severity[index], tags[index],
        "%c (%lu) %s: QEMU simulated event %u; no external operation\n",
        levels[index], (unsigned long)esp_log_timestamp(), tags[index], step);
}

esp_err_t touch_logs_qemu_save(const log_stream_retained_filter_t *filter,
    size_t *saved, log_stream_retained_progress_fn progress, void *ctx)
{
    /* Process a real bounded retained snapshot with deterministic simulated
     * storage latency. The receipt explicitly says no card file was written. */
    log_stream_retained_filter_t frozen = *filter;
    log_stream_retained_line_t line;
    log_stream_retained_page_t page;
    log_stream_retained_info_t info;
    size_t count;
    if (!frozen.before_sequence) {
        esp_err_t result = log_stream_retained_snapshot(&line, 1, NULL, &count, &info);
        if (result != ESP_OK) return result;
        frozen.before_sequence = count ? line.sequence + 1 : 1;
    }
    esp_err_t result = log_stream_retained_snapshot_page(NULL, 0, &frozen, 0, &page, &info);
    if (result != ESP_OK) return result;
    size_t total = page.matching_count;
    *saved = 0;
    progress(0, total, ctx);
    for (size_t offset = 0; offset < total; ++offset) {
        result = log_stream_retained_snapshot_page(&line, 1, &frozen, offset, &page, &info);
        if (result != ESP_OK) return result;
        if (page.returned != 1 || page.matching_count != total) return ESP_ERR_INVALID_STATE;
        ++*saved;
        progress(*saved, total, ctx);
        vTaskDelay(pdMS_TO_TICKS(35));
    }
    return ESP_OK;
}
