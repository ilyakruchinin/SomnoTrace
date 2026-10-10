/*
 * SomnoTrace - SMB upload backend using libsmb2
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

#include "uploader.h"
#include "upload_paths.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include <sys/time.h>
#include <sys/poll.h>

/* SD card paths — must match sd_storage.h */

static const char *TAG = "upload_smb";

/* Write buffer size — allocated in PSRAM */
#define SMB_BUF_SIZE  (32 * 1024)

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Sync callback for smb2_cmd_set_info_async */
struct set_info_sync {
    int is_finished;
    int status;
};

static void set_info_cb(struct smb2_context *smb2, int status,
                        void *command_data, void *private_data)
{
    struct set_info_sync *sync = private_data;
    sync->is_finished = 1;
    sync->status = status;
}

/* Set the last-write and change timestamps on an open SMB file handle
 * to match the local file's mtime.  Uses SMB2 SET_INFO with
 * FILE_BASIC_INFORMATION. */
static int smb_set_mtime(struct smb2_context *smb2, struct smb2fh *fh,
                         time_t mtime)
{
    struct smb2_file_basic_info bi;
    struct smb2_set_info_request si_req;
    struct set_info_sync sync = {0, 0};
    struct smb2_pdu *pdu;

    memset(&bi, 0, sizeof(bi));
    /* Setting times to 0 means "don't change" in SMB2 */
    bi.creation_time.tv_sec = 0;
    bi.creation_time.tv_usec = 0;
    bi.last_access_time.tv_sec = 0;
    bi.last_access_time.tv_usec = 0;
    bi.last_write_time.tv_sec = mtime;
    bi.last_write_time.tv_usec = 0;
    bi.change_time.tv_sec = mtime;
    bi.change_time.tv_usec = 0;
    bi.file_attributes = 0;  /* 0 = don't change */

    memset(&si_req, 0, sizeof(si_req));
    si_req.info_type = SMB2_0_INFO_FILE;
    si_req.file_info_class = SMB2_FILE_BASIC_INFORMATION;
    si_req.additional_information = 0;
    smb2_file_id *fid = smb2_get_file_id(fh);
    memcpy(si_req.file_id, fid, SMB2_FD_SIZE);
    si_req.input_data = &bi;

    pdu = smb2_cmd_set_info_async(smb2, &si_req, set_info_cb, &sync);
    if (!pdu) {
        ESP_LOGW(TAG, "  smb2_cmd_set_info_async failed: %s",
                 smb2_get_error(smb2));
        return -1;
    }
    smb2_queue_pdu(smb2, pdu);

    /* Poll loop — same pattern as libsmb2's sync.c wait_for_reply() */
    while (!sync.is_finished) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = smb2_get_fd(smb2);
        pfd.events = smb2_which_events(smb2);
        if (poll(&pfd, 1, 1000) < 0) {
            ESP_LOGW(TAG, "  set_info poll failed");
            return -1;
        }
        if (pfd.revents == 0) continue;
        if (smb2_service(smb2, pfd.revents) < 0) {
            ESP_LOGW(TAG, "  set_info smb2_service failed: %s",
                     smb2_get_error(smb2));
            return -1;
        }
    }

    return sync.status;
}

