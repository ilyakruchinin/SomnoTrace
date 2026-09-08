/*
 * SomnoTrace - Upload scanner (derives upload units from the SD card tree)
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

#include "upload_scan.h"
#include "upload_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"

static const char *TAG = "up_scan";

/* Read size for content fingerprinting (see fold_fp). */
#define FP_CHUNK  1024

/* ── Filename parsing ─────────────────────────────────────────────────
 * Session EDFs are always "<YYYYMMDD>_<HHMMSS>_<KIND>.edf" — edf_gen builds
 * the prefix with session_timestamp(), which never adds the DST collision
 * suffix used in the raw session store. */

static uint8_t kind_from_name(const char *name, size_t len)
{
    if (len < 8) return 0;
    /* Compare the four characters before ".edf". */
    const char *k = name + len - 8;      /* "_KKK.edf" */
    if (k[0] != '_') return 0;
    if (strncmp(k + 1, "BRP", 3) == 0) return UGK_BRP;
    if (strncmp(k + 1, "PLD", 3) == 0) return UGK_PLD;
    if (strncmp(k + 1, "SA2", 3) == 0) return UGK_SA2;
    if (strncmp(k + 1, "EVE", 3) == 0) return UGK_EVE;
    if (strncmp(k + 1, "CSL", 3) == 0) return UGK_CSL;
    return 0;
}

/* Validate "YYYYMMDD_HHMMSS" and return seconds-since-midnight, or UINT32_MAX. */
static uint32_t parse_prefix(const char *name)
{
    for (int i = 0; i < 15; i++) {
        if (i == 8) {
            if (name[i] != '_') return UINT32_MAX;
        } else if (name[i] < '0' || name[i] > '9') {
            return UINT32_MAX;
        }
    }
    int h = (name[9]  - '0') * 10 + (name[10] - '0');
    int m = (name[11] - '0') * 10 + (name[12] - '0');
    int s = (name[13] - '0') * 10 + (name[14] - '0');
    if (h > 23 || m > 59 || s > 59) return UINT32_MAX;
    return (uint32_t)(h * 3600 + m * 60 + s);
}

/* ── Day enumeration ──────────────────────────────────────────────── */

int upload_scan_days(uint32_t *out_days, int max_out)
{
    if (!out_days || max_out <= 0) return 0;

    DIR *d = opendir(SD_SDCARD_DATALOG);
    if (!d) return 0;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max_out) {
        if (ent->d_name[0] == '.') continue;
        if (strlen(ent->d_name) != 8) continue;
        bool digits = true;
        for (int i = 0; i < 8; i++) {
            if (ent->d_name[i] < '0' || ent->d_name[i] > '9') digits = false;
        }
        if (!digits) continue;
        out_days[n++] = (uint32_t)strtoul(ent->d_name, NULL, 10);
    }
    closedir(d);

    /* Newest first — uploads should favour the most recent night. */
    for (int i = 1; i < n; i++) {
        uint32_t v = out_days[i];
        int j = i - 1;
        while (j >= 0 && out_days[j] < v) { out_days[j + 1] = out_days[j]; j--; }
        out_days[j + 1] = v;
    }
    return n;
}

/* ── Group enumeration ────────────────────────────────────────────── */

int upload_scan_day_groups(const char *day, upload_group_ref_t *out, int max_out)
{
    if (!day || !out || max_out <= 0) return 0;

    char dir_path[160];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", SD_SDCARD_DATALOG, day);

    DIR *d = opendir(dir_path);
    if (!d) return 0;

    int n_groups = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t len = strlen(nm);
        if (len < 23 || len >= UPLOAD_FILENAME_LEN) continue;
        if (strcmp(nm + len - 4, ".edf") != 0) continue;

        uint8_t kind = kind_from_name(nm, len);
        if (!kind) continue;
        uint32_t psec = parse_prefix(nm);
        if (psec == UINT32_MAX) continue;

        /* Find or start the group for this prefix. */
        int gi = -1;
        for (int i = 0; i < n_groups; i++) {
            if (out[i].prefix_sec == psec) { gi = i; break; }
        }
        if (gi < 0) {
            if (n_groups >= max_out) {
                ESP_LOGW(TAG, "day %s has more than %d groups — ignoring extras",
                         day, max_out);
                continue;
            }
            gi = n_groups++;
            memset(&out[gi], 0, sizeof(out[gi]));
            strlcpy(out[gi].day, day, sizeof(out[gi].day));
            memcpy(out[gi].prefix, nm, 15);
            out[gi].prefix[15] = '\0';
            out[gi].prefix_sec = psec;
        }

        if (out[gi].n_files < UPLOAD_GROUP_MAX_FILES) {
            strlcpy(out[gi].files[out[gi].n_files], nm, UPLOAD_FILENAME_LEN);
            out[gi].n_files++;
            out[gi].kinds |= kind;
        }
    }
    closedir(d);

    /* Chronological, so an interrupted run resumes in a sensible order. */
    for (int i = 1; i < n_groups; i++) {
        upload_group_ref_t tmp = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].prefix_sec > tmp.prefix_sec) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tmp;
    }
    return n_groups;
}

