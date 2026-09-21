# Contributing to OpenRAR

Thanks for your interest in OpenRAR — a clean-room C++17 implementation of the
RAR 5.0 archive format. This guide covers how to build, test, style, and land
changes. Where a rule is normative, it says so; everything else is convention
that keeps the history coherent.

## Ground rules (normative)

1. **Clean-room only.** No code, pseudocode, or structure copied from UnRAR,
   WinRAR, or any RAR source. `docs/spec/` is the sole format reference; the
   wire format is documented there from public knowledge. Never open a RAR
   reference implementation while contributing. If you have looked at UnRAR
   source in the past, say so in the PR and avoid touching format code.
2. **No new third-party dependencies.** All crypto and math is in-tree
   (`src/crypto/`, `src/recovery/`). If you need a dependency to solve a
   problem, stop and discuss in an issue first. The WASM build has a hard
   500 KiB size budget (`docs/wasm-limitations.md` §5.3) — dependencies put
   that at risk.
3. **No new test frameworks.** Unit tests are assert()-based executables wired
   through `openrar_add_test()` in CMakeLists.txt.
4. **License.** By contributing you agree your work is released under the
   project license (MIT, see `LICENSE`). Ported algorithms must be
   attributed in `THIRD_PARTY_NOTICES.md`.

## Repository layout

```
src/            core, io, format, compress, archive, recovery, crypto, api, cli, dll, wasm
include/openrar Public API (C++ wrapper openrar.hpp, C ABI openrar_dll.h)
tests/unit      assert()-based ctest suites (one executable per area)
tests/fuzz      fuzz targets + seeds corpus
tools/          gate scripts, node conformance suite, interop gate
docs/spec       RAR5 wire-format specs (NN-topic.md)
wasm/           JS wrapper + tests for the emscripten build
```

Layers are namespaces: `openrar::<subsystem>`. Dependencies point downward
only (`ARCHITECTURE.md` has the layer diagram).

## Toolchain & building

- CMake ≥ 3.20 (presets), a C++17 compiler. Optional: Node ≥ 18 (conformance
  suite), Python 3 (interop gate), PowerShell 7 (`gate.ps1`), Emscripten
  (WASM targets), WinRAR/UnRAR (test oracle).
- Presets (see `CMakePresets.json`): `release`, `debug`, `windows-msvc`,
  `linux-gcc`, `linux-clang`, `macos-clang`, `wasm`.

```sh
cmake --preset windows-msvc        # or linux-gcc / macos-clang / ...
cmake --build --preset windows-msvc
ctest --test-dir build/msvc -C Release
```

The POSIX `makefile` and plain MSBuild also work. WASM builds via
`emcmake cmake --preset wasm` (`wasm/README.md`).

## Code style

`ARCHITECTURE.md` §3–5 is the normative style reference. Summary:

| Symbol                    | Convention                | Example                       |
|---------------------------|---------------------------|-------------------------------|
| Files                     | lower snake_case          | `archive_reader.cpp`          |
| Namespaces                | `openrar::<subsystem>`    | `openrar::compress`           |
| Types / classes / structs | PascalCase                | `ArchiveReader`               |
| Functions                 | lower snake_case          | `read_header()`               |
| Class members             | snake_case + `_`          | `file_stream_`                |
| Struct members (POD)      | snake_case, no `_`        | `fb.pack_size`                |
| Constants / macros        | UPPER_SNAKE_CASE          | `MAX_STREAM_OUTPUT`           |
| Macros                    | `OPENRAR_` prefix         | `OPENRAR_DLL_API`             |
| Enumerators               | PascalCase                | `HeaderResult::Ok`            |
| Header guards             | `OPENRAR_<SUBSYS>_<FILE>_HPP` | `OPENRAR_CRYPTO_AES256_HPP` |

