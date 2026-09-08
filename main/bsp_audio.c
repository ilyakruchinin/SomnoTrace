/*
 * SomnoTrace - ES8311 audio codec driver for alarm tones
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "bsp_audio.h"
#include "bsp_i2c.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_audio";

/* ── Hardware pins (Waveshare ESP32-S3-Touch-LCD-1.54) ────────────── */
#define I2C_FREQ_HZ     BSP_I2C_FREQ_HZ

#define I2S_NUM         0
#define I2S_MCLK_PIN    8
#define I2S_BCLK_PIN    9
#define I2S_WS_PIN      10
#define I2S_DOUT_PIN    12   /* DSDIN — ESP data out to codec DAC */
#define I2S_DIN_PIN     11   /* ASDOUT — codec ADC data in to ESP (unused) */

#define PA_PIN          7    /* NS4150B amplifier enable */

/* ES8311 I2C 7-bit address (CE=0).
 * Espressif headers use 0x30 (8-bit), but the new I2C master API
 * expects 7-bit addresses: 0x30 >> 1 = 0x18. */
#define ES8311_ADDR     0x18

/* ── ES8311 register addresses ────────────────────────────────────── */
#define ES8311_RESET_REG00          0x00
#define ES8311_CLK_MANAGER_REG01    0x01
#define ES8311_CLK_MANAGER_REG02    0x02
#define ES8311_CLK_MANAGER_REG03    0x03
#define ES8311_CLK_MANAGER_REG04    0x04
#define ES8311_CLK_MANAGER_REG05    0x05
#define ES8311_CLK_MANAGER_REG06    0x06
#define ES8311_CLK_MANAGER_REG07    0x07
#define ES8311_CLK_MANAGER_REG08    0x08
#define ES8311_SDPIN_REG09          0x09
#define ES8311_SDPOUT_REG0A         0x0A
#define ES8311_SYSTEM_REG0B         0x0B
#define ES8311_SYSTEM_REG0C         0x0C
#define ES8311_SYSTEM_REG0D         0x0D
#define ES8311_SYSTEM_REG0E         0x0E
#define ES8311_SYSTEM_REG10         0x10
#define ES8311_SYSTEM_REG11         0x11
#define ES8311_SYSTEM_REG12         0x12
#define ES8311_SYSTEM_REG13         0x13
#define ES8311_SYSTEM_REG14         0x14
#define ES8311_ADC_REG15            0x15
#define ES8311_ADC_REG16            0x16
#define ES8311_ADC_REG17            0x17
#define ES8311_ADC_REG1B            0x1B
#define ES8311_ADC_REG1C            0x1C
#define ES8311_DAC_REG31            0x31
#define ES8311_DAC_REG32            0x32
#define ES8311_DAC_REG37            0x37
#define ES8311_GPIO_REG44           0x44
#define ES8311_GP_REG45             0x45

/* ── Audio parameters ─────────────────────────────────────────────── */
#define SAMPLE_RATE     16000
#define MCLK_MULT       I2S_MCLK_MULTIPLE_256

/* ── State ────────────────────────────────────────────────────────── */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static bool s_bus_ready = false;   /* I2C bus + device set up */
static bool s_initialized = false; /* full init (codec + I2S + PA) done */
static uint8_t s_global_volume = 100;  /* scales all beep calls */

/* ── I2C helpers ──────────────────────────────────────────────────── */
static esp_err_t es_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), 100);
}

