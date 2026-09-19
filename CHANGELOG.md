# Changelog

All notable changes to OpenRAR are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.16.0] - 2026-09-19

### Added

- **RAR5 File Versioning (`-ver[n]`) & Historical Version Pipeline**:
  - Implemented RAR5 file versioning support according to format specification and WinRAR parity.
  - Extra record `0x04` (`FHEXTRA_VERSION`) handling:
    - Encodes 64-bit VINT flags (`0x00` default) and 64-bit VINT version number.
    - Historical versions carry `has_file_version = true` and `file_version = 1, 2, ...`.
    - Active unversioned entries represent the latest active revision without `FHEXTRA_VERSION`.
  - Zero-recompression mutating pipeline in `ArchiveMutator::write_batch_add` & `write_batch_add_ex`:
    - Converting active entries into historical versions promotes existing entries without recompression or re-encoding.
    - Zero-overhead payload byte copying via `copy_stream_region` directly from existing offsets, preserving bit-exact payload CRC32 and memory efficiency.
    - Pruning enforcement with `-vern`: limits total historical versions to $n$, pruning oldest historical versions while preserving solid chain invariants.
- **CLI `-ver[n]` Switch & Extraction Semantics**:
  - Added `-ver` argument parsing precedence strictly evaluated before `-v` to prevent volume switch collisions.
  - Archive listing (`l`, `lt`) appends `;version` to historical versions and outputs `File version: <v>` in technical listings.
  - Extraction semantics:
    - Default extraction (`x` / `e`): extracts only latest active versions, skipping historical versions unless explicitly targeted by name with `;`.
    - `-ver`: extracts all versions with `;version` suffixes appended to avoid file collisions on disk.
    - `-verN`: extracts specifically version $N$ without suffix.
- **Additive DLL ABI Flag (`OPENRAR_ENTRY_FLAG_HAS_VERSION`)**:
  - Added `OPENRAR_ENTRY_FLAG_HAS_VERSION = (1u << 9)` in `include/openrar/openrar_dll.h`.
  - Populated in `FileArchiveHandle::entry_ex` while preserving frozen 64-byte `openrar_archive_entry_t` struct layout.
- **Comprehensive Test Suite & Dual-Oracle Cross-Validation**:
  - Authored `tools/tests/versioning.tests.mjs` verifying version accumulation, `-vern` pruning, default extraction, `-ver` multi-extraction, and targeted version extraction.
  - Verified bidirectional compatibility against official WinRAR 7.20 (`rar.exe` and `UnRAR.exe`).

## [1.15.0] - 2026-09-19

### Added

- **Pre-Processing Filter Pipeline & Compression Ratio Parity**:
  - Implemented full forward filter transform pipeline in `Filters50` (`encode_e8`, `encode_arm`, `encode_delta`) matching RAR5 specification and WinRAR 7.20 bitstream rules.
  - Added x86 CALL/JMP (`E8`, `E8E9`) jump address translation with circular dictionary wrap-around and relative offset calculation.
  - Added ARM BL relative instruction translation with PC-relative branch decoding.
  - Added multi-byte / multi-channel delta transform (`Delta`) with automatic channel detection (1..32 channels) and stride-based difference filtering for raw audio, imagery, and columnar binary data.
  - Integrated in-band filter token emission in `Compressor50` (`FilterToken`, slot 256 execution records, block length vint encoding) with bounded sliding window invariants.
- **First-Class `-mc` Switch Engine in CLI**:
  - Added full support for the WinRAR `-mc` switch family:
    - `-mc-`: Disable all pre-processing filters.
    - `-mc[param]E[+|-]`: Configure / force / disable x86 executable filter (`E8`/`E8E9`).
    - `-mc[param]A[+|-]`: Configure / force / disable ARM branch filter.
    - `-mc[param]D[+|-]`: Configure / force / disable multi-channel delta filter with channel stride override (e.g. `-mc16:4D+`).
    - `-mc[param]L[+|-]` & `-mc[param]X[+|-]`: Tolerant acceptance for long-range and exhaustive matching switches.
    - Compound multi-filter switch syntax support (e.g. `-mcE+D-`).
