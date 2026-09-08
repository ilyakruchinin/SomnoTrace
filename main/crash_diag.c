/*
 * SomnoTrace - Crash diagnostics (reset reason + core dump analysis)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "crash_diag.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"

/* Core dump APIs are only available when core dump is enabled in sdkconfig.
 * Guard with CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH so this file compiles even
 * if the feature is later disabled. */
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
#include "esp_core_dump.h"
#endif

static const char *TAG = "crash_diag";

/* ── Crash breadcrumb in RTC memory ─────────────────────────────────────
 *
 * RTC_NOINIT_ATTR data is not cleared by the startup code, so it survives
 * panic, INT_WDT and TASK_WDT resets; on power-on it holds whatever the RAM
 * happened to contain.  A magic value plus a CRC over the payload therefore
 * has to gate every read, otherwise random RAM would be reported as a real
 * breadcrumb.
 *
 * Writes are deliberately trivial — no allocation, no locks, no I/O — so
 * they are safe to call from any task and cannot themselves become the
 * reason a crash goes unreported. */

#define BREADCRUMB_MAGIC 0x536F6D31u   /* "Som1" */

typedef struct {
    uint32_t magic;
    uint32_t crc;               /* CRC32 over everything after this field */
    uint32_t boot_count;
    int64_t  activity_us;       /* esp_timer time of the last activity note */
    char     session_id[40];
    char     activity[24];
} crash_breadcrumb_t;

static RTC_NOINIT_ATTR crash_breadcrumb_t s_bc;

/* Snapshot taken before the live breadcrumb is re-armed for this boot. */
static crash_breadcrumb_t s_bc_prev;
static bool s_bc_prev_valid = false;

static uint32_t breadcrumb_crc(const crash_breadcrumb_t *bc)
{
    const uint8_t *p = (const uint8_t *)&bc->boot_count;
    size_t len = sizeof(*bc) - offsetof(crash_breadcrumb_t, boot_count);
    return esp_rom_crc32_le(0, p, len);
}

static bool breadcrumb_valid(const crash_breadcrumb_t *bc)
{
    return bc->magic == BREADCRUMB_MAGIC && bc->crc == breadcrumb_crc(bc);
}

static void breadcrumb_reseal(void)
{
    s_bc.magic = BREADCRUMB_MAGIC;
    s_bc.crc = breadcrumb_crc(&s_bc);
}

void crash_diag_note_session(const char *session_id)
{
    if (session_id) {
        strlcpy(s_bc.session_id, session_id, sizeof(s_bc.session_id));
    } else {
        s_bc.session_id[0] = '\0';
    }
    breadcrumb_reseal();
}

void crash_diag_note_activity(const char *tag)
{
    if (tag) {
        strlcpy(s_bc.activity, tag, sizeof(s_bc.activity));
    } else {
        s_bc.activity[0] = '\0';
    }
    s_bc.activity_us = esp_timer_get_time();
    breadcrumb_reseal();
}

/* Capture the previous boot's breadcrumb, then re-arm it for this boot.
 * Called once, before anything can overwrite the live copy. */
static void breadcrumb_rotate(bool poweron)
{
    if (!poweron && breadcrumb_valid(&s_bc)) {
        s_bc_prev = s_bc;
        s_bc_prev_valid = true;
    }

    uint32_t boots = s_bc_prev_valid ? s_bc_prev.boot_count + 1 : 1;
    memset(&s_bc, 0, sizeof(s_bc));
    s_bc.boot_count = boots;
    breadcrumb_reseal();
}

/* Report the previous boot's breadcrumb.  Only meaningful after a crash:
 * on a clean restart the same data would describe an orderly shutdown. */
