# Changelog

All notable changes to OpenRAR are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.6.0] - 2026-09-16

### Added

- **Installable CMake package**: `cmake --install` now ships the shared
  library, the CLI, the public headers, and a `find_package(openrar)` config
  exporting `openrar::openrar_dll` / `openrar::openrar` / `openrar::openrar_core`
  (GNUInstallDirs layout; library include dirs are export-clean via
  `BUILD_INTERFACE` generator expressions; `Threads` resolved through
  `find_dependency`). The public C ABI header moved from `src/dll/` to
  `include/openrar/openrar_dll.h` — consumers compiling with `-Iinclude` no
  longer reach into the source tree; `src/dll/openrar_dll.h` remains as a
  forwarding shim for internal translation units.
- **CLI overwrite query**: the documented default (`Prompt`) now actually
  asks — `existing file. Overwrite? [Y]es/[N]o/[A]lways/n[E]ver/[Q]uit` —
  instead of silently overwriting. `-y` answers Yes on every query;
  non-interactive stdin (pipes, CI runners) auto-answers Yes so scripted
  callers keep their previous behavior; `-o+` / `-o-` are now advertised in
  the help text, and `-o-` (skip existing) is applied as a pre-filter so the
  parallel extraction path honors it too.
- **CLI executable in release assets**: release zips now carry the CLI
  alongside the shared library and import library.

### Changed

- CI: the nightly fuzz job installs clang and passes it to CMake, so the
  `-fsanitize=fuzzer` harnesses really build in libFuzzer mode (they silently
  degraded to the standalone sweep under GCC), with a post-build guard that
  fails the job if any harness lacks libFuzzer. The writer-conformance job
  runs the full 11-suite Node test set; the WinRAR-oracle asserts in the
  volume/mutation/recovery/dictionary suites are `oracleAvailable()`-gated
  like roundtrip, and extraction destinations are platform-aware, so every
  suite is POSIX-clean while self-verification runs everywhere.

### Fixed

