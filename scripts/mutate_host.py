#!/usr/bin/env python3
# SomnoTrace — scripts/mutate_host.py
# Copyright 2026 Michal Planicka · SPDX-License-Identifier: Apache-2.0
"""
MUTATION ANALYSIS FOR THE HOST TEST SUITE — does a passing test suite actually notice a change?

A green suite proves the tests ran. It does not prove they would have failed. This changes one thing
in the shipped source, rebuilds the host tests, and reports whether they noticed.

── THE DISTINCTION THIS TOOL EXISTS TO MAKE ────────────────────────────────────────────────────────
A surviving mutant is not one finding, it is one of two, and they have OPPOSITE fixes:

    UNREACHED    no test executes that line       →  wire the file in / write a test that reaches it
    UNASSERTED   tests execute it and do not care →  strengthen an assertion

Reporting both as "SURVIVED" fuses them into a queue nobody can act on. Issue #202 was entirely the
first kind: mutating `spool_to_edf` in edf_summary.c changed nothing because the host suite never
linked that translation unit — the test carried a private copy. A survivor count alone would have
read as "the tests are weak"; the actual fix was a build-and-include change.

Reach is measured with gcov, and ⚠️ THE SKIP LIST FAILS CLOSED. Coverage missing, gcov absent, file
not in the report, unparseable line — every one of those resolves to REACHED, i.e. run the mutant.
Over-running mutants costs a few seconds. Under-running them costs the programme its meaning: a skip
list that fails OPEN quietly stops testing code and reports the silence as progress.

── TWO CONTROLS, BOTH MANDATORY ────────────────────────────────────────────────────────────────────
BASELINE   the unmutated tree must build and pass first. If it does not, nothing below is reported —
           a mutant "killed" by an already-red suite is not a kill.
CANARY     one mutant no working suite could miss: the body of a function the tests exercise is
           emptied outright (extreme mutation — Descartes; Petrović & Ivanković, ICSE-SEIP '18).
           If the canary SURVIVES, the harness is not running what it thinks it is and the entire
           run is VOID rather than reported as "0 survivors". A zero means nothing without it.

── WHAT A SURVIVOR IS NOT ──────────────────────────────────────────────────────────────────────────
Not automatically a gap. `if (x < 0) x = 0;` mutated to `<=` still assigns 0 when x IS 0, and no
input separates them. Such a mutant is EQUIVALENT and unkillable by anyone. Record them in
`scripts/mutate_equivalent.txt` with the reason, one `file:line:op` per line, so the next run does
not re-report them and the reason survives the person who worked it out.

Usage
    python3 scripts/mutate_host.py --selftest             # prove the harness detects and discriminates
    python3 scripts/mutate_host.py main/as11_time.c
    python3 scripts/mutate_host.py main/edf_data_dict.h --limit 40
    python3 scripts/mutate_host.py --all                  # every source a host test links

Exit codes: 0 clean · 1 survivors · 2 VOID (baseline or canary failed) · 3 setup error
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MAIN = os.path.join(ROOT, "main")
INC = [os.path.join(ROOT, "main"), os.path.join(HERE, "test_include"), ROOT]

# Each host test, and the shipped translation units it links. Keeping this explicit (rather than
# globbing main/) is deliberate: the set of files a test links IS the thing #202 was about, so it
# should be visible and reviewable rather than inferred.
# A header a test #includes is a mutation target too, not just a .c it links: the shipped
# spool_to_edf() lives in edf_data_dict.h and is the code #202 was about. Headers are listed
# for targeting and reach; they are never passed to the compiler.
#
# A "#" prefix means the same thing for a .c file: v2.0.0 split the EDF converter into five
# modules, and edf_gen_test.c #includes all five so it can reach their statics. They are part
# of that test's own translation unit, so listing them WITHOUT the prefix would hand gcc a
# second copy and every build would fail on duplicate symbols. They are still shipped code a
# host test executes, so they are targeted and counted; they are just not passed to the
# compiler. Before this marker existed they were invisible: five files the suite has exercised
# since v2.0.0 were reported as untested, because the map is what the harness believes.
TESTS = {
    "as11_time_test":       ["main/as11_time.c"],
    "as11_events_test":     ["main/as11_time.c", "@cjson"],
    "edf_gen_test":         ["main/as11_time.c", "@cjson",
                             "#main/edf_header.c", "#main/edf_waveform.c",
                             "#main/edf_annotations.c", "#main/edf_summary.c",
                             "#main/edf_gen.c"],
    "edf_properties_test":  ["main/as11_time.c", "main/edf_data_dict.h", "@cjson",
                             "#main/edf_summary.c"],
    "vld3_decoder_test":    ["main/oximetry_vld3.c"],
}


def target_path(entry: str) -> str:
    """The repo-relative path a TESTS entry names, without the "#" include-marker."""
    return entry[1:] if entry.startswith("#") else entry

EQUIV_FILE = os.path.join(HERE, "mutate_equivalent.txt")
SURVIVOR_FILE = os.path.join(HERE, "mutation_survivors.txt")


def environment_is_complete() -> tuple[bool, str]:
    """(may this run rewrite the inventory, and if not why not).

    ⚠️ A NARROWER ENVIRONMENT SEES FEWER SURVIVORS. Without cJSON the two EDF suites are
    skipped and this harness reports SCOPE 2 of 80 instead of 8; every survivor in the five
    EDF modules simply is not found. Writing the inventory from that run would DELETE those
    entries, and the diff would read exactly like someone had fixed them.

    So the inventory may only be written where the whole suite builds. Refusing is the
    feature: an inventory that quietly narrows is worse than no inventory, because it is
    trusted."""
    skipped = sorted(t for t in TESTS if sources_for(t) is None)
    if skipped:
        return False, "these suites could not be built: " + ", ".join(skipped)
    if not shutil.which("gcov"):
        return False, ("gcov is absent, so reach is unknown and every mutant is run and "
                       "classified UNASSERTED rather than UNREACHED")
    return True, ""


def load_inventory() -> dict[str, str]:
    out = {}
    if os.path.exists(SURVIVOR_FILE):
        for line in open(SURVIVOR_FILE, encoding="utf-8"):
            key = line.split("#", 1)[0].strip()
            if key:
                out[key] = line.split("#", 1)[1].strip() if "#" in line else ""
    return out


def write_inventory(entries: dict[str, str]) -> None:
    with open(SURVIVOR_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "# UNASSERTED survivors: mutants that RAN and that every host test still passed.\n"
            "# Committed on purpose. A gitignored copy would exist only on the machine that\n"
            "# produced it, and the useful property of this file is that a pull request which\n"
            "# weakens an assertion ADDS A LINE HERE, next to the change that caused it —\n"
            "# visible in review without anyone running the sweep.\n"
            "#\n"
            "# NOT a list of bugs, and not a to-do list. A survivor means the suite does not\n"
            "# distinguish this mutant from the original; whether that matters is a judgement\n"
            "# each one needs on its own. A mutant no input can kill belongs in\n"
            "# mutate_equivalent.txt instead, with the argument written down.\n"
            "#\n"
            "# Regenerate with:  python3 scripts/mutate_host.py --all --write-inventory\n"
            "# It REFUSES on a machine where any suite is skipped — see environment_is_complete().\n"
            "#\n"
            "# Format:  <path>:<line>:<operator>      # the line as it stands today\n"
            "\n")
        # path:line:operator — and the OPERATOR CONTAINS COLONS ("arithmetic:+→-"), so this
        # splits from the left with maxsplit=2. rsplit here silently sorted by the wrong
        # field and then crashed on int().
        for key in sorted(entries, key=lambda k: (k.split(":", 2)[0], int(k.split(":", 2)[1]))):
            src = entries[key]
            f.write(f"{key}\n" if not src else f"{key:<52} # {src}\n")



def find_cjson() -> str | None:
    """cJSON.c, wherever this checkout keeps it. Not bundled, so it is searched rather than assumed.

    ⚠️ BOUNDED ON PURPOSE. An earlier version fell back to a recursive walk of $HOME. On a box
    with a large data volume it never returned: the tool hung before printing a line and its own
    timeout killed it. A search for a build input is not worth an unbounded filesystem walk — look
    where this project puts it, then ask. Override with $SNT_CJSON or --cjson."""
    env = os.environ.get("SNT_CJSON")
    if env and os.path.exists(env):
        return env
    # $CJSON_DIR is what scripts/run_host_tests.sh already documents, so a checkout that
    # can run the host suite can run this too without learning a second variable. It names
    # the DIRECTORY holding cJSON.c/.h, where $SNT_CJSON above names the file.
    envdir = os.environ.get("CJSON_DIR")
    if envdir:
        cand = os.path.join(envdir, "cJSON.c")
        if os.path.exists(cand):
            return cand
    for pat in ("managed_components/*/cJSON.c", "managed_components/*/*/cJSON.c",
                "components/*/cJSON.c", "build/**/cJSON.c"):
        hit = glob.glob(os.path.join(ROOT, pat), recursive=True)
        if hit:
            return hit[0]
    for direct in ("/usr/include/cjson/cJSON.c", "/usr/local/src/cJSON/cJSON.c",
                   "/usr/share/cjson/cJSON.c"):
        if os.path.exists(direct):
            return direct
    return None


CJSON = find_cjson()


def find_cjson_system() -> str | None:
    """The include directory of a cJSON installed as a system library, or None.

    apt's libcjson-dev -- which is what BOTH workflows install -- ships cJSON.h and
    libcjson.so and no cJSON.c at all. find_cjson() above hunts only for the source, so it
    returns None there and every cJSON-linking test is skipped.

    scripts/run_host_tests.sh has always handled this case, with -lcjson. The harness did not,
    and the consequence was measured rather than guessed: in a ubuntu:24.04 container carrying
    exactly the packages the workflows install, run_host_tests.sh reports "5 run, 0 failed,
    0 skipped" while this harness built 2 of the 5 tests and printed SCOPE 2 of 80. A tool
    whose job is to report what the suite does not cover was itself covering less than the
    suite, on the one machine where CI runs.

    Deliberately not a fallback for a missing source: build() prefers cJSON.c when there is
    one, because a source can be compiled in with the same flags as everything else."""
    for d in ("/usr/include/cjson", "/usr/local/include/cjson"):
        if os.path.exists(os.path.join(d, "cJSON.h")):
            return d
    return None


CJSON_SYS = find_cjson_system()


def sources_for(test: str) -> list[str] | None:
    out = []
    for s in TESTS[test]:
        if s == "@cjson":
            if CJSON:
                out.append(CJSON)       # compiled in from source
            elif CJSON_SYS is None:
                return None             # neither source nor system library: genuinely skipped
            # else: linked as -lcjson in build(), so there is no source to add here
        elif s.startswith("#"):
            continue      # already inside the test's translation unit -- see TESTS
        elif not s.endswith(".h"):
            out.append(os.path.join(ROOT, s))
    return out


def build(test: str, outdir: str, coverage: bool = False) -> str | None:
    src = sources_for(test)
    if src is None:
        return None
    exe = os.path.join(outdir, test)
    cmd = ["gcc", "-O0", "-o", exe, os.path.join(HERE, test + ".c"), *src]
    if coverage:
        cmd += ["--coverage"]
    # Header order depends on what the test links, and it has to.
    #
    # scripts/test_include/cJSON.h is not a declaration set: it is a working stub, 17 static
    # inline functions. A test that does NOT link cJSON.c therefore gets its implementation
    # from that header alone, and putting the real cJSON.h in front turns those inlines into
    # ordinary externs -- as11_time.c then fails to link on undefined cJSON_GetObjectItem.
    #
    # A test that DOES link cJSON.c needs the opposite. The stub covers a 20-function subset
    # and lacks cJSON_Print, cJSON_IsTrue and cJSON_AddItemReferenceToObject, so behind the
    # shim such a test dies on implicit declarations. edf_gen_test is the first wired test to
    # reach past the subset, which is why this stayed invisible until it was added to TESTS.
    #
    # run_host_tests.sh has always ordered it this way -- $CJ_INC ahead of $SHIM, and only on
    # the two cJSON legs. This is the harness catching up with the suite it measures.
    links_cjson = "@cjson" in TESTS[test]
    if links_cjson and CJSON:
        cmd.append("-I" + os.path.dirname(CJSON))
    elif links_cjson and CJSON_SYS:
        cmd.append("-I" + CJSON_SYS)
    cmd += [f"-I{d}" for d in INC]
    cmd.append("-lm")
    if links_cjson and not CJSON and CJSON_SYS:
        cmd.append("-lcjson")           # -l goes after the objects that need it
    r = subprocess.run(cmd, capture_output=True, cwd=outdir)
    return exe if r.returncode == 0 else None


def run(exe: str, env_tz: str = "UTC") -> bool:
    """True when the test passes. TZ is pinned: a suite whose result depends on the host clock
    cannot distinguish a killed mutant from a different machine."""
    env = dict(os.environ, TZ=env_tz)
    try:
        return subprocess.run([exe], capture_output=True, env=env, timeout=300).returncode == 0
    except subprocess.TimeoutExpired:
        return False          # a hung mutant is killed, not survived


def suite_passes(outdir: str) -> tuple[bool, str]:
    for t in TESTS:
        exe = build(t, outdir)
        if exe is None:
            if sources_for(t) is None:
                continue      # cJSON absent — that test is skipped, not failed
            return False, f"{t}: build failed"
        if not run(exe):
            return False, f"{t}: failed"
    return True, "all pass"


# ── reach ───────────────────────────────────────────────────────────────────────────────────────
def executed_lines(outdir: str) -> dict[str, set[int]] | None:
    """{abs_path: {line numbers executed by at least one host test}}, or None if reach is unknown.

    None is the FAIL-CLOSED signal: every caller must treat it as "everything is reached"."""
    if not shutil.which("gcov"):
        return None
    cov: dict[str, set[int]] = {}
    ok_any = False
    for t in TESTS:
        if sources_for(t) is None:
            continue
        d = tempfile.mkdtemp(prefix=f"cov-{t}-", dir=outdir)
        exe = build(t, d, coverage=True)
        if exe is None or not run(exe):
            continue
        gcda = glob.glob(os.path.join(d, "*.gcda"))
        if not gcda:
            continue
        subprocess.run(["gcov", "-p", *gcda], capture_output=True, cwd=d)
        for g in glob.glob(os.path.join(d, "*.gcov")):
            src = None
            try:
                with open(g, encoding="utf-8", errors="replace") as f:
                    for line in f:
                        m = re.match(r"\s*-:\s*0:Source:(.*)", line)
                        if m:
                            src = os.path.abspath(m.group(1).strip())
                            continue
                        m = re.match(r"\s*([^:]+):\s*(\d+):", line)
                        if not m or src is None:
                            continue
                        cnt, num = m.group(1).strip(), int(m.group(2))
                        # Two markers mean "executable but never executed": ##### and =====. A count, a
                        # count with gcov's "*" partial-branch suffix ("9*"), or a form this parser has
                        # never seen are all REACHED. The first version accepted only [#\-0-9=] and so
                        # silently dropped every "N*" line: the ternaries and short-circuits, i.e. the
                        # lines mutation exists to test, all reported as UNREACHED.
                        #
                        # "-" WAS ON THAT LIST AND SHOULD NOT HAVE BEEN. It does not mean "never
                        # executed"; it means the line carries no executable code OF ITS OWN, which is
                        # what gcov prints for the CONTINUATION lines of a multi-line expression:
                        #       20:   57:    if (!data || !out || len < OX_VLD3_HEADER_LEN ||
                        #        -:   58:        source_size < OX_VLD3_HEADER_LEN + OX_VLD3_RECORD_LEN)
                        # The whole condition is charged to line 57 and line 58 gets "-", so a mutant on
                        # 58 was reported UNREACHED -- "write a test that reaches it" for a line executed
                        # twenty times -- and a test that DID kill it could not clear the report. Every
                        # wrapped condition in the tree was affected, which is most of the interesting
                        # ones. Reach is UNKNOWN for such a line, and unknown is fail-closed here: keep
                        # it, run the mutant, let the suite answer. Same shape as the "N*" bug above:
                        # a gcov form the parser did not understand, read as absence.
                        if num and cnt not in ("#####", "====="):
                            cov.setdefault(src, set()).add(num)
                ok_any = True
            except OSError:
                return None
    # An empty dict is not "nothing is reached", it is "reach is unknown". Return None so
    # every caller runs every mutant.
    return cov if cov else None


def harness_blind_spot() -> tuple[int, int, list[str]]:
    """(covered, total, a few uncovered names) for the C sources under main/ and components/.

    ⚠️ THE HARNESS ONLY SEES WHAT A HOST TEST LINKS. TESTS above is a hand-written map, so a
    file no wired test compiles is not "clean" here -- it is INVISIBLE, and a run that says
    "0 unasserted survivors" says nothing whatever about it. Measured the day this was
    written: a branch adding 63 new .c/.h files under main/ produced an identical report to
    the branch without them, because none of them is linked by a wired test.

    A count is printed on every run so that reading the summary tells you the denominator.
    Silence about the denominator is how a mutation score becomes a comfort."""
    covered = set()
    for t in TESTS:
        # A test that cannot build covers nothing. Counting its sources anyway inflates the
        # numerator on exactly the machines where it is least true: without cJSON the two EDF
        # suites are skipped, and before this check the summary still credited them.
        if sources_for(t) is None:
            continue
        for src in TESTS[t]:
            if src != "@cjson":
                covered.add(os.path.abspath(os.path.join(ROOT, target_path(src))))
    total = []
    for base in ("main", "components"):
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, base)):
            if any(skip in dirpath for skip in ("/build", "/managed_components", "/third_party")):
                continue
            for f in files:
                if f.endswith((".c", ".h")):
                    total.append(os.path.abspath(os.path.join(dirpath, f)))
    uncovered = sorted(set(total) - covered)
    names = [os.path.relpath(p, ROOT) for p in uncovered[:5]]
    return len(covered), len(total), names


# ── mutants ─────────────────────────────────────────────────────────────────────────────────────
OPS: list[tuple[str, str, str]] = [
    ("<=", "<", "relational"), ("<", "<=", "relational"),
    (">=", ">", "relational"), (">", ">=", "relational"),
    ("==", "!=", "equality"), ("!=", "==", "equality"),
    ("&&", "||", "logical"), ("||", "&&", "logical"),
    (" + ", " - ", "arithmetic"), (" - ", " + ", "arithmetic"),
    (" * ", " / ", "arithmetic"),
]

SKIP_LINE = re.compile(r'^\s*(//|/\*|\*|#include|#pragma)')


def load_equivalents() -> set[str]:
    out = set()
    if os.path.exists(EQUIV_FILE):
        for line in open(EQUIV_FILE, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line:
                out.add(line)
    return out


def gen_mutants(path: str, limit: int) -> list[tuple[int, str, str, str]]:
    """(lineno, op_name, original_line, mutated_line) — textual, one change each."""
    out = []
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()
    for i, line in enumerate(lines, 1):
        if SKIP_LINE.match(line) or '"' in line:
            continue          # string literals: a changed message is not a behaviour change
        for a, b, name in OPS:
            if a in line:
                out.append((i, f"{name}:{a.strip()}→{b.strip()}", line, line.replace(a, b, 1)))
                break
        if len(out) >= limit:
            break
    return out


# A top-level function definition: a line starting at column 0 that is not a control keyword and
# carries an opening paren. Deliberately loose — the previous version required a single-line
# signature returning one of six named types, and across main/*.c it matched NOTHING in 29 files
# while reporting "no canary candidate found" and carrying on. Most functions here return esp_err_t
# or void, and many split their parameters across lines.
_DEF = re.compile(r'^[A-Za-z_][A-Za-z0-9_ \t\*]*\b(\w+)\s*\(')
_NOT_A_DEF = re.compile(r'^\s*(if|for|while|switch|return|else|do|typedef|struct|enum|union)\b')


def canary_for(path: str, lines: list[str]) -> tuple[int, str, str] | None:
    """Empty the body of a function with a substantial body — extreme mutation (Descartes). No suite
    that executes the function can miss it.

    Returns the LARGEST body found rather than the first: a three-line accessor is a weak canary
    (its removal can be invisible if callers ignore the result), and a weak canary that survives
    reads exactly like a broken harness."""
    best = None
    for i, line in enumerate(lines):
        if _NOT_A_DEF.match(line) or not _DEF.match(line) or line.rstrip().endswith(";"):
            continue
        m = _DEF.match(line)
        # walk forward to the opening brace of the body, tolerating a multi-line signature
        j, guard = i, 0
        while j < len(lines) and "{" not in lines[j]:
            if ";" in lines[j] or guard > 6:
                break
            j += 1
            guard += 1
        if j >= len(lines) or "{" not in lines[j]:
            continue
        depth, k = 0, j
        while k < len(lines):
            depth += lines[k].count("{") - lines[k].count("}")
            if depth == 0:
                break
            k += 1
        body_start, body_end = j + 1, k
        n = body_end - body_start
        if n >= 4 and (best is None or n > best[3]):
            best = (body_start, m.group(1), "".join(lines[body_start:body_end]), n)
    return (best[0], best[1], best[2]) if best else None


class MutationTargetMissing(RuntimeError):
    """The text this mutation was meant to change is not in the file. Applying nothing and then
    measuring the suite would report the UNMUTATED tree's result as the mutant's — which for
    the selftest's planted-equivalent control means 'survived (correct)' on a control that
    never ran."""


def apply(path: str, old_text: str, new_text: str) -> None:
    s = open(path, encoding="utf-8").read()
    if old_text not in s:
        raise MutationTargetMissing(f"{os.path.relpath(path, ROOT)}: target text not present")
    open(path, "w", encoding="utf-8", newline="\n").write(s.replace(old_text, new_text, 1))


def apply_line(path: str, lineno: int, orig: str, mut: str) -> None:
    """Replace exactly line `lineno`. A text search would hit the first identical line in the
    file, mutate that, and credit the kill (or survival) to a line that was never touched."""
    lines = open(path, encoding="utf-8").readlines()
    if lines[lineno - 1] != orig:
        raise MutationTargetMissing(f"{os.path.relpath(path, ROOT)}:{lineno}: line text changed")
    lines[lineno - 1] = mut
    open(path, "w", encoding="utf-8", newline="\n").writelines(lines)


def mutate_file(path: str, outdir: str, reach: dict | None, limit: int, equivs: set[str]) -> dict:
    rel = os.path.relpath(path, ROOT)
    backup = path + ".mutbak"
    shutil.copy(path, backup)
    lines = open(path, encoding="utf-8").readlines()
    res = {"file": rel, "killed": 0, "stillborn": 0, "unasserted": [], "unreached": [],
           "equivalent": 0, "canary": None, "generated": 0, "reach_unknown": False}
    try:
        # ── canary first: if it survives, nothing else this run means anything
        c = canary_for(path, lines)
        if c is None:
            # ⚠️ FAIL CLOSED. No canary means no way to show the harness can detect anything in this
            # file, and an uncontrolled run reports "0 survivors" for a file it may never have
            # compiled. The first version carried on regardless — it found no candidate in any of
            # 29 files and would have reported every one of them as clean.
            res["canary"] = {"fn": None, "line": 0, "survived": None}
            return res
        ln, fname, body = c
        apply(path, body, "")
        ok, _ = suite_passes(outdir)
        shutil.copy(backup, path)
        res["canary"] = {"fn": fname, "line": ln, "survived": ok}
        if ok:
            return res

        mutants = gen_mutants(path, limit)
        res["generated"] = len(mutants)
        # ⚠️ Per-file, not just global: a file absent from the coverage report (its coverage build
        # failed, its test timed out, gcov named it differently) is REACH UNKNOWN. Looking it up
        # with a default of set() classified every mutant UNREACHED, compiled nothing, and exited
        # 0 — the exact fail-open the module docstring promises does not exist.
        file_reach = reach.get(os.path.abspath(path)) if reach is not None else None
        res["reach_unknown"] = reach is not None and file_reach is None
        for ln, op, orig, mut in mutants:
            key = f"{rel}:{ln}:{op}"
            if key in equivs:
                res["equivalent"] += 1
                continue
            # gcov marks a #define non-executable, but mutating one acts at every use site: never
            # let the reach filter skip it.
            is_macro = orig.lstrip().startswith("#define")
            if file_reach is not None and not is_macro and ln not in file_reach:
                res["unreached"].append((ln, op, orig.strip()[:70]))
                continue
            apply_line(path, ln, orig, mut)
            ok, why = suite_passes(outdir)
            shutil.copy(backup, path)
            if ok:
                res["unasserted"].append((ln, op, orig.strip()[:70]))
            elif why.endswith("build failed"):
                # A mutant that does not compile proves nothing about the tests. Counting it as a
                # kill inflates the score with kills the compiler made, not the suite.
                res["stillborn"] += 1
            else:
                res["killed"] += 1
    finally:
        shutil.copy(backup, path)
        os.unlink(backup)
    return res


# ── selftest ────────────────────────────────────────────────────────────────────────────────────
def selftest(outdir: str) -> int:
    """A zero means nothing without controls, so prove both directions before trusting a run.

    PLANTED-KILLABLE   a real behaviour change the suite MUST notice.
    PLANTED-EQUIVALENT a change no input can distinguish; it MUST survive, and a harness that
                       'kills' it is reporting noise as signal."""
    print("── selftest")
    ok, why = suite_passes(outdir)
    print(f"   baseline                     {'PASS' if ok else 'FAIL — ' + why}")
    if not ok:
        return 2

    target = os.path.join(MAIN, "as11_time.c")
    backup = target + ".selfbak"
    shutil.copy(target, backup)
    rc = 0
    try:
        # must be caught: the noon rule inverted
        try:
            apply(target, "if (tod < 43200) days -= 1;", "if (tod < 43200) days -= 0;")
        except MutationTargetMissing as e:
            print(f"   planted KILLABLE mutant      TARGET MISSING — {e}")
            print("   VERDICT: HARNESS UNSOUND — the control text is not in the source; nothing was tested")
            return 2
        caught = not suite_passes(outdir)[0]
        shutil.copy(backup, target)
        print(f"   planted KILLABLE mutant      {'caught (correct)' if caught else 'MISSED — harness is blind'}")
        rc |= 0 if caught else 2

        # must survive: floor_div's sign fix is unreachable for the positive inputs the suite uses…
        # …so a harness that reports this as a gap is flooding. It is recorded, not celebrated.
        try:
            apply(target, "int64_t q = a / b;", "int64_t q = (a) / (b);")
        except MutationTargetMissing as e:
            print(f"   planted EQUIVALENT mutant    TARGET MISSING — {e}")
            print("   VERDICT: HARNESS UNSOUND — an unapplied control would have read as 'survived (correct)'")
            return 2
        survived = suite_passes(outdir)[0]
        shutil.copy(backup, target)
        print(f"   planted EQUIVALENT mutant    {'survived (correct)' if survived else 'KILLED — the harness is unstable'}")
        rc |= 0 if survived else 2
    finally:
        shutil.copy(backup, target)
        os.unlink(backup)
    print("   VERDICT:", "harness discriminates" if rc == 0 else "HARNESS UNSOUND — do not trust a run")
    return rc


def main() -> int:
    # Line-buffered: a run that hangs or is killed must still show how far it got. The first version
    # buffered, was killed by a timeout, and printed nothing at all — not even the baseline result
    # that would have said where it stopped.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:      # pragma: no cover
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--limit", type=int, default=25, help="max mutants per file")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--cjson", help="path to cJSON.c when it is not beside the project")
    ap.add_argument("--write-inventory", action="store_true",
                    help="rewrite scripts/mutation_survivors.txt from this run; needs --all, "
                         "and refuses where any suite is skipped")
    a = ap.parse_args()

    global CJSON
    if a.cjson:
        CJSON = a.cjson

    if not shutil.which("gcc"):
        print("gcc not found", file=sys.stderr)
        return 3
    outdir = tempfile.mkdtemp(prefix="snt-mutate-")
    print("cJSON: " + (CJSON or (f"-lcjson from {CJSON_SYS}" if CJSON_SYS else
                                 "<not found — tests needing it are SKIPPED, not failed>")))

    if a.selftest:
        return selftest(outdir)

    ok, why = suite_passes(outdir)
    print(f"BASELINE  {'PASS' if ok else 'FAIL — ' + why}")
    if not ok:
        print("VOID — the baseline does not pass, so no verdict below would mean anything.")
        return 2

    reach = executed_lines(outdir)
    cov_n, cov_total, cov_names = harness_blind_spot()
    if cov_total:
        blind = cov_total - cov_n
        print(f"SCOPE     {cov_n} of {cov_total} C sources are linked by a wired host test; "
              f"{blind} are INVISIBLE to this harness")
        if blind:
            print("          not seen: " + ", ".join(cov_names) +
                  (f", +{blind - len(cov_names)} more" if blind > len(cov_names) else ""))
            print("          a clean report below says nothing about those files.")
    print("REACH     " + ("gcov line coverage available" if reach is not None
                          else "UNAVAILABLE — failing closed, every mutant will be run"))

    targets = a.files or ([os.path.join(ROOT, target_path(s))
                           for t in TESTS for s in TESTS[t] if s != "@cjson"]
                          if a.all else [])
    targets = sorted({os.path.abspath(t if os.path.isabs(t) else os.path.join(ROOT, t))
                      for t in targets})
    if not targets:
        ap.error("name a file, or pass --all")

    equivs = load_equivalents()
    total_surv = 0
    all_surv: dict[str, str] = {}
    for t in targets:
        if not os.path.exists(t):
            print(f"\n{t}: not found"); return 3
        r = mutate_file(t, outdir, reach, a.limit, equivs)
        print(f"\n══ {r['file']}")
        if r["canary"]:
            c = r["canary"]
            if c["survived"] is None:
                print("   🔴 NO CANARY — no function in this file has a body large enough to empty.")
                print("   VOID: without a canary there is no evidence this harness can detect")
                print("         anything here, and '0 survivors' would be indistinguishable from")
                print("         a file that was never compiled.")
                return 2
            if c["survived"]:
                print(f"   🔴 CANARY SURVIVED — {c['fn']}() body emptied and every test still passed.")
                print("   VOID: the harness is not running what it thinks it is. No other result here counts.")
                return 2
            print(f"   canary: {c['fn']}() body emptied → killed")
        if r["reach_unknown"]:
            print("   ⚠️  REACH UNKNOWN for this file (not in the coverage report) — every mutant was run.")
        if r["generated"] == 0:
            print("   ⚠️  NO MUTANTS GENERATED — nothing below was tested. The canary is the only evidence.")
        print(f"   killed {r['killed']}   stillborn {r['stillborn']}   unasserted {len(r['unasserted'])}   "
              f"unreached {len(r['unreached'])}   known-equivalent {r['equivalent']}")
        for ln, op, src in r["unasserted"]:
            print(f"     🔴 UNASSERTED  {r['file']}:{ln}  {op}   {src}")
        for ln, op, src in r["unreached"][:10]:
            print(f"     ·  UNREACHED   {r['file']}:{ln}  {op}   {src}")
        if len(r["unreached"]) > 10:
            print(f"     ·  … and {len(r['unreached']) - 10} more unreached")
        for ln, op, src in r["unasserted"]:
            all_surv[f"{r['file']}:{ln}:{op}"] = src.strip()
        total_surv += len(r["unasserted"])

    print(f"\n{total_surv} unasserted survivor(s)")
    print("UNREACHED lines need a test that reaches them; UNASSERTED need a stronger assertion.")

    complete, why = environment_is_complete()
    if a.write_inventory:
        if not a.all:
            print("\nREFUSED: --write-inventory needs --all. A single file cannot rewrite the "
                  "whole inventory\n         without deleting every entry it did not look at.")
            return 4
        if not complete:
            print(f"\nREFUSED to write {os.path.relpath(SURVIVOR_FILE, ROOT)}: {why}")
            print("         This run saw less than the full suite, so writing would delete "
                  "entries it never\n         looked for — a diff that reads exactly like "
                  "someone had fixed them.")
            return 4
        write_inventory(all_surv)
        print(f"\nwrote {os.path.relpath(SURVIVOR_FILE, ROOT)} — {len(all_surv)} survivor(s)")
    elif a.all and os.path.exists(SURVIVOR_FILE):
        known = load_inventory()
        added = sorted(set(all_surv) - set(known))
        gone = sorted(set(known) - set(all_surv))
        print(f"\nINVENTORY  {len(known)} known, {len(added)} new, {len(gone)} no longer found")
        if not complete:
            print(f"           ⚠️ comparison is UNRELIABLE here: {why}")
            print("           Entries under 'no longer found' may simply not have been looked for.")
        for k in added:
            print(f"     + NEW       {k}   {all_surv[k]}")
        for k in gone:
            print(f"     - was here  {k}")
        if added:
            print("           A new survivor is an assertion that stopped distinguishing "
                  "something.\n           Regenerate with --all --write-inventory once it is "
                  "understood, not before.")
    return 1 if total_surv else 0


if __name__ == "__main__":
    sys.exit(main())

