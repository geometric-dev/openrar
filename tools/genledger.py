import re
import pathlib
import subprocess

# Repository-anchored paths: this script is run from anywhere.
REPO = pathlib.Path(__file__).resolve().parent.parent

# M5 revision (2026-10-19): the doc below regenerates docs/v1.44-verification-
# ledger.md from `tools/anchor_lint.py --ledger-print` (which now counts the
# verified-assert UNION: legacy `assert(` + doctest REQUIRE/CHECK family,
# with locally-defined CHECK macro *definition* lines masked out).

M4 = {  # recomputed ground truth at M4 close (assert-only, code-only)
    "runner_tests.cpp": 46, "test_support.hpp": 1, "cli_tests.cpp": 411,
    "archive_tests.cpp": 216, "buffer_archive_tests.cpp": 166,
    "cdc_chunker_tests.cpp": 15, "cdc_planner_tests.cpp": 19,
    "collision_matrix_tests.cpp": 38, "compress_tests.cpp": 375,
    "crypto_tests.cpp": 74, "dll_cpp_tests.cpp": 42, "dll_tests.cpp": 176,
    "extraction_atomic_tests.cpp": 124, "extraction_containment_tests.cpp": 64,
    "extraction_fidelity_tests.cpp": 119, "extraction_report_tests.cpp": 41,
    "file_handle_tests.cpp": 118, "foreign_format_tests.cpp": 107,
    "format_tests.cpp": 74, "golden_fixtures_tests.cpp": 70,
    "inflate_tests.cpp": 21, "io_tests.cpp": 119,
    "library_mode_tests.cpp": 15, "mapped_file_tests.cpp": 36,
    "mapped_scan_tests.cpp": 17, "metadata_tests.cpp": 105,
    "mutation_tests.cpp": 295, "parallel_compress_tests.cpp": 46,
    "reader_handle_seam_tests.cpp": 3, "recovery_tests.cpp": 234,
    "sandbox_sandboxed_e2e_tests.cpp": 1, "sandbox_worker_e2e_tests.cpp": 3,
    "sfx_e2e_tests.cpp": 23, "sfx_pipeline_tests.cpp": 22,
    "sfx_tests.cpp": 91, "solid_packer_tests.cpp": 40,
    "transcode_tests.cpp": 63, "two_phase_tests.cpp": 90,
    "unknown_extras_tests.cpp": 70, "volume_tests.cpp": 126,
    "attack_corpus_tests.cpp": 18,
}
# Suites that own a local `#define CHECK(cond)` counter-macro; the M4
# assert-only convention skipped their invocations (documented as zero rows).
# adds[name] = self-CHECK invocations first-time counted by the M5 union.
ADDS = {
    "abi_layout_tests.cpp": 17, "archive_api_tests.cpp": 103,
    "wasm_api_tests.cpp": 71, "qo_gap_fallback_tests.cpp": 10,
    "sandbox_ipc_tests.cpp": 55, "stream_encoder_tests.cpp": 64,
    "attack_corpus_tests.cpp": 13, "library_mode_tests.cpp": 25,
    "reader_handle_seam_tests.cpp": 17,
    "sandbox_sandboxed_e2e_tests.cpp": 18,
    "sandbox_worker_e2e_tests.cpp": 27,
}

out = subprocess.run(["python", str(REPO / "tools/anchor_lint.py"), "--ledger-print"], cwd=str(REPO),
                     capture_output=True, text=True, encoding="utf-8",
                     errors="replace").stdout
rows = re.findall(r"^([\w.]+\.(?:cpp|hpp))\s+asserts=(\d+) static_assert=(\d+)$",
                  out, re.M)
total = re.search(r"TOTAL asserts=(\d+) static_assert=(\d+) over (\d+) files", out)
now = {n: int(a) for n, a, s in rows}

# M5 close = the M4 assert-only ground truth + the first-time-counted
# self-macro CHECK inventory. The Phase 2 audit layer (P2) is everything the
# audit's new regression tests added on top of that; the invariant that
# matters is that P2 is never negative (a negative delta = a verified check
# disappeared, which is a regression, not an inventory change).
M5_CLOSE = {n: M4.get(n, 0) + ADDS.get(n, 0) for n in set(M4) | set(ADDS)}

problems = []
p2 = {}
for n, v in now.items():
    delta = v - M5_CLOSE.get(n, 0)
    if delta < 0:
        problems.append("%s: M5-close %d -> now %d (a verified check was LOST)"
                        % (n, M5_CLOSE.get(n, 0), v))
    elif delta:
        p2[n] = delta
if problems:
    for p in problems:
        print("CONTINUITY FAIL: " + p)
    raise SystemExit(1)

table = "\n".join("| `%s` | %s | %s |" % (n, a, s) for n, a, s in rows)
check_rows = "\n".join(
    "| `%s` | %d -> %d |" % (n, M4.get(n, 0), M4.get(n, 0) + ADDS[n])
    for n in sorted(ADDS))