- **Extraction hardening**: `durable_write_to` retries the next temp suffix
  on `CreateNew` collision instead of aborting; every extraction-path
  filesystem call uses `error_code` overloads (including the multivolume
  chain scan's volume probes); a per-entry pre-open gate
  (`convert_self_links` + `has_symlink_parent` + destination-symlink removal)
  runs before any output stream is opened, so a symlink entry followed by a
  file entry can no longer divert the write; failed extractions remove their
  partial output (`FileUnlinker` RAII, `keep_broken` opt-in); the
  stored-payload file path (the CLI's extraction route) now verifies CRC32 —
  on both the contiguous and extent-stitch variants — instead of writing
  corrupted data successfully; encrypted entries stay unverified per the
  RAR5 rule that their header CRC32 does not hold the plaintext CRC.
- **Repair**: inline RR repair detects same-length payload corruption via
  parity syndromes, localizes damaged shards (cross-shard syndromes, header
  scan, CRC32 candidate verification) and reconstructs via Reed-Solomon,
  refusing ambiguous damage.
- **Format**: encrypted-header size VINT scan accepts up to 10 bytes with
  padded-VINT support (2 MiB headers no longer trip bad_password); AES-CBC
  primitives refuse non-block-aligned sizes instead of silently flooring
  (a short ciphertext on extraction now fails as corruption).
- **CLI/Windows**: wildcard arguments (`*`, `*.rar`) are expanded on Windows
  for add/update/freshen/move, `-r`-aware; `-r`, `-ol`/`-ol-`, `-ep1..3` and
  `--` are parsed and wired; the banner prints the project version instead of
  a hardcoded "1.0 (x64)"; the duplicate `-ed` help entry is gone.
- **Portability**: `openrar.hpp` compiles under C++20 (`u8_str` bridge for
  `path::u8string()`'s `char8_t` return); `/dev/urandom` opens with
  `O_CLOEXEC`.
- **API**: `extract_all` on an empty archive returns `RAR_OK` (DLL, C API and
  wasm layers) instead of a false `RAR_ERR_NOMEM`; parity buffer sizing is
  64-bit overflow-checked with a 2 GiB cap; the wasm JS/TS error surface now
  maps `MISSING_VOLUME` (-13) and `BUSY` (-14) instead of degrading them to
  generic `IO`.
- **Tests**: the golden BufferArchive verification block actually compiles
  now (its guard macro was defined nowhere); CLI tests stop creating a
  literal `nul` file on POSIX; new regressions for the corrupt-payload →
  `RAR_ERR_CRC_MISMATCH` mapping, partial-file removal, parent-is-file
  collisions, temp-collision retries, empty-archive extraction, padded VINTs,
  and parity buffer overflow caps.

## [1.5.0] - 2026-09-15

### Added

- **Extended entry metadata** (`OPENRAR_ABI_FEATURE_ENTRY_EX`, bit 5):
  `openrar_archive_handle_entry_ex` returns the fields the frozen 64-byte
  entry struct drops — host attributes, host OS, mtime/ctime/atime as
  FILETIMEs (UTC, 100 ns; both FHEXTRA_HTIME encodings supported —
  FILETIME-format passes through, unix-format converts), solid / encrypted /
  redirection / split / directory flags, dictionary size and the format
  version — with the redirection target as a malloc'd NUL-terminated string
  when present (`openrar_archive_entry_ex_free` frees it; `openrar_free`
  works too). File-mode handles only; buffer handles return
  `RAR_ERR_UNSUPPORTED_FEATURE`.
- **Archive-level info**: `openrar_archive_handle_info` reports main-header
  flags, volume index/count (always determinable — the file-mode open is
  strict), recovery record size, and the archive comment, which is read
  lazily at query time: stored-compressed comments decompress, and a
  payload failing its CRC or decode reports as absent with `RAR_OK` (a
  filesystem read failure is `RAR_ERR_IO`).
- **Refinements** (enhancement plan §4.3, all four): named window-size
  constants `OPENRAR_WINDOW_128K` … `OPENRAR_WINDOW_1M` with the
  bytes-vs-log2 unit difference documented;
  `RAR_ERR_PARTIAL_OK` documented verbatim (returned solely by
  `extract_all`); the thread-local error state documented on
  `openrar_last_error` / `openrar_archive_get_error`; the feature-bit
  registry now lists bits 0–5 and pins the reserve convention (future
  open-time options ship as `openrar_archive_open_file_ex` behind a new
  bit, never as signature changes).
- C++ wrapper: `ArchiveHandle::entry_ex(idx)` / `ArchiveHandle::info()` with
  `EntryEx` / `ArchiveInfo` value types (buffer handles throw
  `UNSUPPORTED_FEATURE`).
- Tests: `metadata_tests` (18th ctest target) — both htime encodings,
  flag coverage, redirection extra lifetime, lazy comment read, RR size,
  volume provenance, buffer-handle refusals.

### Notes

- All additive: `OPENRAR_DLL_API_VERSION` stays 1. Exports 43 → 46; feature
  bit 5 reserved and shipped. This completes the Crate request catalog —
  #6 (extended metadata) lands here; #1–#5 and #7 shipped in v1.2.0–v1.4.0.

## [1.4.0] - 2026-09-15

### Added

- **Atomic deletion** (`OPENRAR_ABI_FEATURE_MUTATION`, bit 4):
  `openrar_archive_delete_entries_file` deletes entries from an existing
  archive **by index** — indices are in the file-handle listing order (the
  sequence `openrar_archive_handle_list` reports on an `open_file` handle;
  file entries only, service headers never exposed). The DLL translates each
  index to the entry's header offset with its own strict reader and deletes
  by that identity, never by name, so entry names containing `*` / `?` are
  safe. The rewrite lands in a temp file, is flushed, and atomically
  replaces the original — untouched on any failure.
- **Batch add/replace**: `openrar_archive_add_files_file` appends files to an
  existing archive with 'u' semantics — incoming names override same-name
  entries (byte-exact UTF-8 compare after normalizing `\` to `/`; all prior
  instances stripped). `method ∈ {0,3,5}`, `window_log2 ∈ [1,4]` (create
  parity; ignored by stored entries). Directory sources become directory
  records (non-recursive). Added files are written unencrypted — password /
  `encrypt_headers` parameters are deliberately deferred.
- **Solid archives — suffix-only delete** (docs/invariants.md §1, now
  enforced): deleting a member of a solid run while a later member of that
  run is retained fails with `RAR_ERR_UNSUPPORTED_FEATURE` instead of
  silently orphaning the LZ chain — the same guard now backs the CLI's
  mask-based delete. Allowed shapes per run: untouched, suffix deletion,
  whole-run deletion. Replacement of any solid-block member (head included)
  is refused; replace the tail of a run via delete + add. Adding new names
  to a solid archive continues its stream.
- **`RAR_ERR_BUSY` (-14)**: the mutation exports pre-check every open
  file-mode handle in the process and fail up front when the target (or any
  volume of its set) is still held open — instead of an opaque Windows
  sharing violation during the final rename. Close handles, then mutate;
  hosts re-open after every mutation.
- **Refusals, all `RAR_ERR_UNSUPPORTED_FEATURE`**: locked (`MHFL_LOCK`),
  multi-volume (`MHFL_VOLUME`) and header-encrypted (`-hp`) archives (the
  mutation surface takes no password; "mutating header-encrypted archive
  requires password").
- Comment (CMT) preserved by both operations; QuickOpen locators stripped;
  recovery records copied verbatim (not recomputed — treat as absent after a
  mutation).
- C++ wrapper: `delete_entries(path, indices)` and `add_files(path, files,
  AddOptions)` free functions mirroring `create_archive`'s ergonomics.
- Tests: `mutation_tests` (17th ctest target) pins the index-space identity,
  solid delete/replace guards, 'u' semantics, atomicity on failed batches,
  the `RAR_ERR_BUSY` handle collision, validation parity and CMT/QO
  behavior. `fuzz_file_handle` gained a mutation leg (delete/add/re-open
  against arbitrary bytes).

### Notes

- All additive: `OPENRAR_DLL_API_VERSION` stays 1; no frozen export changed
  behavior. Exports 41 → 43; feature bit 4 reserved and shipped.
- The frozen `list_file` / `list_file_ex` walks surface comment/recovery
  service blocks as entries and are therefore NOT the delete index space on
  such archives — hosts must list via a file handle (docs/dll-integration-spec.md
  §6.12).

## [1.3.0] - 2026-09-15

### Added

- **File-mode handles** (`OPENRAR_ABI_FEATURE_FILE_HANDLE`, bit 3): the
  streaming reader is now reachable from the DLL. `openrar_archive_open_file`
  opens a scan-once handle over an archive **on disk** — the file stays open
  for the handle's lifetime, headers are walked exactly once, and
  encrypted/solid/multi-volume archives are supported (the buffer handle
  surface keeps its frozen MVP semantics). Passwords enter at open (required
  up front for `-hp`, verified lazily per entry via constant-time PswCheck
  for `-p`); progress/cancel cover the open-time scan; middle-volume paths
  rewind to the derived first volume. The existing handle exports dispatch
  on handle kind: in-memory extract on file handles is capped at 256 MiB
  (`RAR_ERR_NOMEM` above), `extract_all` is `UNSUPPORTED_FEATURE` on file
  handles, and `handle_list` exposes file entries only (service headers are
  internal blocks).
- **Streaming extraction with progress/cancel/durability**:
  `openrar_archive_handle_extract_to_path` writes straight to disk with byte
  progress (uncompressed produced vs `entry.size`), cancel per output chunk,
  and DLL-owned durability — `dest.openrar-tmp.<pid>.<seq>` opened
  CREATE_NEW, flushed (FlushFileBuffers/fsync), atomically renamed; abort or
  failure deletes the temp and never leaves a partial destination. Extracting
  onto the archive (or any volume of its set) is rejected up front.
- **Streaming integrity test**: `openrar_archive_handle_test` verifies
  CRC32 / BLAKE2sp without retaining output — fixed small RAM regardless of
  entry size (stored entries stream in 64 KiB chunks). Encrypted entries are
  verified through chunked AES-256-CBC decrypt (CBC IV carried across
  slices) via PswCheck/MAC — wrong password is `RAR_ERR_BAD_PASSWORD`, never
  `CRC_MISMATCH`, and plaintext is never surfaced.
- **Solid archives**: out-of-order and repeated extraction are correct — the
  reader transparently decodes the solid run prefix through a discard sink
  (catch-up); in-order extraction remains the fast path. Progress stays at
  (0, entry.size) during catch-up; the handle stays usable after any abort.
- **Multi-volume sets**: transparent extent stitching across `.partNN.rar`
  volumes (primary held open, secondaries opened per access); missing
  volumes fail the open or the extraction with the new
  `RAR_ERR_MISSING_VOLUME` (-13) and the offending path in the error detail.
- C++ wrapper: `ArchiveHandle(path, password, progress, cancel, user)`
  constructor plus `extract_to_path` / `test` methods.
- `docs/invariants.md`: the pinned engineering contracts (solid block, RAM
  ceiling, volume lifetime, callback/cancel, durability, key hygiene).
- Fuzzing: new `fuzz_file_handle` harness (file surface, hostile volume
  naming, password variants, decompression-bomb cancel guard) wired into the
  nightly fuzz job with the checked-in fixtures as seeds.

### Notes

- All additive: `OPENRAR_DLL_API_VERSION` stays 1; no frozen export changed
  behavior (the CLI's tolerant missing-volume open behavior is preserved;
  only the strict file-handle surface enforces complete sets). Exports
  38 → 41. See `docs/dll-integration-spec.md` §6.11.

## [1.2.0] - 2026-09-15

### Added

- **Password listing of header-encrypted archives**:
  `openrar_archive_list_file_pw` streams an archive from disk and, when it
  carries a `HEAD_CRYPT` block, derives keys from the supplied password
  (PBKDF2) and decrypts every following header (AES-256-CBC). Wrong password
  → `RAR_ERR_BAD_PASSWORD`; no password on a header-encrypted archive → the
  existing `RAR_ERR_ENCRYPTED` early signal. In this mode file entries with
  encrypted payloads are reported (`is_encrypted = 1`) and the walk
  continues — `-hp` implies encrypted file data, so rejecting them would
  defeat the purpose. Negotiated via `OPENRAR_ABI_FEATURE_LIST_PASSWORD`;
  the frozen v1.1.0 listing semantics are untouched. There is deliberately
  no password variant of the in-memory listing (documented in the header).
- **Callbacks on the handle-API scan**: `openrar_archive_open_ex` runs the
  scan that happens at open time with byte progress and cancel (cancelled →
  handle 0 with an "open aborted" detail). Negotiated via
  `OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS`.
- C++ wrapper: `list_archive_file(path, password, ...)` overload and
  `ArchiveHandle(data, size, progress, cancel, user)` constructors.

## [1.1.0] - 2026-09-14

### Added

- **Progress/cancel on the archive-listing APIs** (additive, new exports
  only): `openrar_archive_list_file_ex` and `openrar_archive_list_ex` take the
  existing `openrar_progress_cb` / `openrar_cancel_cb` convention. Progress is
  byte-based — `done` = archive bytes consumed vs. `total` = archive size,
  polled between header blocks and during the SFX scan, with one final
  `(total, total)` on success — because RAR has no central directory and an
  entry-count denominator cannot work. Cancel is polled between header
  blocks; a non-zero return yields `RAR_ERR_ABORTED` with all outputs left
  untouched and nothing partial allocated.
- **Streaming file listing**: `openrar_archive_list_file_ex` walks the archive
  on disk instead of slurping it into memory, so listing multi-GB archives no
  longer materialises them in RAM and aborts stay responsive while the walk
  seeks across slow (network) storage. The header-walk state machine is now
  shared with the in-memory `BufferArchive::list` so both listing paths
  cannot drift apart.
- **Early password signal**: a header-encrypted archive (`HEAD_CRYPT`) now
  returns the new `RAR_ERR_ENCRYPTED` (-12) from the `_ex` listing exports as
  soon as the block is reached, letting hosts prompt for a password
  immediately. The historical surfaces keep `RAR_ERR_UNSUPPORTED_FEATURE` for
  the same condition.
- **Capability negotiation**: `openrar_abi_features()` returns a feature
  bitmask (`OPENRAR_ABI_FEATURE_LIST_PROGRESS`). `OPENRAR_DLL_API_VERSION`
  stays at 1 by policy — hosts negotiate additive exports via the feature bit
  or `GetProcAddress`, never via the version probe (`docs/versioning.md`).
- C++ wrapper overloads `list_archive(rar, progress, cancel, user)` and
  `list_archive_file(path, progress, cancel, user)`.

## [1.0.126] - 2026-09-13

First release on the public repository (github.com/geometric-dev/openrar).
Rolls up the initial CI bring-up: the codebase had never run under hosted CI,
and the first runs surfaced five platform correctness bugs alongside the
expected workflow fixes.

### Fixed

- **ARM64 CRC-32 produced wrong checksums on Apple silicon** — the ARMv8 CRC
  instructions chain the accumulator in the same raw (pre-inversion)
  convention as the scalar table step; an erroneous double inversion
  corrupted every CRC when the hardware path dispatched (`arm_crc32=1`).
  This path only compiles on macOS, where it had never been exercised.
- **SFX modules built with the bundled `Default.SFX` stub failed in
  UnRAR** ("Main archive header is corrupt"): the stub binary embedded a
  literal RAR5 signature constant, and UnRAR locates the archive behind an
  SFX prefix with a naive first-match scan, stopping inside the module.
  The signature is now assembled at runtime from XOR-masked bytes; the
  literal no longer appears in any binary. WinRAR's own stubs avoid
  embedding it for the same reason.
- **SFX structural test parser**: the JS `findSig` helper locked onto the
  first signature match — inside a module that legitimately embeds one —
  and parsed garbage. Candidates are now validated (header CRC plus a
  main/crypt block type must follow), mirroring the C++ reader.
- POSIX portability: `<sys/stat.h>` include in `archive_mutator.cpp`;
  MSVC ARM64 include guards (`<arm_acle.h>` / `<immintrin.h>` are not
  available there).
- aarch64 GNU/Clang builds now request `-march=armv8-a+crypto`; the AES-256
  and SHA-256 kernels use the crypto intrinsics unconditionally on
  `__aarch64__` and generic cross toolchains don't enable the feature by
  default (Apple clang does).

### Changed

- CI: Windows legs pinned to `windows-2022` (`windows-latest` now ships
  only VS 2026, so the VS 2022 generator cannot configure); the
  `msvc-arm64` leg is cross-compile-only (x64 hosts cannot execute ARM64
  test binaries — aarch64 runtime coverage stays with the QEMU job); all
  third-party actions pinned to commit SHAs; dead `numa08/setup-ninja`
  replaced with `seanmiddleditch/gha-setup-ninja`; formatting gate pinned
  to `clang-format-18` and the tree reformatted with it; Linux test
  runners accept Ninja-style binary paths.
- Repository: `LICENSE` renamed from `license.txt` (acknowledgement footer
  moved out so GitHub detects MIT), trademark/non-affiliation notice added
  to the README, `.gitattributes` added for cross-platform line endings.

## [1.0.121] - 2026-09-10

First tagged release, cut 121 commits past the v1.0.0 baseline. Entries cover
everything since the 2026-08-31 baseline where WinRAR interoperability was
restored and verified and compression speed parity was regained; earlier
development history is not itemized here.

### Added

- **Shared library**: stable C ABI (`openrar_dll.h`) with a C++ wrapper,
  architecture-suffixed binaries (`openrar_x64` / `openrar_arm64`), and an
  integration spec with C#, Python, and Rust examples.
- **WebAssembly build**: in-memory RAR5 create/list/extract with a handle-based
  JS API and a canonical interface contract.
- **Streaming**: incremental block encoder, streaming decompression with
  extraction flush callbacks, and buffered in-memory archive read/write.
- **Multi-volume archives**: streamed multi-volume create, plus `.rev` recovery
  volumes with generation and repair matching WinRAR's shard layout.
- **SFX creation** (`-sfx[name]`): SFX module prepended on create.
- **Header encryption** (`-hp`): supported on write and read.
- **Extended metadata**: nanosecond high-precision times plus OWNER and
  VERSION header extras.
- **Interop gate** (`tools/interop_gate.py`): self-roundtrip, WinRAR
  cross-decode, and a full ctest run, enforced as a pre-commit hook and in CI;
  covers multi-volume and SFX archives.
- **Performance**: compression runs ~2-4x faster than WinRAR on the 50 MB
  benchmark suite; ~2x faster PBKDF2 via cached HMAC midstates; ~8x filter
  speedup on non-SIMD paths; faster crc64 and header reads; leaner per-literal
  token storage.
- **Test suite**: property-based tests, a deterministic shape-aware roundtrip
  fuzzer, a libFuzzer-ready decoder harness, decompression cross-validation,
  WinRAR conformance scripts, golden files, and known-answer tests.
- **Release process**: SemVer versioning with the patch component as a
  monotonic commit counter (`docs/versioning.md`), this changelog, and
  `project(VERSION)` as the single version source of truth.

### Fixed

Safety and robustness:

- Zip-Slip path traversal on extraction; hardlink and FILECOPY sources
  confined to the extraction root; archive-controlled names hardened for
  Windows filesystems; planted-symlink truncation blocked via unique temporary
  names; recovery-record directory conversion only touches links the reader
  itself created.
- Malformed-archive hardening: vint field validation, attacker-controlled size
  clamping, 64-bit recovery-record geometry, decompressor filter/window
  accounting, buffer-extract output budgets, and AVX2 dispatch gated on
  OS-level AVX state (OSXSAVE/XCR0).
- Crypto: constant-time password-check comparison, PBKDF2 iteration-count
  guard, SHA-NI digest self-check, and password-derived material wiped on
  teardown.
- C ABI: escaping C++ exceptions caught at every `extern "C"` boundary, struct
  packing hygiene, and stream callbacks dispatched outside the map mutex.

Correctness:

- WinRAR interoperability restored (absolute Huffman table lengths, BC max
  bits, window-size sync) and locked in by the interop gate.
- Copy-match overlap undefined behavior; encoder corruption past the window
  size on external-buffer sources; large-file compression (empty blocks,
  memory streaming, RLE overflow); compressor state reuse across instances;
  solid-entry decoder state persistence; stored-entry CRC32/BLAKE2sp
  verification.
- Data-loss safety: archive replacement made data-loss-safe; EINTR-safe POSIX
  I/O; 32-bit MSVC intrinsic fixes.

### Changed

- CI builds and publishes the shared library across the matrix and runs the
  hardened interop gate (strict multi-volume cross-check, no weak fallback).
- Volume-chain cap raised to 65535 volumes; SFX size bound raised to 64 MiB.

## [1.0.0] - 2026-08-31

Baseline (development state, not distributed): clean-room RAR5 archiver with
WinRAR interoperability verified by cross-decode and compression running 2-3x
faster than WinRAR on the 50 MB benchmark suite.
