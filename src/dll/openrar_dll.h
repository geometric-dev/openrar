#ifndef OPENRAR_DLL_OPENRAR_DLL_H
#define OPENRAR_DLL_OPENRAR_DLL_H

// ─────────────────────────────────────────────────────────────────────────────
//  openrar_dll.h — Public C ABI for openrar.dll / libopenrar.so
//  Hybrid D: stable C core with header-only C++ wrapper (include/openrar/openrar.hpp).
//  Contracts shared with src/wasm via src/api/abi_contract.hpp (the enum and
//  entry struct below are the C-compatible declarations; dll_api.cpp pins them
//  to the canonical definitions with compile-time equivalence checks)
//  but unified for native DLL (single allocator, file + buffer + streaming).
// ─────────────────────────────────────────────────────────────────────────────

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Export macro ─────────────────────────────────────────────────────────────
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(OPENRAR_DLL_EXPORTS)
#define OPENRAR_DLL_API __declspec(dllexport)
#elif defined(OPENRAR_DLL_STATIC)
#define OPENRAR_DLL_API
#else
#define OPENRAR_DLL_API __declspec(dllimport)
#endif
#else
#if defined(OPENRAR_DLL_EXPORTS)
#define OPENRAR_DLL_API __attribute__((visibility("default")))
#else
#define OPENRAR_DLL_API
#endif
#endif

#ifndef OPENRAR_DLL_CALL
#if defined(_WIN32)
#define OPENRAR_DLL_CALL __cdecl
#else
#define OPENRAR_DLL_CALL
#endif
#endif

// ── Version ──────────────────────────────────────────────────────────────────
// OPENRAR_DLL_API_VERSION changes only on ABI breaks (docs/versioning.md).
// Additive exports must not bump it: embedders probe with strict equality
// (docs/dll-integration-spec.md §3), so a bump would strand every host built
// against the previous header. New capabilities are negotiated via
// openrar_abi_features() / GetProcAddress instead.
#define OPENRAR_DLL_API_VERSION 1

OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_version(void);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_version(void);

// ── Capability negotiation (additive; feature bits) ──────────────────────────
// Bit flags for openrar_abi_features(). Hosts must not assume an export
// exists before the corresponding bit is set (or the symbol resolves via
// GetProcAddress / dlsym).
// Feature-bit registry (assigned only when the gated exports ship):
//   bit 0  LIST_PROGRESS          list_file_ex / list_ex            (v1.1.0)
//   bit 1  LIST_PASSWORD          list_file_pw                      (v1.2.0)
//   bit 2  HANDLE_OPEN_PROGRESS   archive_open_ex                   (v1.2.0)
//   bit 3  FILE_HANDLE            open_file / handle_extract_to_path /
//                                 handle_test                       (v1.3.0)
#define OPENRAR_ABI_FEATURE_LIST_PROGRESS (1ull << 0)        // list_file_ex / list_ex below
#define OPENRAR_ABI_FEATURE_LIST_PASSWORD (1ull << 1)        // list_file_pw below
#define OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS (1ull << 2) // archive_open_ex below
#define OPENRAR_ABI_FEATURE_FILE_HANDLE (1ull << 3)          // file-mode handles below
OPENRAR_DLL_API uint64_t OPENRAR_DLL_CALL openrar_abi_features(void);

// ── Allocator (single heap; must pair alloc ↔ free) ─────────────────────────
OPENRAR_DLL_API void* OPENRAR_DLL_CALL openrar_alloc(size_t bytes);
OPENRAR_DLL_API void OPENRAR_DLL_CALL openrar_free(void* ptr);
// Alias for archive callers that used openrar_archive_alloc/free in WASM
OPENRAR_DLL_API void* OPENRAR_DLL_CALL openrar_archive_alloc(size_t bytes);
OPENRAR_DLL_API void OPENRAR_DLL_CALL openrar_archive_free(void* ptr);