static void log_breadcrumb(void)
{
    if (!s_bc_prev_valid) {
        ESP_LOGW(TAG, "no crash breadcrumb from the previous boot "
                      "(power-on, or firmware predates breadcrumbs)");
        return;
    }

    ESP_LOGW(TAG, "=== CRASH CONTEXT (breadcrumb from previous boot) ===");
    ESP_LOGW(TAG, "  Boot count   : %u", (unsigned)s_bc_prev.boot_count);
    ESP_LOGW(TAG, "  Session      : %s",
             s_bc_prev.session_id[0] ? s_bc_prev.session_id : "(none active)");
    ESP_LOGW(TAG, "  Last activity: %s",
             s_bc_prev.activity[0] ? s_bc_prev.activity : "(none recorded)");
    ESP_LOGW(TAG, "  Uptime at it : %lld ms",
             (long long)(s_bc_prev.activity_us / 1000));
    ESP_LOGW(TAG, "=== END CRASH CONTEXT ===");
}

/* Human-readable names for esp_reset_reason_t values.  Indices match the
 * enum defined in esp_system.h. */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    default:                return "UNKNOWN";
    }
}

#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
/**
 * If a valid core dump exists on flash, read its summary and log the crash
 * information (task name, exception PC, backtrace addresses, ELF SHA256).
 *
 * The backtrace addresses are raw hex PCs.  They can be decoded later with:
 *   xtensa-esp32s3-elf-addr2line -pfiaC -e build/somnotrace.elf 0xADDR ...
 * or:
 *   espcoredump.py info_corefile -t b64 build/somnotrace.elf coredump.bin
 *
 * After logging, the core dump partition is erased so the same crash is not
 * reported on every subsequent boot.
 */
static void log_coredump_summary(void)
{
    /* Check whether a valid core dump image exists on flash. */
    esp_err_t chk = esp_core_dump_image_check();
    if (chk != ESP_OK) {
        /* Be specific: the common cause is that no dump was ever written,
         * because the panic handler faulted while writing one.  Blaming
         * CHECK_BOOT (which this build disables) sent the last investigation
         * down the wrong path. */
        ESP_LOGW(TAG, "no valid core dump on flash (image check: %s)",
                 esp_err_to_name(chk));
        ESP_LOGW(TAG, "  most likely the panic handler faulted while writing "
                      "it — check the console for 'Re-entered core dump!'; a "
                      "damaged task stack makes the dump unwritable");
        ESP_LOGW(TAG, "  the breadcrumb above is the primary evidence in that "
                      "case");
        return;
    }

    esp_core_dump_summary_t *summary = malloc(sizeof(esp_core_dump_summary_t));
    if (!summary) {
        /* Keep the image: it is the only copy of this crash.  It can be read
         * off the device with espcoredump.py and will be retried next boot. */
        ESP_LOGW(TAG, "core dump present but summary allocation failed — "
                      "image RETAINED for offline extraction");
        return;
    }

    esp_err_t err = esp_core_dump_get_summary(summary);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "core dump present but get_summary failed: %s — "
                      "image RETAINED for offline extraction",
                 esp_err_to_name(err));
        free(summary);
        return;
    }

    /* ── Log the crash summary ──────────────────────────────────────── */

    ESP_LOGW(TAG, "=== CRASH DETECTED (previous boot) ===");
    ESP_LOGW(TAG, "  Crashed task : %s", summary->exc_task);
    ESP_LOGW(TAG, "  Exception PC : 0x%08" PRIx32, summary->exc_pc);

    /* Backtrace (architecture-specific).  On Xtensa (ESP32-S3), the
     * exc_bt_info struct contains an array of PC values and a depth. */
    if (summary->exc_bt_info.depth > 0) {
        /* Build a single-line backtrace string for easy log parsing.
         * Max depth is typically 16 frames × 11 chars ("0xABCDEF01 ") ≈ 176. */
        char bt_line[256];
        int pos = 0;
        for (uint32_t i = 0; i < summary->exc_bt_info.depth && pos < (int)sizeof(bt_line) - 12; i++) {
            pos += snprintf(bt_line + pos, sizeof(bt_line) - pos,
                            "0x%08" PRIx32 " ", summary->exc_bt_info.bt[i]);
        }
        if (pos > 0 && bt_line[pos - 1] == ' ') bt_line[pos - 1] = '\0';
        ESP_LOGW(TAG, "  Backtrace    : %s", bt_line);
        ESP_LOGW(TAG, "  BT depth     : %" PRIu32, summary->exc_bt_info.depth);
    } else {
        ESP_LOGW(TAG, "  Backtrace    : (none available)");
    }

    /* ELF SHA256 — allows matching the crash to a specific firmware build.
     * The first 8 bytes (printed as 16 hex chars) are usually sufficient. */
    char sha_str[17];
    for (int i = 0; i < 8; i++) {
        snprintf(sha_str + i * 2, 3, "%02x", summary->app_elf_sha256[i]);
    }
    ESP_LOGW(TAG, "  ELF SHA256   : %s...", sha_str);

    ESP_LOGW(TAG, "=== END CRASH REPORT ===");

    free(summary);

    /* Erase the core dump so it is not reported again on subsequent boots. */
    esp_core_dump_image_erase();
    ESP_LOGI(TAG, "core dump partition erased");
}
#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */

