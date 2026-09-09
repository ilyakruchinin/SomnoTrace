#!/usr/bin/env bash
# SomnoTrace - static analysis, the same checks CI runs
#
# ── Usage ───────────────────────────────────────────────────────────────────
#   ./scripts/lint.sh              # use the tools installed on this machine
#   ./scripts/lint.sh --docker     # use the pinned image, matching CI exactly
#
# THE POINT OF --docker. A linter's finding set depends on its version (see
# scripts/lint-versions.env for the measurements), so "it passes for me" is not
# a claim you can make from a different cppcheck than CI's. With --docker the
# only dependency is Docker, and the answer is the one CI will give.
#
# Exit status is the BLOCKING tier's. The style tier is printed as a count and
# never affects it — the same split the workflow uses, for the same reason: a
# gate that is permanently red is a gate everyone learns to ignore.
set -uo pipefail          # NOT -e: every check must run even after one fails,
                          # or fixing the first hides the second

cd "$(dirname "$0")/.." || exit 2
# shellcheck source=scripts/lint-versions.env
. scripts/lint-versions.env

if [ "${1:-}" = "--docker" ]; then
    exec docker run --rm -v "$PWD:/src" -w /src "$LINT_IMAGE" bash -c '
        apt-get update -qq >/dev/null 2>&1
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq cppcheck shellcheck >/dev/null 2>&1
        ./scripts/lint.sh'
fi

for t in cppcheck shellcheck; do
    command -v "$t" >/dev/null || { echo "$t not installed — try: ./scripts/lint.sh --docker"; exit 2; }
done

# Report drift rather than refusing to run: a newer analyser is usually fine, and the
# reason to say so is that an unexplained new finding is otherwise indistinguishable from
# a regression the contributor introduced.
have_cpp=$(cppcheck --version | awk '{print $2}')
have_sh=$(shellcheck --version | awk '/^version:/{print $2}')
[ "$have_cpp" = "$CPPCHECK_VERSION" ] || \
    echo "note: cppcheck $have_cpp, CI pins $CPPCHECK_VERSION — findings may differ (scripts/lint-versions.env)"
[ "$have_sh" = "$SHELLCHECK_VERSION" ] || \
    echo "note: shellcheck $have_sh, CI pins $SHELLCHECK_VERSION — findings may differ (scripts/lint-versions.env)"

# ── The compile database, and why this is not a directory scan ───────────────────────
# `cppcheck main components` analyses source as TEXT: no -D, no include paths, no idea
# which branch of an #if is live. On a project whose files are selected by Kconfig that
# is not a weaker check, it is a different one — it reads code the compiler never sees
# and misses code the compiler does.
#
# Measured, on a 92,000-line branch proposed for this repo whose new sources all sit
# behind `if(CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B)` in main/CMakeLists.txt:
#
#     directory scan     1 real finding
#     compile database  17, of which 16 were new to that branch
#
# Among the 16: an out-of-bounds read of a 7-element array guarded by `< 8`, on the OTA
# failure-reporting path; an uninitialised variable in session_writer.c; a null
# dereference if an allocation fails. None of them is reachable by a text scan.
#
# `idf.py reconfigure` produces the database WITHOUT compiling, so this costs seconds.
# The paths inside it are the CONTAINER's (/project/...), so they are rewritten to this
# checkout, and the list is filtered to our own sources — unfiltered it carries all
# ~1,100 ESP-IDF translation units and the run takes minutes to tell you nothing.
#
# ⚠️ ONE DATABASE IS ONE CONFIGURATION. It reflects the board that was configured. While
# the repo has a single board that is the whole picture; the day a second one lands this
# wants a matrix, exactly as the build job does, or the unbuilt board goes unlinted —
# which is the failure this comment exists to prevent recurring.
# Every board configuration, not just whichever one happens to be configured.
#
# ⚠️ THIS IS THE WHOLE POINT AND IT IS EASY TO GET WRONG. `idf.py reconfigure` uses the
# sdkconfig already in the tree, so on a clean checkout it silently picks the DEFAULT
# board and the database contains only that board's sources. A file compiled solely under
# `if(CONFIG_SOMNOTRACE_BOARD_X)` is then never analysed, and the gate reports a clean
# tier it did not earn.
#
# That is not hypothetical: measured on a 92,000-line branch whose new sources are all
# behind such a guard, a clean checkout gave 3 findings and the same tree with that board
# configured gave 15 -- including an out-of-bounds array read on the OTA failure path.
# The first run looked like a pass.
#
# So: enumerate the sdkconfig defaults the repo ships and lint each resulting
# configuration, unioning the findings. With one board this is exactly the old behaviour
# and costs one reconfigure. It stops being a no-op the moment a second board lands,
# which is when a single-configuration gate would otherwise start lying.
lint_configs() {
    local base="sdkconfig.defaults" extra
    [ -f "$base" ] || base=""
    printf '%s\n' "${base:-<none>}"
    for extra in sdkconfig.*.defaults; do
        [ -e "$extra" ] || continue
        [ "$extra" = "$base" ] && continue
        printf '%s;%s\n' "$base" "$extra"
    done
}