// ── Error model (mirrors src/archive/buffer_archive.hpp + wasm/archive_api.hpp) ─
enum RarError {
    RAR_OK = 0,
    RAR_ERR_PARTIAL_OK = 1,
    RAR_ERR_NOT_RAR = -1,
    RAR_ERR_UNSUPPORTED_FEATURE = -2,
    RAR_ERR_TRUNCATED = -3,
    RAR_ERR_CRC_MISMATCH = -4,
    RAR_ERR_NOMEM = -5,
    RAR_ERR_IO = -6,
    RAR_ERR_BAD_PASSWORD = -7,
    RAR_ERR_INVALID_ARG = -9,
    RAR_ERR_ABORTED = -11,
    // Archive headers are encrypted (HEAD_CRYPT): a password is required
    // before any header can be read. Returned only by the _ex listing exports
    // below, as soon as the block is reached; the non-_ex listing calls keep
    // their historical RAR_ERR_UNSUPPORTED_FEATURE for the same condition.
    RAR_ERR_ENCRYPTED = -12,
    // A volume of a multi-volume set (.partNN.rar) is missing. Returned by the
    // file-mode handle exports (open_file when the first volume cannot be
    // located, extraction/test when a split entry's extent volume is absent);
    // openrar_archive_get_error carries the missing volume's path.
    RAR_ERR_MISSING_VOLUME = -13
};

OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_last_error(char* buf, int buf_len);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_get_error(char* buf, int buf_len);

// ── Block codec (reuses wasm_api.hpp contract) ───────────────────────────────
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_compress(const uint8_t* src, size_t src_len,
                                                      uint8_t** out_ptr, size_t* out_len,
                                                      int method);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_compress2(const uint8_t* src, size_t src_len,
                                                       uint8_t** out_ptr, size_t* out_len,
                                                       int method, size_t win_size);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_decompress(const uint8_t* src, size_t src_len,
                                                        uint8_t** out_ptr, size_t* out_len);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_decompress2(const uint8_t* src, size_t src_len,
                                                         uint8_t** out_ptr, size_t* out_len,
                                                         size_t win_size);

// ── Streaming block codec (new — src/compress/stream_encoder) ────────────────
OPENRAR_DLL_API uint32_t OPENRAR_DLL_CALL openrar_stream_create(int method, uint32_t window_log2);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_stream_feed(uint32_t handle, const uint8_t* src,
                                                         size_t n);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_stream_finish(uint32_t handle, uint8_t** out_ptr,
                                                           size_t* out_len);
OPENRAR_DLL_API void OPENRAR_DLL_CALL openrar_stream_free(uint32_t handle);

// ── Archive entry layout (stable 64B, mirrors wasm/archive_api.hpp) ──────────
#pragma pack(push, 1)
typedef struct {
    uint32_t path_offset;
    uint32_t path_len;
    uint32_t is_dir;
    uint32_t method;
    uint32_t is_encrypted;
    uint32_t crc32;
    uint64_t size;
    uint64_t packed_size;
    uint64_t mtime;
    uint64_t _pad[2];
} openrar_archive_entry_t;
#pragma pack(pop)

// ── Buffer archive — one-shot (reuses archive_api.hpp but unified allocator) ─
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_list(const uint8_t* data, size_t size,
                                                          uint32_t* count, void** entries_out,
                                                          void** paths_out, size_t* paths_size_out);
OPENRAR_DLL_API void OPENRAR_DLL_CALL openrar_archive_list_free(void* entries, void* paths,
                                                                size_t paths_size);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_extract(const uint8_t* data, size_t size,
                                                             uint32_t entry_index,
                                                             uint8_t** out_ptr, size_t* out_len);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_extract_all(const uint8_t* data, size_t size,
                                                                 uint8_t** buf_out_ptr,
                                                                 size_t* buf_size_out,
                                                                 uint64_t** offsets_out_ptr,
                                                                 uint32_t* offsets_count_out);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_create(
    const uint8_t* const* paths_arr, const uint8_t* const* data_arr, const size_t* sizes_arr,
    uint32_t file_count, int method, uint32_t window_log2, uint8_t** out_ptr, size_t* out_len);

// ── Progress / cancel callbacks ──────────────────────────────────────────────
// Declared here because the handle-API and listing exports below take them;
// wired per-call (listing/open exports) or per-handle (stream setters).
typedef void(OPENRAR_DLL_CALL* openrar_progress_cb)(void* user, uint64_t done, uint64_t total);
typedef int(OPENRAR_DLL_CALL* openrar_cancel_cb)(void* user);

