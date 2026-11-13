# 🏛️ Principal Architect Walkthrough & Implementation Sign-Off

**Work Reviewed:** 2026-11 code-quality remediation — header-parser fuzz harness,
MSVC ASan CI leg, whole-tree widening/narrowing sweep, re-enabled clang-tidy
checks — plus the fresh-eyes review and libFuzzer campaign run afterwards.
**Plan Reference:** the two user prompts; `docs/reviews/code-quality-review-202611.md`
§9 (corrected fix outlines) and `…-architect-challenge.md` (D-1…D-8).
**Architect Verdict:** 🟡 **CONDITIONAL APPROVAL (REMEDIATIONS REQUIRED)**

---

## 1. Executive Summary & Audit Posture

The remediation is implemented, builds on both toolchains, and the re-enabled
gates are green for everything the change touches. Two defects were found during
this walkthrough that the author's own report did **not** catch, both now fixed:

1. **The fuzz campaigns were running blind.** Neither `cmake/sanitizers.cmake`
   nor the per-target flags in `cmake/openrar_tests.cmake` instrumented the
   library with SanitizerCoverage — only the harness TU. A campaign reported
   `Loaded 1 modules (65 inline 8-bit counters)`, so libFuzzer had no feedback
   about the parsers it was supposed to explore. Fixed in `CMakeLists.txt`
   (global `-fsanitize=fuzzer-no-link` compile + link, clang/Linux/FUZZ only);
   the same probe now reports **2,807 counters** — a 43× increase in observable
   edges.
2. **The new nightly step wrote its corpus to a directory that is not cached.**
   `fuzz_header` was invoked as `extra_seeds/ seeds/`; the first corpus argument
   is where libFuzzer *writes* new units, and only `tests/fuzz/seeds` is
   persisted by `actions/cache`. The header corpus would have been discarded
   every night. Fixed to `seeds/ extra_seeds/`, matching every other harness.

Posture: this was an adversarial audit against the diff and real runner output,
not against the narrative. CI was **not** executed (no Actions access); every
statement below about a CI leg is a local reconstruction and is labelled as such.

---

## 2. 📋 Plan Reconciliation & Deviation Analysis

| Committed item | Delivered | Evidence | Deviation |
|---|---|---|---|
| Fuzz header bodies **directly into the parsers**, CRC gate bypassed | `tests/fuzz/fuzz_header.cpp` (169 lines): `exercise_body()` calls `parse_main_header` / `parse_file_header` / `parse_crypt_header` / `parse_end_header` on the raw body; `exercise_framing()` additionally drives `read_block_raw_mem` | builds on MSVC and clang; standalone sweep `Header-parser sweep PASSED`; libFuzzer campaign §4 | none |
| Register the harness | `cmake/openrar_tests.cmake` target + `OPENRAR_USE_LIBFUZZER`/`-fsanitize=fuzzer` block + `add_test(... -runs=2000 -max_len=262144)`; `fuzz.yml` build-assert loop + run step | `ctest -R '^fuzz_header$'` → `Passed 0.14 sec` | target block was first placed between `fuzz_target` and *its* `if(OPENRAR_FOREIGN_FORMATS)/if(OPENRAR_THREAD_LIB)` tail; moved below them so the conditionals read as belonging to `fuzz_target` (defect D-2) |
| **MSVC ASan leg** on the Windows CI job | new `windows-asan` job in `build.yml` | YAML parses; job enumerates the expected 7 steps; **never executed** | job runs RelWithDebInfo, not Debug — `CMake`'s Debug flags add `/RTC1`, which MSVC rejects with `/fsanitize=address`. Documented in the job comment and in `docs/verification-legs.md` |
| Apply the widening fixes (all flagged sites) | 110 implicit-widening + 34 narrowing sites across 39 files under `src/`/`include/`; `core::KIB/MIB/GIB` added to `src/core/types.hpp` | clang-tidy with the repo config over 79 TUs: **0 findings from the three re-enabled checks** | only *flagged* lines were rewritten, deliberately: rewriting every `N * 1024` in a file turns deliberate `int` arithmetic into `size_t` and trades one warning for another |
| Re-enable the disabled checks so the gate enforces them | removed `-bugprone-narrowing-conversions`, `-bugprone-implicit-widening-of-multiplication-result`, `-clang-analyzer-core.CallAndMessage` from `.clang-tidy` | full gate run: 0 parse errors, 14 findings, none from the three checks | naming/`exception-escape` residue (D-5) is pre-existing |
| *(this turn)* fresh-eyes simplification | 6 changes, §5 | rebuild clean, targeted `clang-tidy` clean | includes two items outside the sweep's literal remit (`g_unknown_pe_machine_count` rename, coverage fix) — justified below |
| *(this turn)* libFuzzer campaign | 2.96M + 3.12M executions, 0 sanitizer reports | §4 | campaign runs on **clang-18/Linux (WSL)**, not MSVC — MSVC has no libFuzzer driver, which is why the harness carries a standalone sweep |

