#include "wasm_api.hpp"
#include "../compress/compressor50.hpp"
#include "../compress/decompressor50.hpp"
#include "../compress/stream_encoder.hpp"
#include "../compress/stream_decoder.hpp"

#include <exception>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace openrar::wasm {

bool compress_buffer_raw(const core::byte* src, size_t src_len, std::vector<core::byte>& out,
                         int method, size_t win_size) {
    if (!src && src_len != 0) return false;
    if (src_len == 0) {
        out.clear();
        return true;
    }
    if (win_size == 0) win_size = 0x200000;
    if (win_size > MAX_WIN_SIZE) return false; // §3.2 input validation
    // Clamp method 0..5.
    if (method < 0) method = 3;
    if (method > 5) method = 5;
    if (method == 0) {
        // STORE = passthrough. Caller is responsible for distinguishing
        // stored blocks from compressed blocks on the receiving end.
        out.assign(src, src + src_len);
        return true;
    }
    return openrar::compress::Compressor50::compress_buffer(src, src_len, out, method, win_size);
}

bool decompress_buffer_raw(const core::byte* src, size_t src_len, std::vector<core::byte>& out,
                           size_t win_size) {
    if (!src && src_len != 0) return false;
    if (src_len == 0) {
        out.clear();
        return true;
    }
    if (win_size == 0) win_size = 1024 * 1024;
    if (win_size > MAX_WIN_SIZE) return false; // §3.2 input validation
    openrar::compress::Decompressor50 dec(win_size);
    // §3.3: Decompressor50 may write partial output to the destination
    // vector before deciding the bit-stream is corrupt. Save the prior size
    // and clear on failure so callers never see garbage.
    const size_t prior = out.size();
    const bool ok = dec.decompress_to_vector(src, src_len, out);
    if (!ok) out.resize(prior);
    return ok;
}

} // namespace openrar::wasm

// ── C ABI ────────────────────────────────────────────────────────────────────
extern "C" {

int openrar_version() {
    return openrar::wasm::WASM_API_VERSION;
}

void* openrar_alloc(size_t bytes) {
    return std::malloc(bytes);
}
void openrar_free(void* ptr) {
    std::free(ptr);
}

int openrar_compress(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len,
                     int method) {
    return openrar_compress2(src, src_len, out_ptr, out_len, method, 0x200000);
}

int openrar_compress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len,
                      int method, size_t win_size) {
    try {
        if (!out_ptr || !out_len) return 0;
        if (win_size > openrar::wasm::MAX_WIN_SIZE) return 0; // §3.2 input validation
        *out_ptr = nullptr;
        *out_len = 0;
        std::vector<openrar::core::byte> out;
        if (!openrar::wasm::compress_buffer_raw(src, src_len, out, method, win_size)) return 0;
        if (out.empty()) {
            *out_ptr = nullptr;
            *out_len = 0;
            return 1;
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) return 0;
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return 1;
    } catch (...) {
        return 0;
    }
}

int openrar_decompress(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len) {
    return openrar_decompress2(src, src_len, out_ptr, out_len, 1024 * 1024);
}

// -- Streaming encoder (additive, v2) -----------------------------------------
// A streaming compressor handle. feed() accepts input incrementally; finish()
// returns the concatenated block output, byte-identical to one-shot
// openrar_compress2() of the same input (tests/unit/stream_encoder_tests.cpp).
// Handles are single-threaded; the shared contract table guards ids and
// keeps pinned encoders alive (openrar_stream_free may run concurrently).
namespace openrar::wasm {
namespace {
openrar::api::HandleTable<openrar::compress::StreamEncoder> g_streams;
} // namespace
} // namespace openrar::wasm

uint32_t openrar_stream_create(int method, size_t win_size) {
    try {
        if (win_size == 0) win_size = 4 * 1024 * 1024;
#if defined(__EMSCRIPTEN__) || defined(__wasm__)
        if (win_size > 64ULL * 1024 * 1024) return 0; // Bound WASM allocations to <= 64 MiB
#else
        if (win_size > openrar::wasm::MAX_WIN_SIZE) return 0;
#endif
        auto enc = std::make_shared<openrar::compress::StreamEncoder>(method, win_size);
        return openrar::wasm::g_streams.insert(enc);
    } catch (...) {
        return 0;
    }
}

uint32_t openrar_stream_compress_new(int method, size_t win_size) {
    return openrar_stream_create(method, win_size);
}

int openrar_stream_feed(uint32_t handle, const uint8_t* src, size_t n) {
    try {
        if (!src && n != 0) return -1;
        auto enc = openrar::wasm::g_streams.pin(handle);
        if (!enc) return -1;
        return enc->feed(src, n) ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

int openrar_stream_compress_feed(uint32_t handle, const uint8_t* src, size_t n) {
    return openrar_stream_feed(handle, src, n);
}

int openrar_stream_pull(uint32_t handle, uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!out_ptr || !out_len) return -1;
        *out_ptr = nullptr;
        *out_len = 0;
        auto enc = openrar::wasm::g_streams.pin(handle);
        if (!enc) return -1;
        std::vector<uint8_t> out;
        if (!enc->take_output(out)) return -1;
        if (out.empty()) return 0;
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) return -1;
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return 0;
    } catch (...) {
        return -1;
    }
}

