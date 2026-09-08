# Host tests

Plain-gcc unit tests for the firmware's pure-C parts. No ESP-IDF, no
FreeRTOS, no hardware.

```sh
sudo apt-get install -y libcjson-dev   # or: export CJSON_DIR=/path/to/cJSON
scripts/run_host_tests.sh              # build + run everything
scripts/run_host_tests.sh --only edf_gen_test
python3 scripts/mutants.py             # does the suite notice planted bugs?
```

Both are plain commands with no repo-specific setup, so they drop into CI as
two steps (`apt-get install -y libcjson-dev`, then the two lines above) if
that is wanted; nothing here depends on running there.

## Layout

| file | what |
|---|---|
| `scripts/run_host_tests.sh` | builds each `scripts/*_test.c` and runs it; fails if a `*_test.c` exists that it does not know about |
| `scripts/test_include/` | one-file stand-ins for `esp_log.h`, `esp_err.h`, `esp_timer.h`, `esp_heap_caps.h`, `cJSON.h` |
| `scripts/edf_gen_test.c` | `#include`s the five `edf_*.c` modules — tests the real converter end to end (SNT files in, EDF bytes out) |
| `scripts/as11_time_test.c` | AS11 epoch / noon-day arithmetic |
| `scripts/as11_events_test.c` | event parser (tests a copy of the parser, not the real one) |
| `scripts/vld3_decoder_test.c` | oximeter VLD3 decoder |
| `scripts/mutants.py` | plants one-line bugs into a copy of `main/`, expects the suite to fail |
| `scripts/mutants_probe.py` | optional: asks a local model for more bugs, measures them the same way |

## Rules for a test that is worth having

1. **Test the real code.** `#include "the_file.c"` so static helpers are
   reachable. A test of a copied function passes forever, bug included.
2. **No constants copied from the implementation.** Assert what the
   *format* or the *other side* requires: the AS11 expects `-1` for a
   missing sample, cmH2O × 50 must be exact, noon starts the DATALOG day.
   If you find yourself pasting a number you just read in `main/`, stop.
3. **Prefer differential oracles.** Two paths that must agree — SNT v1 vs
   v2 producing the same EDF, `edf_gen` noon-day vs `as11_time` noon-day
   for the same instant — need no expected values at all.
4. **Pick the input where the bug is biggest.** Reaching a mutated line is
   not enough; an off-by-one at a boundary is only visible *at* the
   boundary (11:59:59 vs 12:00:00, header count = bytes on disk ± 1).
5. **A known bug is a test marked XFAIL**, with the vector that shows it.
   It flips to XPASS the day the fix lands, and the suite says so.
6. **Never read `main/*.c` as text from a test.** It would see every
   mutant and break the baseline; test behaviour, not source.

## How `mutants.py` decides whether its numbers mean anything

Two controls run before any mutant, and a failed control makes the run
**VOID** — no kill count is printed, exit status 2:

- **baseline** — the unmutated tree must pass with nothing skipped. A red
  or half-skipped suite "kills" everything and measures nothing.
- **canary** — one mutant no working suite can miss (every SNT file
  rejected). A runner that always exits non-zero would otherwise report a
  perfect score.

A kill also needs positive evidence: the runner's final `host tests:` line
(missing ⇒ INCONCLUSIVE, the runner did not finish) and the name of the
test that failed (`killed by edf_gen_test: torn tail ...`). A compile error
is a malformed mutant, not a kill.

## Adding a test to `edf_gen_test.c`

```c
static void test_something(void)
{
    char sd[PATH_MAX], root[PATH_MAX], out[PATH_MAX];
    mk(root, ...);                      /* mkdtemp'd tree, cleaned unless KEEP_TEST_TREE=1 */
    build_session(root, 2, SA2_N, sd, sizeof sd);   /* synthetic SNT v2 session */
    esp_err_t err = generate(root, sd, out);
    CHECK(err == ESP_OK, "generate: %d", err);
    /* read back with edf_parse() / edf_sample() and CHECK the contract */
}
/* in main(): */
run("something", test_something, /*expect_fail=*/0);
```

`CHECK(cond, fmt, ...)` records a failure and keeps going; `run()` counts
PASS / FAIL / XFAIL / XPASS. Exit status is non-zero only on FAIL.

## Adding a mutant to `mutants.py`

Append one tuple to `MUTANTS`:

```python
("short-id", "edf_gen.c",
 "exact text as it appears in main/edf_gen.c",
 "the same text with the bug in it",
 "one line: why a real person could write this bug"),
```

The `find` string must occur **exactly once** or the mutant reports
`NOT APPLIED` (so a refactor cannot silently turn the check into a no-op).
A survivor you have read and judged unable to change behaviour goes in the
`EQUIVALENT` list instead, with the reason. Those are still run: if one is
ever killed, it is reported as `REFUTED` and the reason is what needs
fixing. Silence is never equivalence.
A mutant earns its place by naming a bug this code has had, nearly had, or
would have with a one-line slip — not by being a random operator flip.
Run `python3 scripts/mutants.py --only short-id`. If it says `SURVIVED`,
the suite has a hole: write the test that kills it, then commit both.

## Finding holes you did not think of: `mutants_probe.py`

With a local [Ollama](https://ollama.com) and a code model:

```sh
python3 scripts/mutants_probe.py --function snt_available_samples --function noon_day_folder
```

The model proposes one-line bugs for the named functions; each proposal is
applied to a copy of `main/` and the suite is run, exactly as `mutants.py`
does. The model never judges anything — a proposal is measured, not
believed. Survivors are printed as ready-to-paste `MUTANTS` tuples; read
each one and decide whether it is a missing test or an equivalent mutant.
Not part of CI.

## Not covered (yet)

`session_writer.c` (the SNT *writer*) needs FreeRTOS queues and is not
built on the host. A writer→reader round trip is the natural next test.
`bsp_power.c`'s OCV curve and IR-drop compensation are pure functions but
sit in a FreeRTOS/ADC/NVS file; they need a stub layer first.

