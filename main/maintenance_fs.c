#include "maintenance_fs.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int walk(const char *path, maintenance_fs_action_t action, bool edf_root, unsigned depth,
                maintenance_fs_totals_t *totals, maintenance_fs_cancel_fn cancelled, void *context)
{
    if (depth > 8)
        return EOVERFLOW;
    if (cancelled && cancelled(context))
        return ECANCELED;
    DIR *dir = opendir(path);
    if (!dir)
        return errno == ENOENT ? 0 : (errno ? errno : EIO);
    int result = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        if (cancelled && cancelled(context)) {
            result = ECANCELED;
            break;
        }
        char child[512];
        if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >= (int)sizeof(child)) {
            result = ENAMETOOLONG;
            break;
        }
        struct stat st;
#ifdef MAINTENANCE_HOST_TEST
        if (lstat(child, &st)) {
            result = errno;
            break;
        }
        /* Never follow a link out of an authorized tree, even in host fixtures. */
        if (S_ISLNK(st.st_mode)) {
            result = ELOOP;
            break;
        }
#else
        if (stat(child, &st)) {
            result = errno;
            break;
        }
#endif
        if (S_ISDIR(st.st_mode)) {
            /* EDF deletion only visits the documented DATALOG/day tree. */
            if (action == MAINT_FS_DELETE_GENERATED &&
                !((edf_root && !strcmp(ent->d_name, "DATALOG")) ||
                  (depth == 1 && maintenance_day_valid(ent->d_name))))
                continue;
            result = walk(child, action, false, depth + 1, totals, cancelled, context);
            if (!result && action == MAINT_FS_DELETE_TREE && rmdir(child))
                result = errno;
            if (result)
                break;
        } else if (S_ISREG(st.st_mode)) {
            bool generated =
                (edf_root || depth == 2) && maintenance_generated_edf_name(ent->d_name, edf_root);
            if (action == MAINT_FS_DELETE_TREE ||
                (action == MAINT_FS_DELETE_GENERATED && generated)) {
                if (unlink(child)) {
                    result = errno;
                    break;
                }
            }
            if (action == MAINT_FS_COUNT || action == MAINT_FS_DELETE_TREE || generated) {
                totals->files++;
                totals->bytes += (uint64_t)st.st_size;
                if (generated)
                    totals->generated_edfs++;
            }
        }
    }
    if (closedir(dir) && !result)
        result = errno;
    return result;
}
int maintenance_fs_walk(const char *path, maintenance_fs_action_t action, bool edf_root,
                        maintenance_fs_totals_t *totals, maintenance_fs_cancel_fn cancelled,
                        void *context)
{
    if (!path || !totals)
        return EINVAL;
    return walk(path, action, edf_root, 0, totals, cancelled, context);
}
