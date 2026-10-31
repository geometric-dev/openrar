# Versioning policy

OpenRAR releases follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html):
`MAJOR.MINOR.PATCH`. MAJOR and MINOR carry the compatibility promise; PATCH is
a monotonic commit counter (see below), which SemVer permits — it constrains
ordering and compatibility, not what a patch number counts.

> [!NOTE]
> **Monotonic Commit Counter Behavior for External Consumers:**  
> OpenRAR uses a monotonic commit counter for the `PATCH` component. Version
> increments between releases reflect the number of commits merged since the
> `MAJOR.MINOR.0` anchor tag, not unreleased or yanked versions. For example, a release
> jump from `v2.0.1` to `v2.0.15` indicates 14 commits landed on the branch since
> `v2.0.1`, rather than missing or withdrawn releases.

## Single source of truth

The release version lives in `CMakeLists.txt`:

```cmake
project(openrar VERSION 2.0.1 LANGUAGES C CXX)
```

Everything else derives from it or is synced to it in the same release commit.
There are **eight** synchronized copies maintained across the codebase, enforced
in lockstep by `tools/bump_version.py` and the CI version gate:

| File | Field | Purpose |
|---|---|---|
| `CMakeLists.txt` | `project(openrar VERSION X.Y.Z ...)` | Canonical source of truth |
| `wasm/js/package.json` | `"version"` | NPM WASM package distribution |
| `bindings/python/pyproject.toml` | `version` | Python PyPI package manifest |
| `bindings/python/openrar/__init__.py` | `__version__` | Python SDK runtime version |
| `bindings/csharp/OpenRAR/OpenRAR.NET.csproj` | `<Version>` | NuGet package manifest |
| `debian/changelog` | Newest entry header | Launchpad / Debian package version |
| `debian/openrar.1` | `.TH OPENRAR 1 ...` | Debian man page header |
| `docs/man/openrar.1` | `.TH OPENRAR 1 ...` | Repository man page header |

Plus `CHANGELOG.md`, whose newest `## [X.Y.Z]` heading must match. The gate
that enforces consistency is the `version consistency` step in
`.github/workflows/build.yml` (and `tools/bump_version.py --check`). `include/openrar/version.h` is
**not** a static copy to bump — it is a template filled from `project(VERSION)` by
`configure_file` (`cmake/openrar_sources.cmake`).

## Patch = commit counter

PATCH is not a count of bug fixes. It is the number of commits on the default
branch since the start of the current MAJOR.MINOR line, and every commit
advances it by one:

- Anchor for the 2.0 line: the `v2.0.0` tag. On a MINOR bump the counter
  resets; the 1.60 line it replaced anchored at `v1.60.0`.
- Current counter: `git rev-list --count v2.0.0..HEAD`
- Intermediate commits do **not** touch `project(VERSION)` — the stamp is
  updated only in a release commit.
- **The release commit counts itself.** The stamped patch number equals
  `git rev-list --count <anchor>..HEAD` evaluated AT the release commit:
  read the counter on pre-release HEAD and add 1. Worked example
  (v1.21.25): pre-release HEAD had 24 commits after `v1.21.0`, the release
  commit is the 25th, and `git rev-list --count v1.21.0..v1.21.25` == 25.
  This is verifiable from the tag alone — no release history required:
  `git rev-list --count vX.Y.0..<tag>` always equals the tag's patch
  number for every tag on the line.
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

The DLL ABI version (`OPENRAR_DLL_API_VERSION` in `include/openrar/openrar_dll.h`,
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
   `git rev-list --count vX.Y.0..HEAD` + 1 (adjust the anchor
   tag for the active MAJOR.MINOR line).
2. Draft the new `CHANGELOG.md` section for `vX.Y.<counter>` from the commit
   range since the last release tag (highlights only). For patch releases, the
   notes must reflect all changes since the last minor version (e.g., `2.0.1`
   contains `2.0.0` + `2.0.1`), ensuring the release notes provide a complete view.
3. Synchronize all 8 version manifests via `python tools/bump_version.py <version>`.
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
   section embedded directly as its notes.  
   **Crucial:** Release notes must directly contain the relevant notes, highlights,
   and fix summaries from `CHANGELOG.md` in full — never just link to the changelog file.
5. **Wait for CI — do not push-and-pray.** Before treating the release as
   done, and before reporting success to anyone, run:

   ```powershell
   tools\ci-watch.ps1 -Ref vX.Y.<counter>
   ```

   It blocks until the tag's run finishes, prints every job, and exits 0
   exactly when the run itself concluded success — the platform's verdict.
   A continue-on-error leg (`MSan (best-effort)`) prints red on every green
   run and is reported as NON-GATING; a red gating job makes the run conclude
   failure and the script exits non-zero with a failure excerpt. Exit 0 is
   the only "green". The same applies after any ordinary push:
   `tools\ci-watch.ps1` with no arguments watches the current branch. Step 4
   is not complete until this exits 0 — `release-assets` depends on the
   required matrix legs (`msan` is deliberately outside its `needs`), so a
   red job it depends on means no artifacts were published.

## Tag retention & package ecosystem policy

OpenRAR coordinates git tags with external package registries (Debian APT, NuGet, PyPI, NPM):

1. **Immutable Anchor Tags (`vX.Y.0`):** The baseline tag for every `MAJOR.MINOR` line (`vX.Y.0`) is permanent and immutable. It provides the base revision for `git rev-list --count` commit counting, git submodules, and package provenance verification.
2. **Latest Patch Tag Retention (`vX.Y.Z`):** Each `MAJOR.MINOR` branch actively tracks its latest patch release (e.g., `v2.0.1`).
3. **Intermediate Patch Tag Pruning:** To maintain a clean, navigable tag namespace without cluttering git tooling, intermediate pre-release or superseded development patch tags within a minor line (`vX.Y.1` through `vX.Y.(Z-1)`) may be retired once the succeeding patch has completed its release and verification workflow.
4. **Registry Provenance:** Release assets published to permanent external package registries (NuGet, PyPI, NPM, Debian Launchpad) maintain corresponding reachable VCS revisions.

## Embedding the version

Implemented:

- `project(openrar VERSION ...)` as the single source of truth; the
  `openrar_dll` target derives file version and SOVERSION from it.
- `include/openrar/version.h` generated via `configure_file()` with
  `OPENRAR_VERSION_MAJOR`, `OPENRAR_VERSION_MINOR`, `OPENRAR_VERSION_PATCH`,
  `OPENRAR_VERSION_STRING`, and `OPENRAR_VERSION_HEX`, included from
  `include/openrar/openrar.hpp`.
- Exported `const char* openrar_package_version_string(void)` in the public C ABI
  (`include/openrar/openrar_dll.h`), machine-checked by ABI parity suites.
- CLI switch `openrar --version` in `src/cli/main.cpp` outputting the full package version banner.
- Atomic multi-target version synchronizer: `tools/bump_version.py` checking and updating
  all 8 locations with newline protection and anchor linting.