int openrar_stream_compress_pull(uint32_t handle, uint8_t** out_ptr, size_t* out_len) {
    return openrar_stream_pull(handle, out_ptr, out_len);
}

int openrar_stream_finish(uint32_t handle, uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!out_ptr || !out_len) return -1;
        *out_ptr = nullptr;
        *out_len = 0;
        auto enc = openrar::wasm::g_streams.pin(handle);
        if (!enc) return -1;
        std::vector<uint8_t> out;
        if (!enc->finish(out)) return -1;
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size() > 0 ? out.size() : 1));
        if (!buf && !out.empty()) return -1;
        if (!out.empty()) std::memcpy(buf, out.data(), out.size());
        *out_ptr = out.empty() ? nullptr : buf;
        *out_len = out.size();
        if (out.empty()) std::free(buf);
        return 0;
    } catch (...) {
        return -1;
    }
}

int openrar_stream_compress_finish(uint32_t handle, uint8_t** out_ptr, size_t* out_len) {
    return openrar_stream_finish(handle, out_ptr, out_len);
}

void openrar_stream_free(uint32_t handle) {
    openrar::wasm::g_streams.erase(handle);
}

void openrar_stream_compress_free(uint32_t handle) {
    openrar_stream_free(handle);
}

int openrar_decompress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len,
                        size_t win_size) {
    try {
        if (!out_ptr || !out_len) return 0;
        if (win_size > openrar::wasm::MAX_WIN_SIZE) return 0; // §3.2 input validation
        *out_ptr = nullptr;
        *out_len = 0;
        std::vector<openrar::core::byte> out;
        if (!openrar::wasm::decompress_buffer_raw(src, src_len, out, win_size)) {
            // §3.3: out is already cleared by decompress_buffer_raw on failure.
            return 0;
        }
        if (out.empty()) {
            *out_ptr = nullptr;
            *out_len = 0;
            return 1;
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) return 0;
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return 1;
    } catch (...) {
        return 0;
    }
}

// -- Streaming decoder (v1.11) -----------------------------------------------
namespace openrar::wasm {
namespace {
openrar::api::HandleTable<openrar::compress::StreamDecoder> g_stream_decoders;
} // namespace
} // namespace openrar::wasm

uint32_t openrar_stream_decompress_create(size_t win_size) {
    try {
        if (win_size == 0) win_size = 1024 * 1024;
        if (win_size > openrar::wasm::MAX_WIN_SIZE) return 0;
        auto dec = std::make_shared<openrar::compress::StreamDecoder>(win_size);
        if (!dec->is_valid()) return 0;
        return openrar::wasm::g_stream_decoders.insert(dec);
    } catch (...) {
        return 0;
    }
}

int openrar_stream_decompress_feed(uint32_t handle, const uint8_t* src, size_t n) {
    try {
        if (!src && n != 0) return -1;
        auto dec = openrar::wasm::g_stream_decoders.pin(handle);
        if (!dec) return -1;
        return dec->feed(src, n) ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

int openrar_stream_decompress_finish(uint32_t handle, uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!out_ptr || !out_len) return -1;
        *out_ptr = nullptr;
        *out_len = 0;
        auto dec = openrar::wasm::g_stream_decoders.pin(handle);
        if (!dec) return -1;
        std::vector<uint8_t> out;
        if (!dec->finish(out)) return -1;
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size() > 0 ? out.size() : 1));
        if (!buf && !out.empty()) return -1;
        if (!out.empty()) std::memcpy(buf, out.data(), out.size());
        *out_ptr = out.empty() ? nullptr : buf;
        *out_len = out.size();
        if (out.empty()) std::free(buf);
        return 0;
    } catch (...) {
        return -1;
    }
}

int openrar_stream_decompress_pull(uint32_t handle, uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!out_ptr || !out_len) return -1;
        *out_ptr = nullptr;
        *out_len = 0;
        auto dec = openrar::wasm::g_stream_decoders.pin(handle);
        if (!dec) return -1;
        std::vector<uint8_t> out;
        if (!dec->take_output(out)) return -1;
        if (out.empty()) return 0;
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) return -1;
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return 0;
    } catch (...) {
        return -1;
    }
}

void openrar_stream_decompress_free(uint32_t handle) {
    openrar::wasm::g_stream_decoders.erase(handle);
}

} // extern "C"

// No embind layer. The JS wrapper (wasm/js/openrar.js) drives the C ABI
// directly via ccall: one memcpy in (HEAPU8.set), one copy out (safeRead).
// Embind's register_vector<uint8_t> cannot accept typed arrays and
// marshals per element, so it was removed in WASM_API_VERSION 2.
