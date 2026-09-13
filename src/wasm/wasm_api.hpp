#ifndef OPENRAR_WASM_API_HPP
#define OPENRAR_WASM_API_HPP

#include "../api/abi_contract.hpp"
#include "../core/types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openrar::wasm {

// ABI version for JS feature detection.
//   v1: initial release. Reset to v1 because v2 was never shipped.
//   v2: embind layer removed — the module surface is the raw C ABI
//       (_openrar_* exports); the JS wrapper drives it via ccall.
constexpr int WASM_API_VERSION = 2;

// Maximum dictionary window we accept. The compressor caps at 4 GiB
// (RAR5 v0) / 1 TiB (RAR7 ExtraDist); we refuse anything beyond to keep
// the WASM heap bounded and prevent denial-of-service on hostile callers.
// MUST stay uint64_t (see api::MAX_WIN_SIZE): as size_t it truncates to 0
// on wasm32, which made this check reject every call in every wasm build.
constexpr uint64_t MAX_WIN_SIZE = openrar::api::MAX_WIN_SIZE;

// Block-level primitives (survive across rebuilds, used by both C and JS bindings)
bool compress_buffer_raw(const core::byte* src, size_t src_len, std::vector<core::byte>& out,
                         int method = 3, size_t win_size = 0x200000);

bool decompress_buffer_raw(const core::byte* src, size_t src_len, std::vector<core::byte>& out,
                           size_t win_size = 1024 * 1024);

} // namespace openrar::wasm

// C ABI exported to JS via ccall/cwrap (stable, no mangling)
extern "C" {

int openrar_version();

void* openrar_alloc(size_t bytes);
void openrar_free(void* ptr);

// Returns 1 on success, 0 on failure. On success *out_ptr/*out_len are malloc'd and must be free'd via openrar_free.
int openrar_compress(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len,
                     int method);
int openrar_decompress(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len);

int openrar_compress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len,
                      int method, size_t win_size);
int openrar_decompress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len,
                        size_t win_size);
}

#endif // OPENRAR_WASM_API_HPP
