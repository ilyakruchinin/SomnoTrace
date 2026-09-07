#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "maintenance_fs.h"
#include "maintenance_model.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void make(const char *root, const char *name)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", root, name);
    FILE *f = fopen(p, "w");
    assert(f);
    assert(fwrite("12345", 1, 5, f) == 5);
    assert(!fclose(f));
}
static bool has(const char *root, const char *name)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", root, name);
    return access(p, F_OK) == 0;
}
static bool stop(void *p)
{
    int *calls = p;
    return ++*calls > 2;
}
int main(void)
{
    maintenance_hold_t h = {0};
    assert(!maintenance_hold_ready(&h, 10000, true));
    maintenance_hold_press(&h, 100);
    assert(!maintenance_hold_ready(&h, 3099, true));
    maintenance_hold_reset(&h);
    assert(!maintenance_hold_ready(&h, 9000, true));
    maintenance_hold_press(&h, 10000);
    for (uint32_t now = 10100; now < 13000; now += 100) {
        maintenance_hold_press(&h, now); /* repeated press cannot reset origin */
        assert(!maintenance_hold_ready(&h, now, true));
    }
    assert(maintenance_hold_ready(&h, 13000, true));
    assert(!maintenance_hold_ready(&h, 13000, true));
    maintenance_hold_press(&h, 20000);
    assert(!maintenance_hold_ready(&h, 24000, false));
    assert(!maintenance_hold_ready(&h, 24001, true));
    maintenance_hold_press(&h, UINT32_MAX - 1000);
    for (uint32_t elapsed = 100; elapsed < 3000; elapsed += 100)
        assert(!maintenance_hold_ready(&h, UINT32_MAX - 1000 + elapsed, true));
    assert(maintenance_hold_ready(&h, 1999, true));
    maintenance_hold_press(&h, 50000);
    assert(!maintenance_hold_ready(&h, 50100, true));
    /* Even a queued PRESSING after a stalled display cannot finish a hold. */
    assert(!maintenance_hold_ready(&h, 54000, true));
    assert(!h.pressed);
    assert(!maintenance_hold_ready(&h, 54100, true));
    assert(maintenance_day_valid("20240229"));
    assert(!maintenance_day_valid("20260229"));
    assert(!maintenance_day_valid("20261301"));
    assert(!maintenance_day_valid("../x"));
    assert(maintenance_sd_image_name_valid("somnotrace-waveshare-7b-ota.bin"));
    assert(!maintenance_sd_image_name_valid("somnotrace-../secret.bin"));
    assert(!maintenance_sd_image_name_valid("somnotrace-a/evil.bin"));
    assert(!maintenance_sd_image_name_valid("firmware.bin"));
    assert(maintenance_estimated_nights(100, 20, 2) == 5);
    assert(!maintenance_estimated_nights(100, 0, 2));
    assert(!maintenance_estimated_nights(100, 20, 0));
    assert(maintenance_generated_edf_name("20260902_220000_BRP.edf", false));
    assert(!maintenance_generated_edf_name("personal_BRP.edf", false));
    assert(!maintenance_generated_edf_name("20260902_999999_BRP.edf", false));
    char root[] = "/tmp/somnotrace-maintenance-test-XXXXXX";
    assert(mkdtemp(root));
    char datalog[512], day[512], unrelated[512];
    snprintf(datalog, sizeof(datalog), "%s/DATALOG", root);
    assert(!mkdir(datalog, 0700));
    snprintf(day, sizeof(day), "%s/20260902", datalog);
    assert(!mkdir(day, 0700));
    snprintf(unrelated, sizeof(unrelated), "%s/personal", root);
    assert(!mkdir(unrelated, 0700));
    make(root, "STR.edf");
    make(root, "personal.edf");
    make(root, "Identification.json");
    make(day, "20260902_220000_BRP.edf");
    make(day, "20260902_220000_PLD.edf");
    make(day, "flow.snt");
    make(day, "personal.edf");
    make(unrelated, "20260902_220000_BRP.edf");
    maintenance_fs_totals_t count = {0};
    assert(!maintenance_fs_walk(root, MAINT_FS_COUNT, true, &count, NULL, NULL));
    assert(count.files == 8);
    assert(count.bytes == 40);
    maintenance_fs_totals_t deleted = {0};
    assert(!maintenance_fs_walk(root, MAINT_FS_DELETE_GENERATED, true, &deleted, NULL, NULL));
    assert(deleted.files == 3);
    assert(!has(root, "STR.edf"));
    assert(!has(day, "20260902_220000_BRP.edf"));
    assert(has(day, "flow.snt"));
    assert(has(day, "personal.edf"));
    assert(has(root, "personal.edf"));
    assert(has(root, "Identification.json"));
    assert(has(unrelated, "20260902_220000_BRP.edf"));
    int calls = 0;
    maintenance_fs_totals_t partial = {0};
    assert(maintenance_fs_walk(root, MAINT_FS_DELETE_TREE, false, &partial, stop, &calls) ==
           ECANCELED);
    assert(partial.files < 5);
    maintenance_fs_totals_t cleanup = {0};
    assert(!maintenance_fs_walk(root, MAINT_FS_DELETE_TREE, false, &cleanup, NULL, NULL));
    assert(!rmdir(root));
    char links[] = "/tmp/somnotrace-maintenance-links-XXXXXX";
    assert(mkdtemp(links));
    char linkpath[512];
    snprintf(linkpath, sizeof(linkpath), "%s/escape", links);
    assert(!symlink("/", linkpath));
    assert(maintenance_fs_walk(links, MAINT_FS_DELETE_TREE, false, &cleanup, NULL, NULL) == ELOOP);
    assert(!unlink(linkpath));
    assert(!rmdir(links));
    puts("maintenance model/filesystem behavior passed (synthetic temporary data only)");
}
