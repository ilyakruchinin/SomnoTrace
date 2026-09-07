#include "uploader.h"
#include "esp_random.h"
#include "lwip/netdb.h"
#include "smb2.h"
#include "libsmb2.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
/* Diagnostic-only remote file: exclusive random name, readback and cleanup.
 * No local card data is opened and no upload tracking state is changed. */
esp_err_t uploader_smb_probe(void)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);
    if (!cfg.smb_host[0] || !cfg.smb_share[0]) {
        uploader_test_failed(UPLOAD_STAGE_RESOLVE, "Host and share must be configured");
        return ESP_ERR_INVALID_STATE;
    }
    uploader_test_stage(UPLOAD_STAGE_RESOLVE, false, "Resolving SMB host");
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(cfg.smb_host, "445", NULL, &addresses) != 0) {
        uploader_test_failed(UPLOAD_STAGE_RESOLVE,"Host resolution failed"); return ESP_FAIL;
    }
    freeaddrinfo(addresses);
    uploader_test_stage(UPLOAD_STAGE_RESOLVE, true, "SMB host resolved");
    struct smb2_context *ctx = smb2_init_context();
    if (!ctx) { uploader_test_failed(UPLOAD_STAGE_CONNECT,"Not enough memory for SMB test"); return ESP_ERR_NO_MEM; }
    smb2_set_timeout(ctx, 5);
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_user(ctx, cfg.smb_user[0] ? cfg.smb_user : "Guest");
    smb2_set_password(ctx, cfg.smb_pass);
    uploader_test_stage(UPLOAD_STAGE_AUTH_MOUNT, false,
        "Connecting, authenticating and mounting (combined SMB operation)");
    if (smb2_connect_share(ctx, cfg.smb_host, cfg.smb_share, cfg.smb_user[0] ? cfg.smb_user : "Guest") < 0) {
        uploader_test_failed(UPLOAD_STAGE_AUTH_MOUNT,
            "SMB connection/authentication/mount failed; check host, share and credentials");
        smb2_destroy_context(ctx); return ESP_FAIL;
    }
    uploader_test_stage(UPLOAD_STAGE_CONNECT, true, "SMB session connected");
    uploader_test_stage(UPLOAD_STAGE_AUTH_MOUNT, true, "Authenticated and share mounted");
    char path[256]; const char *base = cfg.smb_path;
    while (*base == '/' || *base == '\\') base++;
    snprintf(path, sizeof(path), "%s%s.somnotrace-test-%08lx-%08lx.tmp",
        base, *base ? "/" : "", (unsigned long)esp_random(), (unsigned long)esp_random());
    for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
    uploader_test_stage(UPLOAD_STAGE_WRITE, false, "Writing exclusive 4 KiB diagnostic file");
    struct smb2fh *fh = smb2_open(ctx, path, O_RDWR | O_CREAT | O_EXCL);
    esp_err_t result = ESP_FAIL;
    if (fh) {
        uint8_t data[256], readback[256];
        memset(data, 0x5a, sizeof(data));
        bool ok = true;
        for (int offset = 0; offset < 4096 && ok; offset += sizeof(data))
            ok = smb2_pwrite(ctx, fh, data, sizeof(data), offset) == sizeof(data);
        if (ok) ok = smb2_fsync(ctx, fh) == 0;
        if (!ok) uploader_test_failed(UPLOAD_STAGE_WRITE,"Diagnostic write or flush failed");
        if (ok) {
            uploader_test_stage(UPLOAD_STAGE_WRITE, true, "4 KiB written and flushed");
            uploader_test_stage(UPLOAD_STAGE_VERIFY, false, "Reading diagnostic file back");
            for (int offset = 0; offset < 4096 && ok; offset += sizeof(data))
                ok = smb2_pread(ctx, fh, readback, sizeof(readback), offset) == sizeof(readback) &&
                     !memcmp(data, readback, sizeof(data));
            if (ok) uploader_test_stage(UPLOAD_STAGE_VERIFY, true, "Readback matched");
            else uploader_test_failed(UPLOAD_STAGE_VERIFY,"Diagnostic readback failed or mismatched");
        }
        if (smb2_close(ctx, fh) != 0) { ok = false; uploader_test_failed(UPLOAD_STAGE_CLEANUP,"Closing diagnostic file failed"); }
        uploader_test_stage(UPLOAD_STAGE_CLEANUP, false, "Removing diagnostic file");
        if (smb2_unlink(ctx, path) != 0) {
            uploader_test_failed(UPLOAD_STAGE_CLEANUP, "Cleanup failed; a .somnotrace-test file may remain on the share");
        } else {
            uploader_test_stage(UPLOAD_STAGE_CLEANUP, true, ok ? "Write, readback and cleanup passed" : "Diagnostic file removed; write or readback failed");
            if (ok) result = ESP_OK;
        }
    } else uploader_test_failed(UPLOAD_STAGE_WRITE, "Cannot create test file; check remote path and write permission");
    smb2_disconnect_share(ctx); smb2_destroy_context(ctx);
    memset(&cfg, 0, sizeof(cfg));
    return result;
}
