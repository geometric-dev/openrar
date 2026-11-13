# Verification legs

This page is the **single inventory of every automated check** the project
runs — CI jobs and steps, the nightly fuzz workflow, the local gate scripts,
and the pre-commit hook. It exists because a check that gets added to a
script nobody calls runs nowhere: two lists that drift apart are worse than
one incomplete list, because the second one *looks* authoritative.

**Normative rule:** adding, removing, renaming, or retargeting a leg
requires updating this inventory **in the same change**. Anything not listed
here is advisory by definition — if it should be a gate, list it; if it is
advisory, say so in its row.

---

## CI — `.github/workflows/build.yml` (push/PR to `master`, tags `v*`)

| Leg | What it runs | Notes |
|---|---|---|
| **build-and-test** (5-leg matrix: ubuntu gcc, ubuntu clang, macOS clang, Windows x64 MSVC, Windows ARM64 cross) | CMake configure + build, DLL artifact existence check, interop gate, CLI smoke test, artifact upload | CTest runs on every leg except the ARM64 cross-compile (tests execute under QEMU in the dedicated job below). Artifact upload fails on empty (`if-no-files-found: error`) — a silently-empty artifact would publish an empty release zip. |
| ─ gate-leg steps (ubuntu gcc only) | `OPENRAR_WARNINGS_AS_ERRORS=ON` | The only leg that fails on compiler warnings. |
| | `clang-format-18 --dry-run --Werror` over `src include tests` | Pinned to 18; version drift is a real failure mode. |
| | `bash tools/clang_tidy_check.sh build` over every first-party TU the platform built, using the repository `.clang-tidy` | Fails on any diagnostic with a check name. `bugprone-narrowing-conversions`, `bugprone-implicit-widening-of-multiplication-result` and `clang-analyzer-core.CallAndMessage` are enforced (re-enabled after the 2026-11 code-quality review cleared all 144 sites); size literals use `core::KIB`/`MIB`/`GIB` so the product is `size_t`, not `int`. |
| | `python3 tools/layer_check.py` | Downward-only includes (ARCHITECTURE.md §2). Ratchet: new violations fail; accepted ones live in `tools/layer_baseline.json`, updated manually only (`--update-baseline`). |
| | version consistency | `project(VERSION)` in CMakeLists.txt must equal `wasm/js/package.json`, `bindings/python/pyproject.toml`, `bindings/python/openrar/__init__.py` and `bindings/csharp/OpenRAR/OpenRAR.NET.csproj` (five copies; `docs/versioning.md`). |
| ─ interop gate | `python tools/interop_gate.py --quick` | Authoritative in CI: no `|| weaker-check` fallback. No `rar.exe` in CI, so oracle asserts degrade to self-roundtrip + probes by design. |
| ─ CLI smoke | create → test → extract → diff roundtrip | Exercises the real binary per platform. |
| ─ JUnit aggregation | `python tools/ctest_to_junit.py --log build/Testing/Temporary/LastTest.log --out build/junit-report.xml --min-tests N` (per job) | Fail-closed: parse failure, missing tests, or a `--min-tests` shortfall all exit non-zero. The CMake floor (3.16) has no `ctest --output-junit`, so the aggregator produces the JUnit report that gets uploaded as an artifact. |
| **roadmap-lint** (v1.44 M3) | `python tools/anchor_lint.py --release v1.44` | Three fail-closed checks: every `file:line` anchor in `roadmap-v143.merged.md` resolves; the verification ledger recomputes exactly (`docs/v1.44-verification-ledger.md`); `docs/FMM.md` release ranges are disjoint. Advisory for artifact publication (not in `release-assets.needs`) but required green on the release branch. |
| ─ writer conformance (ubuntu gcc) | `node tools/run-tests.cjs` (full run) | Integrated into the ubuntu-gcc Release leg to reuse the fresh binary without duplicate compilation. Suites self-gate: oracle asserts skip without WinRAR/UnRAR, privilege/OS-dependent suites self-skip. |
| **linux-arm64-qemu** | aarch64 cross build `-Werror`, bounded CTest under QEMU (`--timeout 60`), NEON RS16 activation assert, CLI smoke + benchmark under QEMU | Runs single-threaded in-process suites under QEMU emulation (excluding multi-threaded threadpools and child-process spawns which cause QEMU TCG lock contention/hangs; native ARM64 multi-threading is covered on Apple Silicon & Windows ARM64 runners). The NEON assert fails loudly if the scalar fallback ran (R1 silent-degradation guard). |
| **simd-validation** | native kernel gates, then Intel SDE (pinned) GFNI RS16 + AVX-512 activation asserts, e2e `.rev` generate/destroy/repair under SDE | Activation greps make the gate loud; SDE bench ratios are non-normative (emulated timings). |
| **wasm** | emsdk 3.1.50 (pinned), both wasm presets, `check-exports.mjs` contract, `node --test` JS suites, size budgets | Budgets: block codec ≤ 500 KiB, archive module ≤ 1.5 MiB (`docs/wasm-limitations.md` §5.3). Then the SIMD128 variant rebuilds and re-checks. |
| **sanitizers** (v1.30 M5) | ASan+UBSan over the unit suite on ubuntu/clang (`halt_on_error`); configures through `cmake/sanitizers.cmake` (`-DOPENRAR_SANITIZE=address,undefined`); excludes transcode/archive (long suites) and the SFX spawn suites (the SFX containment sets RLIMIT_AS for the child, incompatible with the child ASan shadow — spawn-under-sanitizer limitation, not a finding) | PRIMARY leg; release-assets waits for it. |
| **tsan** (v1.30 M5) | ThreadSanitizer over the threaded suites (parallel pipeline, slot readers); configures through `cmake/sanitizers.cmake` (`-DOPENRAR_SANITIZE=thread`) | PRIMARY leg. |
| **windows-asan** (2026-11 review) | the same unit suite under MSVC `/fsanitize=address` (`-DOPENRAR_SANITIZE=address`, RelWithDebInfo because Debug's `/RTC1` is incompatible with ASan), same exclusions as the Linux leg; `detect_leaks=0` (LeakSanitizer does not exist on Windows) | PRIMARY-ish: covers the Windows-only paths (NT anchored containment opens, `win32_meta`, sandbox spawn, MOTW, SFX GUI stub) that no Linux sanitizer leg compiles. Not in `release-assets.needs` yet. |
| **msan** (v1.30 M5) | MemorySanitizer over compress tests, instrumented libc++; configures through `cmake/sanitizers.cmake` (`-DOPENRAR_SANITIZE=memory`, which adds `-fsanitize-memory-track-origins` and `-stdlib=libc++`) | BEST-EFFORT per §7.2 revised: `continue-on-error`, reported, never gates. |
| **release-assets** (tags `v*` only) | zips all uploaded artifacts onto the GitHub release, then SHA256SUMS + CycloneDX SBOM (syft) + Sigstore keyless build-provenance attestations (v1.30 M6) | `permissions: contents: write`, needs all jobs above (incl. sanitizers/tsan). |

## Nightly — `.github/workflows/fuzz.yml`

| Leg | What it runs | Notes |
|---|---|---|
| **fuzz** | libFuzzer over `tests/fuzz` targets with ASan/UBSan (`halt_on_error`, `detect_leaks`), 1800 s per harness default (dispatch input overrides); corpus cached across runs (`fuzz-corpus-` cache key) | A crash there becomes the author's bug: fix it and add a deterministic regression test. |
| **header-parser fuzz** (2026-11 review) | `tests/fuzz/fuzz_header.cpp` against `parse_main_header` / `parse_file_header` / `parse_crypt_header` / `parse_end_header` and `read_block_raw_mem`, fed body bytes directly | Closes the gap that made the extra-record parsers unfuzzable: every other harness reaches them through `read_block_raw()`, which verifies the block CRC32 first, so a random mutation had ~2^-32 odds of getting past the checksum. |
| **audit harness** (v1.44) | `tests/fuzz/fuzz_target.cpp` standalone/audit-driven lanes: hostile decode (bounded output), compress→decompress round-trip with truncation-prefix and bit-flip oracles, sequential-vs-two-phase differential, `BitReader` model check | Not a libFuzzer harness: a deterministic, bounded audit target. It found the `flush_cadence` defect that became the `test_small_window_long_match_roundtrip` regression test. |
| **crash dedup** (v1.30 M5) | `tools/dedup_crashes.py` buckets crash artifacts by signature (`tests/fuzz/crash-buckets.json`); known buckets drop, new buckets survive as artifacts | Runs `if: always()` after the fuzz steps. |

## Local (mirrors; not authoritative — CI is)

| Tool | Legs | Notes |
|---|---|---|
| `.githooks/pre-commit` (enable: `git config core.hooksPath .githooks`) | clang-format gate; interop gate when compress/archive/format code changed | Bypass with `--no-verify` only for clearly-non-format commits, and say so in the body. |
| `tools/gate.ps1` (Windows) | Release build → ctest → interop gate `--quick` | The one-shot behind the commit footer `Gate: build + ctest + interop passed.` |
| `tools/preflight.sh` | `[win]` MSVC build+ctest, `[fmt]` format gate, `[ver]` version consistency, `[lay]` layer check, `[wsl]` gcc `-Werror` build+ctest, `[sde]` SDE kernel gates, `[ci]` branch run history | Auto-skips what the machine lacks and prints the residual "commit-and-pray" scope; exit 0 only if every executed leg passed. |
| POSIX by hand | build + `ctest` + `python tools/interop_gate.py --quick` + `python tools/layer_check.py` | |
| `node tools/run-tests.cjs` | full conformance layer locally | Oracle assertions skip when WinRAR/UnRAR isn't installed. |
| `python_conformance` (ctest) | Python SDK normative conformance (bindings/python) against the built DLL (v1.30 M4) | Registered wherever Python3 is found; part of freeze prereq 3. |
| `attack_corpus_tests` (ctest) | CVE-class attack regression corpus (v1.30 M5) | traversal / spoofing / RR overflow / bomb caps / hostile vint. |
| `sandbox_sandboxed_e2e_tests` (ctest) | OBSERVED-denial matrix per sandbox model + extract parity through the sandboxed worker (v1.30 M3a/b) | Windows AppContainer / Linux seccomp; loud `[SKIP]` on other platforms and under sanitizer runtimes (child-process spawn + instrumented runtime produce false findings — the non-spawning suites carry the sanitizer coverage). |
| `abi_layout_tests` / `abi_export_parity` / `js_error_mirror_parity` (ctest) | the v1.30 ABI-freeze enforcement set | docs/abi-freeze.md §14. |
| WASM | `emcmake cmake --preset wasm` / `wasm-archive`, `node wasm/js/test.mjs …`, size budget check | See `wasm/README.md`. |

## Rules

1. **Same-change rule (normative).** A PR that adds, removes, renames, or
   retargets a leg updates this page in the same commit.
2. **CI is the authority.** A local check that CI doesn't run is advisory;
   the gap must be visible in the preflight residual, not discovered after
   merge. A CI-only check must be listed here so preflight can mirror it.
3. **No weak fallbacks.** A gate step may not weaken a stronger check into a
   pass (`|| true`, downgrade to warning). Skips must be loud: preflight
   prints skipped legs, suites print `[SKIP]`.
4. **Baseline ratchets only shrink by hand.** For the layer check,
   `tools/layer_baseline.json` is updated with `--update-baseline` and the
   diff is reviewed in the PR — automation never grows it.
