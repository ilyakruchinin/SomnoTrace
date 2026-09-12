#!/bin/bash
# SomnoTrace - build and run the host (gcc) unit tests
# Copyright (C) 2026 Plantucha <https://github.com/Plantucha>
#
# This file is part of SomnoTrace.
#
# SomnoTrace is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.
#
# ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
# attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
# (https://github.com/ilyakruchinin)." See the NOTICE file for details.
#
# Usage: scripts/run_host_tests.sh [--only <name>] [--quiet]
#
#   MAIN_DIR   source dir to test (default: main).  scripts/mutants.py points
#              this at a mutated copy.
#   CJSON_DIR  directory holding cJSON.c + cJSON.h for edf_gen_test.  If unset,
#              the system libcjson (apt install libcjson-dev) is used.
#   OUT        build dir (default: build/host_tests)
#
# No ESP-IDF needed.  Exit status is non-zero if any test binary fails, if a
# scripts/*_test.c exists that this script does not know about, or (with
# --only) if the requested test was skipped.
#
# The last line is always "host tests: N run, M failed, K skipped".  A caller
# that does not see that line must treat the run as not having happened.
set -u
# `|| exit` is not decoration (SC2164): without it a failed cd leaves the script running
# in whatever directory it was invoked from, where MAIN_DIR and the test globs below
# resolve against the wrong tree — a run that reports on files it did not mean to check.
cd "$(dirname "$0")/.." || exit 1

MAIN_DIR=${MAIN_DIR:-main}
OUT=${OUT:-build/host_tests}
ONLY=""
QUIET=0
while [ $# -gt 0 ]; do
    case "$1" in
        --only) ONLY="$2"; shift 2 ;;
        --quiet) QUIET=1; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
mkdir -p "$OUT"

CC=${CC:-gcc}
CFLAGS="-std=gnu11 -Wall -Wno-unused-function -O1 -g"
SHIM=scripts/test_include

# ── AddressSanitizer + UndefinedBehaviorSanitizer ────────────────────────────────
# WHY. Three mutants in edf_header.c survived a green suite by corrupting memory
# quietly: malloc(fsize + 1) -> (fsize - 1), and edf_write_field(hdr + 88, ...) ->
# (hdr - 88), which writes 80 bytes BEFORE a 256-byte stack array. Nothing asserted
# them because nothing crashed -- a heap chunk has slack and a stack write lands on
# other locals. Under -fsanitize=address the same mutant dies immediately with
# "stack-buffer-overflow ... in memset".
#
# For a firmware project this is the cheap half of the bargain: the host suite runs
# the SAME C the device runs, on a machine with a MMU and a sanitizer, so the class
# of bug that is hardest to see on an ESP32-S3 is the class this catches for free.
#
# Measured before switching on: the suite already passes clean under both
# sanitizers, so this reds nothing today. Set SNT_NO_SANITIZE=1 to opt out, and a
# toolchain without them degrades to a plain build with a note rather than failing
# -- absence of a sanitizer is not absence of a gate.
SANITIZE=""
if [ -z "${SNT_NO_SANITIZE:-}" ]; then
    printf 'int main(void){return 0;}\n' > "$OUT/.sancheck.c"
    if $CC -fsanitize=address,undefined -o "$OUT/.sancheck" "$OUT/.sancheck.c" 2>/dev/null; then
        SANITIZE="-fsanitize=address,undefined -fno-omit-frame-pointer"
        CFLAGS="$CFLAGS $SANITIZE"
        echo "sanitizers: address,undefined"
    else
        echo "sanitizers: UNAVAILABLE in $CC — building without them"
    fi
    rm -f "$OUT/.sancheck" "$OUT/.sancheck.c"
else
    echo "sanitizers: disabled by SNT_NO_SANITIZE"
fi

failed=0
ran=0
skipped=0
known=""      # every test name this script handles, for the roster check

