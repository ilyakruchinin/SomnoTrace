/*
 * SomnoTrace - EDF file generator for ResMed AirSense 11 sessions
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

#include "edf_gen.h"
#include "edf_internal.h"
#include "edf_header.h"
#include "edf_waveform.h"
#include "edf_annotations.h"
#include "edf_summary.h"

static const char *TAG = "edf_gen";

static void format_recording_id_from_ident(char *out, size_t out_len,
                                           int64_t epoch_ms,
                                           const cJSON *ident)
{
    const char *srn = "";
    const char *mid = "";
    const char *vid = "";
    static char mid_buf[16], vid_buf[16];

    if (ident) {
        cJSON *j;
        j = cJSON_GetObjectItem(ident, "SerialNumber");
        if (j && cJSON_IsString(j)) srn = j->valuestring;
        j = cJSON_GetObjectItem(ident, "PlatformIdentifier");
        if (j) {
            if (cJSON_IsString(j)) mid = j->valuestring;
            else if (cJSON_IsNumber(j)) { snprintf(mid_buf, sizeof(mid_buf), "%d", j->valueint); mid = mid_buf; }
        }
        j = cJSON_GetObjectItem(ident, "VariantIdentifier");
        if (j) {
            if (cJSON_IsString(j)) vid = j->valuestring;
            else if (cJSON_IsNumber(j)) { snprintf(vid_buf, sizeof(vid_buf), "%d", j->valueint); vid = vid_buf; }
        }
    }
    as11_time_format_recording_id(out, out_len, epoch_ms, srn, mid, vid);
}

esp_err_t edf_gen_generate(const char *session_dir, const char *session_id,
                           int64_t start_epoch_ms, int64_t end_epoch_ms,
                           int64_t clock_drift_ms)
{
    return edf_gen_generate_ex(SD_SDCARD_DIR, session_dir, session_id,
                               start_epoch_ms, end_epoch_ms, clock_drift_ms,
                               EDF_GEN_ALL);
}

/* Find another session's metadata file (`suffix`) in the same day folder.
 *
 * Used only when the session being exported has none of its own, which in
 * practice means it was reconstructed by crash recovery.  Picks the newest
 * candidate: session ids are timestamps, so the lexicographically largest is
 * the most recent, and therefore the closest description of the device state.
 * Returns false when the day has no other session to borrow from. */
static bool day_metadata_fallback(const char *session_dir, const char *session_id,
                                  const char *suffix, char *out_path,
                                  size_t out_len)
{
    if (!session_dir || !suffix || !out_path) return false;

    DIR *d = opendir(session_dir);
    if (!d) return false;

    char best[64] = {0};
    size_t suffix_len = strlen(suffix);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= suffix_len || len >= sizeof(best)) continue;
        if (strcmp(ent->d_name + len - suffix_len, suffix) != 0) continue;
        /* Skip the session's own (missing) file if it somehow appears. */
        if (session_id && strncmp(ent->d_name, session_id, strlen(session_id)) == 0)
            continue;
        if (!best[0] || strcmp(ent->d_name, best) > 0) {
            strlcpy(best, ent->d_name, sizeof(best));
        }
    }
    closedir(d);

    if (!best[0]) return false;
    snprintf(out_path, out_len, "%s/%s", session_dir, best);
    return true;
}