/* ── ES8311 initialization ────────────────────────────────────────── */
static esp_err_t es8311_init(void)
{
    esp_err_t ret = ESP_OK;

    /* Reset the codec FIRST — on warm reboot (esp_restart()) the ES8311
     * retains its previous state and may NACK register writes until reset.
     * Retry a few times to let the I2C bus recover. */
    bool reset_ok = false;
    for (int i = 0; i < 5; i++) {
        esp_err_t r = es_write_reg(ES8311_RESET_REG00, 0x80);
        if (r == ESP_OK) { reset_ok = true; break; }
        ESP_LOGW(TAG, "ES8311 reset retry %d/5: %s", i + 1, esp_err_to_name(r));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!reset_ok) {
        ESP_LOGE(TAG, "ES8311 reset failed after 5 retries");
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Enhance I2C noise immunity (write twice for reliability) */
    ret |= es_write_reg(ES8311_GPIO_REG44, 0x08);
    ret |= es_write_reg(ES8311_GPIO_REG44, 0x08);

    /* Clock reset sequence — match Espressif es8311_open sequence */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG01, 0x30);
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= es_write_reg(ES8311_ADC_REG16, 0x24);
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= es_write_reg(ES8311_SYSTEM_REG0B, 0x00);
    ret |= es_write_reg(ES8311_SYSTEM_REG0C, 0x00);
    ret |= es_write_reg(ES8311_SYSTEM_REG10, 0x1F);
    ret |= es_write_reg(ES8311_SYSTEM_REG11, 0x7F);

    /* Select MCLK from pin (bit7=0), not inverted (bit6=0), enable clock */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG01, 0x3F);

    /* System config */
    ret |= es_write_reg(ES8311_SYSTEM_REG13, 0x10);
    ret |= es_write_reg(ES8311_ADC_REG1B, 0x0A);
    ret |= es_write_reg(ES8311_ADC_REG1C, 0x6A);
    ret |= es_write_reg(ES8311_GPIO_REG44, 0x58);  /* internal ref (ADCL + DACR) */

    /* ── Configure sample rate: 16 kHz, MCLK = 4096000 ─── */
    /* REG02: pre_div=1 (0<<5), pre_multi=1 (0<<3) → 0x00 */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG02, 0x00);
    /* REG05: adc_div=1 (0<<4), dac_div=1 (0<<0) → 0x00 */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG05, 0x00);
    /* REG03: fs_mode=0 (0<<6), adc_osr=0x10 → 0x10 */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG03, 0x10);
    /* REG04: dac_osr=0x10 → 0x10 */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG04, 0x10);
    /* REG07: lrck_h=0x00 → 0x00 */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG07, 0x00);
    /* REG08: lrck_l=0xFF → 0xFF */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG08, 0xFF);
    /* REG06: bclk_div=4 → (4-1)=0x03 */
    ret |= es_write_reg(ES8311_CLK_MANAGER_REG06, 0x03);

    /* ── I2S format: standard Philips, 16-bit ─── */
    /* REG09 (DAC SDP): 16-bit (bits[4:3]=11), I2S normal (bits[1:0]=00),
     * DAC enabled (bit6=0) → 0x0C */
    ret |= es_write_reg(ES8311_SDPIN_REG09, 0x0C);
    /* REG0A (ADC SDP): 16-bit, I2S normal, ADC disabled (bit6=1) → 0x4C */
    ret |= es_write_reg(ES8311_SDPOUT_REG0A, 0x4C);

    /* ── Power up and start DAC ─── */
    ret |= es_write_reg(ES8311_ADC_REG17, 0xBF);
    ret |= es_write_reg(ES8311_SYSTEM_REG0E, 0x02);
    ret |= es_write_reg(ES8311_SYSTEM_REG12, 0x00);   /* enable DAC */
    ret |= es_write_reg(ES8311_SYSTEM_REG14, 0x1A);
    ret |= es_write_reg(ES8311_SYSTEM_REG0D, 0x01);   /* power up analog */
    ret |= es_write_reg(ES8311_ADC_REG15, 0x40);
    ret |= es_write_reg(ES8311_DAC_REG37, 0x08);      /* DAC ramp rate */
    ret |= es_write_reg(ES8311_GP_REG45, 0x00);

    /* Unmute DAC (REG31 bits[6:5] = 00) */
    ret |= es_write_reg(ES8311_DAC_REG31, 0x00);

    /* Set medium volume (REG32: 0x00=-95.5dB, 0xFF=+32dB) */
    ret |= es_write_reg(ES8311_DAC_REG32, 0xB0);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "ES8311 initialised: 16kHz/16-bit, DAC mode");
    }
    return ret;
}

/* ── Public API ───────────────────────────────────────────────────── */

static esp_err_t i2c_bus_setup(void)
{
    if (s_bus_ready) return ESP_OK;

    s_i2c_bus = bsp_i2c_get_bus_handle();
    if (!s_i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not available");
        return ESP_FAIL;
    }

    /* Probe for ES8311; also scan bus for diagnostics if not found */
    esp_err_t probe = i2c_master_probe(s_i2c_bus, ES8311_ADDR, 100);
    ESP_LOGI(TAG, "I2C probe 0x%02X: %s", ES8311_ADDR, probe == ESP_OK ? "OK" : esp_err_to_name(probe));
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 not found, scanning bus...");
        for (uint8_t addr = 1; addr < 0x80; addr++) {
            if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
                ESP_LOGW(TAG, "  found device at 0x%02X", addr);
            }
        }
        ESP_LOGW(TAG, "  bus scan complete");
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_bus_ready = true;
    return ESP_OK;
}

