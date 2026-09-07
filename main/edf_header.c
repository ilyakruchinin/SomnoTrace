/*
 * SomnoTrace - EDF header writer and identification file generation
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "edf_header.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "edf_hdr";
static esp_err_t s_source_error;
static TaskHandle_t s_source_owner;

static void source_error(esp_err_t err)
{
    if (__atomic_load_n(&s_source_owner, __ATOMIC_ACQUIRE) ==
            xTaskGetCurrentTaskHandle() && s_source_error == ESP_OK) {
        s_source_error = err;
    }
}

void edf_source_error_begin(void)
{
    s_source_error = ESP_OK;
    __atomic_store_n(&s_source_owner, xTaskGetCurrentTaskHandle(),
                     __ATOMIC_RELEASE);
}

esp_err_t edf_source_error_end(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    esp_err_t result =
        __atomic_load_n(&s_source_owner, __ATOMIC_ACQUIRE) == current
            ? s_source_error : ESP_OK;
    if (__atomic_load_n(&s_source_owner, __ATOMIC_ACQUIRE) == current) {
        __atomic_store_n(&s_source_owner, NULL, __ATOMIC_RELEASE);
    }
    return result;
}

/* ════════════════════════════════════════════════════════════════════
 *  CRC functions
 * ════════════════════════════════════════════════════════════════════ */

uint16_t edf_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

uint32_t edf_crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/* ════════════════════════════════════════════════════════════════════
 *  File Operations & Utilities
 * ════════════════════════════════════════════════════════════════════ */

void edf_write_field(char *buf, int width, const char *text)
{
    memset(buf, ' ', width);
    if (text) {
        int len = strlen(text);
        if (len > width) len = width;
        memcpy(buf, text, len);
    }
}

bool edf_write_all(FILE *f, const void *data, size_t len)
{
    return fwrite(data, 1, len, f) == len;
}

FILE *edf_open_atomic_file(const char *path, char *tmp_path, size_t tmp_path_len)
{
    int written = snprintf(tmp_path, tmp_path_len, "%s.tmp", path);
    if (written < 0 || (size_t)written >= tmp_path_len) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    unlink(tmp_path);
    return fopen(tmp_path, "wb");
}

esp_err_t edf_publish_atomic_path(const char *tmp_path, const char *path)
{
    char backup[700];
    int n = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (n < 0 || n >= (int)sizeof(backup)) {
        errno = ENAMETOOLONG;
        return ESP_FAIL;
    }
    struct stat st;
    bool exists = stat(path, &st) == 0;
    if (!exists && errno != ENOENT) return ESP_FAIL;
    if (!exists) {
        if (rename(backup, path) == 0) exists = true;
        else if (errno != ENOENT) return ESP_FAIL;
    }
    if (exists) {
        if (unlink(backup) != 0 && errno != ENOENT) return ESP_FAIL;
        if (rename(path, backup) != 0) return ESP_FAIL;
    }
    if (rename(tmp_path, path) != 0) {
        int first_error = errno;
        if (exists) (void)rename(backup, path);
        errno = first_error;
        return ESP_FAIL;
    }
    if (exists) (void)unlink(backup);
    return ESP_OK;
}

esp_err_t edf_finalize_atomic_file(FILE *f, const char *tmp_path, const char *path)
{
    int saved_errno = 0;
    if (fflush(f) != 0) saved_errno = errno ? errno : EIO;
    if (!saved_errno && fsync(fileno(f)) != 0)
        saved_errno = errno ? errno : EIO;
    if (fclose(f) != 0 && !saved_errno) saved_errno = errno ? errno : EIO;
    if (!saved_errno && edf_publish_atomic_path(tmp_path, path) != ESP_OK)
        saved_errno = errno ? errno : EIO;
    if (saved_errno) {
        unlink(tmp_path);
        errno = saved_errno;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void edf_discard_atomic_file(FILE *f, const char *tmp_path)
{
    int first_error = errno;
    if (f) fclose(f);
    if (tmp_path) unlink(tmp_path);
    errno = first_error;
}

cJSON *edf_read_json_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) source_error(ESP_FAIL);
        return NULL;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    char *buf = NULL;
    if (size <= 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        source_error(ESP_FAIL);
    } else {
        buf = malloc((size_t)size + 1);
        if (!buf) source_error(ESP_ERR_NO_MEM);
        else if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
            source_error(ESP_FAIL);
            free(buf);
            buf = NULL;
        } else {
            buf[size] = '\0';
        }
    }
    if (fclose(f) != 0) {
        source_error(ESP_FAIL);
        free(buf);
        buf = NULL;
    }
    cJSON *json = buf ? cJSON_Parse(buf) : NULL;
    if (buf && !json) source_error(ESP_FAIL);
    free(buf);
    return json;
}

