# DLL Enhancement Plan — post-v1.1.0

**Status:** Approved & Frozen working specification. Phase 0 committed in `master` (pending v1.2.0 tag); Phases 1–3 planned for pickup (v1.3.0–v1.5.0)  
**Source:** Feature request from the Crate project (Windows desktop archiver, Hybrid D consumer, pinned `v1.1.0`), evaluated against the tree at `v1.1.0` + the landed Phase 0 work (`commit d680d4f`).  
**Rules that bind every phase:** Additive exports only — no `OPENRAR_DLL_API_VERSION` bump, no change to the 64-byte `openrar_archive_entry_t`, no behavior change to any existing export (`docs/versioning.md`, `docs/dll-integration-spec.md` §3). New capability is negotiated with `openrar_abi_features()` bits + `GetProcAddress`.

---

## 0. Architecture context & Crate request catalog

### 0.1 Architecture context (why the plan looks like this)

The DLL today is a shell over the MVP core. All archive exports forward to `BufferArchive` (`src/archive/buffer_archive.cpp`), which rejects encrypted data, solid, multi-volume and recovery archives; the file helpers slurp the whole archive into RAM per call (`read_file_bytes` in `src/dll/dll_api.cpp:643`, used by `openrar_archive_extract_file` at `dll_api.cpp:682`). Meanwhile the full engine exists and is exercised only by the CLI:

| Capability | Engine home | DLL reachability today |
|---|---|---|
| Passwords (file + header encryption, constant-time PswCheck) | `ArchiveReader` (`src/archive/archive_reader.hpp:20`) | none (listing half landed in Phase 0) |
| Scan-once file reader, solid chain state, volume stitching | `ArchiveReader::open` / `scan_archive` (`archive_reader.cpp:141`) | none |
| Streaming output hook (progress/cancel attachment point) | `Decompressor50::decompress(..., flush_cb)` (`decompressor50.hpp:128`) | none |
| Streaming integrity test (64 KiB chunks, no materialization) | `ArchiveReader::test_entry` (`archive_reader.cpp:539`) | none |
| Atomic add/delete (temp + replace, QO strip, solid continuation) | `ArchiveMutator` (`src/archive/archive_mutator.hpp:21`) | none |
| Full entry metadata (FILETIMEs, attrs, host OS, redir, win size) | `format::FileBlock` (`src/format/headers.hpp:146`) | none — dropped at the 64-byte boundary |

One structural decision follows: **give the DLL a file-mode handle backed by `ArchiveReader`.** That single export family answers Crate requests 1 (extraction half), 2, 3, 5, most of 6, and 7. Mutation (request 4) is a separate family over `ArchiveMutator`.

### 0.2 Crate feature request catalog

The Crate project submitted 7 distinct feature requests for native archive integration. For an outside reviewer, these requests map to the plan as follows:

| # | Request Name | Motivation & Problem Statement | Target Phase | Delivering API Surface |
|---|---|---|---|---|
| **1** | **Password Support** | Encrypted archives cannot be listed or extracted via DLL; callers need prompt flow for `-hp` and data decryption for `-p` / `-hp`. | Phase 0 (listing)<br>Phase 1 (extract) | `openrar_archive_list_file_pw`<br>`openrar_archive_open_file(..., password_utf8)` |
| **2** | **Persistent File Handle** | File helpers slurp archives into RAM on every call; re-scanning multi-GB files for every entry extract causes severe I/O and memory thrashing. | Phase 1 | `openrar_archive_open_file`<br>`openrar_archive_close`<br>`openrar_archive_handle_list` |
| **3** | **Streaming Extract to Path** | Callers need direct disk extraction with byte-level progress reporting, cancellation, and atomic durability on abort/failure. | Phase 1 | `openrar_archive_handle_extract_to_path` |
| **4** | **Archive Mutation** | Desktop archiver needs atomic file deletion and append/update capabilities without shelling out to external processes. | Phase 2 | `openrar_archive_delete_entries_file`<br>`openrar_archive_add_files_file`<br>`RAR_ERR_BUSY (-14)` |
| **5** | **Streaming Integrity Test** | Verification must test CRC32 / BLAKE2sp integrity without retaining decompressed output in RAM, including encrypted payloads. | Phase 1 (plain & crypto) | `openrar_archive_handle_test` |
| **6** | **Extended Metadata** | The 64-byte entry struct drops Windows FILETIMEs (ctime/mtime/atime), host OS attributes, dictionary size, and symlink/junction targets. | Phase 3 | `openrar_entry_ex_t`<br>`openrar_archive_handle_entry_ex`<br>`openrar_archive_handle_info` |
| **7** | **Multi-Volume Contract** | Ambiguity regarding multi-volume archive support, missing volume handling, and extent stitching behavior in the DLL. | Phase 1 | Transparent extent stitching<br>`RAR_ERR_MISSING_VOLUME (-13)` |

---

## 1. Phase 0 — land the in-flight work → v1.2.0

**Status:** Merged to `master` in commit `d680d4f` (awaiting release tagging of `v1.2.0`).

Implemented, tested, and validated:
- `openrar_archive_list_file_pw` — streaming listing with header decryption (`-hp`), `RAR_ERR_ENCRYPTED` / `RAR_ERR_BAD_PASSWORD` distinction, encrypted file entries reported with `is_encrypted = 1` (`emit_encrypted_entries` mode in `list_file_stream`).
- `openrar_archive_open_ex` — progress/cancel over the buffer handle's open-time scan.
- Feature bits `OPENRAR_ABI_FEATURE_LIST_PASSWORD (1ull << 1)`, `OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS (1ull << 2)`; spec §6.10, wrapper overloads, unit tests in `tests/unit/dll_tests.cpp` / `dll_cpp_tests.cpp` / `buffer_archive_tests.cpp`. *(Note: `OPENRAR_ABI_FEATURE_LIST_PROGRESS (1ull << 0)` shipped in v1.1.0).*

**Communication to Crate:** Ship as v1.2.0. This completes the *prompt flow* for encrypted archives (list → `RAR_ERR_ENCRYPTED` → prompt → re-list with password) but **not** extraction of encrypted entries — say so to Crate so they do not ship `set_password` against v1.2.0 for extraction.

---

## 2. Phase 1 — file-mode handle on `ArchiveReader` → v1.3.0 (keystone)

New feature bit: `OPENRAR_ABI_FEATURE_FILE_HANDLE (1ull << 3)`.