# One configuration -> one filtered, path-rewritten compile database. Echoes the unit
# count; writes .lint-compile-db.json.
compile_db_for() {
    local defaults="$1" bdir="$2"
    # ⚠️ SDKCONFIG_DEFAULTS ONLY APPLIES WHEN THERE IS NO sdkconfig YET. Once one exists
    # in the tree it wins, every later configuration silently resolves to the first, and
    # the run reports N configurations that were all the same one. Measured: four
    # "configurations" each reporting an identical 55 translation units, none of them the
    # board being asked for. Give each its own SDKCONFIG path so the defaults are actually
    # read, and so no run leaves a tree-level sdkconfig behind to poison the next.
    if [ "$defaults" = "<none>" ]; then
        ./scripts/idf.sh -B "$bdir" -D SDKCONFIG="$bdir/sdkconfig" reconfigure >/dev/null 2>&1
    else
        ./scripts/idf.sh -B "$bdir" -D SDKCONFIG="$bdir/sdkconfig" \
                         -D SDKCONFIG_DEFAULTS="$defaults" reconfigure >/dev/null 2>&1
    fi
    [ -f "$bdir/compile_commands.json" ] || return 1
    python3 - "$PWD" "$bdir" <<'PYEOF'
import json, sys
root, bdir = sys.argv[1], sys.argv[2]
CROOT = "/project"          # where scripts/idf.sh bind-mounts this checkout inside the image

def rehost(p):
    """Container path -> host path, ANCHORED AT THE PREFIX.

    str.replace(CROOT, root) rewrites EVERY occurrence, which corrupts any path that
    merely contains the word:
        /project/main/projects.c  ->  <root>/main<root>s.c
    and a checkout kept under ~/projects/ puts the substring back into the result, so the
    damage depends on where the developer keeps their code. Requiring the prefix AND its
    trailing separator means only the mount point moves."""
    if p == CROOT:
        return root
    if p.startswith(CROOT + "/"):
        return root + p[len(CROOT):]
    return p

def rehost_arg(a):
    """Same, for one compiler argument -- where the path usually follows a flag, as in
    -I/project/main or -DX=/project/y. Everything before the first '/' is kept, so an
    argument that merely mentions a similar path (-I/opt/project/include) is untouched."""
    i = a.find("/")
    if i < 0:
        return a
    return a[:i] + rehost(a[i:])

try:
    db = json.load(open(f"{bdir}/compile_commands.json"))
except Exception:
    sys.exit(1)
keep = []
for e in db:
    f = rehost(e["file"])
    if not (f.startswith(root + "/main/") or f.startswith(root + "/components/")):
        continue
    if "/third_party/" in f or "/managed_components/" in f:
        continue
    # Prefer "arguments" when the generator emits it: already tokenised, so each path is
    # rewritten on its own and an argument containing a space cannot be split by accident.
    args = e.get("arguments")
    if args:
        cmd = " ".join(rehost_arg(a) for a in args)
    else:
        cmd = " ".join(rehost_arg(a) for a in (e.get("command") or "").split())
    keep.append({"file": f,
                 "directory": rehost(e.get("directory", "")),
                 "command": cmd})
if not keep:
    sys.exit(1)
json.dump(keep, open(".lint-compile-db.json", "w"))
print(len(keep))
PYEOF
}