/* ── Root bundle ──────────────────────────────────────────────────── */

/* Fold one member's name, size and CONTENT into the running digest.
 *
 * Content, deliberately, not mtime: edf_gen rewrites all five of these files
 * on every export (temp file + rename), so their mtime changes even when the
 * bytes are identical.  Fingerprinting metadata therefore reported "changed"
 * after every single export and made the uploader connect to re-send files
 * the backend already had — including after a short session that produced no
 * session EDFs at all.
 *
 * Reading them costs ~50 KB per scan (STR.edf dominates) and a few
 * milliseconds.  That is a fair price for "changed" actually meaning changed;
 * session EDFs still need no hashing, because they are immutable once written
 * and replacement is signalled explicitly. */
static uint64_t fold_fp(uint64_t fp, const char *name, const char *path,
                        off_t size)
{
    uint64_t h = fp ? fp : 1469598103934665603ULL;   /* FNV-1a offset basis */
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)size;      h *= 1099511628211ULL;

    uint32_t crc = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t *buf = heap_caps_malloc(FP_CHUNK, MALLOC_CAP_SPIRAM);
        if (!buf) buf = malloc(FP_CHUNK);
        if (buf) {
            size_t n;
            while ((n = fread(buf, 1, FP_CHUNK, f)) > 0) {
                crc = esp_rom_crc32_le(crc, buf, n);
            }
            free(buf);
        } else {
            /* Out of memory: fall back to size only.  The digest is then
             * weaker but never wrong in a way that skips a real change,
             * because size is still folded in. */
            ESP_LOGW(TAG, "no memory to hash %s, using size only", name);
        }
        fclose(f);
    }
    h ^= (uint64_t)crc;       h *= 1099511628211ULL;
    return h;
}

bool upload_scan_bundle(upload_bundle_ref_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    static const char *root_names[] = {
        "STR.edf", "Identification.json", "Identification.crc", NULL
    };

    bool have_str = false;
    for (int i = 0; root_names[i]; i++) {
        char path[100];
        snprintf(path, sizeof(path), "%s/%s", SD_SDCARD_DIR, root_names[i]);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (out->n_files < UPLOAD_BUNDLE_MAX_FILES) {
            strlcpy(out->paths[out->n_files], path, sizeof(out->paths[0]));
            strlcpy(out->names[out->n_files], root_names[i], sizeof(out->names[0]));
            out->in_settings[out->n_files] = false;
            out->n_files++;
            out->fp = fold_fp(out->fp, root_names[i], path, st.st_size);
        }
        if (i == 0) have_str = true;
    }

    DIR *d = opendir(SD_SDCARD_SETTINGS);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (out->n_files >= UPLOAD_BUNDLE_MAX_FILES) break;

            char path[320];
            int plen = snprintf(path, sizeof(path), "%s/%s",
                                SD_SDCARD_SETTINGS, ent->d_name);
            /* Skip rather than silently truncate: a truncated path would be
             * uploaded under the wrong name or fail to open. */
            if (plen < 0 || (size_t)plen >= sizeof(out->paths[0]) ||
                strlen(ent->d_name) >= sizeof(out->names[0])) {
                ESP_LOGW(TAG, "settings entry name too long, skipped: %s",
                         ent->d_name);
                continue;
            }
            struct stat st;
            if (stat(path, &st) != 0) continue;

            strlcpy(out->paths[out->n_files], path, sizeof(out->paths[0]));
            strlcpy(out->names[out->n_files], ent->d_name, sizeof(out->names[0]));
            out->in_settings[out->n_files] = true;
            out->n_files++;
            out->fp = fold_fp(out->fp, ent->d_name, path, st.st_size);
        }
        closedir(d);
    }

    /* Without STR.edf a backend cannot interpret any session, so treat the
     * bundle as unavailable rather than uploading a useless partial set. */
    if (!have_str) {
        ESP_LOGW(TAG, "no STR.edf in %s — nothing to upload yet", SD_SDCARD_DIR);
        return false;
    }
    return true;
}

/* ── Reconciliation ───────────────────────────────────────────────── */

