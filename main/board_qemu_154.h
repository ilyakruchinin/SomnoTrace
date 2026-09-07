/* Virtual transport for the original 240x240 compact renderer. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#define QEMU_154_H_RES 240
#define QEMU_154_V_RES 240

esp_err_t board_qemu_154_init(esp_lcd_panel_handle_t *panel);

/* Input is the compact renderer's ST7789 on-wire (byte-swapped) RGB565.
 * QEMU consumes native little-endian RGB565; conversion and rotation happen
 * here without modifying the renderer's framebuffer or allocating DMA. */
esp_err_t board_qemu_154_flush(esp_lcd_panel_handle_t panel,
                             const uint16_t *wire_rgb565, uint16_t rotation,
                             bool backlight_on);