esp_err_t edf_write_json_file(const char *path, const cJSON *json)
{
    char *str = cJSON_Print(json);
    if (!str) {
        source_error(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    char tmp[700];
    FILE *f = edf_open_atomic_file(path, tmp, sizeof(tmp));
    esp_err_t ret = ESP_FAIL;
    if (f) {
        if (!edf_write_all(f, str, strlen(str))) edf_discard_atomic_file(f, tmp);
        else ret = edf_finalize_atomic_file(f, tmp, path);
    }
    free(str);
    if (ret != ESP_OK) source_error(ret);
    return ret;
}

uint8_t *edf_read_bin_file(const char *path, size_t *out_len)
{
    if (out_len) *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) source_error(ESP_FAIL);
        return NULL;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    uint8_t *buf = NULL;
    if (size < 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        source_error(ESP_FAIL);
    } else if (size > 0) {
        buf = malloc((size_t)size);
        if (!buf) source_error(ESP_ERR_NO_MEM);
        else if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
            source_error(ESP_FAIL);
            free(buf);
            buf = NULL;
        }
    }
    if (fclose(f) != 0) {
        source_error(ESP_FAIL);
        free(buf);
        buf = NULL;
    }
    if (buf && out_len) *out_len = (size_t)size;
    return buf;
}

/* ════════════════════════════════════════════════════════════════════
 *  EDF Header Writer
 * ════════════════════════════════════════════════════════════════════ */

int edf_write_header(FILE *f, const char *patient_id,
                     const char *recording_id,
                     const char *start_date, const char *start_time,
                     int record_count, const char *record_dur,
                     const char *reserved,
                     const edf_signal_def_t *signals, int n_signals)
{
    int total_signals = n_signals + 1;
    int header_bytes = 256 + 256 * total_signals;

    /* ── Fixed header (256 bytes) ── */
    char hdr[256];
    memset(hdr, ' ', sizeof(hdr));

    edf_write_field(hdr + 0, 8, "0");                    /* version */
    edf_write_field(hdr + 8, 80, patient_id);            /* patient ID */
    edf_write_field(hdr + 88, 80, recording_id);         /* recording ID */
    edf_write_field(hdr + 168, 8, start_date);           /* start date */
    edf_write_field(hdr + 176, 8, start_time);           /* start time */
    char hb[16];
    snprintf(hb, sizeof(hb), "%d", header_bytes);
    edf_write_field(hdr + 184, 8, hb);                   /* header bytes */
    edf_write_field(hdr + 192, 44, reserved);            /* reserved */
    char rc[16];
    snprintf(rc, sizeof(rc), "%d", record_count);
    edf_write_field(hdr + 236, 8, rc);                   /* record count */
    edf_write_field(hdr + 244, 8, record_dur);           /* record duration */
    char ns[16];
    snprintf(ns, sizeof(ns), "%d", total_signals);
    edf_write_field(hdr + 252, 4, ns);                   /* signal count */

    /* ── Per-signal header blocks (256 bytes per signal) ── */
    int total = total_signals;
    size_t sigblock_size = 256 * total;
    char *sigblock = malloc(sigblock_size);
    if (!sigblock) {
        ESP_LOGE(TAG, "edf_write_header: malloc sigblock %u failed",
                 (unsigned)sigblock_size);
        return -1;
    }
    memset(sigblock, ' ', sigblock_size);

    /* Field 1: label (16 chars each) */
    for (int i = 0; i < n_signals; i++)
        edf_write_field(sigblock + i * 16, 16, signals[i].label);
    edf_write_field(sigblock + n_signals * 16, 16, "Crc16");

    /* Field 2: transducer type (80 chars each) */
    int offset = total * 16;
    for (int i = 0; i < n_signals; i++)
        edf_write_field(sigblock + offset + i * 80, 80, signals[i].transducer);
    edf_write_field(sigblock + offset + n_signals * 80, 80, "");

    /* Field 3: physical dimension (8 chars each) */
    offset = total * (16 + 80);
    for (int i = 0; i < n_signals; i++)
        edf_write_field(sigblock + offset + i * 8, 8, signals[i].unit);
    edf_write_field(sigblock + offset + n_signals * 8, 8, "");

    /* Field 4: physical minimum (8 chars each) */
    offset = total * (16 + 80 + 8);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        if (signals[i].dig_min == 0 && signals[i].dig_max == 16)
            snprintf(buf, sizeof(buf), "%d", signals[i].dig_min);
        else
            snprintf(buf, sizeof(buf), "%.2f", signals[i].phys_min);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "-32768.0");

    /* Field 5: physical maximum (8 chars each) */
    offset = total * (16 + 80 + 8 + 8);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        if (signals[i].dig_min == 0 && signals[i].dig_max == 16)
            snprintf(buf, sizeof(buf), "%d", signals[i].dig_max);
        else
            snprintf(buf, sizeof(buf), "%.2f", signals[i].phys_max);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "32767.00");

    /* Field 6: digital minimum (8 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", signals[i].dig_min);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "-32768");

    /* Field 7: digital maximum (8 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8 + 8);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", signals[i].dig_max);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "32767");

    /* Field 8: prefiltering (80 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8 + 8 + 8);
    for (int i = 0; i < n_signals; i++)
        edf_write_field(sigblock + offset + i * 80, 80, signals[i].prefilter);
    edf_write_field(sigblock + offset + n_signals * 80, 80, "");

    /* Field 9: samples per data record (8 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8 + 8 + 8 + 80);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", signals[i].samples_per_record);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "1");

    /* Field 10: reserved (32 chars each) - already space-padded */

    /* Compute header CRCs */
    uint16_t crc1 = edf_crc16_ccitt((uint8_t *)hdr + 0x19, 256 - 0x19);
    uint16_t crc2 = edf_crc16_ccitt((uint8_t *)sigblock, 256 * total);

    ESP_LOGI(TAG, "edf_write_header: H1=%04X H2=%04X", crc1, crc2);

    /* Write CRC values into the patient ID field */
    char pid[81];
    snprintf(pid, sizeof(pid), "X X X X %04X %04X", crc1, crc2);
    int plen = strlen(pid);
    while (plen < 80) pid[plen++] = ' ';
    pid[80] = '\0';
    memcpy(hdr + 8, pid, 80);

    bool written = edf_write_all(f, hdr, sizeof(hdr)) &&
                   edf_write_all(f, sigblock, 256 * total);

    free(sigblock);
    return written ? header_bytes : -1;
}

