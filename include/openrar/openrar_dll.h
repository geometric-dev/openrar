#ifndef OPENRAR_DLL_OPENRAR_DLL_H
#define OPENRAR_DLL_OPENRAR_DLL_H

// ─────────────────────────────────────────────────────────────────────────────
//  openrar_dll.h — Public C ABI for openrar.dll / libopenrar.so (v1.13.0)
//  Hybrid D: stable C core with header-only C++ wrapper (include/openrar/openrar.hpp).
//  CMake integration: find_package(openrar) provides target openrar::openrar_dll.
//  Public include path: #include <openrar/openrar_dll.h> (or <openrar/openrar.hpp>).
//  Contracts shared with src/wasm via src/api/abi_contract.hpp (the enum and
//  entry struct below are the C-compatible declarations; dll_api.cpp pins them
//  to the canonical definitions with compile-time equivalence checks)
//  but unified for native DLL (single allocator, file + buffer + streaming).
// ─────────────────────────────────────────────────────────────────────────────

#include <stddef.h>
#include <stdint.h>
#if defined(__has_include)
#if __has_include("openrar/version.h")
#include "openrar/version.h"
#elif __has_include("version.h")
#include "version.h"
#endif
#else
#include "version.h"
#endif

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
OPENRAR_DLL_API const char* OPENRAR_DLL_CALL openrar_package_version_string(void);

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
//   bit 4  MUTATION               archive_delete_entries_file /
//                                 archive_add_files_file            (v1.4.0)
//   bit 5  ENTRY_EX               handle_entry_ex / handle_info     (v1.5.0)
//   bit 6  PACKAGE_VERSION        openrar_package_version_string    (v1.7.0)
//   bit 7  SET_LIMITS             openrar_archive_handle_set_limits (v1.10.0)
//   bit 8  REPAIR                 openrar_archive_repair            (v1.11.0)
//   bit 9  CREATE                 openrar_archive_create_file       (v1.14.0)
//   bit 10 FILTERS                openrar_archive_create_file_opts  (v1.15.0)
// Reserve convention: future open-time options (e.g. codepage override,
// custom volume search callbacks) ship as openrar_archive_open_file_ex
// behind a new bit, never as signature changes to open_file.
#define OPENRAR_ABI_FEATURE_LIST_PROGRESS (1ull << 0)        // list_file_ex / list_ex below
#define OPENRAR_ABI_FEATURE_LIST_PASSWORD (1ull << 1)        // list_file_pw below
#define OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS (1ull << 2) // archive_open_ex below
#define OPENRAR_ABI_FEATURE_FILE_HANDLE (1ull << 3)          // file-mode handles below
#define OPENRAR_ABI_FEATURE_MUTATION (1ull << 4)             // mutation exports below
#define OPENRAR_ABI_FEATURE_ENTRY_EX (1ull << 5)             // metadata exports below
#define OPENRAR_ABI_FEATURE_PACKAGE_VERSION (1ull << 6)      // openrar_package_version_string
#define OPENRAR_ABI_FEATURE_SET_LIMITS (1ull << 7)           // openrar_archive_handle_set_limits
#define OPENRAR_ABI_FEATURE_REPAIR (1ull << 8)               // openrar_archive_repair
#define OPENRAR_ABI_FEATURE_CREATE (1ull << 9) // openrar_archive_create_file / create_file_ex
#define OPENRAR_ABI_FEATURE_FILTERS                                                                \
    (1ull << 10) // openrar_archive_create_file_opts / filter controls
#define OPENRAR_ABI_FEATURE_OWNER                                                                  \
    (1ull << 11) // openrar_archive_handle_entry_owner / owner controls
#define OPENRAR_ABI_FEATURE_DICT_EX                                                                \
    (1ull << 12) // RAR 7.0 fractional & non-power-of-two dictionary sizing
#define OPENRAR_ABI_FEATURE_VOL_ENCRYPT (1ull << 13) // multi-volume encryption & metadata parity
#define OPENRAR_ABI_FEATURE_REC_VOL                                                                \
    (1ull << 14) // recovery volume (.rev) & recovery record creation
#define OPENRAR_ABI_FEATURE_PARALLEL_COMPRESS                                                      \
    (1ull << 15) // High-throughput parallel compression (-mt)
