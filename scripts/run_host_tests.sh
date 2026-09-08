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
cd "$(dirname "$0")/.."

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