// ── Handle API (scan-once; avoids double scan for list → extract) ───────────
OPENRAR_DLL_API uint32_t OPENRAR_DLL_CALL openrar_archive_open(const uint8_t* data, size_t size);
// openrar_archive_open with progress/cancel over the scan that happens at
// open time. Same handle semantics and failure behavior as
// openrar_archive_open (0 on failure, detail via openrar_archive_get_error);
// a cancelled scan also returns 0 with an "open aborted" detail. The entry
// semantics are those of openrar_archive_list (rejects encrypted, solid,
// multi-volume and recovery archives). NULL callbacks are allowed and make
// the call equivalent to openrar_archive_open.
OPENRAR_DLL_API uint32_t OPENRAR_DLL_CALL openrar_archive_open_ex(const uint8_t* data, size_t size,
                                                                  openrar_progress_cb progress,
                                                                  openrar_cancel_cb cancel,
                                                                  void* user);
OPENRAR_DLL_API void OPENRAR_DLL_CALL openrar_archive_close(uint32_t handle);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_list(uint32_t handle, uint32_t* count,
                                                                 void** entries_out,
                                                                 void** paths_out,
                                                                 size_t* paths_size_out);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_extract(uint32_t handle,
                                                                    uint32_t entry_index,
                                                                    uint8_t** out_ptr,
                                                                    size_t* out_len);
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_handle_extract_all(uint32_t handle, uint8_t** buf_out_ptr, size_t* buf_size_out,
                                   uint64_t** offsets_out_ptr, uint32_t* offsets_count_out);

// ── File archive helpers (new — buffer + file flexibility) ───────────────────
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_list_file(const char* arc_path,
                                                               uint32_t* count, void** entries_out,
                                                               void** paths_out,
                                                               size_t* paths_size_out);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_extract_file(const char* arc_path,
                                                                  uint32_t entry_index,
                                                                  uint8_t** out_ptr,
                                                                  size_t* out_len);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_extract_file_to_path(const char* arc_path,
                                                                          uint32_t entry_index,
                                                                          const char* dest_path);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_create_from_paths(
    const char* const* src_paths, const char* const* arc_names, uint32_t file_count, int method,
    uint32_t window_log2, uint8_t** out_ptr, size_t* out_len);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_create_to_file(const char* const* src_paths,
                                                                    const char* const* arc_names,
                                                                    uint32_t file_count, int method,
                                                                    uint32_t window_log2,
                                                                    const char* out_path);

// ── Progress / cancel (handle-based; polled between blocks) ──────────────────
// (the callback typedefs live above the handle API; see comment there)
// openrar_set_progress/openrar_set_cancel were removed (report L12/I19): they
// accepted a callback, returned RAR_OK and did nothing. Callbacks are only
// wired at the streaming layer — use openrar_stream_set_progress /
// openrar_stream_set_cancel below.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_stream_set_progress(uint32_t handle,
                                                                 openrar_progress_cb cb,
                                                                 void* user);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_stream_set_cancel(uint32_t handle,
                                                               openrar_cancel_cb cb, void* user);

// ── Listing with progress / cancel (_ex variants; additive) ──────────────────
// Byte-based progress: done = archive bytes consumed (includes any SFX
// prefix), total = archive size. RAR has no central directory, so the entry
// count is unknown until the walk completes — there is no entry-index
// denominator, and an honest percentage must come from bytes. done/total are
// cumulative and monotonic; exactly one final (total, total) callback fires
// on success. The file variant walks the archive on disk (no whole-file
// buffering), so it also avoids materialising multi-GB archives in RAM.
//
// cancel is polled between header blocks (and during the SFX scan); a
// non-zero return aborts with RAR_ERR_ABORTED and leaves every output
// null/zero — nothing partial is ever allocated. Both callbacks run on the
// calling thread with no DLL-internal lock held and receive the same `user`
// pointer. Either callback may be NULL; with both NULL the calls are
// equivalent to their non-_ex counterparts.
//
// Error mapping matches the non-_ex calls, except RAR_ERR_ENCRYPTED (-12) is
// returned as soon as an encrypted-header block (HEAD_CRYPT) is reached, so
// hosts can prompt for a password immediately instead of after a walk that
// could never succeed. Listing itself never needs a password; header-
// encrypted archives cannot be listed without one.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_list_file_ex(
    const char* arc_path, uint32_t* count, void** entries_out, void** paths_out,
    size_t* paths_size_out, openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_list_ex(
    const uint8_t* data, size_t size, uint32_t* count, void** entries_out, void** paths_out,
    size_t* paths_size_out, openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);

