# DLL Enhancement Plan — post-v1.1.0

**Status:** planned, ready for pickup
**Source:** feature request from the Crate project (Windows archiver, Hybrid D
consumer, pinned `v1.1.0`), evaluated against the tree at `v1.1.0` + the
uncommitted `_pw`/`open_ex` work.
**Rules that bind every phase:** additive exports only — no
`OPENRAR_DLL_API_VERSION` bump, no change to the 64-byte
`openrar_archive_entry_t`, no behavior change to any existing export
(`docs/versioning.md`, `docs/dll-integration-spec.md` §3). New capability is
negotiated with `openrar_abi_features()` bits + `GetProcAddress`.

---

## 0. Architecture context (why the plan looks like this)

The DLL today is a shell over the MVP core. All archive exports forward to
`BufferArchive` (`src/archive/buffer_archive.cpp`), which rejects encrypted
data, solid, multi-volume and recovery archives; the file helpers slurp the
whole archive into RAM per call (`read_file_bytes` in `src/dll/dll_api.cpp:643`,
used by `openrar_archive_extract_file` at `dll_api.cpp:682`). Meanwhile the
full engine exists and is exercised only by the CLI:

| Capability | Engine home | DLL reachability today |
|---|---|---|
| Passwords (file + header encryption, constant-time PswCheck) | `ArchiveReader` (`src/archive/archive_reader.hpp:25`) | none (listing half lands in Phase 0) |
| Scan-once file reader, solid chain state, volume stitching | `ArchiveReader::open` / `scan_archive` (`archive_reader.cpp:141`) | none |
| Streaming output hook (progress/cancel attachment point) | `Decompressor50::decompress(..., flush_cb)` (`decompressor50.hpp:128`) | none |
| Streaming integrity test (64 KiB chunks, no materialization) | `ArchiveReader::test_entry` (`archive_reader.cpp:539`) | none |
| Atomic add/delete (temp + replace, QO strip, solid continuation) | `ArchiveMutator` (`src/archive/archive_mutator.hpp`) | none |
| Full entry metadata (FILETIMEs, attrs, host OS, redir, win size) | `format::FileBlock` (`src/format/headers.hpp:146`) | none — dropped at the 64-byte boundary |

One structural decision follows: **give the DLL a file-mode handle backed by
`ArchiveReader`.** That single export family answers Crate requests 1
(extraction half), 2, 3, 5, most of 6, and the behavior behind 7. Mutation
(request 4) is a separate family over `ArchiveMutator`.

## 1. Phase 0 — land the in-flight work → v1.2.0

Already implemented and tested in the working tree (uncommitted):

- `openrar_archive_list_file_pw` — streaming listing with header decryption
  (`-hp`), `RAR_ERR_ENCRYPTED` / `RAR_ERR_BAD_PASSWORD` distinction, encrypted
  file entries reported with `is_encrypted = 1` (`emit_encrypted_entries`
  mode in `list_file_stream`).
- `openrar_archive_open_ex` — progress/cancel over the buffer handle's
  open-time scan.
- Feature bits `OPENRAR_ABI_FEATURE_LIST_PASSWORD` (1<<1),
  `OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS` (1<<2); spec §6.10, wrapper
  overloads, tests in `tests/unit/dll_tests.cpp` / `dll_cpp_tests.cpp` /
  `buffer_archive_tests.cpp`.

Ship as v1.2.0. This completes the *prompt flow* for encrypted archives
(list → `RAR_ERR_ENCRYPTED` → prompt → re-list with password) but **not**
extraction of encrypted entries — say so to Crate so they don't ship
`set_password` against it.

## 2. Phase 1 — file-mode handle on `ArchiveReader` → v1.3.0 (keystone)

New feature bit: `OPENRAR_ABI_FEATURE_FILE_HANDLE (1ull << 3)`.

### Exports