/* Upload a single local file to an SMB path (one attempt). */
static upload_result_t smb_upload_file_once(struct smb2_context *smb2,
                                            const char *local_path,
                                            const char *remote_path)
{
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "  cannot open %s: %s", local_path, strerror(errno));
        return UPLOAD_FAILED;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Open remote file for writing */
    struct smb2fh *fh = smb2_open(smb2, remote_path, O_WRONLY | O_CREAT);
    if (!fh) {
        ESP_LOGW(TAG, "  smb2_open(%s) failed: %s", remote_path,
                 smb2_get_error(smb2));
        /* Try creating parent directory and retry */
        char dir_path[512];
        strlcpy(dir_path, remote_path, sizeof(dir_path));
        char *slash = strrchr(dir_path, '/');
        if (slash) {
            *slash = '\0';
            ESP_LOGI(TAG, "  trying mkdir %s", dir_path);
            smb2_mkdir(smb2, dir_path);
            *slash = '/';
        }
        fh = smb2_open(smb2, remote_path, O_WRONLY | O_CREAT);
        if (!fh) {
            ESP_LOGE(TAG, "  smb2_open retry failed: %s", smb2_get_error(smb2));
            fclose(f);
            return UPLOAD_FAILED;
        }
    }

    /* Allocate read buffer in PSRAM */
    uint8_t *buf = heap_caps_malloc(SMB_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        buf = malloc(SMB_BUF_SIZE);
    }
    if (!buf) {
        ESP_LOGE(TAG, "  buffer alloc failed");
        smb2_close(smb2, fh);
        fclose(f);
        return UPLOAD_FAILED;
    }

    size_t total_written = 0;
    int last_pct = -1;

    while (total_written < file_size) {
        size_t to_read = SMB_BUF_SIZE;
        if (to_read > file_size - total_written)
            to_read = file_size - total_written;

        size_t rd = fread(buf, 1, to_read, f);
        if (rd == 0) break;

        int wr = smb2_pwrite(smb2, fh, buf, rd, total_written);
        if (wr < 0) {
            ESP_LOGE(TAG, "  smb2_pwrite failed at offset %u: %s",
                     (unsigned)total_written, smb2_get_error(smb2));
            free(buf);
            smb2_close(smb2, fh);
            fclose(f);
            return UPLOAD_FAILED;
        }

        total_written += wr;

        int pct = (int)(total_written * 100 / file_size);
        if (pct != last_pct && pct % 25 == 0) {
            ESP_LOGI(TAG, "  %s: %d%% (%u/%u)",
                     strrchr(local_path, '/') ? strrchr(local_path, '/') + 1 : local_path,
                     pct, (unsigned)total_written, (unsigned)file_size);
            last_pct = pct;
        }
    }

    free(buf);

    /* Set remote file timestamps to match local file mtime */
    struct stat local_st;
    if (stat(local_path, &local_st) == 0) {
        int rc = smb_set_mtime(smb2, fh, local_st.st_mtime);
        if (rc != 0) {
            ESP_LOGW(TAG, "  smb_set_mtime failed: %d (continuing)", rc);
        }
    }

    smb2_close(smb2, fh);
    fclose(f);

    ESP_LOGI(TAG, "  uploaded %s (%u bytes)", remote_path, (unsigned)total_written);
    return UPLOAD_OK;
}

/* Upload all .edf files from a DATALOG day folder. */
/* ── Backend interface ──────────────────────────────────────────────
 *
 * One SMB session spans the whole run; day_begin() only has to make sure the
 * remote folder exists.  Group transfers are all-or-nothing so the scheduler
 * can mark a unit uploaded only when every file in it landed. */

static struct smb2_context *s_smb;      /* live for the whole run */
static char s_remote_base[256];
static bool s_signed_session;           /* s_smb was opened with signing on */
static bool s_sign_learned;             /* a signed upload worked this boot */

static bool smb_is_configured(void)
{
    return uploader_is_smb_configured();
}

