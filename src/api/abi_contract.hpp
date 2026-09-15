#ifndef OPENRAR_API_ABI_CONTRACT_HPP
#define OPENRAR_API_ABI_CONTRACT_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/api/abi_contract.hpp — single source of truth for the types shared by
//  the native DLL (src/dll) and the WASM C ABI (src/wasm).
//
//  Both surfaces compile this header natively; nothing here may depend on
//  Emscripten. The canonical error enum lives in buffer_archive.hpp (the core
//  layer both ABIs sit on); this header re-exports it plus the stable entry
//  layout and the two infrastructure pieces both ABIs need (thread-local
//  error state, handle table).
//
//  Contract rules that live with these types:
//    - returned buffers are malloc'd and freed with the matching *_free
//    - malloc(0) is never relied upon: empty outputs are nullptr + len 0
//      (heap_dup enforces this; report M12)
//    - handle ids are nonzero; 0 means failure/absence
//    - host callbacks must never run while a handle-table lock is held
//      (report L12): pin the handle, release, then work
// ─────────────────────────────────────────────────────────────────────────────

#include "../archive/buffer_archive.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace openrar::api {

// Canonical error-code set. JS mirrors it as RarErrorCode
// (wasm/js/openrar-archive.d.ts); the DLL header re-declares the same values
// for C compatibility and dll_api.cpp asserts equivalence at compile time.
// The unscoped enum's enumerators live in openrar::archive (the enum's
// enclosing namespace), so they are re-exported here explicitly.
using RarError = openrar::archive::BufferArchiveError;
using openrar::archive::RAR_ERR_ABORTED;
using openrar::archive::RAR_ERR_BAD_PASSWORD;
using openrar::archive::RAR_ERR_BUSY;
using openrar::archive::RAR_ERR_CRC_MISMATCH;
using openrar::archive::RAR_ERR_ENCRYPTED;
using openrar::archive::RAR_ERR_MISSING_VOLUME;
using openrar::archive::RAR_ERR_INVALID_ARG;
using openrar::archive::RAR_ERR_IO;
using openrar::archive::RAR_ERR_NOMEM;
using openrar::archive::RAR_ERR_NOT_RAR;
using openrar::archive::RAR_ERR_PARTIAL_OK;
using openrar::archive::RAR_ERR_TRUNCATED;
using openrar::archive::RAR_ERR_UNSUPPORTED_FEATURE;
using openrar::archive::RAR_OK;

// Largest accepted dictionary window. MUST stay uint64_t: as size_t the
// 4 GiB value truncates to 0 on wasm32, which made "win > cap" reject every
// call in every wasm build while native builds passed.
inline constexpr uint64_t MAX_WIN_SIZE = 4ULL * 1024 * 1024 * 1024;

// Stable 64-byte list-entry layout shared by the DLL and WASM surfaces.
// All fields fixed-width — identical on every target. Consumers must verify
// their own declarations against this one (see dll_api.cpp static_asserts).
struct alignas(8) ArchiveEntryOut {
    uint32_t path_offset;  // byte offset of UTF-8 path within the paths buffer
    uint32_t path_len;     // path length in bytes (UTF-8)
    uint32_t is_dir;       // 0 or 1
    uint32_t method;       // 0..5
    uint32_t is_encrypted; // 0 or 1
    uint32_t crc32;        // 0 == UNVERIFIED; NOT a verified-zero CRC
    uint64_t size;         // uncompressed bytes
    uint64_t packed_size;  // compressed (or stored) bytes; 0 for dirs
    uint64_t mtime;        // UNIX seconds
    uint64_t _pad[2];      // reserved; consumers must zero
};
static_assert(sizeof(ArchiveEntryOut) == 64, "ArchiveEntryOut must stay 64 bytes");

// Copy `n` bytes into a fresh malloc'd buffer; n == 0 yields nullptr so the
// malloc(0)-may-return-NULL quirk can never be misread as OOM (report M12).
inline uint8_t* heap_dup(const uint8_t* data, size_t n) {
    if (n == 0) return nullptr;
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(n));
    if (buf) std::memcpy(buf, data, n);
    return buf;
}

