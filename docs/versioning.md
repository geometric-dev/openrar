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

## Branch Naming & Experiment Promotion Policy

To prevent premature release commitment, history pollution, and shipping unverified regressions, the repository strictly decouples exploratory research from released code:

1. **Prohibition of Prospective Version Branches:**
   - **Never name an exploratory or developmental branch after a prospective release version** (e.g., branches named `v2.1`, `v2.2`, or `v2.3` used as development sandboxes are strictly prohibited).
   - Naming an unproven branch after a version creates premature psychological and administrative lock-in, encouraging the premature stamping and tagging of broken or regressing code.

2. **Experiment Branches (`exp/<topic>`):**
   - All speculative optimizations, algorithm prototypes, and microarchitectural trials must live on dedicated research branches prefixed with `exp/` (e.g., `exp/bextr-bitreader`, `exp/simd-wildcopy`, `exp/batch-hash-avx2`).
   - Every active experiment branch is recorded in the Roadmap (§11 Discovery & Experiment Register) with its designated kill gate.
   - Experiment branches are **fully disposable, squashable, and rebaseable**.
   - If a candidate fails its kill gate (as occurred with the forward-arrival parser in Arc 2.1 or BEXTR in H-20), the branch is squashed and archived under `archive/exp-<topic>` or pruned completely. **Failed experiments must never leave residual debt in trunk or release branches.**

3. **Feature Promotion (`feat/<topic>` → `master`):**
   - Only when an experiment passes all kill gates and demonstrates verified, reproducible improvements against baseline (`v2.0.23`) is it promoted.
   - The candidate is squashed into atomic, production-quality commits on a `feat/<topic>` branch with complete test and benchmark receipts, then merged to `master`.

4. **Release Branches (`release/vX.Y`):**
   - Release staging branches (e.g., `release/v2.3`) are cut exclusively from `master` when a milestone's promoted features are complete and entering final stabilization.
   - Release tags (`vX.Y.Z`) and CMake version bumps are permitted **only after** passing the 4-leg Release Qualification Gate.

---

## Release Qualification Gate ("Worthwhile Update" Enforcement)

A version must never be stamped as a production release, version-bumped in `CMakeLists.txt`, or tagged in git until it has completed and passed the **Release Qualification Gate (RQG)** against the certified baseline (`v2.0.23` or the latest qualified GA release).

Releases are strictly gated on proving a **meaningful, verified net improvement** without introducing regressions or inferior logic. Development and experimental work (e.g., hypothesis spikes, experimental parsers, ISA prototypes) must remain on feature or development branches; they must never be released to consumers without empirical qualification.

Every release candidate must pass all four qualification legs:

1. **Hard Correctness & Interop (Fail-Closed):**
   - 100% pass across the full test matrix (`ctest` across all 56+ suites).
   - Bit-exact roundtrip parity across all 30 golden test cells across methods `-m0` through `-m5`.
   - External oracle decompression parity (`unrar`, `7-zip`) verified on reference archives.

2. **Non-Regression Guard (`tools/release_regression_guard.py`):**
   - Candidate binary must be evaluated against the certified baseline (`v2.0.23`).
   - Must achieve `[SUCCESS]` with zero unexplained size regressions.
   - Ratchet ceiling: no size regression on deterministic corpora without prior registration in the hypothesis register.
   - Throughput floors strictly enforced: `-m1`..`-m3` $\ge 90\%$, `-m4` $\ge 75\%$, `-m5` $\ge 65\%$ of baseline.

3. **Empirical Optimization Receipts & Kill-Gate Adjudication:**
   - Any feature claiming microarchitectural speedup (SIMD, BMI, prefetching, wildcopy) must include empirical benchmark logs ("the receipts") on reference hardware across **all supported compilers** (MSVC, GCC, Clang).
   - If a mechanism introduces out-of-line ABI call overhead, instruction latency stalls, or regresses throughput on any supported compiler (as occurred with BEXTR in H-20), the kill gate triggers: the optimization is **refuted and excised**, not shipped.
   - Decompression throughput must meet or exceed baseline across text and binary corpora.

4. **Memory & Lifecycle Invariants:**
   - Invariants I-15 & I-16 verified: zero dynamic memory allocations on the decompression hot path (`ScopedAllocTracker`), and lazy committed virtual memory buffers.
   - Sanitizer clean: AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan) pass with zero defects.

