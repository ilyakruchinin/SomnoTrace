#pragma once
#include "maintenance_model.h"
#include <stdbool.h>
#include <stdint.h>
/* Bounded traversal; callers provide the SD lease and cancellation policy. */
typedef bool (*maintenance_fs_cancel_fn)(void *context);
typedef struct {
    uint64_t bytes, files, generated_edfs;
} maintenance_fs_totals_t;
typedef enum {
    MAINT_FS_COUNT,
    MAINT_FS_DELETE_GENERATED,
    MAINT_FS_DELETE_TREE
} maintenance_fs_action_t;
/* Returns 0, an errno, or ECANCELED. Missing directories count as empty.
 * Generated deletion recognizes only our STR/BRP/PLD/SA2/EVE/CSL names. */
int maintenance_fs_walk(const char *path, maintenance_fs_action_t action, bool edf_root,
                        maintenance_fs_totals_t *totals, maintenance_fs_cancel_fn cancelled,
                        void *context);
