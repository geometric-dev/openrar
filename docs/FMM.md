# FMM — Failure-Mode Matrix Registry (standing, `docs/FMM.md`)

Canonical failure-mode matrix required by v1.44 M2 (roadmap §7.2). The roadmap
keeps the prose; this file is the **registry**: one block per release, disjoint
ranges, rows authored once and cited thereafter.

## Range ownership (disjoint, lint-enforced)

A release may only write (add/modify rows in) **its own** block. A write into
another release's block is an error — `tools/anchor_lint.py` (v1.44 M3)
enforces range-disjointness. Roadmap prose may CITE registry rows; it may not
restate them.

| Release | Range | Status |
|---|---|---|
| v1.43 | FMM-43 | sealed (recorded at v1.43 arc close; see roadmap §7.1) |
| v1.44 | FMM-44 | active — this release |
| v1.45 | FMM-45 | pre-seeded from roadmap §7.3 (grow-only inside the range) |
| v1.46 | FMM-46 | reserved (empty) |
| v1.47 | FMM-47 | reserved (empty) |
| v1.48 | FMM-48 | pre-seeded from roadmap §7.6 |
| v1.49 | FMM-49 | reserved (empty; consumes FMM-51-5's shared OOM row when authored) |
| v1.50 | FMM-50 | pre-seeded from roadmap §7.8 |
| v1.51 | FMM-51 | pre-seeded from roadmap §7.9 |
| v1.52 | FMM-52 | reserved (empty) |

Rows are release-labeled and auditable (FMM-02 standing remark: every row
carries its release; the registry as a whole is the audit surface). Rows added
later for a past release may not retro-join another release's range.

## FMM-43 (v1.43 — Ratio Attribution Census + H1 Reachability Probe)

Sealed at v1.43 ARC close. Rows:

| Row | Trigger | Mitigation/consequence |
|---|---|---|
| FMM-43-1 | Corpus-generation disk exhaustion | generator bounds disk usage; exhaustion aborts generation cleanly |
| FMM-43-2 | OOM on 10+ GB inputs | bounded read — no full materialization |
| FMM-43-3 | Analyzer overlong run | reported as *bounded*, never *complete* |
| FMM-43-4 | Cache-state OOM | no heap allocation in `process_available` |

## FMM-44 (v1.44 — Verification Substrate) — active

Prose block (roadmap §7.2):

| Row | Trigger | Mitigation/consequence |
|---|---|---|
| FMM-44-1 | Dropped sub-expressions in assert→doctest rewrites | RAT-B differential per file (M1 harness); unmapped delta = per-file rollback |
| FMM-44-2 | Expensive formerly-skipped checks exploding CI | timing budget: +15% CI-time ceiling when re-enabling skipped checks |
| FMM-44-3 | Intentional-failure suite contradicts the 100% gate | runner `expected_fail` marker (ctest `WILL_FAIL TRUE` equivalent, harness-visible) |
| FMM-44-4 | Parity linter scope/allowlist | `assert(` counted independent of `<cassert>` includes; `static_assert(` excluded |

Substrate machinery rows (plan §9, runner/JUnit/aggregator):

| Row | Path | Failure | Response |
|---|---|---|---|
| FMM-44-5 | Runner dispatch | case aborts mid-run | completed cases pass, aborting case fail, later = skip; rc≠0; stdout flushed pre-case |
| FMM-44-6 | Runner dispatch | `--test-case` matches nothing | exit non-zero (G-9) |
| FMM-44-7 | Runner dispatch | duplicate case name | registration-time hard error |
| FMM-44-8 | Runner dispatch | non-ASCII case name | registration-time hard error (grammar `[A-Za-z0-9_.-]+`) |
| FMM-44-9 | Runner dispatch | unknown flag | hard error, no dispatch |
| FMM-44-10 | Runner dispatch | argv-prehook suite leaks reserved flags | prehook handles before runner; runner hard-errors only on its own namespace |
| FMM-44-11 | Runner dispatch | expected-fail case aborts | recorded expected, harness-visible |
| FMM-44-12 | JUnit writer | abort mid-suite | streamed temp sibling; footer only on completion; real path never written; orphan temp remains |
| FMM-44-13 | JUnit writer | partial write / crash | real path absent (atomic rename only on valid completion) |
| FMM-44-14 | Aggregator | missing/garbled LastTest.log | hard error — never emits empty-but-green XML |
| FMM-44-15 | Aggregator | suite absent from log / `Not Run` | mapped to failed testcase; suite-count floor from CMake registration |
| FMM-44-16 | Aggregator | unknown future format fields | asserted-fields parse; unknown = error |
| FMM-44-17 | RAT-B harness | relative argv0 breaks self-re-exec suites | resolved-absolute exe paths (mirrors ctest context) — *added at M1 close* |

## FMM-45 (v1.45 — Build & Sanitizer Infrastructure)

Pre-seeded from roadmap §7.3 (grow-only inside the range):

| Row | Trigger | Mitigation/consequence |
|---|---|---|
| FMM-45-1 | A modularized leg silently losing a sanitizer/platform | canary leg must fail to build without the flag |
| FMM-45-2 | Refactor changing emitted verdicts | blocked by v1.44 M3 (`interop_gate` equivalence check) |
| FMM-45-3 | A future TU gains per-TU ISA flags and includes an inline-bearing header | CI lint: per-file ISA `COMPILE_OPTIONS` may only include allowlisted declaration-only headers |
| FMM-45-4 | PCLMUL kernel active but diverging from slicing-by-8 on some length/alignment class | CRC differential gate; divergence = ship-blocker |

## FMM-46 (v1.46 — Dictionary Auto-Sizing) — reserved (empty)

## FMM-47 (v1.47 — macOS Seatbelt + Foreign-Format Fuzzing) — reserved (empty)

## FMM-48 (v1.48 — `cv` Phase 2)

Pre-seeded from roadmap §7.6:

| Row | Trigger | Mitigation/consequence |
|---|---|---|
| FMM-48-1 | Disk full mid-set | originals survive (temp-name validate → rename batch protocol) |
| FMM-48-2 | Rename failure | temps unlinked, source + prior destinations untouched |
| FMM-48-3 | Symlink TOCTOU | read-link semantics — targets never traversed |
| FMM-48-4 | Case collision | detect-and-reject per link matrix |
| FMM-48-5 | Reserved DOS names / UNC paths | detect-and-reject per link matrix |
| FMM-48-6 | Dangling link target | per matrix; never traverse to resolve |

## FMM-49 (v1.49 — WASM Streaming Encode & Memory Accounting) — reserved (empty)

Consumes the shared OOM row authored in FMM-51-5 when authored.

## FMM-50 (v1.50 — Parallel CDC Fingerprint Pass)

Pre-seeded from roadmap §7.8:

| Row | Trigger | Mitigation/consequence |
|---|---|---|
| FMM-50-1 | Range join at a boundary | covered by boundary tests in the parallel-pass arc |
| FMM-50-2 | Restart-based halo of any size (cut-lattice drift on constant/periodic runs, S-15) | named test |
| FMM-50-3 | Candidate overlap < 63 bytes (hash drift) | named test |
| FMM-50-4 | Tail chunk < 64 bytes (hash is `hash_range`, not the 64-byte window) | named test |
| FMM-50-5 | Split offset that is a cut vs one that is not | named test |
| FMM-50-6 | Cap reached mid-run | named test |
| FMM-50-7 | Non-divisible corpus | named test |
| FMM-50-8 | Range shorter than the overlap | named test |
| FMM-50-9 | A per-chunk buffer reintroduced above the pinned 64-byte footprint | `cdc_chunker_tests.cpp` pin remains |
| FMM-50-10 | `OPENRAR_CDC_INDEX_CAP` set in CI | honored; no silent bypass |

## FMM-51 (v1.51 — Sandbox Scope Expansion)

Pre-seeded from roadmap §7.9:

| Row | Trigger | User-visible | Test |
|---|---|---|---|
| FMM-51-1 | PBKDF2 over the work bound | `RAR_ERR_PASSWORD_TIMEOUT` (new) → free exit | `test_sandbox_pbkdf2_workbound_rejected` |
| FMM-51-2 | Volume descriptor missing | `RAR_ERR_MISSING_VOLUME` (−13) → `EXIT_OPEN` | `test_sandbox_multivol_descriptor_failure` |
| FMM-51-3 | Worker death (SIGKILL/segfault) | `RAR_ERR_CONTAINMENT_BREACH` (new); **a crash is a decode failure, not a security event** | `test_sandbox_worker_crash_recovery` |
| FMM-51-4 | Corrupted encrypted header | `RAR_ERR_DECRYPT` (new) → `EXIT_BAD_ARCHIVE` | `test_sandbox_encrypted_header_corrupt` |
| FMM-51-5 | **Shared OOM row** (authored here, consumed by v1.49) | `RAR_ERR_RESOURCE_EXHAUSTION` (new) → `EXIT_MEMORY` | `test_sandbox_worker_oom_rejection` |
| FMM-51-6 | Forged success token | `RAR_ERR_CRC_MISMATCH` (−4) → `EXIT_CRC` | `test_sandbox_fail_open_forgery_denied_hmac` |
| FMM-51-7 | **Containment degraded** (`OPENRAR_IN_PROC` or platform model unavailable) | Exit unchanged; **report** carries `sandbox_active: false` | `test_sandbox_degraded_mode_is_loud` (V-2) |

## FMM-52 (v1.52 — Competitive Audit & v2.0 Charter) — reserved (empty)

## Standing remark (FMM-02 lineage)

Historical FMM documents before v1.43 (FMM-02 through FMM-42 in v1.26-v1.42
implementation plans) remain in their plan documents as historical records;
they are NOT copied into this registry (pre-registry rows are not release-
ranged). The registry owns v1.43+ rows only.