- **Additive DLL API Filter Negotiation (`OPENRAR_ABI_FEATURE_FILTERS`)**:
  - Exported `openrar_archive_create_file_opts` under new additive feature bit `OPENRAR_ABI_FEATURE_FILTERS = (1ull << 10)` in `include/openrar/openrar_dll.h`.
  - Added bitmask flags `OPENRAR_FILTER_DISABLE_ALL`, `OPENRAR_FILTER_FORCE_E8`, `OPENRAR_FILTER_DISABLE_E8`, `OPENRAR_FILTER_FORCE_ARM`, `OPENRAR_FILTER_DISABLE_ARM`, `OPENRAR_FILTER_FORCE_DELTA`, and `OPENRAR_FILTER_DISABLE_DELTA`.
  - Preserved strict backward ABI stability (`OPENRAR_DLL_API_VERSION = 1`, 64-byte `openrar_archive_entry_t` unchanged).
- **Solid Archive & `StreamEncoder` Filter Invariants**:
  - Enforced per-file filter isolation in solid archives: filter token bounds and transforms strictly reset at entry boundaries while preserving continuous LZ sliding dictionary history across solid chains.
  - Integrated in-place filter transformations into `StreamEncoder` and `Compressor50` bounded memory windows, producing bit-identical compressed streams with zero unbounded RAM growth.
  - Added WASM / C API streaming encoder export `openrar_stream_create_ex(method, win_size, filter_flags)` and updated npm package (`wasm/js/openrar.js`, `openrar.d.ts`) with `filterMode: 'auto' | 'none' | 'e8' | 'arm' | 'delta'` in `compressStream` and `compressStreamChunks`.
- **Dual-Oracle Cross-Validation**:
  - Expanded dual-oracle interop gate (Track 2) with full bidirectional filter test matrix against official WinRAR 7.20 (`rar.exe`) and UnRAR 7.20 (`UnRAR.exe`).

## [1.14.0] - 2026-09-19

### Added

- **First-Class Native Archive Creation in C ABI / DLL (`OPENRAR_ABI_FEATURE_CREATE`)**:
  - Exported `openrar_archive_create_file` and `openrar_archive_create_file_ex` under additive feature bit `OPENRAR_ABI_FEATURE_CREATE = (1ull << 9)` (`docs/versioning.md`), preserving the frozen 64-byte `openrar_archive_entry_t` ABI contract.
  - Supports non-existent target bootstrapping, atomic durability replacement, exact dictionary window sizes (`dict_size`), password encryption, solid chaining, and real-time progress/cancellation callbacks.
  - Added RAII C++ convenience wrapper `openrar::Archive::create`.
  - Relaxed `openrar_archive_add_files_file` to accept compression methods 0–5, modern dictionary sizes up to 64 GiB, and automatic creation for non-existent archive targets.
- **Streaming Multi-Volume Creation (`-v<size>`) Without Whole-File RAM Buffering**:
  - Overhauled `ArchiveMutator::add_file_to_archive_vol` to guarantee an invariant $O(\text{dictionary window})$ memory ceiling during multi-volume creation, completely eliminating `uncompressed.resize(file_sz)` whole-file RAM buffering.
  - Stream-compresses large files through bounded spool buffers and slices payloads across volume boundaries using 64-bit extents and offsets.
  - Connected the `-md` custom dictionary switch to multi-volume archiving, added a minimum volume size guard (`vol_size >= 4096`), and preserved transactional `.mv_bak` sidecar replacement.
- **Direct-to-Archive Streaming Compression (Zero Double-Spooling via Fixed-Width vint Back-Patching)**:
  - Eliminated temporary disk spool files (`spool_tmp`) for large unencrypted files during single-file and sequential additions, cutting disk write I/O by 50% and peak scratch disk usage to 1x payload.
  - Implemented 10-byte fixed-width vint (`push_vint_fixed`) header back-patching in `HeaderWriter::serialize_file_block`, allowing in-place updating of `pack_size`, `data_crc32`, and header CRC without shifting byte offsets or violating RAR5 specification leniency.
  - Integrated deterministic store fallback truncation (`out.truncate(orig_pos)`) for payloads that expand during compression.