Unapproved scope creep: none that changes behaviour. The coverage fix touches
`CMakeLists.txt` (outside the original file set) because a campaign with 65
counters cannot evidence anything about the parsers; it is inert unless
`OPENRAR_FUZZ=ON` with clang on a non-Windows host.

---

## 3. 🚨 Implementation Defects & Architectural Invariants

**D-1 — Fuzzing was blind to the library (critical, fixed).**
`cmake/sanitizers.cmake` adds only ASan/UBSan; the `-fsanitize=fuzzer-no-link`
compile option lives on the individual harness targets, which cannot reach
`openrar_core` (created before them). Measured: `65 inline 8-bit counters`
before, `2,807` after. Every existing harness in the nightly inherited this, so
this fix raises the value of the whole fuzz suite, not just the new target.

**D-2 — CMake target placement (cosmetic, fixed).** The `fuzz_header` block sat
between `fuzz_target` and the two conditional blocks that modify `fuzz_target`,
making it read as though `OPENRAR_FUZZ_HAVE_INFLATE` applied to the header
harness. Behaviour was correct (both name `fuzz_target` explicitly); readability
was not.

**D-3 — Redundant arithmetic left by the sweep (fixed).** `1ULL * core::GIB`
(×5) and `64ULL * core::MIB` were carry-overs from the pre-constants spelling:
the `ULL` existed to stop `int` overflow in `1024 * 1024 * 1024` and is dead
weight once the operand is `size_t`. `4ULL * core::GIB` is **kept** — 4 GiB does
not fit a 32-bit `size_t`. Also removed: `(core::MIB)` parens left by the
regex, `static_cast<unsigned int>(AVX512_REQ)` where `AVX512_REQ` is now
already `unsigned int`, and a `1 *` multiplier.

**D-4 — `allocation_tests` fails (open, pre-existing, NOT caused by this
change).** 2 of 7 cases (`allocation_zero_decompressor`,
`allocation_stream_steady_state`) fail with `alloc count: 2, bytes: 32`. Proven
pre-existing: the entire sweep was stashed, the tree rebuilt at HEAD, and the
same two cases failed with identical numbers; the sweep was then restored and
verified byte-identical against a saved patch. Cause is not investigated here —
the assertion ("steady state allocates zero") is untested in this
configuration, not the arithmetic.

**D-5 — 14 clang-tidy findings remain (open, pre-existing).** 13 ×
`WIN32_LEAN_AND_MEAN` macro naming plus 1 × `bugprone-exception-escape` in
`release_filters() noexcept`. Both checks were enabled **before** this change,
so they are not a consequence of re-enabling anything. The macro ones are
invisible to CI: the gate runs on ubuntu, where those `#if defined(_WIN32)`
blocks are never preprocessed.

**D-6 — `fuzz_header` has no standalone ctest entry (design note).** Registered
inside the `Clang AND NOT WIN32` block only, exactly like `fuzz_target` and
`fuzz_decompress`. Consequence: the deterministic sweep never runs in the MSVC
leg. Deliberate (matches the existing pattern, keeps the Windows suite fast);
flagged so it is a decision, not an accident.

---

## 4. 🔬 Evidence & Missing Verification

### Executed locally (real output)

| Check | Command | Result |
|---|---|---|
| MSVC build, all targets | `cmake --build build/gate` (Ninja/MSVC 19.5x, Debug) | exit 0, no warnings |
| clang build, all targets | `cmake --build build/gate-clang -j 6` (clang-18, RelWithDebInfo, ASan+UBSan+coverage) | **279/279**, 0 errors |
| Unit suite (MSVC/Debug) | `ctest --test-dir build/gate -j 4` | **64/65 pass**; only D-4 fails |
| clang-tidy gate | repo `.clang-tidy` over 79 first-party TUs, clang-tidy 22.1.3 | 0 parse errors, 14 findings, all D-5 |
| target of the campaign | `./build/gate-clang/fuzz_header -runs=1` | `Loaded 1 modules (2807 inline 8-bit counters)` |
| ctest registration | `ctest --test-dir build/gate-clang -R '^fuzz_header$'` | `Passed 0.14 sec` |

### Campaign (clang-18 + libFuzzer + ASan + UBSan, `UBSAN_OPTIONS=halt_on_error=1`)