#define OPENRAR_ABI_FEATURE_MMAP                                                                   \
    (1ull << 16) // mapped read engine: openrar_archive_handle_read_entry_region
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
    // Returned solely by openrar_archive_extract_all when some entries
    // succeeded and others failed (per-entry results are in the offsets
    // table). No other export can return it: single-entry extraction and
    // every other operation reports exactly one code, so a nonzero single
    // result is always an error, never a partial success.
    RAR_ERR_PARTIAL_OK = 1,
    RAR_ERR_NOT_RAR = -1,
    RAR_ERR_UNSUPPORTED_FEATURE = -2,
    RAR_ERR_TRUNCATED = -3,
    RAR_ERR_CRC_MISMATCH = -4,
    RAR_ERR_NOMEM = -5,
    RAR_ERR_IO = -6,
    RAR_ERR_BAD_PASSWORD = -7,
    /* -8 intentionally skipped (reserved; never assigned to preserve binary
       compatibility with consumers that range-check known error codes). */
    RAR_ERR_INVALID_ARG = -9,
    /* -10 intentionally skipped (reserved; same rationale as -8). */
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
    RAR_ERR_MISSING_VOLUME = -13,
    // The target archive is currently held open by a file-mode handle in this
    // process (open_file keeps the volume open with FILE_SHARE_READ, so the
    // mutator's atomic replace would fail opaquely on Windows). Returned up
    // front by the mutation exports below, before any temp file exists;
    // close every handle on the archive, then retry.
    RAR_ERR_BUSY = -14,
    // A caller-imposed resource limit (ExtractionLimits) was exceeded: per-member
    // output cap, cumulative total-output cap, or header count/byte cap.
    // Value is explicit so C consumers can safely compare without including
    // src/archive/rar_errors.hpp. Gaps at -8 and -10 are documented above.
    // Next available slot: -16.
    RAR_ERR_LIMIT_EXCEEDED = -15
};

// Error details are thread_local state: openrar_last_error and
// openrar_archive_get_error report the message of the most recent call ON
// THE CALLING THREAD, valid until the next archive call on that thread.
// Call them on the same thread as the failing call.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_last_error(char* buf, int buf_len);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_get_error(char* buf, int buf_len);

// ── Block codec (reuses wasm_api.hpp contract) ───────────────────────────────
// Window-size units differ by surface, deliberately: compress_block /
// compress2 / decompress2 take `win_size` in BYTES (values below), while
// CreateOptions-style options and openrar_stream_create take `window_log2`
// as log2 (1 -> 128 KiB ... 4 -> 1 MiB, 5 -> 4 MiB stream-only).
#define OPENRAR_WINDOW_128K (128u * 1024)
#define OPENRAR_WINDOW_256K (256u * 1024)
#define OPENRAR_WINDOW_512K (512u * 1024)
#define OPENRAR_WINDOW_1M (1024u * 1024)
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

// ── Resource limits (additive; v1.10.0) ──────────────────────────────────────
// Configures caller-imposed extraction limits for an open archive handle.
// Any limit set to UINT64_MAX is considered unlimited.
// Scope note: the cumulative total-output counter (max_total_bytes) spans the
// WHOLE handle lifetime and is deliberately never reset — call set_limits
// again with new thresholds to raise/adjust it, or close and reopen the
// handle to start a fresh accounting (v1.21.1 documentation).
// Returns RAR_OK on success, RAR_ERR_INVALID_ARG if handle is invalid,
// or RAR_ERR_BUSY if an operation is currently executing on the handle.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_set_limits(uint32_t handle,
                                                                       uint64_t max_member_bytes,
                                                                       uint64_t max_total_bytes,
                                                                       uint64_t max_header_count,
                                                                       uint64_t max_header_bytes);

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
OPENRAR_DLL_API uint32_t OPENRAR_DLL_CALL openrar_archive_open_file(const char* arc_path,
                                                                    const char* password_utf8,
                                                                    openrar_progress_cb progress,
                                                                    openrar_cancel_cb cancel,
                                                                    void* user);

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
    uint32_t handle, uint32_t entry_index, const char* dest_path, openrar_progress_cb progress,
    openrar_cancel_cb cancel, void* user);

