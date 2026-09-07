/* SomnoTrace audio shim for Waveshare ESP32-S3-Touch-LCD-7B. */
#include "bsp_audio.h"

#include "esp_log.h"

static uint8_t s_volume = 100;

esp_err_t bsp_audio_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_audio_beep(int freq_hz, int duration_ms, uint8_t volume)
{
    (void)freq_hz;
    (void)duration_ms;
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
}

void bsp_audio_set_volume(uint8_t percent)
{
    s_volume = percent > 100 ? 100 : percent;
}

esp_err_t bsp_audio_test_beep(void)
{
    ESP_LOGW("bsp_audio_7b", "7B has no onboard SomnoTrace alert speaker (volume=%u)",
             (unsigned)s_volume);
    return ESP_ERR_NOT_SUPPORTED;
}