// ── Listing with a password (_pw; additive) ──────────────────────────────────
// Streams the archive from disk like list_file_ex and decrypts header-
// encrypted archives (RAR5 -hp): the clear HEAD_CRYPT block after the
// signature selects the KDF parameters, keys derive from `password` via
// PBKDF2, and every following header is read through AES-256-CBC. A password
// on an archive without header encryption is ignored; an empty or NULL
// password counts as none.
//
// Error mapping:
//   RAR_ERR_ENCRYPTED        - NULL/empty password and HEAD_CRYPT present
//                              (the early password signal; prompt and retry).
//   RAR_ERR_BAD_PASSWORD     - password supplied but wrong (PswCheck mismatch
//                              or decrypted-header CRC failure).
//   RAR_ERR_UNSUPPORTED_FEATURE - unknown HEAD_CRYPT crypto version, solid,
//                              multi-volume or recovery archives.
//
// Unlike the frozen list/list_ex/list_file_ex semantics, file entries whose
// payload is encrypted are REPORTED (is_encrypted = 1) and the walk
// continues: -hp implies encrypted file data for every entry, so rejecting
// them would make password listing useless. Extraction of encrypted entries
// is still not supported by this DLL. There is deliberately no password
// variant of the in-memory openrar_archive_list: decrypting headers in the
// memory walker would duplicate the CBC/size-recovery crypto path in
// read_block_raw_mem for a case no host has identified; the streaming export
// covers the real-world flow.
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_list_file_pw(const char* arc_path, const char* password, uint32_t* count,
                             void** entries_out, void** paths_out, size_t* paths_size_out,
                             openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);

// ── File-mode handles (open by path; additive) ───────────────────────────────
// A scan-once handle over an archive on disk, backed by the streaming reader:
// the file stays open for the handle's lifetime, headers are walked exactly
// once at open, and per-entry extraction/test reuse the cached walk. This is
// the surface for multi-GB archives, solid sets and encrypted payloads — the
// in-memory handle API above remains the buffer MVP (encrypted/solid/
// multi-volume archives are rejected there and only listed here).
//
// The existing handle exports dispatch on the handle kind:
//   openrar_archive_handle_list    — packs the cached walk (file entries only;
//                                    service headers such as CMT/RR are not
//                                    exposed as entries, unlike the frozen
//                                    buffer listing which has always surfaced
//                                    them in comment/recovery archives)
//   openrar_archive_handle_extract — entry to a heap buffer (preview path).
//       On file-backed handles ONLY this is capped at
//       OPENRAR_MAX_HEAP_EXTRACT_SIZE (256 MiB) of uncompressed output; larger
//       entries return RAR_ERR_NOMEM — hosts should check entry.size and use
//       openrar_archive_handle_extract_to_path for large entries. Buffer
//       handles keep their historical uncapped behavior. This export has no
//       cancel callback (frozen ABI), so on file handles solid catch-up runs
//       uncancellably to completion; hosts that need cancellation during
//       solid decompression must use openrar_archive_handle_extract_to_path.
//   openrar_archive_handle_extract_all — RAR_ERR_UNSUPPORTED_FEATURE on file
//       handles (slurping a multi-GB archive into one flat buffer is
//       prohibited); extract per entry instead.
//   openrar_archive_close          — closes the reader and the file. The host
//       must close all handles on a set before renaming/deleting any volume.

// In-memory extract cap for file-backed handles (openrar_archive_handle_extract).
#define OPENRAR_MAX_HEAP_EXTRACT_SIZE (256ull * 1024 * 1024)