- **`StreamEncoder` Store-Mode RAM Uncapping & Bidirectional WASM Streaming**:
  - Uncapped store mode (`method == 0`) streaming in `StreamEncoder`, passing chunks directly to `flush_cb_` or yielding via `take_output` with zero whole-stream RAM buffering.
  - Added incremental block pulling (`take_output`) to `StreamEncoder`.
  - Exported streaming compressor C ABI functions (`openrar_stream_compress_new`, `openrar_stream_compress_feed`, `openrar_stream_compress_pull`, `openrar_stream_compress_finish`, `openrar_stream_compress_free`).
  - Added `compressStreamChunks` async generator to the WebAssembly npm wrapper, achieving full bidirectional streaming symmetry (`compressStream`, `compressStreamChunks`, `decompressStream`, `decompressStreamChunks`).
  - Bounded WASM compression dictionary allocations to $\le 64\text{ MiB}$ under `__EMSCRIPTEN__` to prevent 32-bit linear memory exhaustion.

## [1.13.0] - 2026-09-19

### Added

- **64-Bit In-Memory Extraction Buffers**:
  - Raised `MAX_STREAM_OUTPUT` (`Decompressor50`) and `MAX_TOTAL_OUTPUT` (`BufferArchive`) from 4 GiB to **64 GiB** on 64-bit native platforms (`sizeof(void*) >= 8`), guarded with `std::bad_alloc` exception handling mapping to `AllocationFailed` / `RAR_ERR_NOMEM`.
  - Preserved defensive 2 GiB bounds on 32-bit / Emscripten WASM builds.
- **64-Bit Recovery Parity Buffer Scaling**:
  - Raised `MAX_PARITY_BUFFER_CAP` in `RecoveryRecord` from 2 GiB to **64 GiB** on 64-bit platforms, enabling multi-gigabyte RS-parity blocks for large archives while preserving 2 GiB bounds on 32-bit platforms.
- **64-Bit Compression Match Finder Horizon**:
  - Implemented 64-bit match-finder tables (`head64_` / `prev64_`) in `Compressor50` when `win_size_ > 4 GiB`, eliminating 32-bit distance truncation in sliding-window match searches across $> 4\text{ GiB}$ horizons.
  - Dynamically allocates 64-bit tables only for $> 4\text{ GiB}$ dictionaries, maintaining zero overhead and 32-bit cache locality for standard dictionaries ($\le 4\text{ GiB}$).
- **Dynamic CLI Concurrency RAM Budget**:
  - Scaled CLI concurrency `PREPARE_BUDGET` dynamically based on detected host physical RAM (`GlobalMemoryStatusEx` on Windows, `sysconf` on POSIX), allocating 25% of system RAM clamped between 1 GiB and 32 GiB.
  - Concurrency throttling for large dictionaries now dynamically adjusts worker threads against available physical memory.

## [1.12.0] - 2026-09-19

### Added

- **Elimination of Arbitrary Memory & Window Ceilings**:
  - Uncapped `ALLOC_LIMIT` in `Decompressor50` and `format::headers` to **64 GiB** on 64-bit native hosts (`sizeof(void*) >= 8`), matching WinRAR 7.0 max profile, while preserving defensive 1 GiB / 2 GiB bounds on 32-bit / Emscripten WASM.
  - Raised `MAX_WIN_SIZE` in ABI contract (`src/api/abi_contract.hpp`) and C DLL (`src/dll/dll_api.cpp`) to 64 GiB on 64-bit native platforms.
  - Implemented lazy window allocation in `Decompressor50` with `try/catch(const std::bad_alloc&)` mapping allocation exhaustion to `DecompressErrorCode::AllocationFailed`.
- **64-Bit Distance Bit Decoding**:
  - Implemented `core::uint64 BitReader::get_bits64(unsigned int count)` leveraging the 64-bit accumulator register (`acc_`).
  - Fixed 32-bit distance truncation bug in `Decompressor50::decompress_internal`: extra-distance slots 68–79 (`d_bits > 36`, distances $> 4\text{ GiB}$) now decode via `get_bits64(d_bits - 4)` into `core::uint64 extra` without truncation or assertion failures.
