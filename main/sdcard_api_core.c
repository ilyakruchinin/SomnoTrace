/*
 * SomnoTrace - Pure path validation and directory walk for read-only SD API
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

#include "sdcard_api_core.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WALK_MAX_QUEUED_DIRS 512

bool sdcard_api_relpath_valid(const char *relpath)
{
    if (!relpath || relpath[0] == '\0') return false;

    size_t len = strlen(relpath);
    if (len >= SDCARD_API_MAX_RELPATH) return false;
    if (relpath[0] == '/' || relpath[len - 1] == '/') return false;

    const char *seg_start = relpath;
    for (size_t i = 0; i <= len; i++) {
        unsigned char c = (unsigned char)relpath[i];
        if (c == '\\' || (c != '\0' && (c < 0x20 || c == 0x7f))) {
            return false;
        }
        if (c == '/' || c == '\0') {
            size_t seg_len = (size_t)(&relpath[i] - seg_start);
            if (seg_len == 0) return false;
            /* Reject '.', '..', and any dot-prefixed hidden entry. */
            if (seg_start[0] == '.') return false;
            seg_start = &relpath[i + 1];
        }
    }
    return true;
}

bool sdcard_api_build_path(const char *root, const char *relpath, char *out, size_t out_cap)
{
    if (!root || root[0] == '\0' || !out || out_cap == 0) return false;
    if (!sdcard_api_relpath_valid(relpath)) return false;

    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') {
        root_len--;
    }
    size_t rel_len = strlen(relpath);
    /* Avoid double '/' when root is "/". */
    size_t sep = (root_len == 1 && root[0] == '/') ? 0 : 1;
    if (root_len + sep + rel_len >= out_cap) return false;

    memcpy(out, root, root_len);
    if (sep) out[root_len] = '/';
    memcpy(out + root_len + sep, relpath, rel_len);
    out[root_len + sep + rel_len] = '\0';
    return true;
}

int sdcard_api_json_escape(const char *src, char *dst, size_t dst_cap)
{
    if (!src || !dst || dst_cap == 0) return -1;

    static const char hex[] = "0123456789abcdef";
    size_t out = 0;

    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        const char *esc = NULL;
        char ubuf[7];

        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) {
                ubuf[0] = '\\';
                ubuf[1] = 'u';
                ubuf[2] = '0';
                ubuf[3] = '0';
                ubuf[4] = hex[(c >> 4) & 0x0f];
                ubuf[5] = hex[c & 0x0f];
                ubuf[6] = '\0';
                esc = ubuf;
            }
            break;
        }

        if (esc) {
            size_t elen = strlen(esc);
            if (out + elen >= dst_cap) {
                dst[out] = '\0';
                return -1;
            }
            memcpy(dst + out, esc, elen);
            out += elen;
        } else {
            if (out + 1 >= dst_cap) {
                dst[out] = '\0';
                return -1;
            }
            dst[out++] = (char)c;
        }
    }

    dst[out] = '\0';
    return (int)out;
}

typedef struct {
    char rel[SDCARD_API_MAX_RELPATH];
    int  depth;
} walk_dir_item_t;

typedef struct {
    walk_dir_item_t *items;
    size_t count;
    size_t cap;
    char dir_path[512];
    char entry_path[512];
    char rel_buf[SDCARD_API_MAX_RELPATH];
} walk_scratch_t;

static int push_dir(walk_scratch_t *ws, const char *rel, int depth)
{
    if (ws->count >= WALK_MAX_QUEUED_DIRS) return 0;
    if (ws->count == ws->cap) {
        size_t next_cap = ws->cap ? ws->cap * 2 : 16;
        if (next_cap > WALK_MAX_QUEUED_DIRS) next_cap = WALK_MAX_QUEUED_DIRS;
        walk_dir_item_t *grown = realloc(ws->items, next_cap * sizeof(*grown));
        if (!grown) return -ENOMEM;
        ws->items = grown;
        ws->cap = next_cap;
    }
    walk_dir_item_t *it = &ws->items[ws->count++];
    size_t rlen = strlen(rel);
    if (rlen >= sizeof(it->rel)) return -ENAMETOOLONG;
    memcpy(it->rel, rel, rlen + 1);
    it->depth = depth;
    return 0;
}

static int join_rel(const char *base, const char *name, char *out, size_t out_cap)
{
    int n = (base[0] != '\0')
        ? snprintf(out, out_cap, "%s/%s", base, name)
        : snprintf(out, out_cap, "%s", name);
    if (n < 0 || (size_t)n >= out_cap) return -1;
    return 0;
}

int sdcard_api_walk(const char *root, const char *sub_dir, sdcard_api_file_cb_t cb, void *ctx)
{
    if (!root || root[0] == '\0' || !cb) return -EINVAL;

    bool has_sub = (sub_dir && sub_dir[0] != '\0');
    if (has_sub && !sdcard_api_relpath_valid(sub_dir)) return -EINVAL;

    walk_scratch_t *ws = calloc(1, sizeof(*ws));
    if (!ws) return -ENOMEM;

    int rc = push_dir(ws, has_sub ? sub_dir : "", 0);
    if (rc != 0) {
        free(ws->items);
        free(ws);
        return rc;
    }

    size_t head = 0;
    bool root_opened = false;
    while (head < ws->count) {
        walk_dir_item_t cur = ws->items[head++];

        if (cur.rel[0] != '\0') {
            if (!sdcard_api_build_path(root, cur.rel, ws->dir_path, sizeof(ws->dir_path))) {
                continue;
            }
        } else {
            size_t rlen = strlen(root);
            if (rlen >= sizeof(ws->dir_path)) {
                rc = -ENAMETOOLONG;
                break;
            }
            memcpy(ws->dir_path, root, rlen + 1);
        }

        DIR *d = opendir(ws->dir_path);
        if (!d) {
            if (!root_opened) {
                rc = errno ? -errno : -ENOENT;
                break;
            }
            continue;
        }
        root_opened = true;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (join_rel(cur.rel, ent->d_name, ws->rel_buf, sizeof(ws->rel_buf)) != 0) {
                continue;
            }
            if (!sdcard_api_build_path(root, ws->rel_buf, ws->entry_path, sizeof(ws->entry_path))) {
                continue;
            }
            struct stat st;
            if (stat(ws->entry_path, &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                if (cur.depth < SDCARD_API_MAX_DEPTH) {
                    int prc = push_dir(ws, ws->rel_buf, cur.depth + 1);
                    if (prc != 0) {
                        rc = prc;
                        break;
                    }
                }
            } else if (S_ISREG(st.st_mode)) {
                int cbret = cb(ws->rel_buf, (uint64_t)st.st_size, (int64_t)st.st_mtime, ctx);
                if (cbret != 0) {
                    rc = cbret;
                    break;
                }
            }
        }
        closedir(d);
        if (rc != 0) break;
    }

    free(ws->items);
    free(ws);
    return rc;
}