// Scan-once open of an archive on disk; returns a handle or 0 (detail via
// openrar_archive_get_error). password_utf8 is required up front for
// header-encrypted (-hp) archives — headers are unreadable without it — and
// is silently ignored on archives without header encryption; file-data
// passwords (-p) are verified lazily at extract/test time. Progress/cancel
// cover the open-time scan with byte semantics like open_ex (done = archive
// bytes consumed across the volume set, total = bytes of the volumes opened
// so far; cumulative, monotonic, exactly one final (total, total) on
// success). Either callback may be NULL. A cancelled scan returns 0 with an
// "open aborted" detail. On Windows the file is opened with FILE_SHARE_READ
// (write sharing denied): concurrent handles on the same path are fine, an
// external writer is not. Multi-volume sets: if a middle volume
// (.partNN.rar, NN > 01) is passed, the first volume is derived and used
// instead; when it cannot be found or opened the call fails with the detail
// "cannot open first volume: <path>". A missing middle volume fails the open
// with a "missing volume: <path>" detail. Wrong password on a -hp archive
// fails with a "wrong password for encrypted headers" detail; header-
// encrypted archives with no password fail with an "archive headers are
// encrypted" detail (prompt, then re-open with the password — one cheap
// header walk).
OPENRAR_DLL_API uint32_t OPENRAR_DLL_CALL
openrar_archive_open_file(const char* arc_path, const char* password_utf8,
                          openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);

// Extract one entry directly to disk with byte progress + cancel (the
// file-mode handle's main extraction path; the same-name operation on a
// buffer handle extracts in memory and writes the result — both kinds own
// the same durability contract).
//
// Progress: (done, total) = (uncompressed bytes produced, entry.size).
// Cumulative, monotonic, exactly one final (total, total) callback on
// success; no final callback on abort or failure. Cancel is polled per
// output chunk (dictionary-window granularity for compressed entries,
// <= 64 KiB for stored ones) and during solid catch-up. Directory entries
// create the directory and fire exactly one final (0, 0) callback on success.
// Entries may be requested in any order; a solid entry whose predecessors in
// the solid run were not decoded on this handle transparently decodes the
// run prefix first (progress stays at (0, entry.size) during catch-up, so
// hosts should extract in ascending index order for best performance).
//
// Durability: the DLL writes dest_path + ".openrar-tmp.<pid>.<seq>"
// (CREATE_NEW; up to 10 name collisions retried), flushes it to disk, and
// atomically renames over dest_path on success. On abort/failure the temp is
// deleted and dest_path is untouched. dest_path resolving to the archive
// itself (or any volume of its set) fails with RAR_ERR_INVALID_ARG. Orphaned
// ".openrar-tmp.*" files from killed processes can be swept by hosts.
//
// Errors: RAR_ERR_BAD_PASSWORD on PswCheck mismatch (never CRC_MISMATCH for a
// wrong password; headers without a PswCheck record can only surface wrong
// passwords as decode/CRC failures), RAR_ERR_ENCRYPTED when an encrypted
// entry is extracted with no password supplied, RAR_ERR_CRC_MISMATCH on
// genuine checksum failure, RAR_ERR_TRUNCATED when the packed stream ends
// early, RAR_ERR_ABORTED on cancel, RAR_ERR_MISSING_VOLUME when an extent
// volume is absent (path in the error detail), RAR_ERR_IO on write failure.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_extract_to_path(
    uint32_t handle, uint32_t entry_index, const char* dest_path,
    openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);

// Streaming integrity test: verifies CRC32 (or BLAKE2sp when present)
// without retaining decompressed output — fixed, small memory footprint
// (dictionary window + slice buffers, independent of entry size; stored
// entries stream in 64 KiB chunks). Encrypted entries are verified through
// chunked AES-256-CBC decrypt + PswCheck/MAC with the password supplied at
// open_file. Directory and link entries verify trivially (RAR_OK, no
// callbacks). Progress/cancel follow the extract_to_path contract above.
// RAR_OK on verify; RAR_ERR_CRC_MISMATCH on checksum failure;
// RAR_ERR_TRUNCATED on a short decode; RAR_ERR_BAD_PASSWORD /
// RAR_ERR_ENCRYPTED as above; RAR_ERR_ABORTED on cancel. Buffer handles
// return RAR_ERR_UNSUPPORTED_FEATURE (no streaming test exists there).
OPENRAR_DLL_API int OPENRAR_DLL_CALL
openrar_archive_handle_test(uint32_t handle, uint32_t entry_index,
                            openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // OPENRAR_DLL_OPENRAR_DLL_H