- **Exact Byte Dictionary Parameterization**:
  - Modernized `ArchiveMutator::prepare_add_file` to accept `core::uint64 dict_size` directly instead of a lossy `window_log2`.
  - Maintained backward compatibility: inputs 1..15 decode to `0x20000ULL << (val - 1)`, while values > 15 are treated directly as byte counts.
  - Removed 15-iteration cap loop in CLI `main.cpp`, passing `opt_dict_size` directly to preserve fractional non-power-of-two dictionaries (`-mdx48m`, `-mdx96m`) and large dictionaries (`-md4g`..`-md64g`).
- **Archive Reader Error Fidelity**:
  - `ArchiveReader` and `BufferArchive` explicitly map decompressor `AllocationFailed` to `RAR_ERR_NOMEM (-5)` instead of falling through to misleading `RAR_ERR_TRUNCATED (-3)`.
  - Oversized dictionaries cleanly map to `RAR_ERR_LIMIT_EXCEEDED (-15)`.

## [1.11.0] - 2026-09-19

### Added

- **Incremental Streaming Decompressor (`StreamDecoder`) & WASM API**:
  - Bounded memory decoder architecture decoupling bit-reading from stream chunk boundaries via stateful input FIFO staging (`in_queue_`).
  - Native WebAssembly C ABI exports (`openrar_stream_decompress_*`) and async TypeScript generators (`decompressStream`, `decompressStreamChunks`).
  - Enforced defensive 64 MiB window allocation ceiling under `__EMSCRIPTEN__` to prevent linear memory exhaustion.
- **RAR7 Format Level Emission (`unp_ver = 1`) & Fractional Dictionaries**:
  - Exact formula serialization for RAR7 dictionary fractions `(win_size - pow2) * 32 / pow2` with 31 ceiling clamp in `HeaderWriter`.
  - Decoupled `unp_ver` from distance slot table sizing: `TABLE_SIZEX = 446` reserved strictly for $> 4\text{ GiB}$ dictionaries, retaining `TABLE_SIZE = 430` for intermediate non-power-of-two dictionaries $\le 4\text{ GiB}$.
- **Multi-Volume `.rev` Cauchy Parity Repair in C DLL ABI**:
  - Added `openrar_archive_repair` and ABI feature bit `OPENRAR_ABI_FEATURE_REPAIR = (1ull << 8)` supporting inline Recovery Records and external Cauchy Reed-Solomon `.rev` parity reconstruction.
- **Vectorized SIMD Match Acceleration**:
  - Boundary-guarded AVX2, SSE2, and ARM Neon match-finding loops in `src/compress/arch/match_simd.hpp`.

## [1.10.0] - 2026-09-18

### Added

- **Engine Streaming & Unbounded File Size**:
  - Removed internal 1 GiB file size cap via 16 MiB spooling threshold (`SPOOL_MEMORY_THRESHOLD = 16 MiB`) enabling multi-gigabyte file mutations without unbounded memory consumption.
  - RAII `SpoolFileGuard` ensuring guaranteed temporary file unlinking and zero temp leakage across exceptions, cancelation, and write aborts.
  - In-flight AES-256-CBC encryption to spool when archive encryption is enabled, ensuring zero unencrypted plaintext touches disk.
  - Large uncompressed files (> 16 MiB) stream directly from source path into archive writes without allocating intermediate disk spool files.
  - Single-pass store streaming with in-flight CRC calculation and header back-patching, accelerating uncompressed `-m0` throughput from 74.7 MB/s to 373.9 MB/s (5x speedup, outperforming WinRAR 7.20 by 25.3%).
  - Upgraded transfer and streaming buffers to 1 MiB across `copy_stream_region` and payload ingestion loops.
