/* ESP32-S3 QEMU virtual transport for the original 1.54-inch compact UI. */
#include "board_qemu_154.h"

#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"

static const char *TAG = "board_qemu_154";
static uint16_t *s_native_fb;

esp_err_t board_qemu_154_init(esp_lcd_panel_handle_t *panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel output required");
    *panel = NULL;
    const esp_lcd_rgb_qemu_config_t config = {
        .width = QEMU_154_H_RES,
        .height = QEMU_154_V_RES,
        .bpp = RGB_QEMU_BPP_16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_qemu(&config, panel), TAG, "create virtual panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "reset virtual panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "initialize virtual panel");
    void *framebuffer = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_qemu_get_frame_buffer(*panel, &framebuffer),
                        TAG, "get virtual framebuffer");
    ESP_RETURN_ON_FALSE(framebuffer, ESP_ERR_INVALID_STATE, TAG, "virtual framebuffer unavailable");
    s_native_fb = framebuffer;
    return ESP_OK;
}

esp_err_t board_qemu_154_flush(esp_lcd_panel_handle_t panel,
                             const uint16_t *wire_rgb565, uint16_t rotation,
                             bool backlight_on)
{
    ESP_RETURN_ON_FALSE(panel && wire_rgb565 && s_native_fb,
                        ESP_ERR_INVALID_STATE, TAG, "virtual panel not initialized");
    ESP_RETURN_ON_FALSE(rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270,
                        ESP_ERR_INVALID_ARG, TAG, "invalid rotation");

    for (int y = 0; y < QEMU_154_V_RES; ++y) {
        for (int x = 0; x < QEMU_154_H_RES; ++x) {
            int sx = x, sy = y;
            switch (rotation) {
            case 90:  sx = y; sy = QEMU_154_V_RES - 1 - x; break;
            case 180: sx = QEMU_154_H_RES - 1 - x; sy = QEMU_154_V_RES - 1 - y; break;
            case 270: sx = QEMU_154_H_RES - 1 - y; sy = x; break;
            default: break;
            }
            uint16_t wire = backlight_on ? wire_rgb565[sy * QEMU_154_H_RES + sx] : 0;
            /* For example renderer red=0x00f8 becomes native red=0xf800.
             * Keep the existing swapped-color antialias/blend helpers intact. */
            s_native_fb[y * QEMU_154_H_RES + x] = (uint16_t)((wire >> 8) | (wire << 8));
        }
    }
    /* The managed transport waits for this virtual update to complete. It
     * supports no physical backlight, inversion, mirror, SPI, or touch IO. */
    return esp_lcd_rgb_qemu_refresh(panel);
}