```c
// Scan-once open of an archive on disk. Keeps the file open for the handle's
// lifetime. password_utf8 may be NULL (needed up front for -hp archives:
// headers are unreadable without it). Progress/cancel cover the open-time
// scan (byte semantics like open_ex). 0 on failure, detail via
// openrar_archive_get_error; a cancelled scan returns 0 with "open aborted".
uint32_t openrar_archive_open_file(const char* arc_path, const char* password_utf8,
                                   openrar_progress_cb progress,
                                   openrar_cancel_cb cancel, void* user);

// Existing exports become polymorphic over handle kind (same signatures):
//   openrar_archive_handle_list       — packs the reader's cached entries
//   openrar_archive_handle_extract    — entry to a heap buffer (previews)
//   openrar_archive_close             — closes the reader + file

// To-path extraction with byte progress + cancel. DLL owns durability:
// writes dest_path + ".openrar-tmp", renames on success; abort/failure
// deletes the temp and never leaves a partial dest.
int openrar_archive_handle_extract_to_path(uint32_t handle, uint32_t entry_index,
                                           const char* dest_path,
                                           openrar_progress_cb progress,
                                           openrar_cancel_cb cancel, void* user);

// Streaming integrity test: CRC32 / BLAKE2sp verified without retaining
// output (stored entries stream in 64 KiB chunks via the existing
// stream_stored_entry_chunks path).
int openrar_archive_handle_test(uint32_t handle, uint32_t entry_index,
                                openrar_progress_cb progress,
                                openrar_cancel_cb cancel, void* user);
```

### Engine work required

- `ArchiveReader::scan_archive` has no callback hooks today — thread
  progress/cancel through the scan (per header block, per volume) for
  `open_file`.
- Solid **catch-up**: when the requested entry carries the solid flag and the
  chain position is invalid (`solid_chain_ok_` false or position ahead),
  decode the solid run from its start up to the requested entry with a
  discard sink before extracting. In-order extraction never pays this;
  out-of-order access is correct but costs decode time proportional to the
  skipped span. A fresh-window decode of a mid-chain solid entry is *not*
  correct output — catch-up is a correctness requirement, not an
  optimization. (`select_unpacker`'s restart branch, `archive_reader.cpp:473`,
  must not be reachable as a silent wrong-data path for this surface.)
- Entry mapping `ArchiveEntry` → 64-byte ABI struct (path, is_dir, size,
  packed_size = summed extents, mtime = UNIX seconds, crc32, method 0..5,
  is_encrypted). Note the file-handle surface accepts `method` 1/2/4 and
  solid/volume/encrypted-flagged entries that the buffer surface rejects —
  document this per-surface split explicitly in the spec.

### Contract decisions (we own these — write them into `openrar_dll.h` + spec)

- **Password lifetime:** parameter of `open_file`, not a post-open setter
  (header decryption is needed during the scan). Re-prompt = re-open; one
  header walk is cheap. No `handle_set_password` export. `-p` archives
  (clear headers) verify file-data passwords lazily at extract/test via the
  existing constant-time PswCheck compare (`archive_reader.cpp:987`) — no
  eager per-entry PBKDF2 at open (hostile `lg2_count` up to 24 makes that a
  DoS vector). **No standalone `verify_password` export**: `open_file`
  returning `RAR_ERR_BAD_PASSWORD` covers `-hp`; `-p` surfaces at
  extract/test.
- **Error mapping:** `RAR_ERR_BAD_PASSWORD` on PswCheck mismatch (never
  `CRC_MISMATCH` for a wrong password); `RAR_ERR_ENCRYPTED` when an
  encrypted entry is extracted with no password supplied;
  `RAR_ERR_CRC_MISMATCH` for genuine checksum failure; `RAR_ERR_ABORTED` on
  cancel with no partial destination. When a file header carries no PswCheck
  record, wrong passwords can only surface as decode/CRC failure — document
  this corner explicitly (Crate asked).
- **Progress semantics:** (done, total) = (uncompressed bytes produced,
  `entry.size`). Monotonic, cumulative, exactly one final (total, total);
  total = 0 allowed for empty entries and for `FHFL_UNPUNKNOWN` entries
  (done counts bytes, no denominator). Cancel polled per flush chunk
  (window-granularity for compressed entries, 64 KiB for stored).
- **Durability:** DLL-owned temp-and-rename for `handle_extract_to_path`
  only. The frozen `openrar_archive_extract_file_to_path` keeps its current
  direct-write behavior.
- **File lifetime / staleness:** the handle holds the file open; hosts must
  close before renaming/deleting the archive. On-disk change under an open
  handle: reads may fail with `RAR_ERR_IO`; undefined beyond that — hosts
  re-open (Crate already re-open after every mutation).