### 2.1 Exports

```c
// Scan-once open of an archive on disk. Keeps the file open for the handle's
// lifetime. password_utf8 may be NULL (needed up front for -hp archives:
// headers are unreadable without it). Progress/cancel cover the open-time
// scan (byte semantics like open_ex). Either callback may be NULL (NULL progress
// is a no-op; NULL cancel never cancels).
// Returns 0 on failure, detail via openrar_archive_get_error; a cancelled scan
// returns 0 with "open aborted". Multiple concurrent read-only open_file handles
// on the same path are permitted (FILE_SHARE_READ).
OPENRAR_DLL_API uint32_t OPENRAR_DLL_CALL
openrar_archive_open_file(const char* arc_path, const char* password_utf8,
                          openrar_progress_cb progress,
                          openrar_cancel_cb cancel, void* user);

// Existing exports become polymorphic over handle kind (same signatures):
//   openrar_archive_handle_list       — packs the reader's cached entries
//   openrar_archive_handle_extract    — entry to a heap buffer (previews)
//   openrar_archive_close             — closes the reader + file
//
// Behavior splits for existing exports on file-backed handles:
//   - openrar_archive_handle_extract: has no progress/cancel parameters (frozen ABI).
//     On file-backed handles only, capped at MAX_HEAP_EXTRACT_SIZE (256 MiB); larger
//     entries return RAR_ERR_NOMEM. Buffer handles retain their historical behavior
//     without this cap. Hosts should check entry.size and use handle_extract_to_path
//     for large entries. Because openrar_archive_handle_extract lacks a cancel callback,
//     solid catch-up runs uncancellably to completion; hosts requiring cancellation
//     during solid decompression must use openrar_archive_handle_extract_to_path.
//   - openrar_archive_handle_extract_all: returns RAR_ERR_UNSUPPORTED_FEATURE on file
//     handles; slurping multi-GB archives into one flat RAM buffer is prohibited.
//     Callers must extract per-entry via handle_extract_to_path.

// To-path extraction with byte progress + cancel. DLL owns durability:
// writes dest_path + ".openrar-tmp.<pid>.<seq>", flushes buffers, and atomically
// renames on success; abort/failure deletes the temp and leaves dest_path untouched.
// progress/cancel may be NULL.
// Directory entries on extract_to_path: creates the directory on disk (if not existing)
// via create_directories and returns RAR_OK; if progress is non-NULL, fires exactly
// one final progress(user, 0, 0) callback on success.
// Directory entries on test: trivially returns RAR_OK without callbacks.
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_handle_extract_to_path(uint32_t handle, uint32_t entry_index,
                                       const char* dest_path,
                                       openrar_progress_cb progress,
                                       openrar_cancel_cb cancel, void* user);

// Streaming integrity test: CRC32 / BLAKE2sp verified without retaining
// decompressed output in RAM. Uses chunked AES-256-CBC decrypt for encrypted
// entries, streaming stored entries in 64 KiB chunks and compressed entries in
// dictionary window chunks. progress/cancel may be NULL.
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_handle_test(uint32_t handle, uint32_t entry_index,
                            openrar_progress_cb progress,
                            openrar_cancel_cb cancel, void* user);
```

### 2.2 Handle table polymorphism & architecture

In `src/dll/dll_api.cpp`, the global `HandleTable` currently stores a concrete `ArchiveHandle` struct tied to `BufferArchive`:
```cpp
struct ArchiveHandle {
    std::vector<uint8_t> data;
    openrar::archive::BufferArchive ba;
    std::vector<openrar::archive::BufferArchiveEntry> entries;
};
```
To support polymorphism without breaking `HandleTable<T>::pin` lifetime guarantees (L12):
1. Introduce an abstract base `ArchiveHandleBase` in `dll_api.cpp`:
   ```cpp
   struct ArchiveHandleBase {
       virtual ~ArchiveHandleBase() = default;
       virtual const std::filesystem::path& path() const = 0; // empty for buffer handles
       virtual int list(uint32_t* count, void** entries_out, void** paths_out, size_t* paths_size_out) = 0;
       virtual int extract(uint32_t entry_index, uint8_t** out_ptr, size_t* out_len) = 0;
       virtual int extract_all(uint8_t** buf_out_ptr, size_t* buf_size_out,
                               uint64_t** offsets_out_ptr, uint32_t* offsets_count_out) = 0;
       virtual int extract_to_path(uint32_t entry_index, const char* dest_path,
                                   openrar_progress_cb progress, openrar_cancel_cb cancel, void* user) = 0;
       virtual int test(uint32_t entry_index, openrar_progress_cb progress,
                        openrar_cancel_cb cancel, void* user) = 0;
   };
   ```
2. Implement `BufferArchiveHandle : ArchiveHandleBase`: existing in-memory behavior; `extract` retains unrestricted allocation behavior; `test` returns `RAR_ERR_UNSUPPORTED_FEATURE` (no buffer test path exists); `extract_to_path` delegates to buffer extraction followed by disk write (new export, no frozen-behavior concern).
3. Implement `FileArchiveHandle : ArchiveHandleBase` backed by `std::unique_ptr<openrar::archive::ArchiveReader>`.
   - Holds open `io::FileStream` for the primary volume (`.part01.rar` / `.rar`) and cached entries.
   - `extract` enforces the `MAX_HEAP_EXTRACT_SIZE (256 MiB)` cap.
   - `extract_all` returns `RAR_ERR_UNSUPPORTED_FEATURE`.
   - Returns canonical archive path via `path()` for mutator collision pre-checks.
4. Existing dispatchers (`openrar_archive_handle_list`, `openrar_archive_handle_extract`, `openrar_archive_handle_extract_all`, `openrar_archive_close`) forward polymorphically through `ArchiveHandleBase`.

### 2.3 Engine work required

- **Chunked AES-256-CBC decryption (pulled forward from Phase 3):**
  - *Problem:* `ArchiveReader::extract_entry` currently buffers the entire ciphertext into RAM (`archive_reader.cpp:998`). For multi-GB encrypted files, this breaches memory budgets and causes OOM crashes.
  - *Solution:* Implement chunked AES-256-CBC decryption in slices $\le 256\text{ KiB}$. The CBC IV is carried across successive slices: each slice's final 16 bytes of ciphertext become the next slice's IV.
  - *Unified pipeline:* Feeds directly into `Decompressor50::decompress` (for compressed entries) or disk write/CRC/BLAKE2sp (for stored entries).
  - *Integrity test parity:* Enables `handle_test` to test encrypted entries immediately in Phase 1 with fixed RAM, eliminating the "test is weaker than extract" gap.