void crash_diag_check(void)
{
    /* ── Always log the reset reason ─────────────────────────────────── */
    esp_reset_reason_t reason = esp_reset_reason();
    bool is_crash = (reason == ESP_RST_PANIC   ||
                     reason == ESP_RST_INT_WDT  ||
                     reason == ESP_RST_TASK_WDT ||
                     reason == ESP_RST_WDT);

    /* Take the previous boot's breadcrumb and re-arm for this one.  Must
     * happen before any subsystem writes a new note.  A power-on reset
     * leaves indeterminate RTC contents, so the previous copy is discarded
     * rather than trusted. */
    breadcrumb_rotate(reason == ESP_RST_POWERON);

    if (is_crash) {
        ESP_LOGW(TAG, "reset reason: %s (%d)", reset_reason_str(reason), (int)reason);
    } else {
        ESP_LOGI(TAG, "reset reason: %s (%d)", reset_reason_str(reason), (int)reason);
    }

    /* ── Log the watchdog configuration alongside the reason ──────────
     * An INT_WDT reset means interrupts were disabled for longer than this
     * timeout, so the configured value is needed to interpret the event at
     * all.  Logging it every boot means a report from the field carries the
     * threshold with it, and makes it obvious when the timeout has been
     * temporarily raised as a diagnostic probe rather than left at the
     * production value. */
#if defined(CONFIG_ESP_INT_WDT)
    ESP_LOGI(TAG, "watchdogs: INT_WDT enabled, timeout=%d ms",
             (int)CONFIG_ESP_INT_WDT_TIMEOUT_MS);
#else
    ESP_LOGW(TAG, "watchdogs: INT_WDT DISABLED");
#endif
#if defined(CONFIG_ESP_TASK_WDT_EN)
    ESP_LOGI(TAG, "watchdogs: TASK_WDT enabled, timeout=%d s",
             (int)CONFIG_ESP_TASK_WDT_TIMEOUT_S);
#else
    ESP_LOGI(TAG, "watchdogs: TASK_WDT disabled");
#endif
    if (reason == ESP_RST_INT_WDT) {
        ESP_LOGW(TAG, "INT_WDT: interrupts were disabled > %d ms — most often a "
                 "spin on a corrupted lock (a stack overflow into an adjacent "
                 "heap object will do this) or a long driver critical section; "
                 "check the backtrace and storage latency report below",
#if defined(CONFIG_ESP_INT_WDT)
                 (int)CONFIG_ESP_INT_WDT_TIMEOUT_MS
#else
                 0
#endif
                 );
    }

    /* ── Log boot-time heap stats ────────────────────────────────────── */
    ESP_LOGI(TAG, "[heap] boot: internal free=%u, PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ── What was in flight when it died ─────────────────────────────── */
    if (is_crash) {
        log_breadcrumb();
    }

    /* ── Core dump analysis (only if the feature is enabled) ─────────── */
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
    if (is_crash) {
        log_coredump_summary();
    }
#endif
}