# run_test <name> <compile args...>
run_test() {
    local name=$1; shift
    known="$known $name"
    if [ -n "$ONLY" ] && [ "$ONLY" != "$name" ]; then return; fi
    ran=$((ran + 1))
    local bin="$OUT/$name"
    if ! $CC $CFLAGS -o "$bin" "$@"; then
        echo "### $name: BUILD FAILED"
        failed=$((failed + 1))
        return
    fi
    local log="$OUT/$name.log"
    if "$bin" > "$log" 2>&1; then
        echo "### $name: PASS  ($(tail -1 "$log"))"
    else
        echo "### $name: FAIL"
        failed=$((failed + 1))
        # Name the failing test(s) even when quiet: a failure nobody can
        # attribute to a test teaches nothing (mutants.py reads these lines).
        grep -E '^ *(FAILED|XPASS|FAIL)\b' "$log" | head -8 | sed 's/^/    /'
        [ $QUIET = 1 ] || cat "$log"
    fi
}

# run_check <name> <command...> — for a check that is not a compiled C test.
# Shares the counters so one summary line still covers everything that ran.
run_check() {
    local name=$1; shift
    known="$known $name"
    if [ -n "$ONLY" ] && [ "$ONLY" != "$name" ]; then return; fi
    ran=$((ran + 1))
    local log="$OUT/$name.log"
    if "$@" > "$log" 2>&1; then
        echo "### $name: PASS  ($(tail -1 "$log"))"
    else
        echo "### $name: FAIL"
        failed=$((failed + 1))
        [ $QUIET = 1 ] || cat "$log"
    fi
}

# skip_test <name> <reason>
skip_test() {
    known="$known $1"
    echo "### $1: SKIPPED — $2"
    skipped=$((skipped + 1))
    if [ -n "$ONLY" ] && [ "$ONLY" = "$1" ]; then failed=$((failed + 1)); fi
}

# Maintainer's existing tests (shim cJSON is enough for these).
run_test as11_time_test    -I"$SHIM" -I"$MAIN_DIR" scripts/as11_time_test.c "$MAIN_DIR/as11_time.c"
run_test as11_events_test  -I"$SHIM" -I"$MAIN_DIR" scripts/as11_events_test.c
run_test vld3_decoder_test -I"$SHIM" -I"$MAIN_DIR" scripts/vld3_decoder_test.c "$MAIN_DIR/oximetry_vld3.c"

# edf_gen_test #includes the real edf_gen.c and needs a real cJSON.
# Include order matters: the real cJSON.h must shadow the shim in $SHIM.
if [ -n "${CJSON_DIR:-}" ]; then
    CJ_INC="-I$CJSON_DIR"; CJ_SRC="$CJSON_DIR/cJSON.c"; CJ_LIB=""
elif [ -f /usr/include/cjson/cJSON.h ]; then
    CJ_INC="-I/usr/include/cjson"; CJ_SRC=""; CJ_LIB="-lcjson"
else
    CJ_INC=""
    skip_test edf_gen_test "no cJSON (apt install libcjson-dev, or set CJSON_DIR=<dir with cJSON.c/.h>)"
    skip_test edf_properties_test "no cJSON"
fi
if [ -n "$CJ_INC" ]; then
    run_test edf_gen_test $CJ_INC -I"$SHIM" -I"$MAIN_DIR" \
        scripts/edf_gen_test.c "$MAIN_DIR/as11_time.c" $CJ_SRC $CJ_LIB -lm
    # upstream's EDF pipeline property suite (54ae598)
    run_test edf_properties_test $CJ_INC -I"$SHIM" -I"$MAIN_DIR" \
        scripts/edf_properties_test.c "$MAIN_DIR/as11_time.c" $CJ_SRC $CJ_LIB -lm
fi

# The mutation harness's TEXTUAL layer has nothing else to catch a mistake in it. Every
# other part is checked by running — a badly formed mutant fails to compile and is counted
# stillborn — but a mutant generated on the wrong part of a line still builds and still
# passes, which is precisely how operators inside trailing comments produced survivors that
# no test could ever kill. Needs no compiler, so it runs wherever python3 does.
if command -v python3 >/dev/null 2>&1; then
    run_check mutate_host_self_test python3 scripts/mutate_host.py --self-test
else
    skip_test mutate_host_self_test "no python3"
fi

# Roster check: a test file that exists but is not wired in here would never
# run and nobody would notice.  Discovered from the tree, not from a list.
for src in scripts/*_test.c; do
    name=$(basename "$src" .c)
    case " $known " in
        *" $name "*) ;;
        *) echo "### $name: NOT WIRED into $0 — add a run_test line"; failed=$((failed + 1)) ;;
    esac
done

echo "host tests: $ran run, $failed failed, $skipped skipped"
[ $failed = 0 ]