- **Progress/cancel on scan:** `ArchiveReader::scan_archive` currently has no callback hooks. Thread `progress` and `cancel` through the header-block loop and across multi-volume set opens (`open_file`).
- **In-memory extract for `ArchiveReader`:** Add an in-memory extraction method (`ArchiveReader::extract_entry_to_memory`) using a memory buffer sink. Enforce `MAX_HEAP_EXTRACT_SIZE = 256 * 1024 * 1024` (256 MiB) on file handles only; return `RAR_ERR_NOMEM` if `entry.size` exceeds this limit.
- **Solid catch-up mechanics:**
  - *State tracking:* In `ArchiveReader`, track `last_decoded_entry_index_` (initialized to -1) and `solid_run_start_index_`. `last_decoded_entry_index_` is updated *only* upon successful, complete extraction of an entry.
  - *Sequential fast path:* If requested entry index $K == \text{last\_decoded\_entry\_index\_} + 1$ and `solid_chain_ok_`, continue decompression directly without restarting.
  - *Catch-up rewind path:* If $K \le \text{last\_decoded\_entry\_index\_}$ or `!solid_chain_ok_`:
    1. Scan backwards from $K$ to locate the beginning of the solid block (the nearest preceding entry with `!header.is_solid`, or the start of the archive).
    2. Reset `solid_unpacker_` and seek `stream_` to the solid block start.
    3. Decode intermediate entries from block start up to $K-1$ using a discard sink (discarding decompressed bytes, updating LZ history).
    4. Decode entry $K$ into the output destination.
  - *Progress & cancel during catch-up:*
    - An initial `progress(user, 0, entry.size)` callback is fired immediately when catch-up begins so host UIs can reset progress meters and indicate work in progress.
    - Cancel is polled per flush chunk during discard-sink decoding. If non-zero, catch-up aborts immediately with `RAR_ERR_ABORTED`.
    - Progress callback convention: during discard-sink catch-up, `progress(user, 0, entry.size)` is fired periodically (reporting 0 bytes produced towards the target entry). Once entry $K$ decompression begins, `done` advances monotonically from $0 \to \text{entry.size}$.
    - Latency guidance: catch-up cost is $O(\text{solid-run-prefix})$; document that hosts should extract entries in ascending index order for maximum performance.
  - *Error handling & handle usability:*
    - If any intermediate entry in the solid run fails decompression, password check, CRC, or is encrypted with no password supplied, catch-up terminates immediately, invalidates `solid_chain_ok_`, and returns `RAR_ERR_CRC_MISMATCH`, `RAR_ERR_BAD_PASSWORD`, or `RAR_ERR_ENCRYPTED`.
    - *Post-abort handle usability:* The handle remains completely valid and usable after any abort or error. Because `last_decoded_entry_index_` was not advanced to $K$, a subsequent extraction request will cleanly re-run catch-up from the solid block start.
- **Multi-volume extent stitching & missing volume error (pulled forward from Phase 3):**
  - Add additive error code `RAR_ERR_MISSING_VOLUME = -13`:
    ```c
    enum RarError {
        ...
        RAR_ERR_ENCRYPTED = -12,
        RAR_ERR_MISSING_VOLUME = -13
    };
    ```
  - Synchronize across `src/archive/buffer_archive.hpp`, `src/api/abi_contract.hpp`, `src/dll/openrar_dll.h`, `src/wasm/archive_api.hpp`, and integration specs.
  - `open_file` path acceptance: accepts the first volume (`.part01.rar`, `.part1.rar`, or `.rar` in legacy numbering). If a middle volume path is passed, `open_file` uses `derive_first_volume_name` to locate part 1; if part 1 cannot be found or opened, it returns 0 with error code `RAR_ERR_MISSING_VOLUME` and detail `"cannot open first volume: archive.part01.rar"`.
  - Secondary volume lifetime: primary volume (`.part01.rar`) is kept continuously open by the handle. Secondary volumes (`.part02.rar`, etc.) are opened on demand *per access* during scan or extent extraction and closed immediately upon completing reads for that volume. This avoids descriptor exhaustion and permits extract-time missing-volume testing on platforms without mandatory file locking.
  - Unified index space & CRC: entries spanning volume boundaries (`SPLIT_BEFORE` / `SPLIT_AFTER`) are merged into a single logical entry in `handle_list` with summed packed extents and full uncompressed size. The reported `crc32` is the whole-file CRC (carried in the terminating extent block where `SPLIT_AFTER` is 0).
  - Missing volume handling: if a volume file in the extent chain cannot be opened at open time or during extraction across volume boundaries, the operation aborts with `RAR_ERR_MISSING_VOLUME`, and `openrar_archive_get_error` formats the missing path (e.g. `"missing volume: archive.part02.rar"`).
- **Entry mapping:** Map internal `ArchiveEntry` to the 64-byte `openrar_archive_entry_t` (`path_offset`, `path_len`, `is_dir`, `method`, `is_encrypted`, `crc32`, `size`, `packed_size` = sum of extent sizes, `mtime` = Unix seconds).

### 2.4 Contract decisions

- **Password lifetime & security:**
  - `password_utf8` is passed to `open_file` and stored internally in `ArchiveReader::password_`.
  - On `openrar_archive_close`, internal password memory (`password_`), derived PBKDF2 key buffers, and AES key schedules are explicitly zeroized (`SecureZeroMemory` / `explicit_bzero`) prior to deallocation.
  - Password is never retrievable via any API.
  - A password supplied for an unencrypted archive is silently ignored (not an error).
  - Header corruption caveat: on `-hp` archives, structural corruption can cause header CRC or PswCheck failure; this will present as `RAR_ERR_BAD_PASSWORD`.