p2_rows = "\n".join(
    "| `%s` | %d -> %d |" % (n, M5_CLOSE.get(n, 0), M5_CLOSE.get(n, 0) + p2[n])
    for n in sorted(p2))
p2_total = sum(p2.values())


runner = M4["runner_tests.cpp"]
support = M4["test_support.hpp"]
m4_total = sum(M4.values())
prep_total = m4_total - runner - support
union_total = int(total.group(1))
adds_total = sum(ADDS.values())

doc = """# v1.44 Verification Ledger (M5 revision, 2026-10-19)

Measured verified-assert inventory per suite, generated with
`tools/anchor_lint.py --ledger-print` at M5 close and re-verified by the same
tool's parity check (recomputation must reproduce this table exactly or the
lint fails). The pre-M5 revision of this doc (M4 close, 2026-10-18) measured
the same scope with a narrower counted set; every continuity claim below is
machine-checked by `genledger.py` against the M4 recomputation.

## Counted set (the M5 union convention)

The verified-assert set = legacy `assert(` + the doctest assertion macros
(`REQUIRE(`, `CHECK(`, `REQUIRE_FALSE`, `CHECK_FALSE`, and the
throws/nothrow forms). Code-only: each line's `//` suffix and string-literal
region is stripped before matching; `static_assert(` is excluded and counted
separately (2 instances: `cdc_chunker_tests.cpp`, `library_mode_tests.cpp`);
locally-defined macro *definition* lines (`#define CHECK(cond) ...`) are
masked so a definition is never counted as a check. Scope stays
`tests/unit/*.cpp` + `tests/unit/*.hpp` (`tests/fuzz/` and `tests/bench/`
remain out of v1.44 scope per the plan). Files with zero counts do not
appear in the table; `bitreader_stream_tests.cpp` is the single such file.

## Continuity (M5 migration preserved the M4 inventory exactly)

At M4 the counted set was `assert(` only — 3734 over 41 files (3733 in the
40 suites + 1 in `test_support.hpp`). M5 swapped suite asserts 1:1 into
`REQUIRE(`: for all 41 M4 counted files, the M5 family count is

    M4 asserts + self-CHECK inventories == M5 union (see table below,

machine-verified), i.e. every pre-M5 verified check is still there. One
`assert(` intentionally remains in the legacy form:
`test_support.hpp:84` (harness-internal temp-file helper; the runner's own
error contract predates doctest). The driver census quoted 412 raw text
matches for `cli_tests.cpp` and 48 for `runner_tests.cpp`; the code-only
recompute was 411 and 46 at M4 already — the raw deltas are comment
mentions (`// assert(...) hid the cause`) the counting convention never
counted; M5's mechanical rewrite updated that comment text in place
(count-neutral).

## Self-macro CHECK inventory (first-time counted at M5)

Eleven suites define a local counter-style `#define CHECK(cond)` (fprintf +
`fails` counter, non-fatal per check). The M4 assert-only convention
documented these as zero rows; the M5 union convention counts the
invocations (the M4 zero was a convention note, not an absence of checks):

| File | M4 asserts / self-CHECK adds |
|---|---|---|
{check_rows}

TOTAL self-CHECK adds: {adds} over 11 suites.

## Phase 2 audit layer (regression checks added after the M5 close)

The Phase 2 code audit (`docs/review-master-plan.md`) landed new regression
tests for the defects it remediated. Each new test brings its own verified
checks, so the union count grew. The invariant that matters is one-sided:
**no file may lose checks** (a negative delta would mean a verified check
disappeared - a regression, not an inventory change). `tools/genledger.py`
enforces that and fails the generation otherwise.

| File | M5 close -> now |
|---|---|---|
{p2_rows}

TOTAL audit-added checks: {p2_total}. M5 close {m5_total} + {p2_total} = the
current union total {total}.

## Reconciling the 2026-10-16 baseline (as at M4, unchanged)

Baseline census 46 files / 3701 `assert(` / 2 `static_assert` - 15 comment
matches = 3686 code asserts; + 2 uncommitted-tree deltas (A-6) = 3688 over
the 46 pre-runner files. The measured pre-runner rows at M4 summed to
{prep} (the A-6 census prose assumed 3688); + `runner_tests.cpp` {runner}
+ `test_support.hpp` {support} = the M4 total {m4}. M5 added the
self-CHECK inventory: {m4} + {adds} = the M5 close {m5_total}; the Phase 2
audit added {p2_total} more.

## Measured table (recompute check target)

| File | asserts | static_assert |
|---|---|---|
{table}

TOTAL asserts={total} static_assert={static} over {count} files
""".format(check_rows=check_rows, adds=adds_total, p2_rows=p2_rows,
           p2_total=p2_total, m5_total=m4_total + adds_total,
           prep=prep_total, runner=runner, support=support, m4=m4_total,
           total=union_total, static=total.group(2), count=total.group(3),
           table=table)
(REPO / "docs/v1.44-verification-ledger.md").write_text(doc, encoding="utf-8")
print("written; rows:", len(rows), "m4:", m4_total, "adds:", adds_total,
      "audit:", p2_total, "union:", union_total)