static bool smb_open_session(const uploader_config_t *cfg, bool with_sign)
{
    s_smb = smb2_init_context();
    if (!s_smb) {
        ESP_LOGE(TAG, "smb2_init_context failed");
        return false;
    }

    smb2_set_security_mode(s_smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (with_sign) smb2_set_sign(s_smb, 1);
    const char *user = cfg->smb_user[0] ? cfg->smb_user : "Guest";
    smb2_set_user(s_smb, user);
    if (cfg->smb_pass[0]) smb2_set_password(s_smb, cfg->smb_pass);

    ESP_LOGI(TAG, "connecting to %s/%s as %s%s", cfg->smb_host, cfg->smb_share,
             user, with_sign ? " (signed)" : "");

    if (smb2_connect_share(s_smb, cfg->smb_host, cfg->smb_share, user) != 0) {
        ESP_LOGE(TAG, "smb2_connect_share failed: %s", smb2_get_error(s_smb));
        smb2_destroy_context(s_smb);
        s_smb = NULL;
        return false;
    }
    s_signed_session = with_sign;
    ESP_LOGI(TAG, "SMB connected%s", with_sign ? " (signed)" : "");

    /* SMB paths are relative to the share root: a leading slash makes
     * Windows return STATUS_INVALID_PARAMETER. */
    const char *p = cfg->smb_path;
    while (*p == '/' || *p == '\\') p++;
    snprintf(s_remote_base, sizeof(s_remote_base), "%s", p);

    char remote_datalog[400];
    snprintf(remote_datalog, sizeof(remote_datalog), "%s/DATALOG", s_remote_base);
    smb2_mkdir(s_smb, remote_datalog);
    return true;
}

static void smb_session_end(void)
{
    if (!s_smb) return;
    smb2_disconnect_share(s_smb);
    smb2_destroy_context(s_smb);
    s_smb = NULL;
    s_signed_session = false;
}

/* Connect, preferring the learned signing mode. A learned flag that stops
 * working (server config changed, different host) falls back to unsigned so
 * the preference can never wedge the uploader. */
static bool smb_connect_any(const uploader_config_t *cfg)
{
    if (s_sign_learned && smb_open_session(cfg, true)) return true;
    s_sign_learned = false;
    return smb_open_session(cfg, false);
}

static upload_result_t smb_session_begin(void)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);
    if (!cfg.smb_host[0] || !cfg.smb_share[0]) return UPLOAD_NOT_CONFIGURED;

    if (!smb_connect_any(&cfg)) {
        /* Unreachable host and bad credentials are indistinguishable here
         * without parsing the error text, so treat as transient and let the
         * cooldown ladder slow it down. */
        return UPLOAD_ERR_TRANSIENT;
    }
    return UPLOAD_OK;
}

/* Adaptive SMB signing. Some hardened servers (Windows 11 24H2+, per-share
 * "RequireSigning") advertise signing as only *enabled* in negotiate, then
 * reject unsigned requests with STATUS_INVALID_PARAMETER — and libsmb2 on
 * a 3.1.1 session signs the tree connect but nothing else, producing exactly
 * the mixed session those servers reject. On the first INVALID_PARAMETER on
 * a 3.1.1 session, rebuild the connection with signing forced and retry the
 * operation once; on success, s_sign_learned makes later sessions skip the
 * failed attempt entirely. 3.1.1 is the only dialect where this mixed-signing
 * pattern occurs, which keeps the retry scoped to the signature we observed. */
static bool smb_reconnect_signed(void)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);
    smb_session_end();
    return smb_open_session(&cfg, true);
}

/* mkdir every ancestor directory of remote_path. Intermediate failures are
 * ignored — the dir may already exist or the share may not allow mkdir. */
static void smb_mkdir_parents(struct smb2_context *smb2, const char *remote_path)
{
    char dir[512];
    for (const char *s = remote_path; (s = strchr(s, '/')); s++) {
        size_t len = s - remote_path;
        if (len == 0 || len >= sizeof(dir)) continue;
        memcpy(dir, remote_path, len);
        dir[len] = '\0';
        smb2_mkdir(smb2, dir);
    }
}

static upload_result_t smb_upload_file(struct smb2_context *smb2,
                                       const char *local_path,
                                       const char *remote_path)
{
    upload_result_t r = smb_upload_file_once(smb2, local_path, remote_path);
    if (r == UPLOAD_OK || s_signed_session ||
        (uint32_t)smb2_get_nterror(smb2) != SMB2_STATUS_INVALID_PARAMETER ||
        smb2_get_dialect(smb2) != SMB2_VERSION_0311) {
        return r;
    }
    uploader_config_t cfg;
    uploader_load_config(&cfg);
    if (!cfg.smb_pass[0]) return r;   /* guest session: no session key to sign with */

    ESP_LOGW(TAG, "server rejected unsigned request — retrying with SMB signing");
    if (!smb_reconnect_signed()) return r;
    /* mkdir calls made on the unsigned session failed silently, so the
     * parent chain may not exist on the new session. */
    smb_mkdir_parents(s_smb, remote_path);
    r = smb_upload_file_once(s_smb, local_path, remote_path);
    if (r == UPLOAD_OK) {
        s_sign_learned = true;
        ESP_LOGI(TAG, "server requires SMB signing — remembered for this boot");
    }
    return r;
}