---

## Release procedure

1. **Execute Pre-Release Qualification:**
   Run the qualification test suite and regression guard against the certified baseline:
   ```powershell
   python tools\release_regression_guard.py --baseline bin\v2.0.23\openrar.exe --report
   ```
   Confirm that all 4 legs of the Release Qualification Gate pass with zero regressions. If any gate fails, stop; do not proceed with versioning.
2. Read the counter and add 1 (the release commit counts itself — see
   "The release commit counts itself" above):
   `git rev-list --count vX.Y.0..HEAD` + 1 (adjust the anchor
   tag for the active MAJOR.MINOR line).
3. Draft the new `CHANGELOG.md` section for `vX.Y.<counter>` from the commit
   range since the last release tag (highlights only). For patch releases, the
   notes must reflect all changes since the last minor version (e.g., `2.0.1`
   contains `2.0.0` + `2.0.1`), ensuring the release notes provide a complete view.
4. Synchronize all 8 version manifests via `python tools/bump_version.py <version>`.
5. Commit as `release: vX.Y.<counter>`, tag `vX.Y.<counter>`, and push both.
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
6. **Wait for CI — do not push-and-pray.** Before treating the release as
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

## Registry publishing credentials

Publishing runs only from a `v*` tag, and both registry paths are token-free:
`publish-npm` and `publish-nuget` exchange a GitHub Actions OIDC token for a
short-lived, package-scoped credential. The publish-related secrets that remain
are `NUGET_USER` (the nuget.org **profile name**, not the account email) and
`LAUNCHPAD_GPG_KEY`.

### npm — `openrar-wasm` trusted publishing

Package settings → Trusted publishing must match exactly; npm compares the
token's workflow identity against it, so a near miss is a hard rejection:

| Field | Value |
|:---|:---|
| Provider | GitHub Actions |
| Organization or user | `geometric-dev` |
| Repository | `openrar` |
| Workflow filename | `build.yml` — the file name only, not `.github/workflows/build.yml` |
| Environment | *blank* |
| Allowed actions | **npm publish** ticked; a new publisher allows *stage publish* only |

The job needs `permissions: id-token: write`, a GitHub-hosted runner, and npm CLI
≥ 11.5.1 (Node 24 via `setup-node`). `wasm/js/package.json` must keep a
`repository.url` matching the GitHub repository, or provenance validation fails
with a 422.

**Pitfall: any configured token silently disables trusted publishing.** npm
performs the OIDC exchange only when no `_authToken` resolves for the registry.
Two things create one:

- a `NPM_TOKEN` / `NODE_AUTH_TOKEN` environment variable, or `npm config set
  //registry.npmjs.org/:_authToken …`;
- `actions/setup-node` with `registry-url:`, which writes
  `//registry.npmjs.org/:_authToken=${NODE_AUTH_TOKEN}` into `.npmrc` even when
  the secret is unset — the empty expansion still counts as "auth is
  configured", so npm skips the exchange and fails with `ENEEDAUTH`.

`publish-npm` therefore passes no `registry-url`, injects no token, and clears an
ambient `_authToken` first. Do not re-add `NPM_TOKEN`: the token wins, and an
unvalidated publisher **expires two days after creation**.

### Validating the npm publisher without a release

```bash
gh workflow run build.yml --ref master -f npm_oidc_check=true
```

Only the `npm trusted publisher (OIDC exchange)` job runs — every build job
carries the inverse condition, and the publish/verify jobs stay tag-gated — so
this finishes in under a minute without touching a tag or a registry. It posts
the Actions OIDC token to the registry's exchange endpoint
(`/-/npm/v1/oidc/token/exchange/package/<package>`); a matching publisher
answers **201** with `token_type: "oidc"`, and a non-2xx fails with the fields
to check. It has to live in `build.yml`: the publisher names that workflow file,
so a token minted by any other workflow is rejected.

### nuget.org — `OpenRAR.NET` trusted publishing

Policy fields: repository owner `geometric-dev`, repository `openrar`, workflow
file `build.yml`, environment blank, and a package pattern covering the id
(`OpenRAR.NET` or `OpenRAR.*`) with both scopes — *new packages* and *new
versions*. `publish-nuget` uses `NuGet/login@v1` and pushes with
`--skip-duplicate`, so re-running a tag is safe. The policy becomes permanently
active after its first successful publish.

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
