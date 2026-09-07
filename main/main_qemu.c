/* Deterministic UI-only firmware entry point for ESP32-S3 QEMU. */
#include <math.h>
#include <stdint.h>

#include "bsp_display.h"
#include "device_settings.h"
#include "first_run_setup.h"
#include "log_stream.h"
#include "touch_logs_qemu.h"
#include "nvs_writer.h"
#include "psram_task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "somnotrace_qemu";

static void seed_finished_setup_preview(void)
{
    nvs_writer_init();
    esp_err_t err = first_run_setup_load();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        /* QEMU's flash image is disposable.  Recover an incompatible retained
         * setup record so screenshots never depend on a previous preview. */
        ESP_LOGW(TAG, "resetting incompatible QEMU setup state: %s",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(first_run_setup_reset());
    }

    const first_run_setup_observed_t observed = {
        .established_installation = false,
        .wifi_configured = true,
        .time_configured = true,
        .airsense_paired = true,
        .card_present = true,
        .alerts_configured = true,
        .uploads_configured = true,
    };
    ESP_ERROR_CHECK(first_run_setup_reconcile(&observed));

    first_run_setup_snapshot_t snapshot;
    first_run_setup_snapshot(&snapshot);
    ESP_ERROR_CHECK(first_run_setup_is_finished(&snapshot.state)
                        ? ESP_OK
                        : ESP_ERR_INVALID_STATE);
    ESP_LOGI(TAG, "deterministic setup preview ready (finished)");
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    ESP_ERROR_CHECK(psram_task_init());

    /* Exercise the same bounded retained feed as hardware so the native Logs
     * screen is a live acceptance surface rather than a disconnected mock. */
    log_stream_init();

    /* Resolve setup before constructing the shell: the normal QEMU target is
     * a deterministic post-setup product preview, not a retained wizard from
     * whichever flash image happened to run last. */
    seed_finished_setup_preview();

    ESP_ERROR_CHECK(bsp_display_init());
    device_settings_t settings;
    device_settings_load(&settings);
    bsp_display_set_brightness(settings.brightness);
    bsp_display_enable_touch_services(false, false);
    bsp_display_qemu_seed_demo();
    bsp_display_set_wifi_connected(true);
    bsp_display_set_as11_paired(true);
    bsp_display_set_sd_ready(true);
    bsp_display_set_battery(82, false, true);
    bsp_display_set_therapy_start_time(esp_timer_get_time() - 42 * 60 * 1000000LL);
    bsp_display_set_therapy_active(true);
    bsp_display_set_notice("QEMU preview - simulated data");

    ESP_LOGI(TAG, "1024x600 interactive UI preview ready; click to emulate touch");
    unsigned iteration = 0;
    /* Continue exactly where the deterministic pre-filled waveform ended so
     * the preview never displays a moving circular-buffer seam. */
    float phase = 300.0f * 0.06f;
    while (true) {
        if ((iteration % 20) == 0) touch_logs_qemu_tick();
        float flow = 36.0f * sinf(phase) + 7.0f * sinf(phase * 2.3f);
        bsp_display_push_flow(flow);
        if ((iteration % 20) == 0) {
            bsp_display_push_leak(3.2f + 0.8f * sinf(phase * 0.3f));
            bsp_display_push_metrics(8.6f + 0.4f * sinf(phase * 0.2f),
                                     14.4f, 0.08f);
        }
        phase += 0.06f;
        iteration++;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