- **Error mapping:**
  - `open_file` failure: returns 0. If headers are encrypted and no password was provided, `openrar_archive_get_error` contains `"archive headers are encrypted (password required to list)"` (`RAR_ERR_ENCRYPTED`). If the password is wrong, error contains `"wrong password for encrypted headers"` (`RAR_ERR_BAD_PASSWORD`). If cancelled, error contains `"open aborted"` (`RAR_ERR_ABORTED`). If first volume cannot be found or opened, error is `RAR_ERR_MISSING_VOLUME` (`"cannot open first volume: archive.part01.rar"`).
  - `handle_extract_to_path` / `handle_test`: return numeric `enum RarError`.
    - `RAR_ERR_BAD_PASSWORD` on PswCheck mismatch (never `CRC_MISMATCH` for a wrong password).
    - `RAR_ERR_ENCRYPTED` when an encrypted entry is extracted/tested with no password supplied.
    - `RAR_ERR_CRC_MISMATCH` for genuine checksum failure.
    - `RAR_ERR_ABORTED` on cancel with no partial destination.
    - `RAR_ERR_MISSING_VOLUME` when an extent volume is absent.
    - When a file header carries no PswCheck record, wrong passwords can only surface as decode or CRC failure — document this format limitation explicitly.
- **Progress semantics:** $(done, total) = (\text{uncompressed bytes produced}, entry.size)$. Monotonic, cumulative, exactly one final $(total, total)$ callback on success. No final callback fires on abort or failure. Total = 0 allowed for empty entries and `FHFL_UNPUNKNOWN` entries (done counts bytes, no denominator). For directory entries (`is_dir == 1`, `entry.size == 0`), no intermediate data callbacks fire; if `progress` is non-NULL, exactly one final $(0, 0)$ callback is fired on success. Cancel polled per flush chunk (dictionary window granularity for compressed entries, $\le 64\text{ KiB}$ for stored). Cancel returned after atomic rename is committed is ignored.
  - *Cancel polling bound:* On foreign archives with 32 MB dictionaries, cancel is polled per window chunk ($\le 32\text{ MiB}$ decode), typically representing tens of ms on modern hardware (scales with dictionary window size and storage speed).
