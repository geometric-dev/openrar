# Versioning policy

OpenRAR releases follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html):
`MAJOR.MINOR.PATCH`. MAJOR and MINOR carry the compatibility promise; PATCH is
a monotonic commit counter (see below), which SemVer permits — it constrains
ordering and compatibility, not what a patch number counts.

## Single source of truth

The release version lives in `CMakeLists.txt`:

```cmake
project(openrar VERSION 1.0.126 LANGUAGES CXX)
```

Everything else derives from it or is synced to it in the same release commit
(`wasm/js/package.json`, `CHANGELOG.md`).

## Patch = commit counter

PATCH is not a count of bug fixes. It is the number of commits on the default
branch since the start of the current MAJOR.MINOR line, and every commit
advances it by one:

- Anchor for the 1.0 line: the `v1.0.126` tag. The counter carried over from
  private development — the initial public commit was tagged `v1.0.121`
  (121 commits past the original `v1.0.0` baseline of 2026-08-31, which is
  not part of the published history) — and every commit on the public
  repository advances it from there.
- Current counter: `git rev-list --count v1.0.126..HEAD`
- Intermediate commits do **not** touch `project(VERSION)` — the stamp is
  updated only in a release commit, which freezes the counter at that moment.
  The release commit itself documents the state up to that point and is not
  part of its own count.
- On a MAJOR or MINOR bump the counter resets to 0 and the anchor moves to the
  new `vX.Y.0` release tag.

This gives every shipped build a unique, strictly increasing version
(`1.0.126 > 1.0.9` in SemVer ordering) while MAJOR/MINOR keep answering the
compatibility question.

## What bumps what

| Bump  | Triggers |
|-------|----------|
| MAJOR | Breaking changes: C ABI breaks (struct layout change, export removal or signature change), source-breaking C++ API changes in `include/openrar/`, removal or renaming of CLI switches, dropping a supported platform, or any output regression that breaks WinRAR interop. |
| MINOR | Additive, backward-compatible changes: new exports, new CLI switches, new WASM/JS surface, new archive features, notable performance work. |
| PATCH | Automatic: every commit advances the counter; a release ships at whatever the counter reads. |

The DLL ABI version (`OPENRAR_DLL_API_VERSION` in `src/dll/openrar_dll.h`,
currently `1`) is not the package version: it changes only on ABI breaks and
matches the package MAJOR during the 1.x line. The shared-library target
already derives `VERSION ${PROJECT_VERSION}` / `SOVERSION
${PROJECT_VERSION_MAJOR}` from the project version, so a MAJOR bump moves the
soname/dll file version automatically. Embedders keep probing
`openrar_version() == OPENRAR_DLL_API_VERSION` at startup per
`docs/dll-integration-spec.md`.

## Release procedure

1. Read the counter: `git rev-list --count v1.0.126..HEAD` (adjust the anchor
   tag for newer MAJOR.MINOR lines).
2. Draft the new `CHANGELOG.md` section for `vX.Y.<counter>` from the commit
   range since the last release tag (highlights only).
3. Stamp `project(openrar VERSION X.Y.<counter>)`; sync
   `wasm/js/package.json`.
4. Commit as `release: vX.Y.<counter>`, tag `vX.Y.<counter>`, and push both.
   Pushing the tag runs the full CI matrix (the `v*` tag trigger) and its
   `release-assets` job packages every artifact of that run — per-platform
   shared libraries, import libs, and the WASM modules, named
   `openrar<version>-<target>.zip` — and attaches them to the GitHub release
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
