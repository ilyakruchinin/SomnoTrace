#!/usr/bin/env python3
# SomnoTrace - Ask a local model for mutants, let the test suite judge them
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

"""Where are the holes in the host suite that nobody thought of?

scripts/mutants.py plants bugs a person picked.  This script asks a local
model (Ollama) to propose one-line bugs for a function, then applies every
proposal to a copy of main/ and runs the suite — exactly as mutants.py does.
The model's contribution is DATA (find/replace strings), never a verdict:
a proposal is never believed, it is measured.  A bad proposal costs one
build.  Only the survivors matter: each one is a bug the suite would not
notice, and a candidate for a test (and then for the MUTANTS table).

Requires a running Ollama (http://127.0.0.1:11434) with a code model.  Not
part of CI; nothing here is needed to run or trust the suite.

Usage: scripts/mutants_probe.py --function snt_available_samples [--file edf_gen.c]
                                [--model qwen3.8:27b] [--n 8] [--keep] [--dry-run]
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mutants import ROOT, apply, run_suite  # noqa: E402  (same machinery, same rules)

HOST = os.environ.get("OLLAMA_HOST", "http://127.0.0.1:11434")

PROMPT = """You are helping mutation-test C firmware.  Below is one function from {fname}.
Propose {n} plausible ONE-LINE bugs a careful programmer could introduce in it:
off-by-one, inverted or weakened condition, wrong constant or sentinel, dropped
clamp or check, wrong variable, wrong operator.  Each must change behaviour on
some input; no comment-only or whitespace-only edits; do not change signatures.

Return ONLY a JSON object of the form
{{"mutants": [{{"find": "<exact substring of the function, one line>",
              "replace": "<that line with the bug>",
              "why": "<one short sentence: what goes wrong>"}}, ...]}}
"find" must be copied verbatim from the function (including spacing) and must
occur exactly once in it.  "replace" must differ from "find".

