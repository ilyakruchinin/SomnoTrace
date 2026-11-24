/*
 * SomnoTrace - Pure scoped resolver for a day's daily settings evidence
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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decides which evidence, if any, may supply one day's settings.  It sees only
 * typed facts the caller has already verified; it performs no file or JSON
 * I/O, so every rule is testable on the host.  Unknown is always a valid answer. */

typedef enum {
    RES_APPLY_NATIVE = 0,    /* the verified native daily record applies */
    RES_APPLY_LIVE,          /* every local session is verified and agrees */
    RES_UNKNOWN_NO_IDENTITY, /* the export machine identity is not established */
    RES_UNKNOWN_NO_SOURCE,   /* no native record and no local sessions */
    RES_UNKNOWN_IDENTITY,    /* native record present but its identity fails */
    RES_UNKNOWN_SESSION,     /* a local session lacks verified identity */
    RES_UNKNOWN_CONFLICT     /* verified sources or sessions disagree */
} resolver_state_t;

typedef struct {
    bool     verified;        /* identity proven by the session's own evidence */
    bool     live_capture;    /* a provenance-marked live settings capture exists */
    bool     bridge_unverified; /* bridge import whose manifest predates verified identity */
    bool     serial_match;    /* serial equals the export machine serial */
    bool     family_match;    /* family equals the export machine family */
    uint64_t settings_digest; /* digest of the settings group it represents */
    uint64_t id_hash;         /* stable identity of the session (revision key) */
} resolver_session_t;

typedef struct {
    bool export_identity_known;
    bool native_present;
    bool native_verified;
    bool native_serial_match;
    bool native_family_match;
    /* Proof that the native record's own window includes every local session
     * of the day.  Absence of this proof never makes a session covered. */
    bool native_window_covers_all;
    uint64_t native_digest;
    const resolver_session_t *sessions; /* sorted by id_hash by the caller */
    size_t n_sessions;
} resolver_input_t;

resolver_state_t settings_resolve_day(const resolver_input_t *in);
const char *settings_resolver_state_name(resolver_state_t state);

/* Deterministic key over every input that can change the decision.  Callers use
 * it to invalidate derived caches; equal inputs always yield equal keys. */
uint64_t settings_resolver_revision_key(const resolver_input_t *in);
