# Waveshare ESP32-S3-Touch-LCD-7B

Select `CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B` or run `scripts/build-7b.sh` for the 1024×600 RGB target. The original 240×240 SPI display remains the default board. Use a 16 MB flash / 8 MB octal PSRAM module.

The RGB timing is 30.85 MHz, with two PSRAM framebuffers and a ten-line bounce buffer. LVGL renders on the RGB interrupt core and waits for positive frame retirement before reusing scanout memory. These constraints protect ownership; physical tearing and timing still require board testing.

GT911 uses SDA8, SCL9 and INT4; the CH32V003 controller at address 0x24 owns reset, panel power, backlight and TF chip select. Touch observation runs separately from rendering and cancels gestures after errors or stale input. Visibility recovery reasserts the output controller, and a failed wake retries. The status screen consumes the public display APIs and includes a wake-only Screen off control.

The TF socket uses one-bit SDMMC with CLK12, CMD11 and D0=13. The hardware has no supported alert speaker or battery telemetry. Do not use the compact board GPIO2/GPIO0 power/button controls: those pins carry RGB data on this board.

The 1–200 stored brightness range maps to 1–100% on 7B. The default is the steady 100% endpoint. `scripts/test-platform4.sh` exercises host allocation, input recovery, and framebuffer contracts; its first run resolves the exact hash-verified LVGL 8.4.0 input source if IDF has not populated managed components.
