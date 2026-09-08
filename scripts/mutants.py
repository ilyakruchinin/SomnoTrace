#!/usr/bin/env python3
# SomnoTrace - Targeted mutation check for the host unit tests
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

"""Does the host test suite actually notice when the code is wrong?

A test that asserts a constant copied from the implementation passes forever,
bug included.  This script plants a short list of *plausible* bugs — each one
a mistake this code base has had, nearly had, or could have with a one-line
slip — into a copy of main/, runs scripts/run_host_tests.sh against the copy,
and reports which mutants the suite killed.  A surviving mutant is a hole in
the tests, not (necessarily) a bug in the code.

Two controls decide whether the numbers mean anything at all:

  baseline  the unmutated tree must pass, with nothing skipped.  A suite that
            is already red, or that silently skipped the file under test,
            "kills" everything and measures nothing.
  canary    one mutant that no working suite can miss (every SNT file
            rejected).  If it survives, the run is VOID and no kill count is
            printed — a runner that always exits 0 would otherwise look like
            a perfect suite.

A kill needs positive evidence: the runner's final "host tests:" line, and
the name of the test that failed.  A compile error is not a kill (the mutant
was malformed); a missing summary line is not a kill (the runner did not
finish).  Each mutant is an exact-string replacement that must match exactly
once, so a refactor that moves the code fails loudly here instead of silently
testing nothing.  Expected-failure (XFAIL) tests cannot kill anything.

Usage: scripts/mutants.py [--only <id>] [--keep]
Exit status: non-zero if the run is VOID or any mutant survived / did not apply.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, "scripts", "run_host_tests.sh")
TIMEOUT_S = 600          # a hang is a verdict too, not an invisible mutant

# Positive control.  Not counted; it only decides whether the counts are real.
CANARY = ("canary", "snt_format.h",
          "if (hdr->magic != SNT_MAGIC) {",
          "if (hdr->magic == SNT_MAGIC) {",
          "every valid SNT file rejected — a suite that misses this measures nothing")

# (id, file, find, replace, why it is a plausible bug)
MUTANTS = [
    ("reader-v2-sentinel", "snt_format.h",
     "return (version >= 2) ? SNT_MISSING_V2 : SNT_MISSING_V1;",
     "return SNT_MISSING_V1;",
     "reader forgets that SNT v2 changed the missing-data sentinel"),
    ("passthrough-off", "edf_waveform.c",
     "if (passthrough && stored == snt_missing) {",
     "if (false) {",
     "sentinel goes through the physical->digital scaling"),
    ("sentinel-as-zero", "edf_waveform.c",
     "record_buf[ch * samples_per_record + s] = -1;\n                        continue;",
     "record_buf[ch * samples_per_record + s] = 0;\n                        continue;",
     "missing data written as 0 bpm / 0 % (the OSCAR-visible symptom of #189)"),
    ("torn-tail-unclamped", "edf_waveform.c",
     "if (avail < total_samples) {",
     "if (false) {",
     "header sample_count trusted over the bytes on disk"),
    ("spool-sentinel-scaled", "edf_data_dict.h",
     "if (raw == -1 || den <= 0) return -1;\n    int32_t prod",
     "if (den <= 0) return -1;\n    int32_t prod",
     "summary -1 becomes 0 after logical-scale division"),
    ("record-count-off-by-one", "edf_header.c",
     "snprintf(rc, sizeof(rc), \"%d\", record_count);",
     "snprintf(rc, sizeof(rc), \"%d\", record_count + 1);",
     "EDF header promises a record that is never written"),
    ("as11-noon-boundary", "as11_time.c",
     "if (tod < 43200) days -= 1;",
     "if (tod <= 43200) days -= 1;",
     "AS11-side noon put on the previous day"),
    # The four below were found by scripts/mutants_probe.py as survivors, then
    # killed by tests written for them.  They stay here as the regression.
    ("avail-off-by-one", "edf_waveform.c",
     "long end = ftell(f);",
     "long end = ftell(f) + 1;",
     "file length measured one byte long — a torn frame counted as a whole sample"),
    ("overflow-wraps", "snt_format.h",
     "if (val > max_val) return max_val;",
     "if (val > max_val) return min_val;",
     "a sensor spike exported as maximal NEGATIVE flow instead of saturating"),
    ("pld-map-bound", "snt_format.h",
     "if (map[i] < 0 || map[i] >= snt_channels) return false;",
     "if (map[i] < 0 || map[i] > snt_channels) return false;",
     "channel map allowed to index one past the last channel in every frame"),
    ("maskoff-ignored", "edf_waveform.c",
     "if (max_samples > 0 && max_samples < total_samples) {",
     "if (max_samples > 0 && max_samples > total_samples) {",
     "export runs past MaskOff, filling the night with post-therapy data"),
    ("zle-drift-sign", "edf_waveform.c",
     "cand = as11_ms + clock_drift_ms;",
     "cand = as11_ms - clock_drift_ms;",
     "AS11->NTP drift subtracted, so the gate lands on the wrong second"),
    ("zle-rising-edge", "edf_waveform.c",
     "if (want_value == 1) {",
     "if (want_value != 1) {",
     "rising _ZLE no longer stops the scan: the gate becomes the last edge, not the first"),
    ("offset-ignored", "as11_time.c",
     "noon_day_with_offset(as11_epoch_ms, off, out, out_len);",
     "noon_day_esp_local(as11_epoch_ms, out, out_len);",
     "the session's own UTC offset dropped in favour of the device timezone (#183)"),
    ("offset-check-inverted", "as11_time.c",
     "if (as11_time_get_offset(&off)) {",
     "if (!as11_time_get_offset(&off)) {",
     "offset used only when it is absent — the two branches swapped"),
]

# Survivors that were measured, read, and judged unable to change behaviour.
# They are listed — not deleted — because the judgement can be wrong: each one
# is still run, and a mutant here that the suite KILLS is reported as REFUTED,
# which means this comment is what needs fixing, not the test.
EQUIVALENT = [
    ("eq-zero-channels", "edf_waveform.c",
     "if (!f || channels_in_file <= 0) return UINT32_MAX;",
     "if (!f || channels_in_file < 0) return UINT32_MAX;",
     "a 0-channel file is refused by the channel-map / channel-count check before it gets here"),
    ("eq-header-only", "edf_waveform.c",
     "if (end <= (long)sizeof(snt_header_t)) return 0;",
     "if (end < (long)sizeof(snt_header_t)) return 0;",
     "for end == sizeof(header) the fall-through computes 0 / frame == 0, the same answer"),
    ("eq-header-fill", "edf_header.c",
     "memset(hdr, ' ', sizeof(hdr));",
     "memset(hdr, 0, sizeof(hdr));",
     "every one of the 256 fixed-header bytes is overwritten by a space-padded field"),
]

SUMMARY_RE = re.compile(r"^host tests: (\d+) run, (\d+) failed, (\d+) skipped$", re.M)


def apply(mutant, main_dir):
    _, fname, find, repl, _ = mutant
    path = os.path.join(main_dir, fname)
    with open(path, encoding="utf-8") as f:
        src = f.read()
    n = src.count(find)
    if n != 1:
        return f"pattern matched {n} times (want 1)"
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src.replace(find, repl))
    return None


def run_suite(main_dir, out_dir):
    """Returns (verdict, detail, output).  verdict: PASS, FAIL, BUILD-FAIL,
    INCONCLUSIVE (no summary line / timeout)."""
    env = dict(os.environ, MAIN_DIR=main_dir, OUT=out_dir)
    try:
        rc = subprocess.run([RUNNER, "--quiet"], env=env, cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired as e:
        return "INCONCLUSIVE", f"timeout after {TIMEOUT_S}s", (e.stdout or "")
    out = rc.stdout
    m = SUMMARY_RE.search(out)
    if not m:
        return "INCONCLUSIVE", "runner produced no 'host tests:' summary line", out
    ran, failed, skipped = (int(x) for x in m.groups())
    if "BUILD FAILED" in out:
        return "BUILD-FAIL", "mutant does not compile (malformed mutant, not a kill)", out
    if failed:
        # Attribute the kill: binary and, where the harness names it, the test.
        bins = re.findall(r"^### (\S+): FAIL", out, re.M)
        tests = re.findall(r"^\s+FAILED (.+?) \(\d+ checks?\)", out, re.M)
        who = ", ".join(bins) + (": " + "; ".join(tests) if tests else "")
        return "FAIL", f"killed by {who}", out
    if skipped:
        return "FAIL", f"{skipped} test(s) skipped — the suite did not cover the file", out
    return "PASS", f"{ran} run, all passed", out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--keep", action="store_true", help="keep the mutated trees")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="snt_mutants_")
    src_main = os.path.join(ROOT, "main")

    def tree(mid):
        mdir = os.path.join(work, mid)
        shutil.copytree(src_main, os.path.join(mdir, "main"),
                        ignore=shutil.ignore_patterns("*.o", "CMakeLists.txt"))
        return os.path.join(mdir, "main"), os.path.join(mdir, "out")

    # -- controls ----------------------------------------------------------
    void = []
    main_dir, out_dir = tree("baseline")
    verdict, detail, out = run_suite(main_dir, out_dir)
    print(f"baseline   {verdict:<12}  {detail}")
    if verdict != "PASS":
        void.append("baseline does not pass on the unmutated tree")
        sys.stdout.write(out)

    main_dir, out_dir = tree("canary")
    err = apply(CANARY, main_dir)
    if err:
        void.append("canary not applied: " + err)
        print(f"canary     NOT APPLIED   {err}")
    else:
        verdict, detail, out = run_suite(main_dir, out_dir)
        print(f"canary     {'killed' if verdict == 'FAIL' else verdict:<12}  {detail}")
        if verdict != "FAIL":
            void.append("canary survived — the suite cannot tell a broken reader from a working one")
            sys.stdout.write(out)

    if void:
        print("\nVOID — this run measured nothing; no kill count is reported:")
        for v in void:
            print("  - " + v)
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        return 2

    # -- mutants -----------------------------------------------------------
    results = []
    for m in MUTANTS:
        mid = m[0]
        if args.only and args.only != mid:
            continue
        main_dir, out_dir = tree(mid)
        err = apply(m, main_dir)
        if err:
            results.append((mid, "NOT APPLIED", err))
            continue
        verdict, detail, out = run_suite(main_dir, out_dir)
        if verdict == "FAIL":
            results.append((mid, "killed", detail))
        elif verdict == "PASS":
            results.append((mid, "SURVIVED", m[4]))
            sys.stdout.write(out)
        else:
            results.append((mid, verdict, detail))

    # -- classified-equivalent survivors ----------------------------------
    refuted = 0
    if not args.only:
        for m in EQUIVALENT:
            main_dir, out_dir = tree(m[0])
            err = apply(m, main_dir)
            if err:
                results.append((m[0], "NOT APPLIED", err))
                continue
            verdict, detail, _ = run_suite(main_dir, out_dir)
            if verdict == "FAIL":
                refuted += 1
                print(f"{m[0]}  REFUTED — classified equivalent, but the suite killed it "
                      f"({detail}).  The classification is wrong: {m[4]}")
            elif verdict != "PASS":
                results.append((m[0], verdict, detail))

    print()
    width = max(len(r[0]) for r in results) if results else 10
    bad = 0
    for mid, status, detail in results:
        print(f"{mid:<{width}}  {status:<12}  {detail}")
        if status != "killed":
            bad += 1
    print(f"\n{len(results) - bad}/{len(results)} mutants killed (canary passed, baseline green)"
          + (f"; {len(EQUIVALENT)} classified equivalent" if not args.only else ""))
    if args.keep:
        print(f"mutated trees kept in {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if (bad or refuted) else 0


if __name__ == "__main__":
    sys.exit(main())

