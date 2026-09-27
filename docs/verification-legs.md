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
| | `python3 tools/layer_check.py` | Downward-only includes (ARCHITECTURE.md §2). Ratchet: new violations fail; accepted ones live in `tools/layer_baseline.json`, updated manually only (`--update-baseline`). |
| | version consistency | `project(VERSION)` in CMakeLists.txt must equal `wasm/js/package.json` (`docs/versioning.md`). |
| ─ interop gate | `python tools/interop_gate.py --quick` | Authoritative in CI: no `|| weaker-check` fallback. No `rar.exe` in CI, so oracle asserts degrade to self-roundtrip + probes by design. |
| ─ CLI smoke | create → test → extract → diff roundtrip | Exercises the real binary per platform. |
| **linux-arm64-qemu** | aarch64 cross build `-Werror`, CTest under QEMU, NEON RS16 activation assert, CLI smoke under QEMU | The NEON assert fails loudly if the scalar fallback ran (R1 silent-degradation guard). |
| **writer-conformance** | `node tools/run-tests.cjs` (full run) | Blocking. Suites self-gate: oracle asserts skip without WinRAR/UnRAR, privilege/OS-dependent suites self-skip. |
| **simd-validation** | native kernel gates, then Intel SDE (pinned) GFNI RS16 + AVX-512 activation asserts, e2e `.rev` generate/destroy/repair under SDE | Activation greps make the gate loud; SDE bench ratios are non-normative (emulated timings). |
| **wasm** | emsdk 3.1.50 (pinned), both wasm presets, `check-exports.mjs` contract, `node --test` JS suites, size budgets | Budgets: block codec ≤ 500 KiB, archive module ≤ 1.5 MiB (`docs/wasm-limitations.md` §5.3). Then the SIMD128 variant rebuilds and re-checks. |
| **sanitizers** (v1.30 M5) | ASan+UBSan over the full unit suite on ubuntu/clang (`halt_on_error`) | PRIMARY leg; release-assets waits for it. |
| **tsan** (v1.30 M5) | ThreadSanitizer over the threaded suites (parallel pipeline, slot readers) | PRIMARY leg. |
| **msan** (v1.30 M5) | MemorySanitizer over compress tests, instrumented libc++ | BEST-EFFORT per §7.2 revised: `continue-on-error`, reported, never gates. |
| **release-assets** (tags `v*` only) | zips all uploaded artifacts onto the GitHub release, then SHA256SUMS + CycloneDX SBOM (syft) + Sigstore keyless build-provenance attestations (v1.30 M6) | `permissions: contents: write`, needs all jobs above (incl. sanitizers/tsan). |

## Nightly — `.github/workflows/fuzz.yml`

| Leg | What it runs | Notes |
|---|---|---|
| **fuzz** | libFuzzer over `tests/fuzz` targets with ASan/UBSan (`halt_on_error`, `detect_leaks`), 1800 s per harness default (dispatch input overrides); corpus cached across runs (`fuzz-corpus-` cache key) | A crash there becomes the author's bug: fix it and add a deterministic regression test. |
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
| `sandbox_sandboxed_e2e_tests` (ctest) | OBSERVED-denial matrix per sandbox model + extract parity through the sandboxed worker (v1.30 M3a/b) | Windows AppContainer / Linux seccomp; loud `[SKIP]` elsewhere. |
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
