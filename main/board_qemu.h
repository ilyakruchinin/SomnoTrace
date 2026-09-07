/* Host-pointer touch bridge for the ESP32-S3 QEMU RGB panel. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void board_qemu_touch_read(uint16_t *x, uint16_t *y, bool *pressed);
