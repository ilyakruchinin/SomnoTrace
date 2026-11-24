/*
 * SomnoTrace - host tests for the pure daily settings resolver
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

#include <stdio.h>
#include <string.h>

#include "settings_resolver.h"

static int checks, fails;

#define CHECK(cond, msg) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAILED: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static resolver_session_t ok_session(uint64_t id, uint64_t digest)
{
    resolver_session_t s = { .verified = true, .live_capture = true,
                             .serial_match = true, .family_match = true,
                             .settings_digest = digest, .id_hash = id };
    return s;
}

static resolver_input_t base_input(const resolver_session_t *s, size_t n)
{
    resolver_input_t in;
    memset(&in, 0, sizeof(in));
    in.export_identity_known = true;
    in.sessions = s;
    in.n_sessions = n;
    return in;
}

static void test_identity_gate(void)
{
    resolver_input_t in = base_input(NULL, 0);
    in.export_identity_known = false;
    in.native_present = true;
    in.native_verified = true;
    in.native_serial_match = in.native_family_match = true;
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_NO_IDENTITY,
          "unknown export identity blocks everything, even a native record");
    CHECK(settings_resolve_day(NULL) == RES_UNKNOWN_NO_IDENTITY,
          "NULL input is unknown");
}

static void test_native_only(void)
{
    resolver_input_t in = base_input(NULL, 0);
    in.native_present = true;
    in.native_verified = true;
    in.native_serial_match = in.native_family_match = true;
    CHECK(settings_resolve_day(&in) == RES_APPLY_NATIVE,
          "verified native record applies with no local sessions");

    in.native_verified = false;
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_IDENTITY,
          "unverified native record is unknown");
    in.native_verified = true;
    in.native_serial_match = false;
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_IDENTITY,
          "native serial from another machine is unknown");
}

static void test_native_with_sessions(void)
{
    resolver_session_t s[1] = { ok_session(1, 10) };
    resolver_input_t in = base_input(s, 1);
    in.native_present = true;
    in.native_verified = true;
    in.native_serial_match = in.native_family_match = true;
    in.native_window_covers_all = false;
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_CONFLICT,
          "native record that does not cover local sessions is not applied");
    in.native_window_covers_all = true;
    CHECK(settings_resolve_day(&in) == RES_APPLY_NATIVE,
          "native record covering all local sessions applies");
}

static void test_session_gate(void)
{
    resolver_session_t s[2] = { ok_session(1, 10), ok_session(2, 10) };
    s[1].verified = false;
    resolver_input_t in = base_input(s, 2);
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_SESSION,
          "one unverified session makes the day unknown");

    s[1].verified = true;
    s[1].family_match = false;
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_IDENTITY,
          "a session from another family is unknown identity");
}

static void test_session_needs_live_capture(void)
{
    /* A bridge-imported session with no live capture must not be treated as
     * live evidence, even though its identity verifies. */
    resolver_session_t s[1] = { ok_session(1, 0) };
    s[0].live_capture = false;
    resolver_input_t in = base_input(s, 1);
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_SESSION,
          "verified session without live capture is not live evidence");
    in.native_present = true;
    in.native_verified = true;
    in.native_serial_match = in.native_family_match = true;
    in.native_window_covers_all = true;
    CHECK(settings_resolve_day(&in) == RES_APPLY_NATIVE,
          "native record still applies to an imported-only day");
}

static void test_live_only(void)
{
    resolver_session_t agree[2] = { ok_session(1, 42), ok_session(2, 42) };
    resolver_input_t in = base_input(agree, 2);
    CHECK(settings_resolve_day(&in) == RES_APPLY_LIVE,
          "live sessions that agree apply");

    resolver_session_t differ[2] = { ok_session(1, 42), ok_session(2, 43) };
    in = base_input(differ, 2);
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_CONFLICT,
          "disagreeing live sessions are a conflict, not order-dependent");

    in = base_input(NULL, 0);
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_NO_SOURCE,
          "a day with no evidence is unknown");
}

static void test_bridge_unverified_session(void)
{
    /* Option A: an unverified bridge import is covered only by a verified
     * native record whose window includes it. */
    resolver_session_t s[1] = { ok_session(1, 0) };
    s[0].verified = false;
    s[0].bridge_unverified = true;
    s[0].live_capture = false;
    resolver_input_t in = base_input(s, 1);
    in.native_present = true;
    in.native_verified = true;
    in.native_serial_match = in.native_family_match = true;
    in.native_window_covers_all = true;
    CHECK(settings_resolve_day(&in) == RES_APPLY_NATIVE,
          "bridge import covered by a verified native record applies");
    in.native_window_covers_all = false;
    CHECK(settings_resolve_day(&in) != RES_APPLY_NATIVE,
          "bridge import outside the native window does not apply");
    in.native_window_covers_all = true;
    in.native_verified = false;
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_SESSION,
          "bridge import with an unverified native record is unknown");
    in.native_present = false;
    in.native_verified = true;
    CHECK(settings_resolve_day(&in) == RES_UNKNOWN_SESSION,
          "bridge import with no native record is unknown");
}

static void test_revision_key(void)
{
    resolver_session_t s[2] = { ok_session(1, 10), ok_session(2, 20) };
    resolver_input_t a = base_input(s, 2);
    resolver_input_t b = base_input(s, 2);
    CHECK(settings_resolver_revision_key(&a) == settings_resolver_revision_key(&b),
          "equal inputs give equal keys");

    b.native_present = true;
    CHECK(settings_resolver_revision_key(&a) != settings_resolver_revision_key(&b),
          "native presence changes the key");

    resolver_session_t t[2] = { ok_session(1, 10), ok_session(2, 20) };
    t[1].settings_digest = 21;
    resolver_input_t c = base_input(t, 2);
    CHECK(settings_resolver_revision_key(&a) != settings_resolver_revision_key(&c),
          "a session settings change changes the key");

    resolver_session_t u[2] = { ok_session(1, 10), ok_session(3, 20) };
    resolver_input_t d = base_input(u, 2);
    CHECK(settings_resolver_revision_key(&a) != settings_resolver_revision_key(&d),
          "a session identity change changes the key");
}

static void test_state_names(void)
{
    CHECK(strcmp(settings_resolver_state_name(RES_APPLY_NATIVE), "apply_native") == 0,
          "native state name");
    CHECK(strcmp(settings_resolver_state_name(RES_UNKNOWN_CONFLICT),
                 "unknown_conflict") == 0, "conflict state name");
    CHECK(strcmp(settings_resolver_state_name((resolver_state_t)99),
                 "unknown_invalid") == 0, "out-of-range state name");
}

int main(void)
{
    test_identity_gate();
    test_native_only();
    test_native_with_sessions();
    test_session_gate();
    test_live_only();
    test_session_needs_live_capture();
    test_bridge_unverified_session();
    test_revision_key();
    test_state_names();
    printf("settings_resolver_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
