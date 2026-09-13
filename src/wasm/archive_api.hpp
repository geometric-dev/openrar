#ifndef OPENRAR_WASM_ARCHIVE_API_HPP
#define OPENRAR_WASM_ARCHIVE_API_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/wasm/archive_api.hpp — C ABI surface for the in-memory RAR5 reader/writer
// ─────────────────────────────────────────────────────────────────────────────
//
//  Consumed by:
//    - src/wasm/archive_api.cpp (this implementation)
//    - tests/unit/archive_api_tests.cpp (round-trip tests on every CI matrix;
//      runs without emsdk, so the C ABI is exercised end-to-end natively)
//
//  Distinct from src/wasm/wasm_api.{hpp,cpp} (block codec). The block codec
//  is `wasm/dist/openrar.js`; the archive API is `wasm/dist/openrar_archive.js`.
//  See docs/wasm-archive-spec.md for the full contract.
//
//  WASM ABI_VERSION for this module is independent of the block codec's.
//  v1: initial.
// ─────────────────────────────────────────────────────────────────────────────

#include "../api/abi_contract.hpp"
#include "../core/types.hpp"
#include <cstddef>
#include <cstdint>

namespace openrar::wasm {

// ABI version of the archive module. Independent of the block codec's
// WASM_API_VERSION (those live in different wasm/dist/ artifacts).
//   v1: initial.
//   v2: export set corrected (_openrar_archive_list_free), struct-based
//       create with mtime (create2), progress/cancel hooks (extract_all2,
//       handle_extract_all2), openrar_archive_last_error_code.
constexpr int ARCHIVE_WASM_API_VERSION = 2;


// Error codes and the 64-byte ArchiveEntryOut layout are the shared ABI
// contract (src/api/abi_contract.hpp) — canonical in buffer_archive.hpp,
// re-exported here so existing openrar::wasm::RAR_* / ArchiveEntryOut users
// (native tests, JS-facing docs) keep compiling unchanged.
using RarError = openrar::api::RarError;
using openrar::api::RAR_ERR_ABORTED;
using openrar::api::RAR_ERR_BAD_PASSWORD;
using openrar::api::RAR_ERR_CRC_MISMATCH;
using openrar::api::RAR_ERR_INVALID_ARG;
using openrar::api::RAR_ERR_IO;
using openrar::api::RAR_ERR_NOMEM;
using openrar::api::RAR_ERR_NOT_RAR;
using openrar::api::RAR_ERR_PARTIAL_OK;
using openrar::api::RAR_ERR_TRUNCATED;
using openrar::api::RAR_ERR_UNSUPPORTED_FEATURE;
using openrar::api::RAR_OK;
using ArchiveEntryOut = openrar::api::ArchiveEntryOut;

// Maximum dictionary window we accept. MUST stay uint64_t (see
// api::MAX_WIN_SIZE): as size_t it truncates to 0 on wasm32.
constexpr uint64_t ARCHIVE_MAX_WIN_SIZE = openrar::api::MAX_WIN_SIZE;

// ── v2: hooks + structured create ────────────────────────────────────────────
// Progress callback. (done, total) cumulative and monotonic; total never 0
// unless the work is empty. Called once per entry (create + extract_all).
// Parameter order mirrors openrar::archive::progress_cb. i64 parameters
// cross the JS boundary as BigInt (module built with WASM_BIGINT=1) —
// addFunction signature "vijj".
typedef void (*openrar_progress_cb)(uint64_t done, uint64_t total, void* user);
// Cancel callback: polled once per entry; return non-zero to abort with
// RAR_ERR_ABORTED. addFunction signature "ii".
typedef int (*openrar_cancel_cb)(void* user);

struct ArchiveHooks {
    openrar_progress_cb progress; // may be null
    void* progress_user;
    openrar_cancel_cb cancel; // may be null
    void* cancel_user;
};

// Input file for openrar_archive_create2. All pointers are WASM-heap
// addresses. Layout (wasm32, little-endian):
//   0: path (const char*, NUL-terminated UTF-8)
//   4: data (const uint8_t*, may be null when data_len == 0)
//   8: data_len (size_t)
//  12: (padding)
//  16: mtime_unix (uint64_t) — 0 ⇒ now()
//  24: is_dir (uint8_t) — when set, path must end in '/' and data_len must be 0
//  25: reserved[3] — must be zero
// sizeof == 32, alignof == 8.
struct alignas(8) ArchiveInputFile {
    const char* path;
    const uint8_t* data;
    size_t data_len;
    uint64_t mtime_unix;
    uint8_t is_dir;
    uint8_t reserved[3];
};
// The 32-byte layout is the wasm32 ABI (4-byte pointers). On 64-bit hosts
// pointers widen and the struct grows — the assert only applies where the
// ABI is defined.
#if defined(__wasm__) || defined(__EMSCRIPTEN__)
static_assert(sizeof(ArchiveInputFile) == 32, "ArchiveInputFile must stay 32 bytes");
#endif

// Options for openrar_archive_create2. NULL ⇒ method 3, window_log2 4.
struct ArchiveCreateOpts {
    int32_t method;       // 0, 3, or 5 (else RAR_ERR_INVALID_ARG)
    uint32_t window_log2; // 1..4 → 128 KiB / 256 KiB / 512 KiB / 1 MiB
};


// Validate an archive entry path against the documented contract.
// Returns RAR_ERR_INVALID_ARG on failure (and writes a message to
// err_buf); RAR_OK otherwise.
int validate_archive_path_c(const char* path, size_t path_len, int is_dir, char* err_buf,
                            size_t err_buf_len);

} // namespace openrar::wasm