- **No `k`-prefixed constants** (no Google-style `kFoo`); constants are
  UPPER_SNAKE at every scope — namespace, class (`static constexpr`
  members included), and function-local `constexpr`. `.clang-tidy`
  enforces this (`GlobalConstantCase`, `ClassConstantCase`,
  `ConstexprVariableCase`); plain function-local `const` stays
  lower_snake and is unpoliced.
- Fixed-width ints: use `core::uint8…uint64`, `core::byte` (`src/core/types.hpp`),
  not bare `uint32_t`, in project code. Two areas keep stdint types on
  purpose: the C ABI / WASM boundary (`src/dll/`, `src/wasm/`,
  `src/api/`, `src/archive/buffer_archive.*`), where the types are the
  published surface, and SIMD/intrinsic kernel internals
  (`src/compress/arch/`, the NEON blocks in `crypto/sha256.cpp` and
  `compress/filters50.cpp`), which mirror intrinsic signatures.
- Comments: plain `//`, English, no Doxygen. Cross-references to spec files
  and finding IDs are encouraged (`// Constant-time compare (report L3)`).
- `.clang-format` is the canonical formatter (4-space, K&R, 100-col limit).
  Run `clang-format -i` on your files; CI checks formatting on the gate leg (pinned to `clang-format-18`).
  *Note on comments after early returns:* In constructs like `if (cond) return;`, an immediately following
  comment without an intervening empty line is treated by `clang-format-18` as a continuation indented with
  8 spaces. Maintain this indentation or insert a blank line to prevent format gate check failures.
- **Boundary exemptions** (C ABI / WASM API code in `src/dll/`, `src/wasm/`,
  `src/archive/buffer_archive.*`): raw `malloc`/`new` is allowed where it
  crosses the C boundary, but must pair with `openrar_free` ("single heap"),
  and the C ABI's plain `enum RarError` with `RAR_ERR_*` values is part of the
  frozen ABI surface — don't rename it.

### Compiler warnings (normative)

The tree builds clean with **MSVC `/W4`** and **`-Wall -Wextra`** on
GCC/Clang/Emscripten. New code must not introduce warnings:

- Fix warnings in code; don't suppress them. A narrow `#pragma warning` /
  `#pragma GCC diagnostic` is acceptable only in intrinsic/boundary headers,
  with a comment explaining why (`src/crypto/aes256.hpp` C4324 is the model).
- Mark parameters/variables that are conditionally used
  (`#ifdef`-gated debug or platform paths) with `[[maybe_unused]]`.
- `OPENRAR_WARNINGS_AS_ERRORS=ON` is a CMake option; CI enables it on the
  ubuntu/gcc gate leg. Run it locally before landing broad changes.

## Testing

What to run depends on what you touched (run the full local gate before
landing anything):

| You changed…                          | Minimum verification                                        |
|---------------------------------------|-------------------------------------------------------------|
| `src/compress/`, `src/crypto/`        | ctest + fuzz sweep (`-DOPENRAR_FUZZ=ON`, `ctest -R fuzz`) + goldens |
| `src/format/`                         | ctest + goldens + interop gate                              |
| `src/archive/`, volumes, recovery     | ctest + interop gate + volume/recovery suites               |
| `src/wasm/`, `wasm/js`                | `node wasm/js/test.mjs` + WASM build + size budget          |
| `include/openrar/` (public API)       | ctest incl. `dll_*` suites + one of each usage example      |
| Docs only                             | build still passes; no gate needed                          |

Full suite, local:

- **`powershell -File tools/gate.ps1`** — Release build + ctest + interop
  gate in one shot (Windows). On POSIX: build + `ctest` +
  `python tools/interop_gate.py --quick` by hand.