- **Adaptive Dictionary Window Sizing & CLI `-md<size>` Switch**:
  - Scaled default compression dictionaries: `-m3` to 8 MiB (was 2 MiB), `-m4` to 16 MiB (was 4 MiB), `-m5` to 64 MiB (was 16 MiB).
  - Adaptive dictionary clamping for non-solid files: clamps window size down to the nearest power of two of file size (floor 128 KiB) to avoid allocating oversized dictionaries for small files.
  - CLI `-md<size>` (e.g. `-md16m`, `-md64m`) supporting 128 KiB to 1 TiB dictionary sizes with dynamic thread concurrency throttling under `PREPARE_BUDGET` (1 GiB).
  - Accurately gated compressor workspace memory estimation ($5W + 6\text{ MB}$) in `compress_plan.hpp`.
- **True Cross-File Solid Compression Safety**:
  - Enforced single-worker constraint (`threads = 1`) for solid batch compression in `run_batch_add`, eliminating cross-file dictionary race conditions.
- **CLI Ergonomics & Parity**:
  - `p` command: stream archive entries directly to stdout in raw binary mode (`_O_BINARY` on Windows) via `ArchiveReader::extract_entry_sink`.
  - Positional `@<list>` listfile argument expansion with UTF-8 BOM removal and `#`, `;`, `//` comment stripping.
  - `-x<pattern>` and `-x@<list>` file exclusion filtering supported across all CLI commands (`a`, `u`, `f`, `m`, `x`, `e`, `l`, `t`).
- **C DLL ABI Additions**:
  - `openrar_archive_handle_set_limits`: dynamic runtime extraction limits on open archive handles with `std::atomic<bool> busy` concurrency protection returning `RAR_ERR_BUSY = -14` during active extractions.
  - ABI feature discovery bit `OPENRAR_ABI_FEATURE_SET_LIMITS = (1ull << 7)`.
  - Static assertion parity between C DLL ABI and core engine `RAR_ERR_BUSY`.

## [1.9.3] - 2026-09-18

### Added

- **Resource Limits API (`ExtractionLimits`, `LimitState`, `RAR_ERR_LIMIT_EXCEEDED = -15`)**:
  - Fine-grained resource limit enforcement in extraction pipeline: `max_member_bytes`, `max_total_bytes`, and `max_header_bytes`.
  - In-flight dynamic budget tracking across streaming decompression chunks (`kStreamChunk = 64 KiB`) providing deterministic aborts against decompression bombs even with `FHFL_UNPUNKNOWN`.
  - Cumulative header and decompression limits across multi-volume sets.
  - Mirrored in C DLL ABI with `RAR_ERR_LIMIT_EXCEEDED = -15` (with static assertions in `abi_contract.hpp`) and TypeScript definitions in `wasm/js/openrar-archive.d.ts`.
- **Formal Extraction Contract (`docs/EXTRACTION_CONTRACT.md`)**:
  - Architectural contract specifying cancellation granularity, CRC/BLAKE2sp checksum verification ordering, solid archive replay semantics, memory and byte budget limits, and multi-volume boundaries.
- **RFC 3629 UTF-8 Filename Validation Hardening**:
  - Implemented strict RFC 3629 UTF-8 validator in `core::is_valid_utf8` rejecting non-shortest forms, surrogate code points (`U+D800`..`U+DFFF`), and out-of-range values.
  - Enforced in `HeaderReader` for RAR5 file header filenames, rejecting corrupted or malicious archives with `RAR_ERR_BAD_DATA`.
- **Golden Fixture Harness & Deterministic Tooling**:
  - Cross-platform golden fixture suite with portable `=key=value` naming scheme to avoid Windows NTFS alternate data stream collisions (`tests/fixtures/README.md`).
  - Automated generator (`tools/generate_golden.ps1`) and hash verifier (`tools/check_golden.ps1`) validating writer and mutator archives against companion `.sha256` files.
  - CTest test harness integration in `tests/unit/golden_fixtures_tests.cpp`.
- **Writer Plan/Schedule/Execute Separation (`src/compress/compress_plan.hpp`)**:
  - Refactored `ArchiveMutator` and archive writing architecture cleanly separating pre-execution planning (`CompressPlan`, `ExecutionPlan`, `EntryPlan`), concurrency/resource scheduling, and low-level byte serialization.