esp_err_t edf_gen_generate_ex(const char *out_root,
                              const char *session_dir, const char *session_id,
                              int64_t start_epoch_ms, int64_t end_epoch_ms,
                              int64_t clock_drift_ms, uint32_t flags)
{
    if (!session_dir || !session_id || !out_root) return ESP_ERR_INVALID_ARG;
    if (!sd_storage_is_ready()) {
        ESP_LOGW(TAG, "SD not ready, skipping EDF generation");
        return ESP_ERR_INVALID_STATE;
    }

    /* Serialise against other exports, day rebuilds and destructive actions.
     * Recursive, so a rebuild that already holds the lease can call in here. */
    bool have_lease = sd_storage_lease_acquire(SD_LEASE_EXPORT, 120000);
    if (!have_lease) {
        ESP_LOGW(TAG, "export lease unavailable, skipping EDF generation");
        return ESP_ERR_TIMEOUT;
    }
    /* Reject sessions with invalid timestamps — these result from crash
     * recovery on 0-byte .snt files and would produce bogus 19691231 folders. */
    if (start_epoch_ms < 946684800000LL) {  /* < 2000-01-01T00:00:00Z */
        ESP_LOGW(TAG, "invalid start_epoch_ms=%lld, skipping EDF generation for %s",
                 (long long)start_epoch_ms, session_id);
        sd_storage_lease_release(SD_LEASE_EXPORT);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "=== EDF GENERATION START ===");
    ESP_LOGI(TAG, "session=%s id=%s drift=%lldms",
             session_dir, session_id, (long long)clock_drift_ms);
    ESP_LOGI(TAG, "start_epoch_ms=%lld end_epoch_ms=%lld",
             (long long)start_epoch_ms, (long long)end_epoch_ms);

    /* ── Create SDCARD export directory structure ──
     * EDF files go to /somnotrace/SDCARD/DATALOG/YYYYMMDD/
     * STR.edf goes to /somnotrace/SDCARD/STR.edf (root level)
     * Identification.json goes to /somnotrace/SDCARD/
     * This is the ResMed-compatible SD card image, fully derived from .somnotrace/sessions/ */
    /* Output paths are derived from out_root so a day rebuild can generate
     * into a staging tree and publish only on success. */
    char root_datalog[200], root_settings[200];
    snprintf(root_datalog, sizeof(root_datalog), "%s/DATALOG", out_root);
    snprintf(root_settings, sizeof(root_settings), "%s/SETTINGS", out_root);

    mkdir(out_root, 0775);
    mkdir(root_datalog, 0775);
    mkdir(root_settings, 0775);

    /* The SDCARD export uses NTP-corrected time throughout: DATALOG days,
     * filenames, EDF headers, STR session boundaries, and annotations must
     * agree so importers group sessions correctly and graphs show real time. */

    /* Create date subdirectory inside DATALOG (noon-based day using AS11 offset).
     * Layout: DATALOG/YYYYMMDD/YYYYMMDD_HHMMSS_TYPE.edf */
    char day_folder[16];
    as11_time_noon_day(start_epoch_ms - clock_drift_ms, day_folder, sizeof(day_folder));

    char day_dir[240];
    snprintf(day_dir, sizeof(day_dir), "%s/%s", root_datalog, day_folder);
    mkdir(day_dir, 0775);

    /* Session timestamp prefix for EVE/CSL EDF filenames (NTP TherapyStart). */
    char ts_prefix[32];
    as11_time_format_session_prefix(start_epoch_ms, ts_prefix, sizeof(ts_prefix));

    /* ── Load post-therapy data from .somnotrace/sessions/streams/YYYYMMDD/ ──
     * Files use prefix-based naming: <session_id>_ident.json, etc. */

    /* Read identification.json for EDF header fields.
     *
     * A session recovered after a crash never ran post-therapy collection, so
     * it has no _ident.json or _settings.json of its own.  These describe the
     * device and its therapy settings — not the session's samples — so the
     * same night's other sessions carry equivalent values.  Borrowing them is
     * what makes an automatically exported recovered session usable instead of
     * headerless.  The substitution is logged so it is never silently assumed. */
    char ident_path[330];
    snprintf(ident_path, sizeof(ident_path), "%s/%s_ident.json", session_dir, session_id);
    cJSON *ident = edf_read_json_file(ident_path);
    if (!ident && day_metadata_fallback(session_dir, session_id, "_ident.json",
                                        ident_path, sizeof(ident_path))) {
        ident = edf_read_json_file(ident_path);
        if (ident) {
            ESP_LOGW(TAG, "session %s has no ident.json (recovered session?) — "
                     "using %s from the same day", session_id, ident_path);
        }
    }

    /* Read settings.json for STR.edf settings fields */
    char settings_path[330];
    snprintf(settings_path, sizeof(settings_path), "%s/%s_settings.json", session_dir, session_id);
    cJSON *settings = edf_read_json_file(settings_path);
    if (!settings && day_metadata_fallback(session_dir, session_id, "_settings.json",
                                           settings_path, sizeof(settings_path))) {
        settings = edf_read_json_file(settings_path);
        if (settings) {
            ESP_LOGW(TAG, "session %s has no settings.json (recovered session?) "
                     "— using %s from the same day", session_id, settings_path);
        }
    }
    ESP_LOGI(TAG, "settings.json: path=%s %s", settings_path,
             settings ? "loaded OK" : "FAILED to load");

    /* Path to events.snt for EVE.edf / CSL.edf generation */
    char events_snt_path[330];
    snprintf(events_snt_path, sizeof(events_snt_path), "%s/%s_events.snt", session_dir, session_id);

    /* ── Build EDF header timestamps ──
     *
     * AS11 uses two different start timestamps:
     *   - EVE/CSL: TherapyStart (session start event)
     *   - BRP/PLD/SA2: _ZLE (Zero Leak Estimate) gating signal, falling
     *     back to MaskOn if _ZLE is unavailable.  _ZLE is the AS11's
     *     actual data gating signal; MaskOn is ~7s after TherapyStart.
     * The .snt files capture from TherapyStart, so BRP/PLD/SA2 need
     * skip_samples to discard pre-_ZLE/pre-MaskOn data.
     *
     * SDCARD EDF headers and filenames use NTP time.  The STR generator
     * applies the same correction to its AS11-derived MaskOn/MaskOff values. */

    /* TherapyStart timestamp (for EVE/CSL, NTP clock domain). */
    int64_t edf_start_ms = start_epoch_ms;
    char start_date[16], start_time[16];
    as11_time_format_edf_datetime(edf_start_ms, start_date, sizeof(start_date),
                        start_time, sizeof(start_time));

    /* BRP/PLD/SA2 start timestamp (NTP clock domain).
     * Prefer _ZLE (Zero Leak Estimate) ValueChange — the AS11's actual
     * data gating signal — over MaskOn.  Fall back to MaskOn if _ZLE
     * is not available (e.g. older sessions or subscription not accepted).
     * Rationale: https://github.com/ilyakruchinin/SomnoTrace/issues/20#issuecomment-4975037843 */
    int64_t maskon_start_ms = start_epoch_ms;
    uint32_t brp_skip = 0, sa2_skip = 0, pld_skip = 0;
    uint32_t brp_max = 0, sa2_max = 0, pld_max = 0;
    /* True once a real data-gating signal (_ZLE rising or MaskOn) was found.
     * When false the AS11 never began delivering gated flow data. */
    bool therapy_gated = false;
    {
        int64_t zle_ntp = edf_find_zle_edge_time(events_snt_path, 1, clock_drift_ms);
        if (zle_ntp > 0 && zle_ntp > start_epoch_ms && zle_ntp < end_epoch_ms) {
            maskon_start_ms = zle_ntp;
            therapy_gated = true;
            int64_t skip_ms = zle_ntp - start_epoch_ms;
            brp_skip = edf_ms_to_samples_25hz(skip_ms);
            sa2_skip = edf_ms_to_samples_1hz(skip_ms);
            pld_skip = edf_ms_to_samples_pld(skip_ms);
            ESP_LOGI(TAG, "_ZLE: ntp=%lld skip_ms=%lld "
                         "brp_skip=%u sa2_skip=%u pld_skip=%u",
                     (long long)zle_ntp, (long long)skip_ms,
                     (unsigned)brp_skip, (unsigned)sa2_skip,
                     (unsigned)pld_skip);
        } else {
            if (zle_ntp > 0) {
                ESP_LOGW(TAG, "_ZLE NTP time %lld out of session range "
                             "[%lld, %lld], falling back to MaskOn",
                         (long long)zle_ntp,
                         (long long)start_epoch_ms,
                         (long long)end_epoch_ms);
            } else {
                ESP_LOGI(TAG, "_ZLE not found in events.snt, using MaskOn");
            }
            /* Fall back to MaskOn */
            int64_t maskon_as11 = edf_find_mask_on_time(events_snt_path);
            if (maskon_as11 > 0) {
                int64_t maskon_ntp = maskon_as11 + clock_drift_ms;
                if (maskon_ntp > start_epoch_ms && maskon_ntp < end_epoch_ms) {
                    maskon_start_ms = maskon_ntp;
                    therapy_gated = true;
                    int64_t skip_ms = maskon_ntp - start_epoch_ms;
                    brp_skip = edf_ms_to_samples_25hz(skip_ms);
                    sa2_skip = edf_ms_to_samples_1hz(skip_ms);
                    pld_skip = edf_ms_to_samples_pld(skip_ms);
                    ESP_LOGI(TAG, "MaskOn: as11=%lld ntp=%lld skip_ms=%lld "
                                 "brp_skip=%u sa2_skip=%u pld_skip=%u",
                             (long long)maskon_as11, (long long)maskon_ntp,
                             (long long)skip_ms,
                             (unsigned)brp_skip, (unsigned)sa2_skip,
                             (unsigned)pld_skip);
                } else {
                    ESP_LOGW(TAG, "MaskOn NTP time %lld out of session range "
                                 "[%lld, %lld], using TherapyStart",
                             (long long)maskon_ntp,
                             (long long)start_epoch_ms,
                             (long long)end_epoch_ms);
                }
            } else {
                ESP_LOGI(TAG, "MaskOn not found in events.snt, using TherapyStart "
                             "for BRP/PLD/SA2");
            }
        }

        /* BRP/PLD/SA2 end timestamp (for end truncation).
         * AS11 EDF data spans exactly the _ZLE gating window, but .snt
         * captures TherapyStart→TherapyStop, so the tail must be truncated.
         *
         * Prefer the _ZLE falling edge — the symmetric counterpart of the
         * rising edge used for the start, in the same clock domain.  Fall back
         * to the last MaskOff only when no falling edge is present.  MaskOff
         * alone is unreliable: many sessions emit no MaskOff event at all
         * (e.g. SmartStop), in which case end truncation silently never ran
         * and the file simply ended wherever the .snt capture stopped. */
        int64_t end_ntp = edf_find_zle_edge_time(events_snt_path, 0, clock_drift_ms);
        const char *end_src = "_ZLE-falling";
        if (end_ntp <= 0) {
            int64_t maskoff_as11 = edf_find_mask_off_time(events_snt_path);
            if (maskoff_as11 > 0) {
                end_ntp = maskoff_as11 + clock_drift_ms;
                end_src = "MaskOff";
            }
        }
        if (end_ntp > 0) {
            if (end_ntp > maskon_start_ms && end_ntp <= end_epoch_ms) {
                int64_t dur_ms = end_ntp - maskon_start_ms;
                brp_max = edf_ms_to_samples_25hz(dur_ms);
                sa2_max = edf_ms_to_samples_1hz(dur_ms);
                pld_max = edf_ms_to_samples_pld(dur_ms);
                ESP_LOGI(TAG, "end (%s): ntp=%lld dur_ms=%lld "
                             "brp_max=%u sa2_max=%u pld_max=%u",
                         end_src, (long long)end_ntp, (long long)dur_ms,
                         (unsigned)brp_max, (unsigned)sa2_max,
                         (unsigned)pld_max);
            } else {
                ESP_LOGW(TAG, "end (%s) NTP time %lld out of range "
                             "(%lld, %lld], no end truncation",
                         end_src, (long long)end_ntp,
                         (long long)maskon_start_ms,
                         (long long)end_epoch_ms);
            }
        } else {
            ESP_LOGI(TAG, "no _ZLE falling edge or MaskOff in events.snt, "
                          "no end truncation");
        }
    }
    /* ── Aborted session with no therapy: emit no per-session files ──
     * The AS11 writes no DATALOG files at all for a session where the mask was
     * never detected as on and no gated flow data was produced (e.g. therapy
     * started then SmartStop fired seconds later).  Previously we still emitted
     * header-only 0-record BRP/PLD/SA2 files while skipping EVE/CSL — breaking
     * an invariant the AS11 holds without exception (all 31 zero-record
     * sessions in a reference export carry a matching EVE+CSL pair) and adding
     * a spurious session to the day.
     *
     * Both conditions are required.  A session with no gating signal but a
     * meaningful amount of data (e.g. an older recording, or one where the
     * _ZLE/MaskOn subscription was not accepted) is still exported, since the
     * AS11 would have written files for it and discarding it would lose real
     * data.  "Meaningful" = at least one full 60 s data record of BRP samples. */
    bool no_therapy = false;
    if (!therapy_gated) {
        char brp_snt[300];
        snprintf(brp_snt, sizeof(brp_snt), "%s/%s_flow.snt", session_dir, session_id);
        FILE *bf = fopen(brp_snt, "rb");
        if (!bf) {
            snprintf(brp_snt, sizeof(brp_snt), "%s/%s_brp.snt", session_dir, session_id);
            bf = fopen(brp_snt, "rb");
        }
        uint32_t brp_samples = 0;
        if (bf) {
            snt_header_t bhdr;
            if (snt_read_header(bf, &bhdr) == ESP_OK)
                brp_samples = bhdr.sample_count;
            fclose(bf);
        }
        if (brp_samples < 1500) {       /* < one 60 s record at 25 Hz */
            no_therapy = true;
            ESP_LOGI(TAG, "session %s: no _ZLE/MaskOn and only %u BRP samples "
                          "(<1500) — no therapy delivered, skipping all "
                          "per-session EDF files", session_id,
                     (unsigned)brp_samples);

            /* The day folder was created before we knew there would be nothing
             * to put in it.  Leaving it behind makes the card look as though a
             * day exists, which skews anything that enumerates DATALOG — the
             * uploader's day window included.  rmdir() only succeeds on an
             * empty directory, so a day that already holds real sessions is
             * untouched. */
            if (rmdir(day_dir) == 0) {
                ESP_LOGI(TAG, "removed empty day folder %s", day_dir);
            }
        }
    }

    char maskon_date[16], maskon_time[16];
    as11_time_format_edf_datetime(maskon_start_ms, maskon_date, sizeof(maskon_date),
                        maskon_time, sizeof(maskon_time));
    char maskon_ts_prefix[32];
    as11_time_format_session_prefix(maskon_start_ms, maskon_ts_prefix,
                      sizeof(maskon_ts_prefix));

    /* Recording ID: TherapyStart for EVE/CSL, MaskOn for BRP/PLD/SA2. */
    char recording_id[128];       /* TherapyStart-based (EVE/CSL) */
    format_recording_id_from_ident(recording_id, sizeof(recording_id),
                        edf_start_ms, ident);
    char maskon_recording_id[128]; /* MaskOn-based (BRP/PLD/SA2) */
    format_recording_id_from_ident(maskon_recording_id, sizeof(maskon_recording_id),
                        maskon_start_ms, ident);

    /* Patient ID has CRC filled in by edf_write_header.
     * Initial value is the "X X X X" prefix with placeholder zeros. */
    char patient_id[81] = "X X X X 0000 0000";

    int errors = 0;

    /* ── Generate BRP.edf (25 Hz breath waveform) ──
     * v2 format: flow.snt + press.snt (separate 1-ch files).
     * v1 (backwards compat): brp.snt (single 2-ch interleaved file).
     * Try v2 first; if flow.snt doesn't exist, fall back to v1 brp.snt. */
    if ((flags & EDF_GEN_PER_SESSION) && !no_therapy) {
        char snt_path[300], edf_path[350], press_path[300];
        snprintf(snt_path, sizeof(snt_path), "%s/%s_flow.snt", session_dir, session_id);
        snprintf(press_path, sizeof(press_path), "%s/%s_press.snt", session_dir, session_id);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_BRP.edf", day_dir, maskon_ts_prefix);

        edf_signal_def_t brp_sigs[] = {
            { .label = "Flow.40ms", .transducer = "",
              .unit = "L/s", .phys_min = -2.0, .phys_max = 3.0,
              .dig_min = -1000, .dig_max = 1500,
              .prefilter = "", .samples_per_record = 1500 },
            { .label = "Press.40ms", .transducer = "",
              .unit = "cmH2O", .phys_min = 0.0, .phys_max = 40.0,
              .dig_min = 0, .dig_max = 2000,
              .prefilter = "", .samples_per_record = 1500 },
        };

        /* Check if v2 flow.snt exists; if not, fall back to v1 brp.snt */
        struct stat st_test;
        const char *press_arg = press_path;
        if (stat(snt_path, &st_test) != 0) {
            /* v1 fallback: use brp.snt (2-ch interleaved, no second file) */
            snprintf(snt_path, sizeof(snt_path), "%s/%s_brp.snt", session_dir, session_id);
            press_arg = NULL;
        }
        if (edf_convert_snt_to_edf(snt_path, edf_path, patient_id, maskon_recording_id,
                               maskon_date, maskon_time, brp_sigs, 2, "60.00",
                               NULL, brp_skip, brp_max, press_arg) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate SA2.edf (1 Hz SpO2/pulse) ── */
    if ((flags & EDF_GEN_PER_SESSION) && !no_therapy) {
        char snt_path[300], edf_path[350];
        snprintf(snt_path, sizeof(snt_path), "%s/%s_sa2.snt", session_dir, session_id);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_SA2.edf", day_dir, maskon_ts_prefix);

        edf_signal_def_t sa2_sigs[] = {
            { .label = "Pulse.1s", .transducer = "",
              .unit = "bpm", .phys_min = 0.0, .phys_max = 300.0,
              .dig_min = 0, .dig_max = 300,
              .prefilter = "", .samples_per_record = 60,
              .invalid_passthrough = true },
            { .label = "SpO2.1s", .transducer = "",
              .unit = "%", .phys_min = 0.0, .phys_max = 100.0,
              .dig_min = 0, .dig_max = 100,
              .prefilter = "", .samples_per_record = 60,
              .invalid_passthrough = true },
        };
        if (edf_convert_snt_to_edf(snt_path, edf_path, patient_id, maskon_recording_id,
                               maskon_date, maskon_time, sa2_sigs, 2, "60.00",
                               NULL, sa2_skip, sa2_max, NULL) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate PLD.edf (0.5 Hz per-breath stats) ──
     *
     * BLE quantisation note (2026-07-05 reverse-engineering):
     * The AS11 sends PLD values via BLE at phys×100 (integer physical
     * units).  Some channels have coarser BLE resolution than the AS11's
     * internal SD-card EDF:
     *   - RespRate: BLE sends integer bpm; AS11 internal EDF has 0.2 bpm
     *     resolution (dig_max=450, phys_max=90).  ~20 % of samples differ
     *     by ±0.2–1.0 bpm.
     *   - MinVent: BLE sends 0.01 L/min; AS11 internal EDF has 0.125 L/min
     *     resolution.  ~24 % of samples differ by ±0.02–0.12.
     *   - MaskPress: during pressure ramp-up (first ~20 s) BLE and SD sample
     *     at slightly different moments within the 2 s window, causing
     *     ±0.02–0.16 cmH2O differences.  Converges to ≤0.04 once stable.
     * Other channels (Press, EprPress, Leak, TidVol, Snore, FlowLim) match
     * ≥94 % at offset=0.  These differences are a fundamental BLE data-path
     * limitation and cannot be fixed by firmware changes.
     * See spec/archive/edf-as11-comparison-20260629.md §3.6. */
    if ((flags & EDF_GEN_PER_SESSION) && !no_therapy) {
        char snt_path[300], edf_path[350];
        snprintf(snt_path, sizeof(snt_path), "%s/%s_pld.snt", session_dir, session_id);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_PLD.edf", day_dir, maskon_ts_prefix);

        edf_signal_def_t pld_sigs[] = {
            { .label = "MaskPress.2s", .transducer = "",
              .unit = "cmH2O", .phys_min = 0.0, .phys_max = 40.0,
              .dig_min = 0, .dig_max = 2000,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "Press.2s", .transducer = "",
              .unit = "cmH2O", .phys_min = 0.0, .phys_max = 50.0,
              .dig_min = 0, .dig_max = 2500,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "EprPress.2s", .transducer = "",
              .unit = "cmH2O", .phys_min = 0.0, .phys_max = 30.0,
              .dig_min = 0, .dig_max = 1500,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "Leak.2s", .transducer = "",
              .unit = "L/s", .phys_min = 0.0, .phys_max = 2.0,
              .dig_min = 0, .dig_max = 100,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "RespRate.2s", .transducer = "",
              .unit = "bpm", .phys_min = 0.0, .phys_max = 90.0,
              .dig_min = 0, .dig_max = 450,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "TidVol.2s", .transducer = "",
              .unit = "L", .phys_min = 0.0, .phys_max = 4.0,
              .dig_min = 0, .dig_max = 200,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "MinVent.2s", .transducer = "",
              .unit = "L/min", .phys_min = 0.0, .phys_max = 30.0,
              .dig_min = 0, .dig_max = 240,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "Snore.2s", .transducer = "",
              .unit = "", .phys_min = 0.0, .phys_max = 5.0,
              .dig_min = 0, .dig_max = 250,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "FlowLim.2s", .transducer = "",
              .unit = "", .phys_min = 0.0, .phys_max = 1.0,
              .dig_min = 0, .dig_max = 100,
              .prefilter = "", .samples_per_record = 30,
              .invalid_passthrough = true },
        };
        /* PLD .snt has 12 channels but AS11 EDF (VID=3) only has 9.
         * Channel order in .snt: 0=MaskPress, 1=Press, 2=EprPress, 3=Leak,
         * 4=RespRate, 5=TidVol, 6=MinVent, 7=TgtVent, 8=IERatio,
         * 9=Snore, 10=FlowLim, 11=Ti
         * EDF drops TgtVent(7), IERatio(8), Ti(11). */
        static const int pld_ch_map[] = {0, 1, 2, 3, 4, 5, 6, 9, 10};
        if (edf_convert_snt_to_edf(snt_path, edf_path, patient_id, maskon_recording_id,
                               maskon_date, maskon_time, pld_sigs, 9, "60.00",
                               pld_ch_map, pld_skip, pld_max, NULL) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate STR.edf from per-day summary spool files ──
     * STR.edf goes in the SDCARD root (not inside DATALOG/) — it is a
     * multi-day cumulative file with one record per day. */
    if ((flags & EDF_GEN_SHARED) &&
        edf_generate_str_edf(out_root, patient_id, recording_id,
                         "", settings,
                         session_dir, session_id,
                         start_epoch_ms, end_epoch_ms, clock_drift_ms) != ESP_OK) {
        errors++;
    }

    /* ── Check if session is long enough for EVE/CSL ──
     * AS11 does not write EVE.edf or CSL.edf for sessions shorter than
     * one data record (60 seconds).  Match this behaviour by checking
     * the BRP .snt sample count — if it's less than one record's worth
     * (1500 samples at 25 Hz), skip EVE/CSL generation. */
    /* ── Generate EVE.edf from events.snt ──
     * Both event onsets and the EDF header use NTP-corrected time.
     *
     * These are written for every exported session, including very short ones.
     * EVE/CSL are EDF+D annotation files whose records are event-driven rather
     * than fixed-duration, so a sub-60 s session still gets its "Recording
     * starts" record — which is exactly what the AS11 does (verified: all 31
     * zero-record sessions in a reference export have EVE+CSL, none omit them).
     * The previous "session too short" skip was based on the opposite, and
     * incorrect, assumption.  Sessions with no therapy at all are excluded
     * earlier via no_therapy, which drops the whole session. */
    if ((flags & EDF_GEN_PER_SESSION) && !no_therapy) {
        char eve_path[350];
        snprintf(eve_path, sizeof(eve_path), "%s/%s_EVE.edf", day_dir, ts_prefix);
        if (edf_generate_eve_edf(eve_path, events_snt_path,
                                 start_epoch_ms, clock_drift_ms,
                                 patient_id, recording_id,
                                 start_date, start_time) != ESP_OK) {
            errors++;
        }

        /* ── Generate CSL.edf (CSR event log) from events.snt ──
         * CSL.edf contains only CSR (Cheyne-Stokes Respiration) events.
         * For sessions with no CSR events, CSL.edf contains only the
         * "Recording starts" marker record. */
        char csl_path[350];
        snprintf(csl_path, sizeof(csl_path), "%s/%s_CSL.edf", day_dir, ts_prefix);
        if (edf_generate_csl_edf(csl_path, events_snt_path,
                                 start_epoch_ms, clock_drift_ms,
                                 patient_id, recording_id,
                                 start_date, start_time) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate Identification.json + .crc ── */
    if ((flags & EDF_GEN_SHARED) &&
        edf_generate_identification_files(out_root, ident_path) != ESP_OK) {
        errors++;
    }

    /* ── Copy settings to SDCARD/SETTINGS/CurrentSettings.json ──
     * The device returns {"SettingProfiles":{...}} but AS11 nests this under
     * a "FlowGenerator" wrapper: {"FlowGenerator":{"SettingProfiles":{...}}}.
     * Use a reference wrapper so the original `settings` tree is freed once
     * below (cJSON_AddItemReferenceToObject does not transfer ownership). */
    if ((flags & EDF_GEN_SHARED) && settings) {
        cJSON *cs_root = cJSON_CreateObject();
        cJSON_AddItemReferenceToObject(cs_root, "FlowGenerator", settings);
        char *settings_str = cJSON_PrintUnformatted(cs_root);
        cJSON_Delete(cs_root);
        if (settings_str) {
            /* cJSON drops ".0" for integer-valued doubles (e.g. 7.0 → 7),
             * but AS11 preserves it for pressure/temperature fields.
             * Post-process the string to add ".0" back for known float
             * fields so the output matches AS11 byte-for-byte. */
            static const char *const float_fields[] = {
                "StartPressure", "MaxPressure", "MinPressure",
                "SetPressure", "HeatedTubeTemperature", NULL
            };
            for (int fi = 0; float_fields[fi]; fi++) {
                char pattern[64];
                snprintf(pattern, sizeof(pattern), "\"%s\":", float_fields[fi]);
                size_t plen = strlen(pattern);
                char *p = settings_str;
                while ((p = strstr(p, pattern)) != NULL) {
                    char *val_start = p + plen;
                    /* Skip optional minus sign */
                    char *v = val_start;
                    if (*v == '-') v++;
                    /* Check if value is purely integer (all digits, no '.') */
                    char *scan = v;
                    while (*scan >= '0' && *scan <= '9') scan++;
                    if (scan > v && *scan != '.') {
                        /* Integer value — insert ".0" before the terminator */
                        size_t insert_pos = scan - settings_str;
                        size_t tail_len = strlen(scan) + 1; /* includes NUL */
                        /* Realloc to make room for 2 extra bytes */
                        size_t old_len = strlen(settings_str);
                        char *tmp = realloc(settings_str, old_len + 3);
                        if (tmp) {
                            settings_str = tmp;
                            memmove(settings_str + insert_pos + 2,
                                    settings_str + insert_pos, tail_len);
                            settings_str[insert_pos] = '.';
                            settings_str[insert_pos + 1] = '0';
                        }
                        p = settings_str + insert_pos + 2;
                    } else {
                        p = scan;
                    }
                }
            }
            size_t slen = strlen(settings_str);
            char cs_path[300];
            char cs_crc_path[300];
            char cs_tmp[380];
            snprintf(cs_path, sizeof(cs_path), "%s/CurrentSettings.json", root_settings);
            snprintf(cs_crc_path, sizeof(cs_crc_path), "%s/CurrentSettings.crc", root_settings);

            bool cs_json_ok = false;
            FILE *csf = edf_open_atomic_file(cs_path, cs_tmp, sizeof(cs_tmp));
            if (csf) {
                if (!edf_write_all(csf, settings_str, slen)) {
                    ESP_LOGE(TAG, "cannot write %s: %s", cs_path, strerror(errno));
                    edf_discard_atomic_file(csf, cs_tmp);
                } else if (edf_finalize_atomic_file(csf, cs_tmp, cs_path) != ESP_OK) {
                    ESP_LOGE(TAG, "cannot finalize %s: %s", cs_path, strerror(errno));
                } else {
                    ESP_LOGI(TAG, "wrote %s", cs_path);
                    cs_json_ok = true;
                }
            } else {
                ESP_LOGE(TAG, "cannot create %s: %s", cs_path, strerror(errno));
            }

            /* Write CurrentSettings.crc (CRC-32 LE, same format as Identification.crc) */
            if (cs_json_ok) {
                csf = edf_open_atomic_file(cs_crc_path, cs_tmp, sizeof(cs_tmp));
                if (csf) {
                    uint32_t cs_crc = edf_crc32_ieee((const uint8_t *)settings_str, slen);
                    uint8_t crc_bytes[4] = {
                        (uint8_t)(cs_crc & 0xFF),
                        (uint8_t)((cs_crc >> 8) & 0xFF),
                        (uint8_t)((cs_crc >> 16) & 0xFF),
                        (uint8_t)((cs_crc >> 24) & 0xFF),
                    };
                    if (!edf_write_all(csf, crc_bytes, 4)) {
                        ESP_LOGE(TAG, "cannot write %s: %s", cs_crc_path, strerror(errno));
                        edf_discard_atomic_file(csf, cs_tmp);
                        unlink(cs_crc_path);
                        errors++;
                    } else if (edf_finalize_atomic_file(csf, cs_tmp, cs_crc_path) != ESP_OK) {
                        ESP_LOGE(TAG, "cannot finalize %s: %s", cs_crc_path, strerror(errno));
                        unlink(cs_crc_path);
                        errors++;
                    } else {
                        ESP_LOGI(TAG, "wrote %s (crc32=0x%08X)", cs_crc_path, (unsigned)cs_crc);
                    }
                } else {
                    ESP_LOGE(TAG, "cannot create %s: %s", cs_crc_path, strerror(errno));
                    unlink(cs_crc_path);
                    errors++;
                }
            } else {
                unlink(cs_crc_path);
                ESP_LOGW(TAG, "CurrentSettings.json write failed — removed stale %s", cs_crc_path);
                errors++;
            }
            free(settings_str);
        }
    }

    /* ── Cleanup ── */
    if (ident) cJSON_Delete(ident);
    if (settings) cJSON_Delete(settings);
    /* events_data no longer used */

    ESP_LOGI(TAG, "=== EDF GENERATION DONE (%d errors) ===", errors);

    sd_storage_lease_release(SD_LEASE_EXPORT);
    return errors > 0 ? ESP_FAIL : ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 12: Transactional single-day rebuild
 *
 *  Rebuilding a day is not "call the generator once per session".  Each
 *  call also rewrites the shared root artifacts, and STR.edf synthesises
 *  its current-day record from whichever single session it was given — so
 *  a naive loop leaves STR describing only the last session.  Nor is
 *  per-file atomicity enough: finalize_atomic_file() unlinks the target
 *  before renaming, so a mid-rebuild failure would leave the day's export
 *  partially destroyed.
 *
 *  This builds the whole day into a staging tree, publishes it with a
 *  directory swap only when every session succeeded, and then runs the
 *  shared pass exactly once.
 * ════════════════════════════════════════════════════════════════════ */

#define REBUILD_MAX_SESSIONS  64
#define REBUILD_STAGING_DIR   SD_MOUNT_POINT "/SDCARD/.rebuild"

/* Publish sentinel.
 *
 * Staging is failure-safe, but publication is not atomic: the live day is
 * deleted and the staged files are moved in one by one, and the shared
 * STR/Identification pass runs after that swap.  A reset in that window
 * leaves a day that looks published but is incomplete.
 *
 * The sentinel names the day currently being published and is removed only
 * after the whole rebuild succeeds.  A sentinel found at boot therefore means
 * "this day's export was interrupted", and the day is re-queued for an
 * automatic rebuild.  It is written outside the day folder because the day
 * folder itself is deleted during publication. */
#define REBUILD_SENTINEL      SD_SDCARD_DIR "/.rebuilding"

typedef struct {
    char    session_id[40];
    int64_t start_epoch_ms;
    int64_t end_epoch_ms;
    int64_t clock_drift_ms;
    bool    interrupted;    /* reconstructed by crash recovery */
} rebuild_session_t;

/* Recursively delete a directory tree (staging cleanup / old day removal). */
static void rebuild_rmtree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char child[400];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (ent->d_type == DT_DIR) rebuild_rmtree(child);
        else unlink(child);
    }
    closedir(d);
    rmdir(path);
}

/* Move every regular file from src into dst (same volume).  Used as the
 * publish step: rename() of a directory is not reliable across all FATFS
 * builds, so the contents are moved individually and verified. */
static esp_err_t rebuild_move_dir(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    if (!d) return ESP_FAIL;
    mkdir(dst, 0775);

    esp_err_t ret = ESP_OK;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_type != DT_REG) continue;
        char from[656], to[656];
        snprintf(from, sizeof(from), "%s/%s", src, ent->d_name);
        snprintf(to, sizeof(to), "%s/%s", dst, ent->d_name);
        unlink(to);
        if (rename(from, to) != 0) {
            ESP_LOGE(TAG, "rebuild: publish failed for %s: %s",
                     ent->d_name, strerror(errno));
            ret = ESP_FAIL;
            break;
        }
    }
    closedir(d);
    return ret;
}

/* Read one session manifest into a rebuild descriptor. */
static bool rebuild_read_manifest(const char *day_path, const char *fname,
                                  rebuild_session_t *out)
{
    const char *suffix = "_session.json";
    size_t slen = strlen(suffix), flen = strlen(fname);
    if (flen <= slen || strcmp(fname + flen - slen, suffix) != 0) return false;

    size_t prefix_len = flen - slen;
    if (prefix_len == 0 || prefix_len >= sizeof(out->session_id)) return false;

    char json_path[656];
    snprintf(json_path, sizeof(json_path), "%s/%s", day_path, fname);
    FILE *f = fopen(json_path, "r");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 16384) { fclose(f); return false; }
    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        ESP_LOGW(TAG, "rebuild: %s is not valid JSON, skipping", fname);
        return false;
    }

    bool ok = false;
    cJSON *js = cJSON_GetObjectItem(j, "start_epoch_ms");
    if (js && cJSON_IsNumber(js)) {
        int64_t start = (int64_t)js->valuedouble;
        if (start >= 946684800000LL) {          /* >= 2000-01-01 */
            memcpy(out->session_id, fname, prefix_len);
            out->session_id[prefix_len] = '\0';
            out->start_epoch_ms = start;
            cJSON *je = cJSON_GetObjectItem(j, "end_epoch_ms");
            cJSON *jd = cJSON_GetObjectItem(j, "clock_drift_ms");
            out->end_epoch_ms = (je && cJSON_IsNumber(je)) ? (int64_t)je->valuedouble : 0;
            out->clock_drift_ms = (jd && cJSON_IsNumber(jd)) ? (int64_t)jd->valuedouble : 0;

            /* An unusable drift estimate is still better than 0 (which would
             * put every spool-sourced timestamp ~7-8 min out), but say so. */
            /* Sessions rebuilt from a crash have partially damaged raw data by
             * definition; the day rebuild treats their failures differently. */
            cJSON *jst = cJSON_GetObjectItem(j, "state");
            out->interrupted = (jst && cJSON_IsString(jst) &&
                                strcmp(jst->valuestring, "interrupted") == 0);

            cJSON *ju = cJSON_GetObjectItem(j, "clock_drift_usable");
            cJSON *jv = cJSON_GetObjectItem(j, "clock_drift_valid");
            bool measured = jv && cJSON_IsTrue(jv);
            bool usable = ju ? cJSON_IsTrue(ju) : measured;
            if (!measured) {
                ESP_LOGW(TAG, "rebuild: %s drift is an estimate (usable=%d)",
                         out->session_id, (int)usable);
            }
            ok = true;
        } else {
            ESP_LOGW(TAG, "rebuild: skipping %s (invalid start_epoch_ms=%lld)",
                     fname, (long long)start);
        }
    }
    cJSON_Delete(j);
    return ok;
}

esp_err_t edf_gen_rebuild_day(const char *day_folder)
{
    if (!day_folder || strlen(day_folder) != 8) return ESP_ERR_INVALID_ARG;
    if (!sd_storage_is_ready()) return ESP_ERR_INVALID_STATE;

    /* Hold the lease for the entire transaction: no other export, no upload
     * of this day, and no destructive action may interleave. */
    if (!sd_storage_lease_acquire(SD_LEASE_EXPORT, 120000)) {
        ESP_LOGE(TAG, "rebuild %s: storage busy", day_folder);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "=== REBUILD DAY %s ===", day_folder);

    char day_path[300];
    snprintf(day_path, sizeof(day_path), "%s/%s", SD_STREAMS_DIR, day_folder);

    rebuild_session_t *sessions = calloc(REBUILD_MAX_SESSIONS,
                                         sizeof(rebuild_session_t));
    if (!sessions) {
        sd_storage_lease_release(SD_LEASE_EXPORT);
        return ESP_ERR_NO_MEM;
    }

    int n = 0;
    bool truncated = false;
    DIR *d = opendir(day_path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_type != DT_REG) continue;
            if (n >= REBUILD_MAX_SESSIONS) { truncated = true; break; }
            if (rebuild_read_manifest(day_path, ent->d_name, &sessions[n])) n++;
        }
        closedir(d);
    }

    if (truncated) {
        /* Silently dropping sessions would produce a day that looks complete
         * but is not, so this is a hard error. */
        ESP_LOGE(TAG, "rebuild %s: more than %d sessions — refusing to "
                 "publish a partial day", day_folder, REBUILD_MAX_SESSIONS);
        free(sessions);
        sd_storage_lease_release(SD_LEASE_EXPORT);
        return ESP_ERR_INVALID_SIZE;
    }
    if (n == 0) {
        ESP_LOGW(TAG, "rebuild %s: no usable sessions found", day_folder);
        free(sessions);
        sd_storage_lease_release(SD_LEASE_EXPORT);
        return ESP_ERR_NOT_FOUND;
    }

    /* Chronological order (insertion sort — n is small). */
    for (int i = 1; i < n; i++) {
        rebuild_session_t tmp = sessions[i];
        int j = i - 1;
        while (j >= 0 && sessions[j].start_epoch_ms > tmp.start_epoch_ms) {
            sessions[j + 1] = sessions[j];
            j--;
        }
        sessions[j + 1] = tmp;
    }

    ESP_LOGI(TAG, "rebuild %s: %d session(s)", day_folder, n);

    /* ── Pass 1: per-session artifacts into staging ── */
    rebuild_rmtree(REBUILD_STAGING_DIR);
    mkdir(REBUILD_STAGING_DIR, 0775);

    esp_err_t ret = ESP_OK;
    int degraded = 0;
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "rebuild %s: session %d/%d: %s",
                 day_folder, i + 1, n, sessions[i].session_id);
        esp_err_t r = edf_gen_generate_ex(REBUILD_STAGING_DIR, day_path,
                                         sessions[i].session_id,
                                         sessions[i].start_epoch_ms,
                                         sessions[i].end_epoch_ms,
                                         sessions[i].clock_drift_ms,
                                         EDF_GEN_PER_SESSION);
        if (r != ESP_OK) {
            if (sessions[i].interrupted) {
                /* A crash-recovered fragment can be damaged in ways no amount
                 * of retrying will fix.  Letting it veto the rebuild would
                 * lose the whole night — every healthy session included — which
                 * is a far worse outcome than exporting the night without this
                 * one fragment.  Loud, not silent: the skipped session is named
                 * here and counted in the completion line. */
                ESP_LOGE(TAG, "rebuild %s: interrupted session %s could not be "
                         "exported (%s) — SKIPPING it and continuing with the "
                         "rest of the day", day_folder,
                         sessions[i].session_id, esp_err_to_name(r));
                degraded++;
                continue;
            }
            ESP_LOGE(TAG, "rebuild %s: session %s failed (%s) — aborting, "
                     "existing export left untouched", day_folder,
                     sessions[i].session_id, esp_err_to_name(r));
            ret = r;
            break;
        }
    }

    if (ret != ESP_OK) {
        rebuild_rmtree(REBUILD_STAGING_DIR);
        free(sessions);
        sd_storage_lease_release(SD_LEASE_EXPORT);
        return ret;
    }

    /* ── Publish: swap the staged day into place ──
     * Only now is the previous export replaced.  Up to this point a failure
     * costs nothing. */
    char staged_day[400], live_day[400];
    snprintf(staged_day, sizeof(staged_day), "%s/DATALOG/%s",
             REBUILD_STAGING_DIR, day_folder);
    snprintf(live_day, sizeof(live_day), "%s/%s", SD_SDCARD_DATALOG, day_folder);

    struct stat st;
    if (stat(staged_day, &st) != 0) {
        ESP_LOGE(TAG, "rebuild %s: staging produced no day folder", day_folder);
        rebuild_rmtree(REBUILD_STAGING_DIR);
        free(sessions);
        sd_storage_lease_release(SD_LEASE_EXPORT);
        return ESP_FAIL;
    }

    mkdir(SD_SDCARD_DIR, 0775);
    mkdir(SD_SDCARD_DATALOG, 0775);

    /* Mark the day as mid-publication before anything is destroyed, so an
     * interruption from here on is detectable on the next boot. */
    {
        FILE *sf = fopen(REBUILD_SENTINEL, "w");
        if (sf) {
            fprintf(sf, "%s\n", day_folder);
            fflush(sf);
            fsync(fileno(sf));
            fclose(sf);
        } else {
            ESP_LOGW(TAG, "rebuild %s: could not write publish sentinel — an "
                     "interrupted publish will not self-heal", day_folder);
        }
    }

    rebuild_rmtree(live_day);
    ret = rebuild_move_dir(staged_day, live_day);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rebuild %s: publish failed — day may be incomplete, "
                 "re-run the rebuild", day_folder);
        rebuild_rmtree(REBUILD_STAGING_DIR);
        free(sessions);
        sd_storage_lease_release(SD_LEASE_EXPORT);
        return ret;
    }
    rebuild_rmtree(REBUILD_STAGING_DIR);

    /* ── Pass 2: shared artifacts, exactly once ──
     * STR.edf is multi-day and cumulative, and its current-day record is
     * synthesised from the session given here — so it must be the newest
     * session of the day, not an arbitrary one.  Identification and
     * CurrentSettings likewise come from the newest session's captured data. */
    const rebuild_session_t *newest = &sessions[n - 1];
    esp_err_t shared = edf_gen_generate_ex(SD_SDCARD_DIR, day_path,
                                          newest->session_id,
                                          newest->start_epoch_ms,
                                          newest->end_epoch_ms,
                                          newest->clock_drift_ms,
                                          EDF_GEN_SHARED);
    if (shared != ESP_OK) {
        /* The day's per-session files are published and correct; only the
         * cumulative/shared files are stale.  Report it without claiming the
         * rebuild succeeded. */
        ESP_LOGE(TAG, "rebuild %s: shared pass (STR/Identification) failed",
                 day_folder);
        free(sessions);
        sd_storage_lease_release(SD_LEASE_EXPORT);
        return shared;
    }

    /* Fully published, including the shared pass: the day is consistent, so
     * retire the sentinel.  Every failure path above deliberately leaves it in
     * place so the day is rebuilt again. */
    unlink(REBUILD_SENTINEL);

    if (degraded > 0) {
        ESP_LOGW(TAG, "=== REBUILD DAY %s COMPLETE (%d sessions, %d skipped as "
                 "unexportable) ===", day_folder, n - degraded, degraded);
    } else {
        ESP_LOGI(TAG, "=== REBUILD DAY %s COMPLETE (%d sessions) ===",
                 day_folder, n);
    }
    free(sessions);
    sd_storage_lease_release(SD_LEASE_EXPORT);
    return ESP_OK;
}

bool edf_gen_take_interrupted_rebuild(char *out_day, size_t out_len)
{
    if (!out_day || out_len < 9) return false;

    FILE *f = fopen(REBUILD_SENTINEL, "r");
    if (!f) return false;

    char buf[32] = {0};
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';

    /* Trim whitespace/newline. */
    for (char *p = buf; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == ' ') { *p = '\0'; break; }
    }

    bool valid = (strlen(buf) == 8);
    for (int i = 0; valid && i < 8; i++) {
        if (buf[i] < '0' || buf[i] > '9') valid = false;
    }

    /* Consume it either way: a malformed sentinel would otherwise be reported
     * on every boot for ever. */
    unlink(REBUILD_SENTINEL);
    if (!valid) {
        ESP_LOGW(TAG, "discarding malformed publish sentinel");
        return false;
    }

    strlcpy(out_day, buf, out_len);
    return true;
}