- **Threading:** one thread per handle at a time (matches existing handle
  contract; the reader's solid state is sequential). Callbacks run on the
  calling thread with no DLL lock held (`HandleTable::pin` guarantees this),
  and must not call back into the same handle.

### Tests

- Fixtures: existing `tests/hello5_p.rar` / `hello5_hp.rar`; generate a solid
  archive (`ArchiveMutator::write_batch_add(solid=true)`), a two-volume set
  (`add_file_to_archive_vol`), and a >1 GiB stored entry in CI (or a scaled
  64 MiB variant for local runs).
- Contract tests mirroring `_ex` listing style: progress monotonic + final
  (total,total); cancel → `RAR_ERR_ABORTED`, no file at dest; wrong password
  → `RAR_ERR_BAD_PASSWORD` for both `-hp` (at open) and `-p` (at extract);
  solid out-of-order extract equals in-order bytes; multi-volume extract
  roundtrip; 1,000-entry multi-extract walks headers once (assert via a
  progress-counting probe during open only).

## 3. Phase 2 — mutation exports → v1.4.0

New feature bit: `OPENRAR_ABI_FEATURE_MUTATION (1ull << 4)`. Free functions
over `ArchiveMutator` — **not** handle-based (see non-goals).

```c
// Delete entries by index (indices refer to a fresh listing order).
int openrar_archive_delete_entries_file(const char* arc_path,
                                        const uint32_t* entry_indices,
                                        uint32_t count);

// Append/replace files, 'u' semantics: incoming names override same-name
// entries. method ∈ {0,3,5}, window_log2 ∈ [1,4] (parity with create).
int openrar_archive_add_files_file(const char* arc_path,
                                   const char* const* src_paths,
                                   const char* const* arc_names,
                                   uint32_t file_count, int method,
                                   uint32_t window_log2);
```

- **Core addition:** `ArchiveMutator::delete_entries_by_index` — delete is
  wildcard-mask based today (`archive_mutator.cpp:302`); index→name→mask
  translation breaks on names containing `*`/`?`. Match by entry identity
  (header offset) instead; share the rewrite/QO-strip path with the existing
  function.
- **Durability:** core-owned (write temp + atomic replace, already the
  `write_batch_add` contract: original untouched on failure). No `tmp_path`
  parameter — the DLL chooses a sibling temp. Crate wrap their own
  temp+rename anyway; the contract just says the core never corrupts.
- **Solid:** appending to a solid archive continues the stream (already
  implemented, `archive_mutator.cpp:750`). A delete that would orphan a solid
  run (removing run members while leaving later ones) must return a distinct
  error rather than rewrite a broken chain — verify what the current delete
  path does and fix if needed.
- **Locked archives** (`MHFL_LOCK`): refuse with `RAR_ERR_UNSUPPORTED_FEATURE`.
- **Handle interaction:** none. Handles over a mutated archive are stale by
  definition; hosts re-open (documented; Crate already do).

## 4. Phase 3 — metadata, encrypted streaming test, volumes, nits → v1.5.0

### 4.1 Extended entry metadata (Crate request 6)

Feature bit `OPENRAR_ABI_FEATURE_ENTRY_EX (1ull << 5)`. File handles only —
fields come straight from `format::FileBlock`; the 64-byte struct and its
WASM-reserved `_pad` (`src/api/abi_contract.hpp:73`) stay frozen.

```c
typedef struct {
    uint32_t attrs;           // host-OS attributes (FILE_ATTRIBUTE_* when host_os==0)
    uint32_t host_os;         // 0 = Windows, 1 = Unix
    uint64_t mtime_ft, ctime_ft, atime_ft; // FILETIME (UTC, 100ns); 0 = not stored
    uint32_t flags;           // SOLID | ENCRYPTED | REDIR | SPLIT_BEFORE | SPLIT_AFTER ...
    uint32_t win_size;        // dictionary bytes, 0 if n/a
    uint32_t redir_type;      // 0 none / 1 unixsymlink / 2 winsymlink / 3 junction / 4 hardlink / 5 filecopy
    uint32_t version_needed;  // unp_ver (0 = RAR5)
} openrar_entry_ex_t;         // packed, fixed size

int openrar_archive_handle_entry_ex(uint32_t handle, uint32_t entry_index,
                                    openrar_entry_ex_t* out,
                                    void** extra_out, size_t* extra_size_out);
void openrar_archive_entry_ex_free(void* extra);
```

`extra` blob carries variable-length strings (redir target). "Not stored" is
zero + a `flags` bit per timestamp — distinguishable from epoch, as Crate
asked. Archive-level companion: `openrar_archive_handle_info` (comment UTF-8
in the extra blob, main-header flags, volume count/index, recovery-record
size).

### 4.2 Streaming encrypted test (finishes request 5)

`test_entry` currently skips encrypted entries entirely
(`archive_reader.cpp:545`) and the encrypted extract path buffers the whole
cipher (`archive_reader.cpp:998`). Add chunked decrypt (CBC in ≤256 KiB
slices) feeding CRC/BLAKE2 — then `handle_test` covers encrypted entries
with fixed memory, verifying via PswCheck + MAC without surfacing plaintext.
Until this lands, `handle_test` on an encrypted entry returns
`RAR_ERR_UNSUPPORTED_FEATURE` (honest, not a silent pass).

### 4.3 Multi-volume documentation (request 7)

No new volume ABI. `open_file` inherits transparent set-walking and
cross-volume extraction from the reader's extent stitching
(`archive_reader.cpp:141`) — document that as the contract. Additions:
`SPLIT_BEFORE`/`SPLIT_AFTER` in `entry_ex.flags` (already tracked on
`ArchiveEntry`), and a distinct `RAR_ERR_MISSING_VOLUME (-13)` (additive enum
extension; sync `abi_contract.hpp`, WASM surface, spec) with the failing
volume path in the error string. A host-supplied volume-list escape hatch
stays out of scope until a host needs it.

### 4.4 Documentation nits (all four, one PR)

1. `OPENRAR_WINDOW_128K … OPENRAR_WINDOW_1M` named constants + header table:
   `window_log2` 1–4 → 128 KB…1 MB (5 → 4 MB, stream-only); document that
   `compress_block`'s `win_size` is **bytes** while `CreateOptions.window_log2`
   is log2 — two scales, currently undiscoverable.
2. `openrar_archive_get_error` is `thread_local` (`dll_api.cpp:64`) — state it
   in the public header (spec §10 already does).
3. `RAR_ERR_PARTIAL_OK`: returned only by `extract_all` (some entries failed,
   succeeded ones included in output); a single-entry extract can never
   return it. Document verbatim.
4. Feature-bit registry: one header block listing assigned bits and reserved
   ranges; policy — a bit is assigned only in the release whose exports it
   gates ship in.

## 5. Non-goals / pushed-back designs

| Proposal | Why not | Instead |
|---|---|---|
| Per-export `_pw` variants (`extract_file_pw`, `extract_file_to_path_pw`, …) | Combinatorial fork of every future suffix; password-as-parameter has no session home | Password is an `open_file` parameter; the lone `list_file_pw` (Phase 0) serves the frozen pre-handle prompt flow |
| Handle-based mutation (`handle_delete`/`handle_add`) | The mutator rewrites the archive on disk (temp + atomic replace) — invalidates the handle's open stream and cached indices, and collides with Windows sharing violations on rewrite-under-open | Free-function exports; hosts re-open (they already do) |
| Free-function `extract_file_to_path_ex` / `extract_file_ex` | Keeps the per-call slurp + re-walk tax that request 3 exists to remove | `handle_extract_to_path` with progress/cancel |
| Standalone `verify_password` | Redundant for `-hp` (open_file already fails fast); eager PBKDF2 for `-p` is a DoS vector | Lazy verification at extract/test via constant-time PswCheck |
| Fixing the frozen file exports' slurp (routing through the streaming engine) | Would silently change `HEAD_CRYPT` error codes (`UNSUPPORTED_FEATURE` → `ENCRYPTED`), breaking the frozen-semantics promise | Accepted, documented debt; the handle is the good path |
| Password/extract upgrades to the in-memory buffer surface | WASM-parity frozen MVP; no host need identified (Crate reads from disk) | Buffer surface stays as-is |
| `test_all` export | Trivially driven per-entry by the host | Skip |
| Widening the 64-byte entry struct or using `_pad` | `_pad` is reserved by the shared WASM contract ("consumers must zero") | Additive `entry_ex` query |

## 6. Versioning & communication

- Every phase is additive → MINOR bumps: v1.2.0 (Phase 0, in flight), v1.3.0
  (file handle), v1.4.0 (mutation), v1.5.0 (metadata + encrypted test +
  volume docs + nits). `OPENRAR_DLL_API_VERSION` stays 1 throughout.
- Each release updates `openrar_dll.h` (declarations + bit registry),
  `include/openrar/openrar.hpp` (wrapper overloads), `docs/dll-integration-spec.md`,
  `CHANGELOG.md`; the release pipeline packages artifacts as usual.
- Reply to Crate after v1.2.0 with this plan's mapping: their #1 blocking
  request completes at v1.3.0; #2/#3/#5 at v1.3.0; #4 at v1.4.0; #6/#7 and
  all four nits at v1.5.0 — plus the two contract answers they asked us to
  own (dest-file durability on abort: DLL-owned temp+rename; solid access:
  automatic catch-up, in-order is the fast path).