int upload_scan_reconcile_day(uint32_t day)
{
    char daystr[12];
    snprintf(daystr, sizeof(daystr), "%08u", (unsigned)day);

    upload_group_ref_t *refs = heap_caps_calloc(UPLOAD_MAX_GROUPS_PER_DAY, sizeof(*refs), MALLOC_CAP_SPIRAM);
    if (!refs) refs = calloc(UPLOAD_MAX_GROUPS_PER_DAY, sizeof(*refs));
    if (!refs) return 0;

    int n = upload_scan_day_groups(daystr, refs, UPLOAD_MAX_GROUPS_PER_DAY);
    if (n == 0) {
        /* Day folder gone or empty: forget it so state does not linger. */
        if (upload_index_day(day, false)) {
            ESP_LOGI(TAG, "day %s has no EDF groups — dropping state", daystr);
            upload_index_forget_day(day);
        }
        free(refs);
        return 0;
    }

    upload_day_t *d = upload_index_day(day, true);
    if (!d) { free(refs); return 0; }

    /* Add new groups; refresh any whose file set changed. */
    for (int i = 0; i < n; i++) {
        upload_group_t *g = upload_index_group(d, refs[i].prefix_sec, true);
        if (!g) continue;

        bool is_new = (g->n_files == 0 && g->kinds == 0);
        bool changed = (!is_new && (g->n_files != refs[i].n_files ||
                                    g->kinds != refs[i].kinds));

        if (is_new) {
            g->kinds = refs[i].kinds;
            g->n_files = (uint8_t)refs[i].n_files;
            d->dirty = true;
            ESP_LOGI(TAG, "new group %s (%d file(s))", refs[i].prefix,
                     refs[i].n_files);
        } else if (changed) {
            /* More kinds appeared for this prefix (or fewer). Re-upload it. */
            ESP_LOGI(TAG, "group %s changed (%u->%d files) — re-uploading",
                     refs[i].prefix, g->n_files, refs[i].n_files);
            g->kinds = refs[i].kinds;
            g->n_files = (uint8_t)refs[i].n_files;
            for (int b = 0; b < UPLOAD_MAX_BACKENDS; b++) {
                g->be[b].status = UG_PENDING;
                g->be[b].attempts = 0;
            }
            d->dirty = true;
        }
    }

    /* Drop groups that no longer exist on the card. */
    for (int i = d->n_groups - 1; i >= 0; i--) {
        bool found = false;
        for (int k = 0; k < n; k++) {
            if (refs[k].prefix_sec == d->groups[i].prefix_sec) { found = true; break; }
        }
        if (!found) {
            ESP_LOGI(TAG, "day %s group %06u gone — dropping",
                     daystr, (unsigned)d->groups[i].prefix_sec);
            upload_index_drop_group(d, d->groups[i].prefix_sec);
        }
    }

    upload_index_save_day(d);
    free(refs);
    return d->n_groups;
}

int upload_scan_reconcile_all(int max_days, const int *slots, int n_slots)
{
    if (max_days <= 0) max_days = UPLOAD_DEFAULT_MAX_DAYS;
    if (max_days > UPLOAD_MAX_DAYS_CAP) max_days = UPLOAD_MAX_DAYS_CAP;

    uint32_t *days = heap_caps_calloc(UPLOAD_MAX_DAYS_CAP, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!days) days = calloc(UPLOAD_MAX_DAYS_CAP, sizeof(uint32_t));
    if (!days) return 0;

    int n_days = upload_scan_days(days, UPLOAD_MAX_DAYS_CAP);

    /* Only days that actually contain groups consume a window slot.  A folder
     * with no EDF files still gets reconciled (which drops any stale state for
     * it) but must not push a real day out of the window — otherwise a handful
     * of empty folders silently shrinks how much history is kept in sync. */
    int i = 0, with_data = 0, empty = 0;
    for (; i < n_days && with_data < max_days; i++) {
        if (upload_scan_reconcile_day(days[i]) > 0) with_data++;
        else empty++;
    }

    /* Days beyond the window: forget their state so the directory and the
     * progress denominator stay bounded. */
    int beyond = 0;
    for (; i < n_days; i++) {
        if (upload_index_day(days[i], false)) upload_index_forget_day(days[i]);
        beyond++;
    }

    /* Forget orphan days: index entries whose SD card folder no longer
     * exists.  The card is the truth about which files exist — a stale
     * state file (e.g. after a manual folder deletion) must not inflate
     * the progress count.  Guarded by n_days > 0 so a transient card
     * absence (remount, glitch) does not wipe the entire index. */
    int orphaned = 0;
    if (n_days > 0) {
        int n_idx = upload_index_day_count();
        for (int j = n_idx - 1; j >= 0; j--) {
            upload_day_t *d = upload_index_day_at(j);
            if (!d) continue;
            bool found = false;
            for (int k = 0; k < n_days; k++) {
                if (days[k] == d->day) { found = true; break; }
            }
            if (!found) {
                upload_index_forget_day(d->day);
                orphaned++;
            }
        }
    }

    int pending = 0;
    for (int s = 0; s < n_slots; s++) {
        pending += upload_index_backend_pending(slots[s], max_days);
    }

    ESP_LOGI(TAG, "scan: %d folder(s) on card, %d day(s) with data in window, "
             "%d empty, %d beyond window, %d orphaned, %d unit(s) pending",
             n_days, with_data, empty, beyond, orphaned, pending);
    free(days);
    return pending;
}