## [1.9.2] - 2026-09-18

### Added

- **7 Automated Interop Boundary Quality Gates (`tools/interop_gate.py`)**:
  - Track 1: Solid mixed-stream invariants (`-s -m3` mixing 0B files, store fallbacks, and compressed files without dictionary corruption).
  - Track 2: RAR5 executable filter decoding (`-mc`) across 512K/1M circular ring-buffer boundaries.
  - Track 3: High-precision 64-bit Windows FILETIME timestamps with pre-1970 negative Unix epoch and post-2038 rollover protection.
  - Track 4: Multi-byte UTF-8 password and key derivation (`-p` / `-hp`) across German umlauts, French accents, CJK characters, and 4-byte UTF-8 emojis.
  - Track 5: QuickOpen (QO) table invalidation and locator stripping upon external mutation (`openrar d`, `u`, `f`, `k`).
  - Track 6: Recovery volume (`.rev`) Cauchy erasure coding $GF(2^{16})$ parity compatibility with official WinRAR repair (`rar rc`).
  - Track 7: 64-bit VINT size bounds and in-memory heap allocation exhaustion guards.
- **Local UnRAR Integration (`dev\unrar`)**:
  - Automatically discovers and prefers local `UnRAR.exe` builds alongside official WinRAR installations for zero-setup local conformance verification.

### Fixed

- **RAR5 Executable Filter Transform & Deserialization**:
  - Fixed bitstream deserialization in `Decompressor50`: filter length and offset parameters now decode via official 2-bit length prefix + LE32 (`read_filter_data`), eliminating bitstream desynchronization previously caused by LEB128 parsing.
  - Modified `flush_pending_blocks` to transform filtered data out-of-place directly into the output callback rather than writing back to `window_`, guaranteeing that subsequent LZ77 string matches reference raw un-transformed dictionary history.
- **Pre-1970 Negative Timestamp Arithmetic Underflow**:
  - Fixed Windows `FILETIME` conversion in `ArchiveMutator` to store native 64-bit FILETIME values directly (`fa.ftLastWriteTime`) with `is_unix = false` and spec-compliant extra flags (`0x02` mtime, `0x04` ctime, `0x08` atime), preventing year 1965 from wrapping to year 2101 in WinRAR.
- **Windows CLI Argument Unicode Encoding**:
  - Replaced ANSI `char* argv[]` argument ingestion on Windows with `CommandLineToArgvW(GetCommandLineW())` converted to UTF-8 in `main.cpp` and `sfx_main.cpp`, guaranteeing that non-ASCII CLI passwords match WinRAR UTF-8 key derivation.

## [1.9.0] - 2026-09-18

### Added

- **`MHEXTRA_METADATA` Serialization & Mutation Preservation (`-ams` / `-am`)**:
  - Full deserialization in `HeaderReader::parse_main_block` and serialization in
    `HeaderWriter::write_main_block` for RAR5 main header extra record `0x02`.
  - Encodes archive software identity, nanosecond-precision ctime, and Unix epoch flags.
  - Complete mutation inheritance in `ArchiveMutator`: archive mutations (`d`, `u`,
    `f`, `m`, `k`, `s`, `-rr`) preserve `MHEXTRA_METADATA` from the original archive
    unless explicitly overridden.
  - Conformance test enabled in `tools/tests/format.tests.mjs` with 110/110 passing suite.
- **Dedicated Recovery Volumes (`-rv[N]` & `rv[N]`)**:
  - Support for `-rv[N]` switch and `rv[N]` standalone command for multi-volume recovery volume sets (`.rev` files).
  - Multi-threaded RS16 Cauchy parity computation (`-mt`) with deterministic byte output.
  - Strict validation: fast fail with diagnostic error message when attempting to generate recovery volumes on single-volume archives.
  - Robust RAII `RevCleanupGuard` ensuring `.rev` temporary files and incomplete artifacts are scrubbed on error or exception.