// ── Random-read region export (v1.25.0, OPENRAR_ABI_FEATURE_MMAP) ───────────
// Reads `length` bytes of an entry's STORED payload starting at `offset`
// into `out_buf` (caller-allocated, `out_len` capacity). Decompressed
// entries are served through the decompressor for the requested range.
// `out_len` receives the bytes actually read; a value < `length` signals
// truncation or EOF. When the requested range covers the whole payload and
// the entry carries a checksum, it is verified (RAR_ERR_CRC_MISMATCH on
// mismatch); partial ranges are unverified by contract. The backing engine
// is the mapped read view when available, buffered reads otherwise —
// identical results either way (SECURITY_ARCHITECTURE §5.2: the view is
// never the extraction input).
// Returns RAR_OK, RAR_ERR_INVALID_ARG (handle/index/offset/length/buffer),
// RAR_ERR_UNSUPPORTED_FEATURE (compressed entries cannot be range-read —
// use extract_to_path), RAR_ERR_IO, RAR_ERR_BAD_PASSWORD / RAR_ERR_ENCRYPTED
// (encrypted entries are not range-readable), or RAR_ERR_TRUNCATED.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_read_entry_region(
    uint32_t handle, uint32_t entry_index, uint64_t offset, uint32_t length, void* out_buf,
    size_t out_len, size_t* out_len_written);

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
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_test(uint32_t handle,
                                                                 uint32_t entry_index,
                                                                 openrar_progress_cb progress,
                                                                 openrar_cancel_cb cancel,
                                                                 void* user);

// ── Archive mutation (free functions; additive) ─────────────────────────────
// Atomic write-path operations over an existing archive on disk, backed by
// the same engine the CLI uses: the rewrite lands in a temp file in the
// archive's directory, is flushed to disk, and atomically replaces the
// original — the archive on disk is untouched on any failure. Deliberately
// NOT handle methods: a mutation rewrites the archive and invalidates every
// cached walk, so these are free functions and hosts re-open afterwards.
//
// Index space (delete): indices refer to the FILE-HANDLE LISTING ORDER — the
// sequence openrar_archive_handle_list reports on an open_file handle: file
// entries only, service headers (CMT/RR/QO) never exposed. The frozen
// one-shot list_file* walk surfaces comment/recovery service blocks as
// entries, so on archives carrying those it is NOT the same index space.
// The DLL translates each index to the entry's header offset with its own
// strict reader and deletes by that identity — never by name, so entry
// names containing '*' or '?' are safe. Indices shift after every
// mutation; re-list.
//
// Handle collision: before touching the archive, both exports check every
// open file-mode handle in this process — if any holds the target (or a
// volume of its set) open, the call fails up front with RAR_ERR_BUSY (-14)
// instead of a sharing violation mid-rename. Cross-process writers are
// outside this check: a file mutated under an open handle makes that
// handle's reads fail with RAR_ERR_IO — close and re-open.

// Delete entries by index. count must be > 0 and every index < the
// handle-listing entry count (out of range: RAR_ERR_INVALID_ARG, detail
// "entry index N out of range (archive has M entries)").
//
// Solid archives allow suffix-shaped deletes only (docs/invariants.md §1):
// deleting a member of a solid run while any later member of that run is
// retained would silently corrupt the LZ chain and fails with
// RAR_ERR_UNSUPPORTED_FEATURE — detail "cannot delete head of solid block
// without recompressing chain" for a run head, "cannot delete entries from
// solid archive without recompressing chain" for a mid-run member.
// Untouched runs, tail (suffix) deletions and whole-run deletions are
// allowed, and the archive stays loadable after every refusal.
//
// Refused up front with RAR_ERR_UNSUPPORTED_FEATURE: locked archives
// (MHFL_LOCK), multi-volume sets (MHFL_VOLUME) and header-encrypted (-hp)
// archives (detail "mutating header-encrypted archive requires password" —
// the mutation surface takes no password). The archive comment (CMT) is
// preserved; QuickOpen locators are stripped; a recovery record (RR) is
// copied verbatim and NOT recomputed — treat it as absent after a
// mutation. RAR_ERR_IO when arc_path cannot be read or rewritten.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_delete_entries_file(
    const char* arc_path, const uint32_t* entry_indices, uint32_t count);

