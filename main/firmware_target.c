#include "firmware_target.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "sdkconfig.h"
#include <string.h>
const somnotrace_firmware_target_t somnotrace_firmware_target
    __attribute__((section(".rodata_custom_desc"), used)) = {
        .magic = "SomnoTraceTarget",
#if CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B
        .board = "waveshare-7b",
#elif CONFIG_SOMNOTRACE_BOARD_QEMU
#if CONFIG_SOMNOTRACE_QEMU_DISPLAY_154
        .board = "qemu-154",
#else
        .board = "qemu-ui",
#endif
#else
        .board = "waveshare-154",
#endif
};
