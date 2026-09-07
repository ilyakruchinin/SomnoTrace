/* Deterministic preview of the original compact display, with UART scenes. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "bsp_display.h"
#include "psram_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "somnotrace_qemu_154";

void app_main(void)
{
    /* No NVS writes, networking, BLE, physical power, audio, or touch setup.
     * Public BSP APIs seed the real compact renderer's in-memory state. */
    ESP_ERROR_CHECK(psram_task_init());
    ESP_ERROR_CHECK(bsp_display_init());
    bsp_display_qemu_seed_demo();

    ESP_LOGI(TAG, "simulated compact preview; UART: 0=status 1=flow 2=info 3=notice r=rotate b=backlight");
    uint16_t rotation = 0;
    bool backlight_on = true;
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    /* Default UART VFS reads can be nonblocking. Single-character commands
     * also work over QEMU TCP serial without terminal line editing. */
    for (;;) {
        char commands[16];
        ssize_t count = read(STDIN_FILENO, commands, sizeof(commands));
        for (ssize_t i = 0; i < count; ++i) {
            char command = commands[i];
            if (command >= '0' && command <= '3') {
                uint8_t scene = (uint8_t)(command - '0');
                bsp_display_qemu_set_tab(scene);
                rotation = 0;
                backlight_on = true;
                ESP_LOGI(TAG, "QEMU 1.54 command: scene=%u", (unsigned)scene);
            } else if (command == 'r') {
                rotation = (rotation + 90) % 360;
                bsp_display_set_rotation(rotation);
                ESP_LOGI(TAG, "QEMU 1.54 command: rotation=%u", (unsigned)rotation);
            } else if (command == 'b') {
                backlight_on = !backlight_on;
                bsp_display_set_backlight(backlight_on);
                ESP_LOGI(TAG, "QEMU 1.54 command: backlight=%s", backlight_on ? "on" : "off");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
