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
bool somnotrace_firmware_target_matches(const void *prefix, size_t size)
{
    const size_t offset =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    return prefix && size >= offset + sizeof(somnotrace_firmware_target) &&
           memcmp((const uint8_t *)prefix + offset, &somnotrace_firmware_target,
                  sizeof(somnotrace_firmware_target)) == 0;
}
