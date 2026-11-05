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

#include "settings_resolver.h"

/* FNV-1a over 64-bit words: deterministic, dependency-free revision hashing. */
static uint64_t mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 0x100000001b3ULL;
    }
    return h;
}

const char *settings_resolver_state_name(resolver_state_t state)
{
    switch (state) {
    case RES_APPLY_NATIVE:         return "apply_native";
    case RES_APPLY_LIVE:           return "apply_live";
    case RES_UNKNOWN_NO_IDENTITY:  return "unknown_no_identity";
    case RES_UNKNOWN_NO_SOURCE:    return "unknown_no_source";
    case RES_UNKNOWN_IDENTITY:     return "unknown_identity";
    case RES_UNKNOWN_SESSION:      return "unknown_session";
    case RES_UNKNOWN_CONFLICT:     return "unknown_conflict";
    }
    return "unknown_invalid";
}

resolver_state_t settings_resolve_day(const resolver_input_t *in)
{
    if (!in || !in->export_identity_known)
        return RES_UNKNOWN_NO_IDENTITY;

    /* Every local session must be verified for this machine.  An unverified
     * session makes the day unknown whether or not a native record exists. */
    bool all_sessions_ok = true;
    for (size_t i = 0; i < in->n_sessions; i++) {
        const resolver_session_t *s = &in->sessions[i];
        if (!s->verified) {
            /* Bridge imports whose manifests predate verified identity are
             * covered only by the card's own verified native record for the
             * day, and only when that record's window includes them. */
            if (s->bridge_unverified && in->native_present && in->native_verified &&
                in->native_window_covers_all)
                continue;
            return RES_UNKNOWN_SESSION;
        }
        if (!s->serial_match || !s->family_match) all_sessions_ok = false;
    }
    if (!all_sessions_ok) return RES_UNKNOWN_IDENTITY;

    /* Native daily record: applies when verified, same machine, and its own
     * window covers every local session of the day. */
    if (in->native_present) {
        if (!in->native_verified || !in->native_serial_match ||
            !in->native_family_match)
            return RES_UNKNOWN_IDENTITY;
        if (in->n_sessions > 0 && !in->native_window_covers_all)
            return RES_UNKNOWN_CONFLICT;
        return RES_APPLY_NATIVE;
    }

    if (in->n_sessions == 0) return RES_UNKNOWN_NO_SOURCE;

    /* Without a native record, each session needs its own live capture for
     * the live path; bridge-imported sessions have none and cannot supply it. */
    for (size_t i = 0; i < in->n_sessions; i++)
        if (!in->sessions[i].live_capture) return RES_UNKNOWN_SESSION;

    /* Live-only day: every session must agree on the represented settings. */
    uint64_t first = in->sessions[0].settings_digest;
    for (size_t i = 1; i < in->n_sessions; i++)
        if (in->sessions[i].settings_digest != first)
            return RES_UNKNOWN_CONFLICT;
    return RES_APPLY_LIVE;
}

uint64_t settings_resolver_revision_key(const resolver_input_t *in)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    if (!in) return mix(h, 0);
    h = mix(h, in->export_identity_known);
    h = mix(h, in->native_present);
    h = mix(h, in->native_verified);
    h = mix(h, in->native_serial_match);
    h = mix(h, in->native_family_match);
    h = mix(h, in->native_window_covers_all);
    h = mix(h, in->native_digest);
    h = mix(h, (uint64_t)in->n_sessions);
    for (size_t i = 0; i < in->n_sessions; i++) {
        const resolver_session_t *s = &in->sessions[i];
        h = mix(h, s->id_hash);
        h = mix(h, s->verified);
        h = mix(h, s->live_capture);
        h = mix(h, s->bridge_unverified);
        h = mix(h, s->serial_match);
        h = mix(h, s->family_match);
        h = mix(h, s->settings_digest);
    }
    return h;
}