- **`node tools/run-tests.cjs`** — the node conformance layer (oracle
  assertions skip when WinRAR/UnRAR isn't installed).
- Fuzz targets: configure with `-DOPENRAR_FUZZ=ON`, run `ctest -R fuzz`.
  The nightly workflow fuzzes with libFuzzer + sanitizers; a crash there
  becomes your bug.

Adding tests: one executable per area in `tests/unit/`, registered via
`add_openrar_test()`; assert()-based with `[PASS]`/`[SKIP]` markers; shared
helpers in `tests/unit/test_support.hpp`. Golden archives are regenerated
deterministically with `tests/make_archives.ps1` — regenerate and commit
them only when a format change is intended, and call it out in the PR.
Crypto changes must come with known-answer vectors where the algorithm has
published ones (see `crypto_tests.cpp` for the existing KAT set).

## Local gate & hooks

Install the shared pre-commit hook (it runs the clang-format gate on every
commit, plus the interop gate when compress/archive/format code changes):

```sh
git config core.hooksPath .githooks
```

The format gate mirrors the CI format leg (clang-format 18, full tree).
clang-format is resolved in this order: `clang-format-18`, `clang-format`
on PATH, then the `pip install clang-format` binary (any major version
other than 18 prints a warning, since CI pins 18). If the tool is missing
the gate skips with a warning — CI still enforces it.

Bypass with `git commit --no-verify` only for clearly-non-format commits
that tripped the gate accidentally — and say so in the commit body.

## Commit messages (normative)

Format: `type(scope): subject`

- **Types**: `feat`, `fix`, `docs`, `test`, `ci`, `build`, `perf`, `refactor`,
  `chore`, `release`.
- **Scope**: a subsystem (`wasm`, `archive`, `format`, `interop`, `core`, `crypto`, `cli`, …).
- **Subject**: imperative, lowercase start, no trailing period, ≤ 72 chars.
- **Body** (expected on `fix`/`feat`): root cause, chosen-fix rationale,
  what tests were added/updated. Wrap at ~75 cols.
- **Footer**: end fix/feat bodies with the verification footer you ran, e.g.

  ```
  Gate: build + ctest + interop passed.
  ```

- **AI attribution**: if AI tooling authored substantive code in the commit,
  add the trailer `Co-Authored-By: internal-model` to the trailer block.

Example:

```
fix(L13,L14,L16): compressor short-source failure, 32-bit intrinsics, const_cast

Root cause: ... (what was wrong and why)
Chosen fix: ... (why this shape over alternatives)
Tests: ... (what now covers it)

Gate: build + ctest + interop passed.
Co-Authored-By: internal-model
```

## Branches & history

- The canonical branch is **`master`**. CI runs on it; PRs target it.
- **Linear history only** — no merge commits. Work lands via rebase or
  cherry-pick. Throwaway working branches are named `wt-<topic>`.
- One logical change per commit; a fix and the tests covering it belong in
  the same commit.

## Pull requests

One logical change per PR. Title = a conventional-commit subject as above.

- **PR checklist**: local gate green (paste the `Gate:` line); the relevant
  test layers from the table above were run; finding IDs resolve to tracked
  docs; no scratch/build artifacts added; clean-room confirmation if you
  touched `src/format/`, `src/compress/`, or `src/crypto/`.
- **CI gates**: the 5-leg matrix (linux gcc/clang, macOS, Windows x64/arm64)
  must pass — build, ctest, interop gate (authoritative in CI, no weak
  fallback), CLI smoke test. The gate leg additionally fails on warnings and
  checks formatting. The WASM job enforces the 500 KiB budget. The writer
  conformance job is advisory (`continue-on-error`) while the known writer
  drift is unresolved.
- **Merging**: squash-merge to keep history linear; the squashed title
  follows the commit-message rules above.

## Versioning & releases (maintainers)

Documented in `docs/versioning.md`: SemVer where PATCH is a monotonic commit
counter since `v1.0.0`. `project(VERSION)` in `CMakeLists.txt` is the single
source of truth — never hand-bump it; releases happen via the documented
release procedure (`release: vX.Y.N` commit + tag + `CHANGELOG.md` entry).

## Documentation conventions

- Specs: `docs/spec/NN-topic.md` (zero-padded two-digit prefix).
- Architecture and design: `ARCHITECTURE.md` and `docs/`.
- Issues and feature tracking: standard GitHub Issues and Pull Requests.