// ── C ABI (extern "C", no name mangling) ────────────────────────────────────
extern "C" {

int openrar_archive_version(void);

// Parse a single-volume RAR5 archive from a buffer. On RAR_OK, *count is
// set to the number of entries and *entries_ptr is set to a heap pointer
// describing them. The caller must free with openrar_free. The strings
// pointed to by path_offset within each entry live in *paths_ptr; free
// with one openrar_free call.
int openrar_archive_list(const uint8_t* data, size_t size, uint32_t* count,
                         void** entries_out, // pointer to ArchiveEntryOut[]
                         void** paths_out,   // contiguous UTF-8 buffer
                         size_t* paths_size_out);

// Free a buffer previously returned by openrar_archive_list. Safe to call
// with NULL.
void openrar_archive_list_free(void* entries, void* paths, size_t paths_size);

// Decompress a single entry by index into a freshly-allocated buffer.
// On RAR_OK, *out_ptr and *out_len are set and must be freed via
// openrar_free.
int openrar_archive_extract(const uint8_t* data, size_t size, uint32_t entry_index,
                            uint8_t** out_ptr, size_t* out_len);

// Decompress every entry in the archive into a single WASM-heap buffer.
// On RAR_OK, *buf_out_ptr / *buf_size_out / *offsets_out_ptr /
// *offsets_count_out are set. Offsets table is an array of
// { uint64 offset, uint64 size } per entry. Caller frees buf with
// openrar_free and offsets with openrar_free.
int openrar_archive_extract_all(const uint8_t* data, size_t size, uint8_t** buf_out_ptr,
                                size_t* buf_size_out, uint64_t** offsets_out_ptr,
                                uint32_t* offsets_count_out);

// Create a single-volume RAR5 archive from in-memory inputs. `files_arr`
// is an array of `const uint8_t*` pointers (file payloads); `paths_arr`
// is an array of `const uint8_t*` UTF-8 paths. On RAR_OK, *out_ptr and
// *out_len are set and must be freed via openrar_free.
int openrar_archive_create(const uint8_t* const* paths_arr, const uint8_t* const* data_arr,
                           const size_t* sizes_arr, uint32_t file_count, int method,
                           uint32_t window_log2, uint8_t** out_ptr, size_t* out_len);

// Returns the last error message set by the most recent archive C ABI
// call in this thread, copied into `buf` (up to `buf_len - 1` chars, then
// NUL-terminated). Returns the number of chars written (excluding NUL).
// Returns 0 if no error is set. Useful for surfacing human-readable context
// alongside numeric error codes.
int openrar_archive_get_error(char* buf, int buf_len);

// ── v2 entry points ──────────────────────────────────────────────────────────
// Struct-based create. files_arr is an array of ArchiveInputFile (heap
// addresses). opts may be null (method 3, window_log2 4). hooks may be null.
// On RAR_OK, *out_ptr/*out_len are malloc'd; free with openrar_archive_free.
int openrar_archive_create2(const openrar::wasm::ArchiveInputFile* files_arr, uint32_t file_count,
                            const openrar::wasm::ArchiveCreateOpts* opts,
                            const openrar::wasm::ArchiveHooks* hooks, uint8_t** out_ptr,
                            size_t* out_len);

// extract_all with progress/cancel hooks. Same outputs as
// openrar_archive_extract_all.
int openrar_archive_extract_all2(const uint8_t* data, size_t size,
                                 const openrar::wasm::ArchiveHooks* hooks, uint8_t** buf_out_ptr,
                                 size_t* buf_size_out, uint64_t** offsets_out_ptr,
                                 uint32_t* offsets_count_out);

// Handle-based variant of extract_all2 (no archive re-scan).
int openrar_archive_handle_extract_all2(uint32_t handle, const openrar::wasm::ArchiveHooks* hooks,
                                        uint8_t** buf_out_ptr, size_t* buf_size_out,
                                        uint64_t** offsets_out_ptr, uint32_t* offsets_count_out);

// Numeric error code (a RarError value) of the most recent archive C ABI
// call in this thread. RAR_OK if the last call succeeded. Unlike
// openrar_archive_get_error, this never loses the specific code — callers
// no longer have to guess (e.g. openrar_archive_open returning 0).
int openrar_archive_last_error_code(void);

// ── Handle API (avoids double scan for list → extract flows) ───────────────
// One-shot helpers above re-scan on each call. For sequential access
// (list then extract the same archive) use the handle API: open once,
// then call handle_list / handle_extract without re-scanning.
uint32_t openrar_archive_open(const uint8_t* data, size_t size);
void openrar_archive_close(uint32_t handle);
int openrar_archive_handle_list(uint32_t handle, uint32_t* count, void** entries_out,
                                void** paths_out, size_t* paths_size_out);
int openrar_archive_handle_extract(uint32_t handle, uint32_t entry_index, uint8_t** out_ptr,
                                   size_t* out_len);
int openrar_archive_handle_extract_all(uint32_t handle, uint8_t** buf_out_ptr, size_t* buf_size_out,
                                       uint64_t** offsets_out_ptr, uint32_t* offsets_count_out);

// Allocate / free WASM heap memory. Same semantics as the block codec's
// openrar_alloc / openrar_free. Returned pointers must be freed with the
// matching free; mixing with malloc/free is undefined.
void* openrar_archive_alloc(size_t bytes);
void openrar_archive_free(void* ptr);

} // extern "C"

#endif // OPENRAR_WASM_ARCHIVE_API_HPP