/* ── "Test connection" (web UI) ───────────────────────────────────────
 *
 * The same handshake as smb_session_begin() — negotiate, authenticate, open
 * the share — on a private context so it can run from the httpd task while
 * the scheduler is idle, followed by a stat of the configured folder: the
 * uploader's mkdir calls do not create parents, so a remote path that does
 * not exist on the share fails on the first upload rather than here.
 * Nothing is created or written. */
#define SMB_TEST_TIMEOUT_S 10

static bool smb_test(const uploader_config_t *cfgp, char *msg, size_t msg_len)
{
    /* By value, so the rest of this function is untouched and every `cfg.field` below still
     * reads the same way. The copy is ~450 bytes on a task with 12 KB. */
    uploader_config_t cfg = *cfgp;
    if (!cfg.smb_host[0] || !cfg.smb_share[0]) {
        snprintf(msg, msg_len, "Host and share name are required");
        return false;
    }
    const char *user = cfg.smb_user[0] ? cfg.smb_user : "Guest";

    struct smb2_context *ctx = smb2_init_context();
    if (!ctx) {
        snprintf(msg, msg_len, "Out of memory");
        return false;
    }
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (s_sign_learned) smb2_set_sign(ctx, 1);   /* mirror the upload path */
    smb2_set_timeout(ctx, SMB_TEST_TIMEOUT_S);
    smb2_set_user(ctx, user);
    if (cfg.smb_pass[0]) smb2_set_password(ctx, cfg.smb_pass);

    ESP_LOGI(TAG, "test: connecting to %s/%s as %s", cfg.smb_host, cfg.smb_share, user);
    if (smb2_connect_share(ctx, cfg.smb_host, cfg.smb_share, user) != 0) {
        snprintf(msg, msg_len, "Cannot open //%s/%s as %s: %s",
                 cfg.smb_host, cfg.smb_share, user, smb2_get_error(ctx));
        smb2_destroy_context(ctx);
        return false;
    }

    /* Relative to the share root, as in smb_session_begin(). */
    const char *p = cfg.smb_path;
    while (*p == '/' || *p == '\\') p++;

    bool ok = true;
    if (!*p) {
        snprintf(msg, msg_len, "Connected to //%s/%s (share root)",
                 cfg.smb_host, cfg.smb_share);
    } else {
        struct smb2_stat_64 st;
        if (smb2_stat(ctx, p, &st) == 0) {
            snprintf(msg, msg_len, "Connected to //%s/%s, folder '%s' found",
                     cfg.smb_host, cfg.smb_share, p);
        } else {
            snprintf(msg, msg_len, "Connected to //%s/%s, but folder '%s' was not "
                     "found on the share: create it or fix Remote Path",
                     cfg.smb_host, cfg.smb_share, p);
            ok = false;
        }
    }
    smb2_disconnect_share(ctx);
    smb2_destroy_context(ctx);
    return ok;
}

static upload_result_t smb_day_begin(const char *day)
{
    if (!s_smb) return UPLOAD_ERR_TRANSIENT;
    char remote_day[512];
    snprintf(remote_day, sizeof(remote_day), "%s/DATALOG/%s", s_remote_base, day);
    smb2_mkdir(s_smb, remote_day);   /* EEXIST is fine */
    return UPLOAD_OK;
}