Function:
```c
{body}
```
"""


def extract_function(src, name):
    """Return (start, end) offsets of the definition of `name` in src, or None.
    Definition = a line starting at column 0 that contains `name(` and is not a
    prototype, followed by a brace-balanced body."""
    # `)` may be followed by a trailing comment and/or a newline before `{`.
    tail = r"\s*(?:/\*.*?\*/|//[^\n]*)?\s*\{"
    for m in re.finditer(r"^[A-Za-z_][^;{}\n]*\b" + re.escape(name) + r"\s*\([^;{}]*\)" + tail,
                         src, re.M | re.S):
        start = m.start()
        depth, i = 0, m.end() - 1
        while i < len(src):
            c = src[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return start, i + 1
            i += 1
    return None


def ask(model, prompt, timeout_s):
    req = {"model": model, "prompt": prompt, "stream": False, "format": "json",
           "options": {"temperature": 0.7, "num_ctx": 8192}}
    for think in (False, None):          # qwen3-style models: switch thinking off if supported
        body = dict(req)
        if think is not None:
            body["think"] = think
        data = json.dumps(body).encode()
        try:
            with urllib.request.urlopen(urllib.request.Request(
                    HOST + "/api/generate", data=data,
                    headers={"Content-Type": "application/json"}), timeout=timeout_s) as r:
                return json.loads(r.read())["response"]
        except urllib.error.HTTPError as e:
            if think is not None and e.code == 400:
                continue                  # model does not take the think flag; retry without
            raise
    raise RuntimeError("unreachable")


def parse_proposals(text):
    text = text.strip()
    text = re.sub(r"^```(?:json)?\s*|\s*```$", "", text)
    obj = json.loads(text)
    out = []
    for p in obj.get("mutants", []) if isinstance(obj, dict) else obj:
        if not isinstance(p, dict):
            continue
        f, r, why = p.get("find"), p.get("replace"), p.get("why", "")
        if isinstance(f, str) and isinstance(r, str) and f and r and f != r:
            out.append((f, r, str(why)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--function", required=True, action="append",
                    help="function name (repeatable)")
    ap.add_argument("--file", default="edf_gen.c", help="file under main/ (default edf_gen.c)")
    ap.add_argument("--model", default="qwen3.8:27b")
    ap.add_argument("--n", type=int, default=8, help="proposals to ask for per function")
    ap.add_argument("--timeout", type=int, default=900, help="seconds to wait for the model")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="show proposals, do not build")
    args = ap.parse_args()

    path = os.path.join(ROOT, "main", args.file)
    with open(path, encoding="utf-8") as f:
        src = f.read()

    proposals = []                       # (function, find, replace, why)
    missing = []
    for fn in args.function:
        span = extract_function(src, fn)
        if not span:
            # Not fatal: the other functions in this batch are still worth
            # probing, and a typo should not throw away a 10-minute run.
            print(f"{fn}: definition not found in {args.file} — skipped", file=sys.stderr)
            missing.append(fn)
            continue
        body = src[span[0]:span[1]]
        print(f"=== {fn} ({body.count(chr(10)) + 1} lines): asking {args.model} for {args.n} mutants")
        try:
            text = ask(args.model, PROMPT.format(fname=args.file, n=args.n, body=body), args.timeout)
            props = parse_proposals(text)
        except (urllib.error.URLError, json.JSONDecodeError, KeyError, OSError) as e:
            print(f"    model call failed: {e}", file=sys.stderr)
            return 2
        for find, repl, why in props:
            n_fn, n_file = body.count(find), src.count(find)
            if n_fn != 1 or n_file != 1:
                print(f"    rejected (find occurs {n_fn}x in function, {n_file}x in file): {find.strip()[:70]}")
                continue
            proposals.append((fn, find, repl, why))
        print(f"    {len(props)} proposed, {sum(1 for p in proposals if p[0] == fn)} usable")

    if args.dry_run:
        for fn, find, repl, why in proposals:
            print(f"\n[{fn}] {why}\n  - {find.strip()}\n  + {repl.strip()}")
        return 0

    work = tempfile.mkdtemp(prefix="snt_probe_")
    src_main = os.path.join(ROOT, "main")
    results = []
    for i, (fn, find, repl, why) in enumerate(proposals):
        mdir = os.path.join(work, f"m{i}")
        shutil.copytree(src_main, os.path.join(mdir, "main"),
                        ignore=shutil.ignore_patterns("*.o", "CMakeLists.txt"))
        err = apply((f"m{i}", args.file, find, repl, why), os.path.join(mdir, "main"))
        if err:
            results.append((fn, "NOT APPLIED", err, find, repl, why))
            continue
        verdict, detail, _ = run_suite(os.path.join(mdir, "main"), os.path.join(mdir, "out"))
        status = {"FAIL": "killed", "PASS": "SURVIVED"}.get(verdict, verdict)
        results.append((fn, status, detail, find, repl, why))
        print(f"  m{i} {fn:<24} {status:<12} {detail}")

    survivors = [r for r in results if r[1] == "SURVIVED"]
    print(f"\n{len(proposals)} proposals: "
          f"{sum(1 for r in results if r[1] == 'killed')} killed, "
          f"{len(survivors)} survived, "
          f"{sum(1 for r in results if r[1] not in ('killed', 'SURVIVED'))} inconclusive")
    if survivors:
        print("\nSURVIVORS — the suite would not notice these.  Each is either a missing test\n"
              "or an equivalent mutant; decide which by reading it, then add a test and the\n"
              "tuple below to MUTANTS in scripts/mutants.py:\n")
        for fn, _, _, find, repl, why in survivors:
            print(f'    ("{fn}-?", "{args.file}",\n'
                  f"     {json.dumps(find)},\n"
                  f"     {json.dumps(repl)},\n"
                  f"     {json.dumps(why)}),")
    if missing:
        print(f"\nnot found in {args.file}, nothing measured for: {', '.join(missing)}")
    if args.keep:
        print(f"\nmutated trees kept in {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 3 if missing else 0


if __name__ == "__main__":
    sys.exit(main())

