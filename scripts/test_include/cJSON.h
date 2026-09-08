/*
 * SomnoTrace - Minimal cJSON test shim for host unit tests
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

#pragma once

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct cJSON {
    struct cJSON *child;
    struct cJSON *next;
    char *string;
    char *valuestring;
    int valueint;
    double valuedouble;
    int type;
} cJSON;

#define cJSON_Number 8
#define cJSON_String 16
#define cJSON_Array  32
#define cJSON_Object 64

static inline cJSON *cJSON_GetObjectItem(const cJSON *o, const char *key)
{
    if (!o) return NULL;
    for (cJSON *c = o->child; c; c = c->next) {
        if (c->string && key && strcmp(c->string, key) == 0) return c;
    }
    return NULL;
}

static inline bool cJSON_IsString(const cJSON *i)
{
    return i && i->type == cJSON_String;
}

static inline bool cJSON_IsNumber(const cJSON *i)
{
    return i && i->type == cJSON_Number;
}

static inline bool cJSON_IsArray(const cJSON *i)
{
    return i && i->type == cJSON_Array;
}

static inline int cJSON_GetArraySize(const cJSON *a)
{
    if (!a) return 0;
    int count = 0;
    for (cJSON *c = a->child; c; c = c->next) count++;
    return count;
}

static inline cJSON *cJSON_GetArrayItem(const cJSON *a, int index)
{
    if (!a) return NULL;
    int i = 0;
    for (cJSON *c = a->child; c; c = c->next, i++) {
        if (i == index) return c;
    }
    return NULL;
}

static inline cJSON *cJSON_CreateObject(void)
{
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (item) item->type = cJSON_Object;
    return item;
}

static inline cJSON *cJSON_CreateArray(void)
{
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (item) item->type = cJSON_Array;
    return item;
}

static inline void cJSON_AddItemToArray(cJSON *arr, cJSON *item)
{
    if (!arr || !item) return;
    if (!arr->child) {
        arr->child = item;
    } else {
        cJSON *c = arr->child;
        while (c->next) c = c->next;
        c->next = item;
    }
}

static inline char *_test_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *p = (char *)malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

static inline void cJSON_AddItemToObject(cJSON *obj, const char *name, cJSON *item)
{
    if (!obj || !item) return;
    if (name) item->string = _test_strdup(name);
    cJSON_AddItemToArray(obj, item);
}

static inline void cJSON_AddStringToObject(cJSON *obj, const char *name, const char *val)
{
    if (!obj) return;
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (!item) return;
    item->type = cJSON_String;
    if (name) item->string = _test_strdup(name);
    if (val) item->valuestring = _test_strdup(val);
    cJSON_AddItemToArray(obj, item);
}

static inline void cJSON_AddNumberToObject(cJSON *obj, const char *name, double num)
{
    if (!obj) return;
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (!item) return;
    item->type = cJSON_Number;
    item->valuedouble = num;
    item->valueint = (int)num;
    if (name) item->string = _test_strdup(name);
    cJSON_AddItemToArray(obj, item);
}

static inline void cJSON_AddNullToObject(cJSON *obj, const char *name)
{
    if (!obj) return;
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (!item) return;
    if (name) item->string = _test_strdup(name);
    cJSON_AddItemToArray(obj, item);
}

static inline cJSON *cJSON_Parse(const char *json)
{
    (void)json;
    return NULL;
}

static inline char *cJSON_PrintUnformatted(const cJSON *item)
{
    (void)item;
    return NULL;
}

static inline void cJSON_Delete(cJSON *item)
{
    if (!item) return;
    cJSON *c = item->child;
    while (c) {
        cJSON *next = c->next;
        cJSON_Delete(c);
        c = next;
    }
    if (item->string) free(item->string);
    if (item->valuestring) free(item->valuestring);
    free(item);
}