static upload_result_t smb_put_group(const char *day, const upload_group_ref_t *g)
{
    if (!s_smb || !g) return UPLOAD_ERR_TRANSIENT;

    for (int i = 0; i < g->n_files; i++) {
        char local[512], remote[640];
        snprintf(local, sizeof(local), "%s/%s/%s", SD_SDCARD_DATALOG, day,
                 g->files[i]);
        snprintf(remote, sizeof(remote), "%s/DATALOG/%s/%s", s_remote_base, day,
                 g->files[i]);

        if (smb_upload_file(s_smb, local, remote) != UPLOAD_OK) {
            ESP_LOGW(TAG, "  failed to upload %s", g->files[i]);
            return UPLOAD_ERR_TRANSIENT;
        }
    }
    ESP_LOGI(TAG, "  group %s: %d file(s)", g->prefix, g->n_files);
    return UPLOAD_OK;
}

static upload_result_t smb_put_bundle(const char *day,
                                      const upload_bundle_ref_t *b, bool changed)
{
    (void)day;
    if (!s_smb || !b) return UPLOAD_ERR_TRANSIENT;

    /* Unlike SleepHQ, SMB is a plain file tree: the root files only need
     * re-sending when they actually changed. */
    if (!changed) return UPLOAD_OK;

    char remote_settings[400];
    snprintf(remote_settings, sizeof(remote_settings), "%s/SETTINGS",
             s_remote_base);
    smb2_mkdir(s_smb, remote_settings);

    int count = 0;
    for (int i = 0; i < b->n_files; i++) {
        char remote[640];
        if (b->in_settings[i]) {
            snprintf(remote, sizeof(remote), "%s/%s", remote_settings, b->names[i]);
        } else {
            snprintf(remote, sizeof(remote), "%s/%s", s_remote_base, b->names[i]);
        }
        if (smb_upload_file(s_smb, b->paths[i], remote) != UPLOAD_OK) {
            ESP_LOGW(TAG, "  failed to upload %s", b->names[i]);
            return UPLOAD_ERR_TRANSIENT;
        }
        count++;
    }
    ESP_LOGI(TAG, "  root bundle: %d file(s)", count);
    return UPLOAD_OK;
}

static upload_result_t smb_ox_day_begin(const char *day)
{
    if (!s_smb || !day) return UPLOAD_ERR_TRANSIENT;
    char path[640];
    snprintf(path, sizeof(path), "%s/OXYMETRY", s_remote_base); smb2_mkdir(s_smb, path);
    snprintf(path, sizeof(path), "%s/OXYMETRY/%s", s_remote_base, day); smb2_mkdir(s_smb, path);
    return UPLOAD_OK;
}

static upload_result_t smb_put_oximetry(const upload_ox_ref_t *ref)
{
    if (!s_smb || !ref || !ref->recording_id[0]) return UPLOAD_ERR_TRANSIENT;
    for (int i = 0; i < ref->n_files; i++) {
        const char *rel = ref->relative_paths[i];
        if (strcmp(rel, "source/source.bin") != 0 &&
            strcmp(rel, "source/source.vld") != 0) continue;
        const char *ext = strstr(rel, ".vld") ? ".vld" : ".bin";
        char path[760];
        snprintf(path, sizeof(path), "%s/OXYMETRY/%s/%s%s", s_remote_base,
                 ref->day, ref->recording_id, ext);
        return smb_upload_file(s_smb, ref->local_paths[i], path);
    }
    return UPLOAD_ERR_PERMANENT;
}

static upload_result_t smb_day_end(const char *day, bool any_uploaded)
{
    (void)day;
    (void)any_uploaded;
    return UPLOAD_OK;   /* nothing to finalise on a file share */
}

const upload_backend_t smb_backend = {
    .id = "smb",
    .label = "SMB Network Share",
    .bundle_only_ok = true,     /* plain file copy, no side effects */
    .supports_ox = true,
    .is_configured = smb_is_configured,
    .session_begin = smb_session_begin,
    .day_begin = smb_day_begin,
    .put_group = smb_put_group,
    .ox_day_begin = smb_ox_day_begin,
    .put_oximetry = smb_put_oximetry,
    .ox_day_end = smb_day_end,
    .put_bundle = smb_put_bundle,
    .day_end = smb_day_end,
    .session_end = smb_session_end,
    .test = smb_test,
};
