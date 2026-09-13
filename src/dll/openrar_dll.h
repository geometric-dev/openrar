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
#define OPENRAR_DLL_API_VERSION 1

OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_version(void);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_archive_version(void);

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
    RAR_ERR_ABORTED = -11
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

// ── Handle API (scan-once; avoids double scan for list → extract) ───────────
OPENRAR_DLL_API uint32_t OPENRAR_DLL_CALL openrar_archive_open(const uint8_t* data, size_t size);
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
typedef void(OPENRAR_DLL_CALL* openrar_progress_cb)(void* user, uint64_t done, uint64_t total);
typedef int(OPENRAR_DLL_CALL* openrar_cancel_cb)(void* user);
// openrar_set_progress/openrar_set_cancel were removed (report L12/I19): they
// accepted a callback, returned RAR_OK and did nothing. Callbacks are only
// wired at the streaming layer — use openrar_stream_set_progress /
// openrar_stream_set_cancel below.
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_stream_set_progress(uint32_t handle,
                                                                 openrar_progress_cb cb,
                                                                 void* user);
OPENRAR_DLL_API int OPENRAR_DLL_CALL openrar_stream_set_cancel(uint32_t handle,
                                                               openrar_cancel_cb cb, void* user);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // OPENRAR_DLL_OPENRAR_DLL_H