// Batch add/replace with 'u' semantics: incoming entries are appended at
// the end in src_paths order, and every existing entry whose name equals an
// incoming arc_name is replaced by it — byte-exact UTF-8 compare after
// normalizing '\' to '/' in arc_names, with ALL prior instances of the name
// stripped. method ∈ {0,3,5}; window_log2 ∈ [1,4] (create parity; ignored
// by method 0). A src_path that names a directory becomes a directory
// record (non-recursive — enumerate contents yourself; empty directories
// are preserved). Added files are written unencrypted: password /
// encrypt_headers parameters are deliberately not exposed yet.
//
// Replacing an entry that belongs to a solid block — including its head —
// is rejected with RAR_ERR_UNSUPPORTED_FEATURE ("cannot replace entry in
// solid archive without recompressing chain"); replace the tail of a run
// via delete + add instead (this asymmetry is intentional: replacement
// strips historical entries by name, deletion verifies index boundaries).
// Adding NEW names to a solid archive continues its stream. The existing
// archive comment (CMT) is kept; QuickOpen locators are stripped.
//
// Errors: RAR_ERR_INVALID_ARG (null arc_path/src_paths/arc_names,
// file_count == 0, method ∉ {0,3,5}, window_log2 ∉ [1,4]); RAR_ERR_IO
// (arc_path or a src_path missing/unreadable, write failure); RAR_ERR_BUSY
// (open file-mode handle on the archive); RAR_ERR_UNSUPPORTED_FEATURE
// (locked, multi-volume, header-encrypted, or solid replacement as above).
// Atomic like delete: the archive on disk is replaced only after every
// file in the batch has been written.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_add_files_file(const char* arc_path,
                                                                    const char* const* src_paths,
                                                                    const char* const* arc_names,
                                                                    uint32_t file_count, int method,
                                                                    uint32_t window_log2);

// ── Native archive creation (free functions; additive; v1.14.0) ─────────────
// Creates a new RAR5 archive on disk directly from source files, without
// requiring an existing archive.
//
// Parameters:
//   arc_path       — destination archive path (.rar).
//   src_paths      — array of source file/directory paths on disk.
//   arc_names      — array of entry names inside the archive ('\' normalized to '/').
//   file_count     — count of entries in src_paths / arc_names (must be > 0).
//   method         — compression level 0..5 (0=store, 1=fastest, 2=fast, 3=normal, 4=good, 5=best).
//   dict_size      — dictionary window size: 0 for tuned defaults per method (8 MB for -m3,
//                    64 MB for -m5), 1..15 for legacy log2 (128 KiB..2 GiB), > 15 for exact bytes
//                    (up to 64 GiB on 64-bit platforms).
//
// openrar_archive_create_file_ex adds optional encryption and solid chaining:
//   password_utf8   — UTF-8 password string, or NULL / "" for no encryption.
//   encrypt_headers — non-zero to encrypt headers (-hp; requires non-empty password).
//   solid           — non-zero to compress entries in a solid LZ chain (-s).
//   progress/cancel — optional callbacks covering file commit and cancellation.
//
// Durability: writes to arc_path + ".openrar-tmp.<pid>.<seq>", flushes, and
// atomically replaces arc_path on completion. On abort or failure, the temp
// file is cleaned up and arc_path is untouched.
//
// Errors: RAR_ERR_INVALID_ARG on null args / empty batch / method not in 0..5;
// RAR_ERR_BUSY (-14) if an open file-mode handle in this process holds arc_path;
// Filter flags for openrar_archive_create_file_opts or filter configuration (bit 10):
#define OPENRAR_FILTER_DEFAULT 0u
#define OPENRAR_FILTER_DISABLE_ALL (1u << 0)   // Force disable all filters (-mc-)
#define OPENRAR_FILTER_FORCE_E8 (1u << 1)      // Force x86 E8/E8E9 filter (-mcE+)
#define OPENRAR_FILTER_DISABLE_E8 (1u << 2)    // Disable x86 E8/E8E9 filter (-mcE-)
#define OPENRAR_FILTER_FORCE_ARM (1u << 3)     // Force ARM BL filter (-mcA+)
#define OPENRAR_FILTER_DISABLE_ARM (1u << 4)   // Disable ARM BL filter (-mcA-)
#define OPENRAR_FILTER_FORCE_DELTA (1u << 5)   // Force delta filter (-mcD+)
#define OPENRAR_FILTER_DISABLE_DELTA (1u << 6) // Disable delta filter (-mcD-)

OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_create_file(const char* arc_path,
                                                                 const char* const* src_paths,
                                                                 const char* const* arc_names,
                                                                 uint32_t file_count, int method,
                                                                 uint64_t dict_size);

OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_create_file_ex(
    const char* arc_path, const char* const* src_paths, const char* const* arc_names,
    uint32_t file_count, int method, uint64_t dict_size, const char* password_utf8,
    int encrypt_headers, int solid, openrar_progress_cb progress, openrar_cancel_cb cancel,
    void* user);

OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_create_file_opts(
    const char* arc_path, const char* const* src_paths, const char* const* arc_names,
    uint32_t file_count, int method, uint64_t dict_size, const char* password_utf8,
    int encrypt_headers, int solid, uint32_t filter_flags, openrar_progress_cb progress,
    openrar_cancel_cb cancel, void* user);

OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_create_file_opts_mt(
    const char* arc_path, const char* const* src_paths, const char* const* arc_names,
    uint32_t file_count, int method, uint64_t dict_size, const char* password_utf8,
    int encrypt_headers, int solid, uint32_t filter_flags, uint32_t threads,
    openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);

// ── Extended metadata (file-mode handles; additive) ─────────────────────────
// The 64-byte entry struct is frozen (shared WASM contract); these queries
// surface what it drops, straight from the archive headers. File-mode
// handles only — buffer handles return RAR_ERR_UNSUPPORTED_FEATURE.

// Bit flags for openrar_entry_ex_t.flags.
#define OPENRAR_ENTRY_FLAG_SOLID (1u << 0)
#define OPENRAR_ENTRY_FLAG_ENCRYPTED (1u << 1)
#define OPENRAR_ENTRY_FLAG_REDIR (1u << 2)
#define OPENRAR_ENTRY_FLAG_SPLIT_BEFORE (1u << 3)
#define OPENRAR_ENTRY_FLAG_SPLIT_AFTER (1u << 4)
#define OPENRAR_ENTRY_FLAG_HAS_MTIME (1u << 5)
#define OPENRAR_ENTRY_FLAG_HAS_CTIME (1u << 6)
#define OPENRAR_ENTRY_FLAG_HAS_ATIME (1u << 7)
#define OPENRAR_ENTRY_FLAG_DIRECTORY (1u << 8)
#define OPENRAR_ENTRY_FLAG_HAS_VERSION (1u << 9)
#define OPENRAR_ENTRY_FLAG_HAS_OWNER (1u << 10)

#pragma pack(push, 1)
typedef struct {
    uint32_t attrs;          // host-OS attributes (FILE_ATTRIBUTE_* when host_os == 0)
    uint32_t host_os;        // 0 = Windows, 1 = Unix
    uint64_t mtime_ft;       // FILETIME (UTC, 100ns); 0 = not stored (check HAS_MTIME)
    uint64_t ctime_ft;       // FILETIME (UTC, 100ns); 0 = not stored (check HAS_CTIME)
    uint64_t atime_ft;       // FILETIME (UTC, 100ns); 0 = not stored (check HAS_ATIME)
    uint32_t flags;          // Bitmask of OPENRAR_ENTRY_FLAG_*
    uint32_t win_size;       // dictionary bytes, 0 if n/a
    uint32_t redir_type;     // 0 none / 1 unixsymlink / 2 winsymlink / 3 junction /
                             // 4 hardlink / 5 filecopy
    uint32_t version_needed; // unp_ver format level (0 = RAR5, 1 = RAR7)
} openrar_entry_ex_t;        // 48 bytes, fixed size
#pragma pack(pop)

// Query extended metadata for entry_index (same index space as
// openrar_archive_handle_list). Timestamps come from FHEXTRA_HTIME records:
// a FILETIME-format record is reported as stored; a unix-format record is
// converted (seconds since 1601 + nanoseconds*100). A timestamp absent from
// the archive is 0 with its HAS_* flag clear — the 32-bit DOS-time mtime of
// the frozen 64-byte entry is NOT repeated here. SPLIT_BEFORE/AFTER refer
// to the logical merged entry on multi-volume sets.
//
// extra_out receives a malloc'd NUL-terminated UTF-8 string containing the
// redirection target path when redir_type != 0 (*extra_size_out is the byte
// length excluding the NUL); free it with openrar_archive_entry_ex_free or
// openrar_free. If the entry has no redirection, *extra_out is NULL and
// *extra_size_out is 0. On failure (including RAR_ERR_UNSUPPORTED_FEATURE
// on a buffer handle) nothing is allocated.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_entry_ex(uint32_t handle,
                                                                     uint32_t entry_index,
                                                                     openrar_entry_ex_t* out,
                                                                     void** extra_out,
                                                                     size_t* extra_size_out);

// Free helper for extra_out (openrar_free is also valid).
OPENRAR_DLL_API void OPENRAR_DLL_CALL openrar_archive_entry_ex_free(void* extra);

