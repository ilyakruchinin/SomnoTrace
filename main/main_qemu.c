/* Minimal virtual 7B board: public status sinks, no later feature backends. */
#include "bsp_display.h"
#include "psram_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    ESP_ERROR_CHECK(psram_task_init());
    ESP_ERROR_CHECK(bsp_display_init());
    const char *lines[] = {"QEMU board preview", "Simulated display data"};
    bsp_display_show_lines("SomnoTrace", lines, 2);
    bsp_display_set_wifi_connected(true);
    bsp_display_set_as11_paired(true);
    bsp_display_set_sd_ready(true);
    bsp_display_set_battery(82, false, true);
    ESP_LOGI("somnotrace_qemu", "interactive UI preview ready");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
