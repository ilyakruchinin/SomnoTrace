/*
 * SomnoTrace - pure helpers for the GitHub update-availability check
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
#include "update_check_core.h"

#include <string.h>

bool upd_slug_is_repo(const char *slug)
{
    if (!slug || !*slug) return false;

    int slashes = 0;
    int seg_len = 0;
    for (const char *p = slug; *p; p++) {
        char c = *p;
        if (c == '/') {
            if (++slashes > 1 || seg_len == 0) return false;
            seg_len = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-')) {
            return false;
        }
        seg_len++;
    }
    return slashes == 1 && seg_len > 0;
}

int upd_extract_tag(const char *location, char *out, size_t out_len)
{
    static const char marker[] = "/releases/tag/";
    if (!location || !out || out_len < 2) return -1;

    const char *p = strstr(location, marker);
    if (!p) return -1;
    p += sizeof(marker) - 1;

    size_t n = 0;
    while (p[n] && p[n] != '?' && p[n] != '#') {
        if (n + 1 >= out_len) return -1;
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0 ? 0 : -1;
}

typedef struct {
    int major, minor, patch;
    const char *pre;
    size_t pre_len;
    bool dirty;
    bool dev_describe;
} upd_ver_t;

/* Parse an optional-v, 1..3-component numeric core, an optional
 * prerelease, and optional build metadata.  Unknown forms are rejected
 * rather than guessed. */
static bool upd_parse_ver(const char *s, upd_ver_t *v)
{
    memset(v, 0, sizeof(*v));
    if (!s || !*s) return false;
    if (*s == 'v' || *s == 'V') s++;

    int part[3] = { 0, 0, 0 };
    int n = 0;
    const char *p = s;
    while (n < 3) {
        if (*p < '0' || *p > '9') return false;
        long val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            if (val > 1000000) return false;
            p++;
        }
        part[n++] = (int)val;
        if (*p != '.') break;
        p++;
    }
    if (n == 0 || *p == '.') return false;
    v->major = part[0];
    v->minor = n > 1 ? part[1] : 0;
    v->patch = n > 2 ? part[2] : 0;

    if (*p == '-') {
        p++;
        const char *pre = p;
        while (*p && *p != '+') p++;
        v->pre = pre;
        v->pre_len = (size_t)(p - pre);
        if (v->pre_len == 0) return false;
    }
    if (*p == '+') {
        p++;
        if (!*p) return false;
        while (*p) p++;
    }
    if (*p) return false;

    v->dirty = strstr(s, "dirty") != NULL;
    if (v->pre_len >= 3) {
        size_t i = 0;
        while (i < v->pre_len && v->pre[i] >= '0' && v->pre[i] <= '9') i++;
        if (i > 0 && i + 1 < v->pre_len && v->pre[i] == '-' &&
            v->pre[i + 1] == 'g')
            v->dev_describe = true;
    }
    return true;
}

static int upd_cmp_id(const char *a, size_t alen, const char *b, size_t blen)
{
    bool an = alen > 0, bn = blen > 0;
    for (size_t i = 0; i < alen; i++)
        if (a[i] < '0' || a[i] > '9') an = false;
    for (size_t i = 0; i < blen; i++)
        if (b[i] < '0' || b[i] > '9') bn = false;
    if (an && bn) {
        size_t ai = 0, bi = 0;
        while (ai + 1 < alen && a[ai] == '0') ai++;
        while (bi + 1 < blen && b[bi] == '0') bi++;
        size_t al = alen - ai, bl = blen - bi;
        if (al != bl) return al < bl ? -1 : 1;
        int c = memcmp(a + ai, b + bi, al);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (an != bn) return an ? -1 : 1;
    size_t m = alen < blen ? alen : blen;
    int c = memcmp(a, b, m);
    if (c) return c < 0 ? -1 : 1;
    return alen == blen ? 0 : alen < blen ? -1 : 1;
}

static int upd_cmp_pre(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t ai = 0, bi = 0;
    while (ai < alen || bi < blen) {
        size_t ae = ai;
        while (ae < alen && a[ae] != '.') ae++;
        size_t be = bi;
        while (be < blen && b[be] != '.') be++;
        if (ai == ae && bi == be) return 0;
        if (ai == ae) return -1;
        if (bi == be) return 1;
        int c = upd_cmp_id(a + ai, ae - ai, b + bi, be - bi);
        if (c) return c;
        ai = ae < alen ? ae + 1 : ae;
        bi = be < blen ? be + 1 : be;
    }
    return 0;
}

static int upd_cmp_ver(const upd_ver_t *a, const upd_ver_t *b)
{
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    bool ap = a->pre_len > 0, bp = b->pre_len > 0;
    if (!ap && !bp) return 0;
    if (!ap) return 1;
    if (!bp) return -1;
    return upd_cmp_pre(a->pre, a->pre_len, b->pre, b->pre_len);
}

/* True only when the release tag is strictly newer than the running
 * firmware.  Older/equal tags, dirty/dev builds of the same core,
 * optional-v equivalence, build metadata, and unparseable values never
 * count as an update. */
bool upd_is_update(const char *tag, const char *fw_ver)
{
    upd_ver_t t, f;
    if (!upd_parse_ver(tag, &t) || !upd_parse_ver(fw_ver, &f)) return false;
    if (upd_cmp_ver(&t, &f) <= 0) return false;
    if ((f.dirty || f.dev_describe) &&
        t.major == f.major && t.minor == f.minor && t.patch == f.patch)
        return false;
    return true;
}