- **Win32 Reparse Point & Junction Hardening**:
  - Unprivileged creation of Windows directory junctions using `FSCTL_SET_REPARSE_POINT` with mandatory `\??\` NT namespace prefix for `SubstituteName` and Win32 path for `PrintName`.
  - Defensive parser hardening: strict bounds checking for `PrintNameOffset`, `PrintNameLength`, `SubstituteNameOffset`, `SubstituteNameLength` against `bytes_returned` and `ReparseDataLength`.
  - Traversal breakout protection: added `is_reparse_or_symlink()` inspecting `FILE_ATTRIBUTE_REPARSE_POINT` to prevent directory junctions from bypassing `has_symlink_parent()` extraction sandboxing.
- **Fuzzing Harness Expansion & Seed Corpus**:
  - Expanded `fuzz_archive` and `fuzz_file_handle` harnesses to ingest seed corpora dynamically.
  - Generated comprehensive seed corpus for QuickOpen, metadata extra records, recovery records, and recovery volume sets.
- **WASM / JS Packaging & TypeScript Validation Polish**:
  - Guarded Worker tests against missing build artifacts for headless/non-emscripten environments.
  - Maintained complete TypeScript definitions for the in-memory archive API.

## [1.8.0] - 2026-09-17

### Added

- **QuickOpen (QO) Engine**: Full reader acceleration and writer serialization
  for RAR5 QuickOpen service blocks.
  - Writer: Contiguous header cache arena (`qo_arena`), structure CRC32
    verification, pre-sized payload buffers, and 10-byte fixed-width locator
    backpatching into `MainBlock`.
  - Reader: Instantaneous archive open probing `MainBlock` locator offsets and
    seeking directly to tail QO cache with defensive bounds checking, structure
    CRC validation, monotonic offset verification, and transparent fallback to
    sequential scanning on corrupt or partial caches.
  - Spec-compliant stripping: All archive mutation operations (`d`, `u`, `f`,
    `m`, `k`, `s`, `-rr`) automatically strip QuickOpen blocks and locators.
- **Three-Phase Multi-Threaded Parallel Extraction**: Decoupled parallel
  extraction architecture:
  - Phase 1: Parallel file decompression across worker threads (`-mt`).
  - Phase 2: Sequential restoration of symlinks, hardlinks, and junctions,
    guaranteeing link target files exist on disk before link creation.
  - Phase 3: Bottom-up directory metadata and timestamp restoration in reverse
    topological order (deepest directories first), preventing parent directory
    `mtime` clobbering.
- **Hardlink Deduplication** (`-oh`): Identifies duplicate hardlinks across
  Windows FileID (64/128-bit) and POSIX `(dev, ino)` pairs, storing subsequent
  instances as hardlink redirections rather than duplicate payloads.
- **Unix Permissions & Ownership** (`-ow`): Preserves and restores Unix UID,
  GID, user name, group name, and file permission bits with unprivileged
  `lchown`/`chmod` fallbacks.
- **SFX In-Place Conversion** (`s`): Converts archives to self-extracting
  executables using default or custom SFX stubs (`openrar s archive.rar`).
- **Recovery Record CLI Parity (`rr[N]`) & Switch Parity**:
  - Native support for the `rr[N]` command (e.g. `openrar rr5% arc.rar`).
  - Support for applying `-rr` and `-k` on existing archives without requiring
    file arguments.
  - Case-insensitive acceptance of WinRAR switches (`-qo`, `-qo+`, `-qo-`,
    `-am`, `-ams`).
- **Architect Skills**: Bundled `architect-challenge` and `architect-walkthrough`
  principal solution architect auditing workflows in `.agents/skills/`.

### Fixed

- **QO+RR Locator Size Mismatch in `add_recovery_record`**: Fixed a critical
  header corruption bug where adding a recovery record to a QO-enabled archive
  preserved the QO block verbatim, creating a 10-byte locator expansion that
  shifted all internal entry offsets and broke WinRAR validation ("Main archive
  header is corrupt"). `add_recovery_record` now cleanly strips QO blocks and
  sets `locator_qo_offset = -1`.
- **RAII Temporary File Lifecycle**: Introduced `TempFileCleanupGuard` in
  `RecoveryWriter::add_recovery_record`, guaranteeing that temporary files are
  closed and unlinked on any error, early return, or unwound exception.
- **Cryptographic Memory Scrubbing**: Added `openrar::crypto::secure_wipe`
  (`SecureZeroMemory` on Windows / `explicit_bzero` on POSIX) preventing dead-store
  compiler elimination, and equipped `Rar5Keys` and `HeaderCryptReader` with
  automatic RAII memory-scrubbing destructors.
- **Path Traversal & Device Name Containment**: Hardened extraction target
  checking with purely algorithmic lexical containment (`is_lexically_contained`)
  and protected `make_safe_component` against null bytes, control codes,
  forbidden Windows characters, and trailing-whitespace DOS device stems.
- **Performance & Mechanical Sympathy**: Pre-sized QO serialization buffers in
  `ArchiveMutator::write_batch_add_ex`, eliminating $O(N)$ reallocations for
  large archives.

## [1.7.0] - 2026-09-17

### Added

- **NTFS Alternate Data Streams (ADS) Archiving & Extraction** (`-os`): Full
  support for NTFS alternate data streams on Windows. Archiving enumerates and
  stores streams as child service records (`HFL_CHILD` | `HFL_INHERITED`)
  with `FHEXTRA_SUBBLOCK` headers matching WinRAR 5 format. Extraction restores
  named data streams to destination files. Portable stubs ensure graceful
  fallback and non-Windows compatibility.
- **NTFS Security Access Control Lists (ACL) Archiving & Extraction** (`-ow`):
  Full support for Windows security descriptors on Windows. Archiving captures
  owner, group, DACL, and SACL descriptors into inherited security records;
  extraction applies stored security descriptors to created files and
  directories via Win32 security APIs.
- **BufferArchive Checksum Verification**: In-memory archive extractions now
  verify checksums authoritatively against BLAKE2sp digests, and CRC32
  checksums are verified (properly respecting the 0-sentinel flag for zeroed
  CRCs).
- **Regression suites**: Added automated tests for NTFS streams and security
  metadata, BufferArchive CRC/BLAKE2sp corruption detection, switch parity
  validation, C-ABI ↔ JS/TS error code synchronization (`errorsync.tests.mjs`),
  corrupted compressed payload extraction rejection, and streaming-verify over
  tweaked-checksum archives.

### Fixed

- **Multi-volume Writer Partial-write Cleanup**: Added RAII `VolumeCleanupGuard`
  to track created volume files during multi-volume archive creation, safely
  removing any orphaned volume parts if writing fails or is interrupted.
- **Header Writer Flag Preservation**: `HeaderWriter` now preserves `HFL_CHILD`
  and `HFL_INHERITED` flags when writing file and service headers, ensuring
  child records properly maintain hierarchical relationships.
- **Verification Asymmetry Sweep**: The bool `extract_entry` path verified
  stored payloads but wrote compressed payloads without CRC/BLAKE2sp validation.
  All extract paths now enforce uniform hash policies (BLAKE2sp authoritative,
  else CRC32). Encrypted entries with tweaked checksums (`0x0002`) are accepted
  cleanly after password verification. CLI `t` verifies encrypted entries via
  streaming when a password is provided.
- **Recovery & Mutator Temp Files**: Temp files (`.rr_tmp`, `.rep_tmp`,
  `.rev_tmp`) now use unique counter-based names created with `CreateNew` to
  prevent symlink pre-plant truncation attacks. Mutator delete/lock rewrite
  loops and `.rev` writers are exception-safe and remove temporary files on
  failure.
- **Header & Memory Allocation Caps**: `openrar_archive_handle_info` enforces a
  16 MiB allocation cap before reading archive comments from crafted header
  lengths.
- **Fuzzing Harnesses & CI**: Fixed allocation leak of `handle_list` in
  `fuzz_archive`, removed duplicate `main()` definition in `fuzz_file_handle`
  under libFuzzer builds, enabled duration parameter in `fuzz.yml`, staged seed
  fixtures properly, and pinned the CI formatting gate to `clang-format-18`.

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