// ── POSIX ownership (v1.17.0) ────────────────────────────────────────────────
#define OPENRAR_OWNER_FLAG_HAS_UID (1u << 0)
#define OPENRAR_OWNER_FLAG_HAS_GID (1u << 1)
#define OPENRAR_OWNER_FLAG_HAS_USER (1u << 2)
#define OPENRAR_OWNER_FLAG_HAS_GROUP (1u << 3)

#pragma pack(push, 1)
typedef struct {
    uint64_t uid;
    uint64_t gid;
    uint32_t flags;      // Bitmask of OPENRAR_OWNER_FLAG_*
} openrar_entry_owner_t; // 20 bytes packed
#pragma pack(pop)

// Query POSIX ownership information (FHEXTRA_OWNER) for entry_index.
// If the entry carries ownership records, owner_out receives the numeric UID/GID
// and flags, and if username_out/groupname_out are non-NULL, they receive malloc'd
// NUL-terminated UTF-8 strings (free with openrar_archive_entry_owner_free).
// If the entry has no owner extra, owner_out receives 0s, strings receive NULL,
// and RAR_OK is returned.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_entry_owner(
    uint32_t handle, uint32_t entry_index, openrar_entry_owner_t* owner_out, char** username_out,
    char** groupname_out);

// Free helper for username_out and groupname_out strings (openrar_free is also valid).
OPENRAR_DLL_API void OPENRAR_DLL_CALL openrar_archive_entry_owner_free(char* str);

// Archive-level properties of the handle's volume set.
#pragma pack(push, 1)
typedef struct {
    uint32_t flags;         // main-header flags (MHFL_VOLUME, MHFL_SOLID, MHFL_LOCK, ...)
    uint32_t volume_index;  // 0-based volume number (0 if not multi-volume)
    uint32_t volume_count;  // total volumes of the set; the file-mode open is
                            // strict, so a successfully opened set always has
                            // every volume scanned (1 for single-volume)
    uint64_t recovery_size; // recovery record (RR service) size in bytes, 0 if none
    uint32_t comment_len;   // archive comment length in bytes, 0 if none
} openrar_archive_info_t;   // 24 bytes, fixed size
#pragma pack(pop)

// Query archive-level properties. If comment_len > 0, comment_out receives a
// malloc'd UTF-8 string with the archive comment payload (read lazily from
// the CMT service header at query time — stored compressed comments are
// decompressed, and a payload that fails its CRC or decode is reported as
// absent with RAR_OK). Capped at 16 MiB: comment payloads exceeding 16 MiB
// return RAR_ERR_UNSUPPORTED_FEATURE to protect against speculative memory allocation.
// If the archive has no comment, *comment_out is NULL
// and *comment_size_out is 0; free a returned comment with openrar_free.
// RAR_ERR_IO when the lazy payload read hits a filesystem error. Buffer
// handles return RAR_ERR_UNSUPPORTED_FEATURE.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_handle_info(uint32_t handle,
                                                                 openrar_archive_info_t* out,
                                                                 void** comment_out,
                                                                 size_t* comment_size_out);

// ── Archive Repair (v1.11.0, additive) ──────────────────────────────────────
// Repairs corrupted or missing archive volumes via inline Recovery Records (RR)
// or external Cauchy Reed-Solomon parity volumes (.rev).
// Returns RAR_OK (0) on success, or a negative RAR_ERR_* code on failure.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_repair(const char* arc_path,
                                                            openrar_progress_cb progress,
                                                            openrar_cancel_cb cancel, void* user);

// ── Recovery Volume & Record Creation (v1.20.0, additive) ───────────────────
// Generates external Reed-Solomon (.rev) recovery volumes for a multi-volume set.
// count_or_percent: number of .rev volumes (if is_percent == 0) or percentage (if is_percent != 0).
// threads: number of worker threads (1 = serial).
// Returns RAR_OK (0) on success, or a negative RAR_ERR_* code on failure.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_create_rev_volumes(
    const char* arc_path, uint32_t count_or_percent, int is_percent, unsigned int threads,
    openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);

// Appends an inline Recovery Record (RR) service block to a single-volume archive.
// percent: recovery percentage (1..1000).
// threads: number of worker threads (1 = serial).
// Returns RAR_OK (0) on success, or a negative RAR_ERR_* code on failure.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_add_recovery_record(
    const char* arc_path, uint32_t percent, unsigned int threads, openrar_progress_cb progress,
    openrar_cancel_cb cancel, void* user);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // OPENRAR_DLL_OPENRAR_DLL_H