esp_err_t bsp_audio_init(void)
{
    if (s_initialized) return ESP_OK;

    /* I2C bus setup (skipped on retry if already done) */
    esp_err_t ret = i2c_bus_setup();
    if (ret != ESP_OK) return ret;

    /* ── ES8311 codec ───
     * The ES8311 may NACK register writes immediately after power-up
     * even when the I2C probe succeeds — the codec ACKs its address
     * but is not yet ready to accept data.  Retry the full init
     * sequence with a delay between attempts. */
    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = es8311_init();
        if (ret == ESP_OK) break;
        if (attempt < 3) {
            ESP_LOGW(TAG, "ES8311 init attempt %d/3 failed, retrying in 300ms", attempt);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed after 3 attempts, audio will be unavailable");
        return ret;
    }

    /* ── PA enable GPIO ─── */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << PA_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(PA_PIN, 0);  /* PA off until beep */

    /* ── I2S TX channel ─── */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ret = i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_clk_config_t clk_cfg = {
        .sample_rate_hz = SAMPLE_RATE,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = MCLK_MULT,
    };
    i2s_std_gpio_config_t gpio_cfg = {
        .bclk = I2S_BCLK_PIN,
        .ws = I2S_WS_PIN,
        .dout = I2S_DOUT_PIN,
        .din = I2S_DIN_PIN,
        .mclk = I2S_MCLK_PIN,
        .invert_flags = {
            .mclk_inv = false,
            .bclk_inv = false,
            .ws_inv = false,
        },
    };
    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = gpio_cfg,
    };
    ret = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "audio subsystem ready");
    return ESP_OK;
}

esp_err_t bsp_audio_beep(int freq_hz, int duration_ms, uint8_t volume)
{
    if (!s_initialized || !s_i2s_tx) return ESP_ERR_INVALID_STATE;
    if (freq_hz <= 0 || duration_ms <= 0) return ESP_ERR_INVALID_ARG;

    /* Map volume 0-100 to codec DAC register 0x00-0xFF.
     * Scale by s_global_volume so the user-configured alert volume
     * applies universally to all beeps. */
    uint8_t effective_vol = (uint8_t)((volume * s_global_volume) / 100);
    uint8_t vol_reg = (uint8_t)((effective_vol * 0xFF) / 100);
    es_write_reg(ES8311_DAC_REG32, vol_reg);

    /* Generate one period of square wave */
    int period_samples = SAMPLE_RATE / freq_hz;
    if (period_samples < 2) period_samples = 2;

    /* Use stereo frames: I2S STD mono mode sends on left channel.
     * Each sample is 2 bytes (16-bit). */
    int buf_samples = period_samples * 2;  /* *2 for stereo frame (L+R) */
    int buf_bytes = buf_samples * sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "beep buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    int half = period_samples;
    for (int i = 0; i < period_samples; i++) {
        int16_t val = (i < half / 2) ? 16000 : -16000;
        buf[i * 2]     = val;   /* left */
        buf[i * 2 + 1] = val;   /* right */
    }

    /* Enable PA and I2S */
    gpio_set_level(PA_PIN, 1);
    i2s_channel_enable(s_i2s_tx);

    /* Play for the requested duration */
    int total_bytes = (SAMPLE_RATE * duration_ms / 1000) * 2 * sizeof(int16_t);
    int written = 0;
    while (written < total_bytes) {
        size_t to_write = buf_bytes;
        if (written + (int)to_write > total_bytes) {
            to_write = total_bytes - written;
        }
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_i2s_tx, buf, to_write, &bytes_written, 200);
        if (err != ESP_OK || bytes_written == 0) {
            ESP_LOGW(TAG, "I2S write: err=%s written=%d", esp_err_to_name(err), (int)bytes_written);
            break;
        }
        written += bytes_written;
    }

    /* Stop I2S and disable PA */
    i2s_channel_disable(s_i2s_tx);
    gpio_set_level(PA_PIN, 0);

    free(buf);
    return ESP_OK;
}

void bsp_audio_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_global_volume = percent;
    ESP_LOGI(TAG, "global alert volume set to %u%%", percent);
}

esp_err_t bsp_audio_test_beep(void)
{
    if (!s_initialized) {
        esp_err_t ret = bsp_audio_init();
        if (ret != ESP_OK) return ret;
    }
    return bsp_audio_beep(880, 300, 100);
}