# The pointer-subtraction pair is suppressed for the WHOLE tree, not per site.
# ESP-IDF's EMBED_FILES gives each blob a `_binary_<name>_start[]` / `_binary_<name>_end[]`
# pair of linker symbols, and `end - start` is the documented way to get its length.
# cppcheck sees two unrelated extern arrays and calls it undefined behaviour. It is a
# false positive every time, and a NEW one appears for every blob anyone embeds — so a
# per-site inline suppression means the gate breaks on a change that is entirely correct,
# and the person who hits it has to know this to get past it.
#
# WHAT THIS GIVES UP, stated rather than buried: a genuine subtraction of pointers into
# two different objects would no longer be caught. Every instance in this tree today is
# the `_binary_*` pattern, so the check has found nothing else; if that stops being true
# the answer is a targeted assertion, not re-enabling a check with a 100% false-positive
# rate. `comparePointers` is the same finding under cppcheck 2.13's name for it.
# -i excludes files we ASK cppcheck to check; it does not stop it reporting inside a
# vendored header reached through an #include. With the compile database cppcheck follows
# every include the compiler does, so LVGL and the Espressif components arrive as findings
# we neither own nor can fix. They are suppressed BY PATH below, or the gate is red on
# third-party code.
#
# Keep this note out of the array literal. An apostrophe inside a comment there ends up
# opening a quote that the next quoted argument closes, and the suppressions silently do
# not apply -- which is exactly how this was written wrong the first time.
CPPCHECK_COMMON=(--std=c11 --language=c --inline-suppr
                 --suppress=missingInclude --suppress=missingIncludeSystem
                 --suppress=unmatchedSuppression
                 --suppress=subtractPointers --suppress=comparePointers
                 -i third_party -i build -i managed_components
                 --suppress=*:*/managed_components/*
                 --suppress=*:*/third_party/*
                 --suppress=*:*/esp-idf/*
                 --template='{severity}: {file}:{line}: {message} [{id}]')

rc=0
: > /tmp/lint-style.$$
n_cfg=0
any_db=0

while IFS= read -r cfg; do
    n_cfg=$((n_cfg + 1))
    bdir="build-lint-$n_cfg"
    if units=$(compile_db_for "$cfg" "$bdir"); then
        any_db=1
        CPP_TARGET=(--project="$PWD/.lint-compile-db.json")
        printf '\n▸ cppcheck — configuration %s (%s translation units)\n' \
               "$([ "$cfg" = "<none>" ] && echo "default" || echo "$cfg")" "$units"
    else
        CPP_TARGET=(main components)
        printf '\n▸ cppcheck — NO COMPILE DATABASE for %s, falling back to a directory scan\n' "$cfg"
        printf '  This is the weaker check: no -D, no include paths, no idea which branch of\n'
        printf '  an #if is live. Sources selected by Kconfig may go unanalysed entirely.\n'
        printf '  Needs Docker (or a prior build).\n'
    fi

    printf '  blocking tier:\n'
    cppcheck --enable=warning,performance,portability "${CPPCHECK_COMMON[@]}" \
             --error-exitcode=1 "${CPP_TARGET[@]}" || rc=1

    cppcheck --enable=style --suppress=unusedFunction "${CPPCHECK_COMMON[@]}" \
             "${CPP_TARGET[@]}" 2>&1 >/dev/null | grep '^style:' >> /tmp/lint-style.$$ || true
    # scripts/idf.sh runs the container as root against a bind mount, so build-lint-*
    # and everything under it is owned by root on the host. A plain rm then fails with
    # Permission denied on any system without inherited directory ACLs, and the tree is
    # left dirty. Ask the container to remove it -- it is the one user that can.
    if ! rm -rf "$bdir" 2>/dev/null; then
        ./scripts/idf.sh exec rm -rf "/project/$bdir" >/dev/null 2>&1 || true
    fi
    if [ -e "$bdir" ]; then
        printf "  ⚠️ could not remove %s — remove it by hand before the next run\n" "$bdir"
    fi
    rm -f .lint-compile-db.json
done < <(lint_configs)

printf '\n▸ cppcheck — style tier (advisory, all %d configuration(s))\n' "$n_cfg"
sort -u /tmp/lint-style.$$ > /tmp/lint-style-u.$$
printf '  %s style finding(s)\n' "$(wc -l < /tmp/lint-style-u.$$)"
awk -F'[][]' '{print $2}' /tmp/lint-style-u.$$ | sort | uniq -c | sort -rn | sed 's/^/    /'
rm -f /tmp/lint-style.$$ /tmp/lint-style-u.$$
[ "$any_db" = 1 ] || printf '\n  ⚠️ no configuration produced a compile database; every tier above is the weak scan\n'

printf '\n▸ shellcheck — our own scripts\n'
# OUR scripts only: third_party ships more that we do not maintain.
mapfile -t sh_files < <(git ls-files '*.sh' 2>/dev/null | grep -v '^third_party/')
# Fall back to a walk when git cannot answer — inside --docker the checkout is bind-mounted
# and `git ls-files` returns nothing, which fed shellcheck an EMPTY argument list. shellcheck
# then exits non-zero for having no input, and the run reported BLOCKING TIER FAILED with no
# finding to show for it: a red gate that names nothing is worse than no gate.
if [ "${#sh_files[@]}" -eq 0 ]; then
    mapfile -t sh_files < <(find . -name '*.sh' -not -path './third_party/*' \
                                   -not -path './build*/*' -not -path './.git/*' | sed 's|^\./||')
fi
printf '  %d script(s)\n' "${#sh_files[@]}"
if [ "${#sh_files[@]}" -eq 0 ]; then
    echo "::warning::no shell scripts found to check — the shell tier ran on nothing"
fi
# SC2034 ("appears unused") is EXCLUDED from the blocking tier and reported below with the
# other advisories instead. It is not a correctness finding, and it is structurally noisy on
# any script that destructures a record:
#
#     IFS=$'\t' read -r BOARD BUILD_DIR SDKCONFIG DEFAULTS BUILD_REQUEST CLEAN_REQUEST <<< "$P"
#
# You cannot read positional fields without naming the ones you do not use, so a script that
# parses records trips this once per unused field, every time, for correct code. Measured on
# a 92,000-line branch proposed for this repo: 8 blocking shellcheck findings, 7 of them this
# one shape and none of them a bug.
#
# Everything else at --severity=warning stays blocking, and deserves to. The eighth finding in
# that run was SC2164 — a `cd` with no `|| exit` — which is exactly the kind of thing this gate
# is for: on failure the script keeps running in the wrong directory and reports on a tree it
# never meant to read. That one is fixed in this branch rather than suppressed.
shellcheck --severity=warning -e SC2034 -f gcc "${sh_files[@]}" || rc=1

# Advisory shell tier, mirroring the cppcheck split above: counted, never blocking.
printf '\n▸ shellcheck — style tier (advisory)\n'
sh_style=$(shellcheck --severity=style -f gcc "${sh_files[@]}" 2>/dev/null | grep -cE 'warning|note|error' || true)
printf '  %s style/info finding(s)\n' "${sh_style:-0}"
shellcheck --severity=style -f gcc "${sh_files[@]}" 2>/dev/null \
  | grep -oE 'SC[0-9]+' | sort | uniq -c | sort -rn | head -10 | sed 's/^/       /' || true

rm -f .lint-compile-db.json

printf '\n%s\n' "$([ $rc -eq 0 ] && echo 'lint: blocking tier clean' || echo 'lint: BLOCKING TIER FAILED')"
exit $rc