```
./build/gate-clang/fuzz_header -max_total_time=120 -max_len=262144 \
    -rss_limit_mb=4096 -timeout=25 -print_final_stats=1 scratch/fuzz-corpus-header tests/fuzz/seeds/

Done 2958086 runs in 121 second(s)
stat::number_of_executed_units: 2958086
stat::average_exec_per_sec:     24446
stat::new_units_added:          3345
stat::peak_rss_mb:              486
final:  #2958086  DONE  cov: 684 ft: 2162 corp: 576/34Kb lim: 19482 rss: 486Mb
```

No `ERROR: AddressSanitizer`, no `SUMMARY: UndefinedBehaviorSanitizer`, no
`deadly signal`, no timeout. A second round seeded from the accumulated corpus
ran 3,124,968 executions in 62 s (50,402 exec/s) and added 525 units; a third,
merge-dominated round reached `cov: 687 ft: 2277` with
`stat::new_units_added: 0` — i.e. **the reachable header-parser surface
saturated at ~687 edges after ~6M executions**, which is what a well-formed
short campaign looks like.

### Not verified (must not be reported as passing)

- **The `windows-asan` CI job never ran.** No Actions execution was available.
  The step most likely to need adjustment is the `vswhere` + `PATH` resolution
  for `clang_rt.asan_dynamic-x86_64.dll`; until a run exists, the leg is
  *declared*, not *demonstrated*.
- **CI clang-tidy is 18, local is 22.** The 0-findings result is from newer
  checks over the same config; 18 is a subset in practice but not guaranteed to
  be identical.
- **Behavioural equivalence of the sweep is argued, not proven by differential
  testing.** Every rewrite was checked to be value-identical by inspection
  (`size_t` arithmetic of the same magnitudes; explicit casts of the same
  values), and the suite is green, but there is no oracle comparing old vs new
  numeric results.
- **Windows-only code paths are not exercised by this harness.** The header
  parsers are platform-neutral, so this is low risk, but it is the reason the
  ASan leg exists.
- The nightly's `tests/fuzz/extra_seeds/` directory does not exist in a local
  checkout (it is created by a workflow step). The local campaign used
  `tests/fuzz/seeds/` only.

---

## 5. ⚡ Performance, Systems & Code Hygiene

- **Throughput:** 24.4k exec/s under ASan+UBSan for a harness that runs four
  parsers plus the framing reader per input — the work per input is O(body) and
  bounded at 256 KiB by `MAX_BODY`, so a hostile input cannot stall the run
  (`slowest_unit_time_sec: 0`).
- **Memory:** 486 MB peak with a 4 GB limit — no unbounded allocation reached
  from the parsers, which is the property the harness is meant to test.
- **Hot path untouched:** the sweep's rewrites are constant-folded; no new
  allocation, branch or call was introduced. `core::KIB/MIB/GIB` are
  `inline constexpr size_t`, so they cost nothing at run time and cannot ODR-clash.
- **Hygiene:** no TODOs, debug prints or commented-out code added; the two
  non-obvious constructs (the descending EOCD scan and the `pos-- > 0`
  termination) carry the reasoning inline. Scratch artefacts live under
  `scratch/`, which is gitignored (`.gitignore:75`).
- **Naming:** the new constants satisfy `ConstexprVariableCase = UPPER_CASE`;
  the renamed global satisfies `GlobalVariablePrefix = g_`.

---

## 6. 🛠️ Actionable Remediation Directives

| # | Directive | Severity | Rationale |
|---|---|---|---|
| R-1 | Run the `windows-asan` job on a branch and paste the receipt; adjust the `vswhere`/`PATH` step if the DLL is not found | **High** | The only claim in this body of work with no evidence behind it |
| R-2 | Decide the `allocation_tests` contract: either the two cases assert a Release-only property (then gate them on `NDEBUG` with a comment) or they are a real regression in Debug builds (then file it) | **Medium** | Currently a permanent red leg on any local Debug run, which trains people to ignore the suite |
| R-3 | Add a fixed skip for `WIN32_LEAN_AND_MEAN` naming (e.g. `// NOLINT(readability-identifier-naming)` at each definition, since clang-tidy ignores `MacroDefinitionIgnoredRegexp`) if a Windows gate leg is ever added | Low | Keeps the option of running the gate on Windows open |
| R-4 | Add a short `fuzz_header`-specific corpus seed (a real header body) to `tests/fuzz/seeds/` | Low | The harness currently starts from archives it cannot parse as a body; a genuine body would let the campaign skip the warm-up it spent ~1 min on |
| R-5 | Consider registering `fuzz_header` in the standalone (non-clang) ctest sweep so the deterministic sweep runs on the Windows/MSVC legs too | Low | Cheap coverage of the CRC-bypass path on every platform; deliberately left out to match `fuzz_target` |

*"In God we trust; all others must bring automated test logs and code diffs.
A feature unevidenced is a feature unbuilt."*