- **Durability & temp files:**
  - Destination guard: if `dest_path` resolves to the archive file itself or any volume file in the active volume set, the operation fails immediately with `RAR_ERR_INVALID_ARG` (detail: `"destination path cannot be the archive file"`), preventing self-clobber.
  - Path pattern: `dest_path + ".openrar-tmp.<pid>.<seq>"` located in the destination directory to guarantee atomic single-volume filesystem rename.
  - Open temporary file with `CREATE_NEW` (`O_CREAT | O_EXCL`) to prevent collisions, symlink attacks, and predictable file reuse. If `CREATE_NEW` fails with `EEXIST` (e.g. from PID reuse colliding with an orphaned temp), the DLL retries with incrementing `seq` up to 10 times before returning `RAR_ERR_IO`.
  - Safely create parent directories: call `std::filesystem::create_directories(dest.parent_path())` only when `dest.has_parent_path() && !dest.parent_path().empty()`, preventing throws on relative paths.
  - Data flush & atomic rename: call `FlushFileBuffers` (Windows) / `fsync` (POSIX) on the temp file descriptor prior to closing; execute atomic rename (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` on Windows, `rename()` on POSIX); on POSIX, fsync the parent directory after rename.
  - Abort or error immediately closes and deletes the temporary file, leaving `dest_path` untouched.
  - Orphan cleanup: document the `.openrar-tmp.*` naming pattern so hosts can sweep abandoned temp files from interrupted processes.
  - The frozen `openrar_archive_extract_file_to_path` keeps its current direct-write behavior.
- **Memory footprint:**
  - RAM usage during extraction is bounded by $O(\text{dictionary window} + \text{slice buffers})$, independent of entry size.
  - For stored entries (plain or encrypted): bounded by the $\le 256\text{ KiB}$ slice buffer $+ 64\text{ KiB}$ I/O buffer ($\le 1\text{ MiB}$ total process working set increase).
  - For compressed entries (plain or encrypted): bounded by the dictionary window buffer ($\le 32\text{ MiB}$ for RAR5; $\le 64\text{ MiB}$ for RAR7) $+ \le 256\text{ KiB}$ slice buffer, strictly constant regardless of uncompressed entry size.
  - An idle open file handle maintains the open file descriptor and cached entries (~few KB per 1,000 entries).
- **File lifetime, staleness & share mode:**
  - The handle holds the archive file open. On Windows, `open_file` opens the primary volume with `FILE_SHARE_READ` (denying write sharing). External mutators attempting to open the archive for writing will fail immediately with sharing violation (`ERROR_SHARING_VIOLATION`), preventing concurrent on-disk corruption while a handle is active.
  - Hosts must close all open handles before renaming or deleting any archive file in the volume set.
  - If the archive is modified on disk under an open handle (e.g. by external processes on POSIX or without mandatory locking), subsequent handle reads fail with `RAR_ERR_IO`; handles become stale by definition and hosts must close and re-open.
  - In-process handle collision with the DLL's own mutation exports is caught and rejected up front via `RAR_ERR_BUSY (-14)`.
- **Threading:** One thread per handle at a time (matches existing handle contract; the reader's solid state is sequential). Callbacks run on the calling thread with no DLL lock held (`HandleTable::pin` guarantees this), and must not call back into the same handle.

### 2.5 Tests

- **Fixtures:** `tests/hello5_p.rar`, `tests/hello5_hp.rar`; generated solid archives via `ArchiveMutator::write_batch_add(solid=true)`; multi-volume sets via `add_file_to_archive_vol`; and stored entries > 64 MiB.
- **Contract tests:**
  - Progress monotonicity and final $(total, total)$ callback; assert no final callback on abort.
  - Cancel callback returning 1 during extraction aborts with `RAR_ERR_ABORTED` and ensures no temporary or partial file remains at `dest_path`.
  - Cancel callback during solid discard-sink catch-up aborts immediately with `RAR_ERR_ABORTED`.
  - Password tests: wrong password returns `RAR_ERR_BAD_PASSWORD` for `-hp` at open and for `-p` at extract; missing password returns `RAR_ERR_ENCRYPTED`.
  - Solid catch-up: verify out-of-order extraction produces byte-identical output to in-order extraction; verify repeated extraction of the same entry succeeds.
  - Chunked AES decrypt RAM ceiling:
    - For 100 MiB encrypted stored files: assert peak working set increase is $\le 1\text{ MiB}$.
    - For 100 MiB encrypted compressed files: assert peak working set increase is $\le \text{window\_size} + 2\text{ MiB}$, and identical between 10 MiB and 100 MiB extractions.
  - Multi-volume: verify 2-volume archive extract stitches across volumes; missing second volume at open or extract returns `RAR_ERR_MISSING_VOLUME` with volume path in error string.
  - Fault injection: destination locked by external process causes rename failure $\to$ temp file cleaned up, returns `RAR_ERR_IO`.

---

## 3. Phase 2 — mutation exports → v1.4.0

New feature bit: `OPENRAR_ABI_FEATURE_MUTATION (1ull << 4)`. Free functions over `ArchiveMutator` — **not** handle-based (see non-goals).

### 3.1 Exports

```c
// Delete entries by index (indices refer to a fresh listing order).
// count must be > 0. Rejects mutations on locked, multi-volume, or entries inside
// a solid block where deletion would orphan subsequent solid entries.
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_delete_entries_file(const char* arc_path,
                                    const uint32_t* entry_indices,
                                    uint32_t count);

// Append/replace files, 'u' semantics: incoming names override same-name
// entries in non-solid chains.
// method ∈ {0,3,5}. For method ∈ {3,5}, window_log2 ∈ [1,4]; for method == 0,
// window_log2 is ignored (accepts [0,4]; > 4 returns RAR_ERR_INVALID_ARG).
// If a path in src_paths is a directory, it is added as a directory record (non-recursive).
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_add_files_file(const char* arc_path,
                               const char* const* src_paths,
                               const char* const* arc_names,
                               uint32_t file_count, int method,
                               uint32_t window_log2);
```

### 3.2 Engine work & contract rules

- **In-DLL handle sharing collision check (`RAR_ERR_BUSY = -14`):**
  - Add additive error code `RAR_ERR_BUSY = -14` to `openrar_dll.h`, `buffer_archive.hpp`, `abi_contract.hpp`, `archive_api.hpp`, and documentation:
    ```c
    enum RarError {
        ...
        RAR_ERR_MISSING_VOLUME = -13,
        RAR_ERR_BUSY = -14
    };
    ```
  - Before modifying `arc_path`, verify `arc_path` exists on disk using `std::filesystem::exists(arc_path, ec)` with `std::error_code` (if `!exists || ec`, return `RAR_ERR_IO`).
  - Resolve path using `std::filesystem::weakly_canonical(arc_path, ec)` with `std::error_code` (if `ec`, return `RAR_ERR_IO`) to prevent uncaught exceptions.
  - Inspect `g_handles` for any open `FileArchiveHandle` matching the canonical path.
  - If an active handle is detected, the call fails fast with `RAR_ERR_BUSY (-14)` (detail: `"archive is currently open in a handle; close it before mutating"`), avoiding Windows sharing violations during rewrite-under-open.
  - *TOCTOU coordination:* Callers must coordinate handle opening and mutation sequentially per archive path; concurrent handle creation during mutation is a host protocol violation.
- **Core addition in `ArchiveMutator`:**
  - Today, `ArchiveMutator::delete_entries` (`archive_mutator.cpp:255`) is wildcard-mask based (`io::wildcard_match`). Translation through index $\to$ name $\to$ mask breaks on filenames containing wildcards (`*`, `?`).
  - Add `ArchiveMutator::delete_entries_by_index`: matches entries by identity (header offset / index in fresh listing). Listing order matches `openrar_archive_list_file` and `openrar_archive_handle_list` identically.
  - If an index in `entry_indices` is $\ge \text{entry\_count}$, fail with `RAR_ERR_INVALID_ARG` with detail `"entry index N out of range (archive has M entries)"`.
  - Share the rewrite and QuickOpen (QO) strip paths with the existing mutator.
- **Solid mutation rules (solid block head & dependency invariants):**
  - *Full removal set invariant check:* The invariant check runs over the union of ALL indices removed or replaced by the operation (every name-matched instance across the archive, plus every explicitly specified index). If ANY removed index violates the solid dependency invariant, the entire operation is rejected with `RAR_ERR_UNSUPPORTED_FEATURE`.
  - *Dependency invariant:* In RAR5, solid decompression of entry $S$ depends on the decoder state accumulated from its block head $H(S)$ (the nearest preceding entry with `!is_solid`, or the start of the archive) through every entry in $[H(S), S)$. Although $H(S)$ carries `is_solid = 0`, it is the foundation of the solid block: deleting $H(S)$ or modifying/replacing $H(S)$ corrupts the end-of-stream decoder state that $S$ chains from.
  - *Core invariant:* **For every retained solid entry $S$ with block head $H(S)$, no deleted or replaced index may lie in $[H(S), S)$.**
  - Equivalently, for every solid block in the archive:
    1. Leave the solid block completely untouched.
    2. Delete a **suffix** of the solid block (e.g. retain $[H(S), \dots, S]$, delete all subsequent entries $[S+1, \dots]$ in that solid run).
    3. Delete the solid block **entirely**, head $H(S)$ included (every member of the block is deleted simultaneously).
  - *Deletion contract:* Deleting the block head $H(S)$ while leaving any later entry in that solid block retained is rejected with `RAR_ERR_UNSUPPORTED_FEATURE` (detail: `"cannot delete head of solid block without recompressing chain"`). Deleting a mid-block entry while leaving later entries retained is rejected with `RAR_ERR_UNSUPPORTED_FEATURE` (detail: `"cannot delete entries from solid archive without recompressing chain"`).
  - *Replacement contract ("u" semantics):* Replacing any entry inside an active solid block (including the block head $H(S)$) is strictly rejected with `RAR_ERR_UNSUPPORTED_FEATURE` (detail: `"cannot replace entry in solid archive without recompressing chain"`). Replacing truly independent non-solid entries (entries that do not serve as a block head for any subsequent solid entries) is permitted.
  - *Replace-vs-delete+add asymmetry:* Replacing the *last* solid entry in a block is rejected by the replacement contract, but can be achieved via suffix-deletion (permitted) followed by append-add. This asymmetry is intentional: replacement strips historical entries by name, whereas deletion explicitly verifies index boundaries.
  - *Appending to solid archives:* Adding *new* entries (names not present in archive) continues the solid stream (`is_solid = 1`, continuing LZ stream as supported by `archive_mutator.cpp:765`).
  - *Non-solid archive replacement:* In non-solid runs, replacement matching is performed after `\` $\to$ `/` normalization, is byte-exact (no case-folding, UTF-8 binary compare), and strips *all* prior instances of matching names from the archive. QuickOpen is unconditionally stripped.
- **Window size in `add_files_file`:**
  - `ArchiveMutator::prepare_add_file` (`archive_mutator.cpp:508`) currently hard-codes dictionary size to 2 MiB (`fb.win_size = (method > 0) ? 0x200000u : 0`).
  - Update `prepare_add_file` to accept `window_log2`.
    - For compressed methods (`method ∈ {3, 5}`): `window_log2 ∈ [1, 4]` (128 KiB to 1 MiB). Passing 0 or $> 4$ returns `RAR_ERR_INVALID_ARG`.
    - For stored method (`method == 0`): `window_log2` is ignored, but must be $\le 4$ (`[0, 4]` accepted; $> 4$ returns `RAR_ERR_INVALID_ARG`).
  - *Decoder compatibility note:* `ArchiveReader` and `Decompressor50` support decoding any valid historical power-of-two window up to 32 MiB (64 MiB for RAR7), regardless of the creation limits of $[1, 4]$.
- **Directory handling (non-recursive):**
  - In `openrar_archive_add_files_file`, check each source path with `std::filesystem::is_directory`. If true, delegate to `ArchiveMutator::prepare_add_dir`, creating an `FHFL_DIRECTORY` record with zero uncompressed size and no data area.
  - *Non-recursive:* Directory adds do not traverse subdirectories. Callers must enumerate directories and files explicitly and pass the full list. Empty directories are preserved.
- **Arcname separator normalization:**
  - Archive format canonical separator is forward slash `/`. The DLL normalizes backslashes `\` to `/` in `arc_names` before header serialization.
- **QuickOpen (QO) handling:**
  - Both `delete_entries_file` and `add_files_file` strip existing `QO` service headers and reset locator flags in the main header (`mb.has_locator = false`), preventing stale seek locators.
- **Durability:**
  - Core-owned: write temp + `FlushFileBuffers` / `fsync` + atomic replace (`write_batch_add`). Original untouched on failure.
- **Encrypted archive boundary:**
  - Phase 2 mutation functions do not take a password parameter.
  - If `arc_path` has encrypted headers (`-hp`), mutation returns `RAR_ERR_UNSUPPORTED_FEATURE` (detail: `"mutating header-encrypted archive requires password"`).
  - Encrypting new files being added is deferred; added files are written unencrypted.
- **Error mapping:**
  - `RAR_ERR_INVALID_ARG`: `arc_path` null, `file_count == 0`, `count == 0`, `method` ∉ {0, 3, 5}, `window_log2` invalid, or `entry_indices[i] >= total_entries`.
  - `RAR_ERR_IO`: `arc_path` not found or unreadable, `src_paths[i]` does not exist, or filesystem write failure.
  - `RAR_ERR_BUSY (-14)`: archive is currently open in an active handle.
  - `RAR_ERR_UNSUPPORTED_FEATURE`: archive is locked (`MHFL_LOCK`), multi-volume (`MHFL_VOLUME`), header-encrypted (`-hp`), or solid deletion/replacement would orphan LZ state.
- **Handle interaction:** In-process handle collision is detected and rejected up front with `RAR_ERR_BUSY (-14)`. Across processes or if an external tool mutates the archive on disk, open handles become stale by definition and subsequent reads fail with `RAR_ERR_IO`; hosts must re-open after any mutation.

---

## 4. Phase 3 — metadata, archive info, nits → v1.5.0

### 4.1 Extended entry metadata (Crate request 6)

Feature bit: `OPENRAR_ABI_FEATURE_ENTRY_EX (1ull << 5)`. File handles only — fields come straight from `format::FileBlock` (`src/format/headers.hpp:146`); the 64-byte `openrar_archive_entry_t` and its WASM-reserved `_pad` remain frozen. Calling on a buffer handle returns `RAR_ERR_UNSUPPORTED_FEATURE`.

```c
// Bit flags for openrar_entry_ex_t.flags
#define OPENRAR_ENTRY_FLAG_SOLID         (1u << 0)
#define OPENRAR_ENTRY_FLAG_ENCRYPTED     (1u << 1)
#define OPENRAR_ENTRY_FLAG_REDIR         (1u << 2)
#define OPENRAR_ENTRY_FLAG_SPLIT_BEFORE  (1u << 3)
#define OPENRAR_ENTRY_FLAG_SPLIT_AFTER   (1u << 4)
#define OPENRAR_ENTRY_FLAG_HAS_MTIME     (1u << 5)
#define OPENRAR_ENTRY_FLAG_HAS_CTIME     (1u << 6)
#define OPENRAR_ENTRY_FLAG_HAS_ATIME     (1u << 7)
#define OPENRAR_ENTRY_FLAG_DIRECTORY     (1u << 8)

#pragma pack(push, 1)
typedef struct {
    uint32_t attrs;           // host-OS attributes (FILE_ATTRIBUTE_* when host_os == 0)
    uint32_t host_os;         // 0 = Windows, 1 = Unix
    uint64_t mtime_ft;        // FILETIME (UTC, 100ns); 0 = not stored (check HAS_MTIME)
    uint64_t ctime_ft;        // FILETIME (UTC, 100ns); 0 = not stored (check HAS_CTIME)
    uint64_t atime_ft;        // FILETIME (UTC, 100ns); 0 = not stored (check HAS_ATIME)
    uint32_t flags;           // Bitmask of OPENRAR_ENTRY_FLAG_*
    uint32_t win_size;        // dictionary bytes, 0 if n/a
    uint32_t redir_type;      // 0 none / 1 unixsymlink / 2 winsymlink / 3 junction / 4 hardlink / 5 filecopy
    uint32_t version_needed;  // unp_ver format level (0 = RAR5, 1 = RAR7)
} openrar_entry_ex_t;         // 48 bytes, fixed size
#pragma pack(pop)

// Query extended metadata for an entry.
// extra_out receives a malloc'd NUL-terminated UTF-8 string containing the
// redirection target path when redir_type != 0. If no extra data exists,
// *extra_out is NULL and *extra_size_out is 0.
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_handle_entry_ex(uint32_t handle, uint32_t entry_index,
                                openrar_entry_ex_t* out,
                                void** extra_out, size_t* extra_size_out);

// Free helper for extra_out (openrar_free is also valid).
OPENRAR_DLL_API void OPENRAR_DLL_CALL
openrar_archive_entry_ex_free(void* extra);
```

### 4.2 Archive-level metadata companion

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t flags;           // main-header flags (MHFL_VOLUME, MHFL_SOLID, MHFL_LOCK, etc.)
    uint32_t volume_index;    // 0-based volume index (0 if not multi-volume)
    uint32_t volume_count;    // total volumes if known, 0 otherwise (from locator/tail)
    uint64_t recovery_size;   // recovery record size in bytes, 0 if none / unparsed
    uint32_t comment_len;     // archive comment length in bytes, 0 if none
} openrar_archive_info_t;
#pragma pack(pop)

// Query archive-level properties. If comment_len > 0, comment_out receives a
// malloc'd UTF-8 string containing the archive comment. If calling on a buffer
// handle, returns RAR_ERR_UNSUPPORTED_FEATURE. On I/O read failure when reading
// lazy archive headers or comment payload, returns RAR_ERR_IO.
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_handle_info(uint32_t handle, openrar_archive_info_t* out,
                            void** comment_out, size_t* comment_size_out);
```

#### Engine work required for archive-level info

- **Lazy comment parsing (`CMT`):** The scan-time header pass records only the header offset and length of any `HEAD_SERVICE` where `service_type == "CMT"` or `file_name == "CMT"`. `handle_info` reads and decrypts the comment payload lazily on demand, avoiding upfront memory allocation. If unencrypted (or if encrypted and a valid password was supplied), reports `comment_len` and returns the payload. If encrypted comment decryption fails (e.g. password missing, wrong password, or corrupt service data), the comment is reported as absent (`comment_len = 0`, `*comment_out = NULL`) and the query succeeds with `RAR_OK` (comment decryption failure does not fail archive inspection).
- **Recovery record size (`RR`):** When `HEAD_SERVICE` with `service_type == "RR"` is encountered, record its `data_size` as `recovery_size` (0 if no recovery record exists).
- **Volume count provenance:** Set `volume_index = main_block_.vol_number`. Set `volume_count` to the total volume count *only* if determinable (e.g. all volumes present and scanned in chain); otherwise 0.
- **I/O error handling:** If reading the archive file fails during lazy header or comment inspection, `handle_info` returns `RAR_ERR_IO`.

### 4.3 Documentation nits (all four, one PR)

1. **Window size constants:** Add `OPENRAR_WINDOW_128K … OPENRAR_WINDOW_1M` named constants to `openrar_dll.h`. Explicitly document that `compress_block`'s `win_size` is in **bytes** whereas `CreateOptions.window_log2` is **$\log_2$** ($1 \to 128\text{ KiB}, \dots, 4 \to 1\text{ MiB}$).
2. **Thread-local errors:** Document explicitly in `openrar_dll.h` that `openrar_archive_get_error` and `openrar_last_error` report `thread_local` state.
3. **`RAR_ERR_PARTIAL_OK`:** Document verbatim in the public header and spec that `RAR_ERR_PARTIAL_OK (1)` is returned solely by `extract_all` when some entries succeed and others fail; single-entry extraction can never return it.
4. **Feature-bit registry & naming conventions:**
   - Centralized feature-bit registry comment block in `openrar_dll.h` listing all assigned bits (`1ull << 0` through `1ull << 5`).
   - Reserve convention: future options on file opening (e.g. codepage override, custom volume search callbacks) will be introduced as `openrar_archive_open_file_ex`.

---

## 5. Non-goals / pushed-back designs

| Proposal | Why not | Instead |
|---|---|---|
| Per-export `_pw` variants (`extract_file_pw`, `extract_file_to_path_pw`, …) | Combinatorial explosion of exports; password-as-parameter has no session home | Password is an `open_file` parameter; the lone `list_file_pw` (Phase 0) serves the frozen pre-handle prompt flow |
| Handle-based mutation (`handle_delete`/`handle_add`) | The mutator rewrites the archive on disk (temp + atomic replace) — invalidates the handle's open stream and cached indices, and collides with Windows sharing violations on rewrite-under-open | Free-function exports; hosts re-open (Crate already does) |
| Free-function `extract_file_to_path_ex` / `extract_file_ex` | Keeps the per-call slurp + re-walk tax that request 3 exists to remove | `handle_extract_to_path` with progress/cancel |
| Standalone `verify_password` | Redundant for `-hp` (open_file fails fast); eager PBKDF2 for `-p` is a DoS vector | Lazy verification at extract/test via constant-time PswCheck |
| Fixing the frozen file exports' slurp (routing through the streaming engine) | Would silently change `HEAD_CRYPT` error codes (`UNSUPPORTED_FEATURE` $\to$ `ENCRYPTED`), breaking the frozen-semantics promise | Accepted, documented debt; the handle is the good path |
| Password/extract upgrades to the in-memory buffer surface | WASM-parity frozen MVP; no host need identified (Crate reads from disk) | Buffer surface stays as-is |
| `test_all` export | Trivially driven per-entry by the host loop | Skip |
| Widening the 64-byte entry struct or using `_pad` | `_pad` is reserved by the shared WASM contract ("consumers must zero") | Additive `entry_ex` query |
| `openrar_archive_handle_extract_all` for file handles | Decompressing multi-GB archives into a single contiguous RAM buffer invites OOM | Returns `RAR_ERR_UNSUPPORTED_FEATURE`; use per-entry extract |

---

## 6. Versioning, feature bits & communication roadmap

### 6.1 Release roadmap & feature bits

| OpenRAR Version | Phase | Feature Bits Added | Delivered Capabilities |
|---|---|---|---|
| **v1.2.0** | Phase 0 | `OPENRAR_ABI_FEATURE_LIST_PASSWORD (1ull << 1)`<br>`OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS (1ull << 2)` | Password listing (`list_file_pw`), open callbacks (`open_ex`). *(Note: `LIST_PROGRESS (1ull << 0)` shipped in v1.1.0)* |
| **v1.3.0** | Phase 1 | `OPENRAR_ABI_FEATURE_FILE_HANDLE (1ull << 3)` | Persistent file handle (`open_file`), streaming extract to path with progress/cancel (`handle_extract_to_path`), chunked AES-256-CBC decrypt, streaming integrity test (`handle_test`), multi-volume extent stitching, `RAR_ERR_MISSING_VOLUME (-13)` |
| **v1.4.0** | Phase 2 | `OPENRAR_ABI_FEATURE_MUTATION (1ull << 4)` | Atomic deletion (`delete_entries_file`), append/replace files (`add_files_file`), handle collision check (`RAR_ERR_BUSY = -14`) |
| **v1.5.0** | Phase 3 | `OPENRAR_ABI_FEATURE_ENTRY_EX (1ull << 5)` | Extended metadata (`entry_ex`, `handle_info`), documentation cleanup & window constants |

- `OPENRAR_DLL_API_VERSION` remains `1` across all phases.
- Each release updates `src/dll/openrar_dll.h` (declarations + bit registry), `include/openrar/openrar.hpp` (C++ wrapper overloads), `docs/dll-integration-spec.md`, and `CHANGELOG.md`.

### 6.2 Communication to Crate

Reply to Crate with this feature-to-release mapping:
1. **v1.2.0 (now):** Enables password prompt flow for `-hp` archives via `openrar_archive_list_file_pw`. Note: extraction of encrypted entries is gated on v1.3.0.
2. **v1.3.0 (keystone):** Delivers requests #1 (extraction), #2 (persistent file handle), #3 (streaming extract to disk with progress/cancel), #5 (integrity test with chunked decrypt), and #7 (multi-volume stitching and missing volume handling). Includes all contract answers requested:
   - *Durability on abort:* DLL-owned temp-and-rename (`.openrar-tmp.<pid>.<seq>`) with `FlushFileBuffers` / `fsync`; partial files are never left behind.
   - *Solid access:* Automatic catch-up decompressing intermediate solid runs; in-order extraction remains the zero-overhead fast path.
   - *Encrypted memory ceiling:* Chunked AES-256-CBC decrypt ensures RAM usage is strictly bounded by $O(\text{dictionary window} + \text{slice buffers})$, independent of entry size.
3. **v1.4.0:** Delivers request #4 (atomic mutation: add and delete), with in-DLL handle sharing collision detection (`RAR_ERR_BUSY = -14`).
4. **v1.5.0:** Delivers request #6 (extended metadata: FILETIMEs, host attributes, symlink targets, archive comments), and documentation cleanup.
5. **Integration deliverables:** Provide Crate with a unified v1.1.0 $\to$ v1.5.0 migration guide and publish Release Candidate (`-rc1`) builds prior to official tags.

---

## 7. Test plan & quality assurance

1. **Automated ABI compatibility CI:**
   - Compile a standalone test client executable pinned strictly against v1.1.0 headers (`openrar_dll.h`). Run against `openrar.dll` built from v1.2.0, v1.3.0, v1.4.0, and v1.5.0 to verify export ordinal stability, struct alignment, and runtime compatibility.
   - Compile a client against *new* headers and run against the *old* v1.1.0 DLL; assert `openrar_abi_features()` reports new feature bits as absent and functions gracefully fall back or fail fast via `GetProcAddress`.
2. **Solid catch-up & cancellation tests:**
   - Test out-of-order extraction across solid archive chains (e.g. extracting entries 10, 2, 8, 2) and verify output matches sequential extraction byte-for-byte.
   - Assert cancellation during discard-sink catch-up returns `RAR_ERR_ABORTED` immediately and leaves the handle fully reusable for subsequent requests.
   - Test solid archives spanning multiple volumes.
3. **Encrypted chunked decrypt & RAM ceiling tests:**
   - Stored entries: extract 100 MiB encrypted stored entry; assert peak working set increase is $\le 1\text{ MiB}$.
   - Compressed entries: extract 10 MiB vs 100 MiB encrypted compressed entry; assert peak working set increase is bounded by $\text{window\_size} + 2\text{ MiB}$ and identical across both sizes.
   - Test `handle_test` on encrypted entries with correct password (returns `RAR_OK`), wrong password (`RAR_ERR_BAD_PASSWORD`), and missing password (`RAR_ERR_ENCRYPTED`).
4. **Multi-volume fault injection:**
   - Open-time missing volume: open a 3-part set where part 2 is missing; assert `open_file` returns 0 with `RAR_ERR_MISSING_VOLUME` and missing path in error detail.
   - Extract-time missing volume: open a 3-part set, delete volume 2 from disk after open, call `handle_extract_to_path` across the boundary; assert `RAR_ERR_MISSING_VOLUME` is returned.
   - Middle-volume open: pass `archive.part02.rar` to `open_file`; verify it locates `part01` or returns 0 with `RAR_ERR_MISSING_VOLUME` and `"cannot open first volume: archive.part01.rar"` in detail.
   - Volume switch cancellation: simulate multi-volume set where cancel callback returns 1 during volume transition; assert operation aborts immediately with `RAR_ERR_ABORTED`.
5. **Durability & fault injection:**
   - Destination locking: external process holds exclusive lock on `dest_path` during rename $\to$ assert temporary file is deleted, `dest_path` is untouched, and `RAR_ERR_IO` is returned.
   - Disk full during extraction: simulate write error $\to$ assert temporary file deleted and `RAR_ERR_IO` returned.
   - Destination guard: call `handle_extract_to_path` specifying the archive file itself as `dest_path` $\to$ assert `RAR_ERR_INVALID_ARG`.
6. **Mutation & collision tests:**
   - Open a handle on `test.rar`; call `openrar_archive_delete_entries_file` on `test.rar` $\to$ assert fast failure with `RAR_ERR_BUSY (-14)`.
   - Index-order identity: assert `openrar_archive_list_file` and `openrar_archive_handle_list` produce identical entry sequences across all non-volume fixtures.
   - Solid block head tests:
     - Attempt to delete the block head of an active solid block while keeping solid entries $\to$ assert `RAR_ERR_UNSUPPORTED_FEATURE`.
     - Attempt to replace the block head of an active solid block while keeping solid entries $\to$ assert `RAR_ERR_UNSUPPORTED_FEATURE`.
     - Attempt to replace a name that has multiple historical instances where one instance is the head of an active solid block $\to$ assert `RAR_ERR_UNSUPPORTED_FEATURE`.
     - Attempt to delete a mid-block entry while keeping subsequent solid entries $\to$ assert `RAR_ERR_UNSUPPORTED_FEATURE`.
     - Suffix deletion of solid block $\to$ assert success, retained entries extract byte-identically.
     - Whole-block deletion (deleting head and all solid entries in the run) $\to$ assert success.
     - Positive control for permitted replacement: replace an independent non-solid entry (not serving as a block head) $\to$ assert success, prior matching instances stripped, `handle_list` reflects new entry.
7. **Fuzzing scan-with-callbacks:**
   - Fuzz `openrar_archive_open_file` with malformed and truncated headers while firing progress and cancel callbacks at high frequency.
   - Verify callbacks that attempt re-entrant calls into the handle table do not deadlock or crash.
