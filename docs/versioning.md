# Versioning policy

OpenRAR releases follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html):
`MAJOR.MINOR.PATCH`. MAJOR and MINOR carry the compatibility promise; PATCH is
a monotonic commit counter (see below), which SemVer permits — it constrains
ordering and compatibility, not what a patch number counts.

## Single source of truth

The release version lives in `CMakeLists.txt`:

```cmake
project(openrar VERSION 1.2.0 LANGUAGES CXX)
```

Everything else derives from it or is synced to it in the same release commit
(`wasm/js/package.json`, `CHANGELOG.md`).

## Patch = commit counter

PATCH is not a count of bug fixes. It is the number of commits on the default
branch since the start of the current MAJOR.MINOR line, and every commit
advances it by one:

- Anchor for the 1.2 line: the `v1.2.0` tag. On a MINOR bump the counter
  resets; the 1.1 line it replaced anchored at `v1.1.0`.
- Current counter: `git rev-list --count v1.2.0..HEAD`
- Intermediate commits do **not** touch `project(VERSION)` — the stamp is
  updated only in a release commit.
- **The release commit counts itself.** The stamped patch number equals
  `git rev-list --count <anchor>..HEAD` evaluated AT the release commit:
  read the counter on pre-release HEAD and add 1. Worked example
  (v1.21.25): pre-release HEAD had 24 commits after `v1.21.0`, the release
  commit is the 25th, and `git rev-list --count v1.21.0..v1.21.25` == 25.
  This is verifiable from the tag alone — no release history required:
  `git rev-list --count v1.21.0..<tag>` always equals the tag's patch
  number for every tag on the 1.21 line.
- Sanity check before tagging: after committing the release, confirm
  `git rev-list --count <anchor>..HEAD` equals the stamped patch. If it
  does not, fix the stamp before tagging (moving a tag that exists only
  locally is fine; never move a pushed one — delete and re-tag only if the
  tag was never consumed by CI).
- On a MAJOR or MINOR bump the counter resets to 0 and the anchor moves to the
  new `vX.Y.0` release tag.

This gives every shipped build a unique, strictly increasing version
(`1.0.126 > 1.0.9` in SemVer ordering) while MAJOR/MINOR keep answering the
compatibility question.

## Platform support policy

Modern 64-bit only; no legacy CPU or legacy format support.

- **Packaged targets**: x86-64 (Windows, Linux), ARM64 (Windows, Linux,
  macOS universal), and wasm32 (Emscripten — the one ILP32 target, as the
  browser product surface).
- **Not supported**: 32-bit native (i386, ARMv7/armhf) and anything older
  than the CPU baselines the kernels require (SSE2 x86, ARMv8 AArch64).
  The ILP32 code guards that exist (1 GiB dictionary ceilings,
  `static_assert`s on width constants) exist to keep the wasm32 build and
  any accidental 32-bit compile *correct*, not to promise support.

## What bumps what

| Bump  | Triggers |
|-------|----------|
| MAJOR | Breaking changes: C ABI breaks (struct layout change, export removal or signature change), source-breaking C++ API changes in `include/openrar/`, removal or renaming of CLI switches, dropping a supported platform, or any output regression that breaks WinRAR interop. |
| MINOR | Additive, backward-compatible changes: new exports, new CLI switches, new WASM/JS surface, new archive features, notable performance work. |
| PATCH | Automatic: every commit advances the counter; a release ships at whatever the counter reads. |

The DLL ABI version (`OPENRAR_DLL_API_VERSION` in `src/dll/openrar_dll.h`,
currently `1`) is not the package version: it changes only on ABI breaks and
matches the package MAJOR during the 1.x line. Additive exports must not bump
it — embedders probe with strict equality per `docs/dll-integration-spec.md`,
so a bump would strand every host built against the previous header. New
capabilities are negotiated at runtime instead: `openrar_abi_features()`
(feature bitmask) or `GetProcAddress`/`dlsym` on the new symbol. The shared-library target
already derives `VERSION ${PROJECT_VERSION}` / `SOVERSION
${PROJECT_VERSION_MAJOR}` from the project version, so a MAJOR bump moves the
soname/dll file version automatically. Embedders keep probing
`openrar_version() == OPENRAR_DLL_API_VERSION` at startup per
`docs/dll-integration-spec.md`.

## Release procedure

1. Read the counter and add 1 (the release commit counts itself — see
   "The release commit counts itself" above):
   `git rev-list --count v1.2.0..HEAD` + 1 (adjust the anchor
   tag for newer MAJOR.MINOR lines).
2. Draft the new `CHANGELOG.md` section for `vX.Y.<counter>` from the commit
   range since the last release tag (highlights only).
3. Stamp `project(openrar VERSION X.Y.<counter>)`; sync
   `wasm/js/package.json`.
4. Commit as `release: vX.Y.<counter>`, tag `vX.Y.<counter>`, and push both.
   Pushing the tag runs the full CI matrix (the `v*` tag trigger) and its
   `release-assets` job packages every artifact of that run — per-platform
   native binaries (shared library `.dll`/`.so`/`.dylib`, import lib where
   the platform has one, and the CLI) and the WASM modules — named
   `openrar<version>-<platform-target>.zip` with no library-type segment
   (the library form differs per platform; `wasm` is the one labeled
   package) — and attaches them to the GitHub release
   named after the tag, creating the release with generated notes if it does
   not exist yet. Create or edit the release with the drafted CHANGELOG
   section as its notes.

## Embedding the version

Implemented:

- `project(openrar VERSION ...)` as the single source of truth; the
  `openrar_dll` target derives file version and SOVERSION from it.

Planned:

- Generate `openrar/version.h` via `configure_file()` with
  `OPENRAR_VERSION_MAJOR` / `OPENRAR_VERSION_MINOR` / `OPENRAR_VERSION_PATCH`
  / `OPENRAR_VERSION_STRING` / `OPENRAR_VERSION_HEX`, included from
  `include/openrar/openrar.hpp`.
- Add `const char *openrar_package_version_string(void)` to the C ABI
  (additive export = MINOR bump). Keep `openrar_version()` as the ABI probe
  only; never repurpose it.
- Add `openrar --version` in `src/cli/main.cpp` (the banner currently carries
  no version).
- Optional CI guard: fail if `wasm/js/package.json`, the newest CHANGELOG
  section, or `git rev-list --count` against the line's anchor tag disagree
  with `project(VERSION)` on a release commit.