// Pack parsed entries into the flat ArchiveEntryOut array + contiguous path
// buffer. Shared verbatim by the DLL and WASM list paths. Returns false when
// cumulative path bytes would exceed the u32 ABI width (report L17).
inline bool pack_entries(const std::vector<openrar::archive::BufferArchiveEntry>& src,
                         std::vector<uint8_t>& entries, std::vector<uint8_t>& paths) {
    entries.resize(src.size() * sizeof(ArchiveEntryOut));
    paths.clear();
    paths.reserve(src.size() * 32);
    auto* out = reinterpret_cast<ArchiveEntryOut*>(entries.data());
    for (size_t i = 0; i < src.size(); ++i) {
        const auto& e = src[i];
        if (paths.size() + e.path.size() > 0xFFFFFFFFull) return false;
        out[i].path_offset = static_cast<uint32_t>(paths.size());
        out[i].path_len = static_cast<uint32_t>(e.path.size());
        out[i].is_dir = e.is_dir ? 1u : 0u;
        out[i].method = static_cast<uint32_t>(e.method);
        out[i].is_encrypted = e.is_encrypted ? 1u : 0u;
        out[i].crc32 = e.crc32;
        out[i].size = e.size;
        out[i].packed_size = e.packed_size;
        out[i].mtime = e.mtime;
        out[i]._pad[0] = 0;
        out[i]._pad[1] = 0;
        paths.insert(paths.end(), e.path.begin(), e.path.end());
    }
    return true;
}

// Thread-local last-error state. Each ABI translation unit owns ONE instance
// — deliberately not `inline`: the DLL and the WASM shims must keep
// independent error states even when linked into the same process.
//
// Conventions across both surfaces:
//   - set(code, msg) records a failure (code + human-readable detail)
//   - set_message(msg) records detail only, leaving the code untouched
//     (the DLL surface reports messages; it has no code getter)
//   - fail(rc) records rc and returns it, for inner-rc propagation paths
//   - reset_code() runs at entry-point start where the surface promises
//     "code of the most recent call" semantics
class ThreadError {
public:
    void set(int code, const std::string& msg) {
        code_ = code;
        msg_ = msg;
    }
    void set_message(const std::string& msg) { msg_ = msg; }
    int fail(int rc) {
        code_ = rc;
        return rc;
    }
    void reset_code() { code_ = RAR_OK; }
    int code() const { return code_; }
    // Copy the message into buf (NUL-terminated); returns chars written.
    int copy_message_to(char* buf, int buf_len) const {
        if (!buf || buf_len <= 0) return 0;
        int n = static_cast<int>(std::min<size_t>(msg_.size(), static_cast<size_t>(buf_len - 1)));
        if (n > 0) std::memcpy(buf, msg_.data(), static_cast<size_t>(n));
        buf[n] = '\0';
        return n;
    }

private:
    int code_ = RAR_OK;
    std::string msg_;
};

// Handle table for the open/close ABI patterns both surfaces expose.
//
// ids are nonzero and monotonically increasing (wrapping skips 0). pin()
// returns a shared_ptr so callers can release the table lock before running
// work or host callbacks — a callback that re-enters any table-taking entry
// point, or that closes the handle concurrently, must not deadlock or use a
// freed handle (report L12).
template <typename T> class HandleTable {
public:
    uint32_t insert(std::shared_ptr<T> h) {
        std::lock_guard<std::mutex> lk(m_);
        uint32_t id = next_++;
        if (next_ == 0) next_ = 1;
        map_[id] = std::move(h);
        return id;
    }
    // nullptr when the id is unknown (caller reports INVALID_ARG).
    std::shared_ptr<T> pin(uint32_t id) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = map_.find(id);
        return it == map_.end() ? nullptr : it->second;
    }
    void erase(uint32_t id) {
        std::lock_guard<std::mutex> lk(m_);
        map_.erase(id);
    }
    // Snapshot of every live handle (shared_ptrs pin lifetimes). Callers may
    // then inspect the entries with the lock released — the mutation
    // exports' RAR_ERR_BUSY pre-check sweeps this to find archives an open
    // file-mode handle still holds.
    std::vector<std::shared_ptr<T>> snapshot() {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::shared_ptr<T>> out;
        out.reserve(map_.size());
        for (auto& [id, h] : map_) out.push_back(h);
        return out;
    }

private:
    std::mutex m_;
    std::unordered_map<uint32_t, std::shared_ptr<T>> map_;
    uint32_t next_ = 1;
};

} // namespace openrar::api

#endif // OPENRAR_API_ABI_CONTRACT_HPP