/* ════════════════════════════════════════════════════════════════════
 *  Identification.json and Identification.crc generation
 * ════════════════════════════════════════════════════════════════════ */

esp_err_t edf_generate_identification_files(const char *edf_dir,
                                            const char *ident_json_path)
{
    cJSON *ident = edf_read_json_file(ident_json_path);
    if (!ident) {
        ESP_LOGW(TAG, "identification.json not found: %s", ident_json_path);
        return ESP_FAIL;
    }

    const char *get_str(const cJSON *obj, const char *key) {
        cJSON *j = cJSON_GetObjectItem(obj, key);
        if (j && cJSON_IsString(j)) return j->valuestring;
        if (j && cJSON_IsNumber(j)) {
            static char num_buf[32];
            snprintf(num_buf, sizeof(num_buf), "%d", j->valueint);
            return num_buf;
        }
        return "";
    }

    cJSON *product = cJSON_CreateObject();
    cJSON_AddStringToObject(product, "UniversalIdentifier",
        get_str(ident, "UniversalIdentifier"));
    cJSON_AddStringToObject(product, "SerialNumber",
        get_str(ident, "SerialNumber"));
    cJSON_AddStringToObject(product, "SerialNumberVerificationCode", "");
    cJSON_AddStringToObject(product, "ProductCode",
        get_str(ident, "ProductCode"));
    cJSON_AddStringToObject(product, "ProductName", get_str(ident, "ProductName"));
    cJSON_AddStringToObject(product, "FdaUniqueDeviceIdentifier", "");
    cJSON_AddStringToObject(product, "ProductGeographicIdentifier",
        get_str(ident, "ProductGeographicIdentifier"));

    cJSON *hardware = cJSON_CreateObject();
    cJSON_AddStringToObject(hardware, "HardwareIdentifier",
        get_str(ident, "HardwareIdentifier"));

    cJSON *software = cJSON_CreateObject();
    cJSON_AddStringToObject(software, "BootloaderIdentifier",
        get_str(ident, "BootloaderIdentifier"));
    cJSON_AddStringToObject(software, "ApplicationIdentifier",
        get_str(ident, "ApplicationIdentifier"));
    cJSON_AddStringToObject(software, "ConfigurationIdentifier",
        get_str(ident, "ConfigurationIdentifier"));
    {
        cJSON *v = cJSON_GetObjectItem(ident, "PlatformIdentifier");
        cJSON_AddNumberToObject(software, "PlatformIdentifier",
            v ? v->valuedouble : 0);
        v = cJSON_GetObjectItem(ident, "VariantIdentifier");
        cJSON_AddNumberToObject(software, "VariantIdentifier",
            v ? v->valuedouble : 0);
        v = cJSON_GetObjectItem(ident, "RegionIdentifier");
        cJSON_AddNumberToObject(software, "RegionIdentifier",
            v ? v->valuedouble : 0);
    }
    cJSON_AddStringToObject(software, "ProfileVariationIdentifier",
        get_str(ident, "ProfileVariantIdentifier"));
    {
        cJSON *v = cJSON_GetObjectItem(ident, "DataVersionIdentifier");
        cJSON_AddNumberToObject(software, "DataVersionIdentifier",
            v ? v->valuedouble : 0);
    }
    cJSON_AddStringToObject(software, "DataModelVersionIdentifier",
        get_str(ident, "DataModelVersionIdentifier"));

    cJSON *profiles = cJSON_CreateObject();
    cJSON_AddItemToObject(profiles, "Product", product);
    cJSON_AddItemToObject(profiles, "Hardware", hardware);
    cJSON_AddItemToObject(profiles, "Software", software);

    cJSON *fg = cJSON_CreateObject();
    cJSON_AddItemToObject(fg, "IdentificationProfiles", profiles);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "FlowGenerator", fg);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cJSON_Delete(ident);

    if (!json_str) {
        ESP_LOGE(TAG, "generate_identification: cJSON_PrintUnformatted failed");
        return ESP_FAIL;
    }

    char json_path[300];
    char crc_path[300];
    char tmp_path[380];
    size_t json_len = strlen(json_str);
    snprintf(json_path, sizeof(json_path), "%s/Identification.json", edf_dir);
    snprintf(crc_path, sizeof(crc_path), "%s/Identification.crc", edf_dir);

    bool json_ok = false;
    FILE *f = edf_open_atomic_file(json_path, tmp_path, sizeof(tmp_path));
    if (f) {
        if (!edf_write_all(f, json_str, json_len)) {
            ESP_LOGE(TAG, "cannot write %s: %s", json_path, strerror(errno));
            edf_discard_atomic_file(f, tmp_path);
        } else if (edf_finalize_atomic_file(f, tmp_path, json_path) != ESP_OK) {
            ESP_LOGE(TAG, "cannot finalize %s: %s", json_path, strerror(errno));
        } else {
            ESP_LOGI(TAG, "wrote %s (%u bytes)", json_path, (unsigned)json_len);
            json_ok = true;
        }
    } else {
        ESP_LOGE(TAG, "cannot create %s: %s", json_path, strerror(errno));
    }

    bool crc_ok = false;
    if (json_ok) {
        uint32_t crc = edf_crc32_ieee((const uint8_t *)json_str, json_len);
        f = edf_open_atomic_file(crc_path, tmp_path, sizeof(tmp_path));
        if (f) {
            uint8_t crc_bytes[4] = {
                (uint8_t)(crc & 0xFF),
                (uint8_t)((crc >> 8) & 0xFF),
                (uint8_t)((crc >> 16) & 0xFF),
                (uint8_t)((crc >> 24) & 0xFF),
            };
            if (!edf_write_all(f, crc_bytes, 4)) {
                ESP_LOGE(TAG, "cannot write %s: %s", crc_path, strerror(errno));
                edf_discard_atomic_file(f, tmp_path);
                unlink(crc_path);
            } else if (edf_finalize_atomic_file(f, tmp_path, crc_path) != ESP_OK) {
                ESP_LOGE(TAG, "cannot finalize %s: %s", crc_path, strerror(errno));
                unlink(crc_path);
            } else {
                ESP_LOGI(TAG, "wrote %s (crc32=0x%08X)", crc_path, (unsigned)crc);
                crc_ok = true;
            }
        } else {
            ESP_LOGE(TAG, "cannot create %s: %s", crc_path, strerror(errno));
            unlink(crc_path);
        }
    } else {
        unlink(crc_path);
        ESP_LOGW(TAG, "Identification.json write failed — removed stale %s", crc_path);
    }

    free(json_str);
    return (json_ok && crc_ok) ? ESP_OK : ESP_FAIL;
}
