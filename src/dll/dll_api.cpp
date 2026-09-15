#include "openrar_dll.h"

#include "../api/abi_contract.hpp"
#include "../archive/buffer_archive.hpp"
#include "../compress/compressor50.hpp"
#include "../compress/decompressor50.hpp"
#include "../compress/stream_encoder.hpp"

#include <exception>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

// ── Compile-time equivalence against the shared ABI contract ────────────────
// openrar_dll.h re-declares the contract in C-compatible form for C hosts;
// this TU pins it to the canonical definitions in src/api/abi_contract.hpp
// so the two can never drift apart silently.
static_assert(static_cast<int>(RAR_OK) == static_cast<int>(openrar::api::RAR_OK));
static_assert(static_cast<int>(RAR_ERR_PARTIAL_OK) ==
              static_cast<int>(openrar::api::RAR_ERR_PARTIAL_OK));
static_assert(static_cast<int>(RAR_ERR_NOT_RAR) == static_cast<int>(openrar::api::RAR_ERR_NOT_RAR));
static_assert(static_cast<int>(RAR_ERR_UNSUPPORTED_FEATURE) ==
              static_cast<int>(openrar::api::RAR_ERR_UNSUPPORTED_FEATURE));
static_assert(static_cast<int>(RAR_ERR_TRUNCATED) ==
              static_cast<int>(openrar::api::RAR_ERR_TRUNCATED));
static_assert(static_cast<int>(RAR_ERR_CRC_MISMATCH) ==
              static_cast<int>(openrar::api::RAR_ERR_CRC_MISMATCH));
static_assert(static_cast<int>(RAR_ERR_NOMEM) == static_cast<int>(openrar::api::RAR_ERR_NOMEM));
static_assert(static_cast<int>(RAR_ERR_IO) == static_cast<int>(openrar::api::RAR_ERR_IO));
static_assert(static_cast<int>(RAR_ERR_BAD_PASSWORD) ==
              static_cast<int>(openrar::api::RAR_ERR_BAD_PASSWORD));
static_assert(static_cast<int>(RAR_ERR_INVALID_ARG) ==
              static_cast<int>(openrar::api::RAR_ERR_INVALID_ARG));
static_assert(static_cast<int>(RAR_ERR_ABORTED) == static_cast<int>(openrar::api::RAR_ERR_ABORTED));
static_assert(static_cast<int>(RAR_ERR_ENCRYPTED) ==
              static_cast<int>(openrar::api::RAR_ERR_ENCRYPTED));
static_assert(sizeof(openrar_archive_entry_t) == sizeof(openrar::api::ArchiveEntryOut),
              "openrar_archive_entry_t layout drifted from api::ArchiveEntryOut");
#define OPENRAR_DLL_ENTRY_FIELD(field)                                                             \
    static_assert(offsetof(openrar_archive_entry_t, field) ==                                      \
                      offsetof(openrar::api::ArchiveEntryOut, field),                              \
                  "openrar_archive_entry_t." #field " offset drifted")
OPENRAR_DLL_ENTRY_FIELD(path_offset);
OPENRAR_DLL_ENTRY_FIELD(path_len);
OPENRAR_DLL_ENTRY_FIELD(is_dir);
OPENRAR_DLL_ENTRY_FIELD(method);
OPENRAR_DLL_ENTRY_FIELD(is_encrypted);
OPENRAR_DLL_ENTRY_FIELD(crc32);
OPENRAR_DLL_ENTRY_FIELD(size);
OPENRAR_DLL_ENTRY_FIELD(packed_size);
OPENRAR_DLL_ENTRY_FIELD(mtime);
#undef OPENRAR_DLL_ENTRY_FIELD

namespace {
// Thread-local error state from the shared contract. The DLL surface reports
// messages only (no numeric getter), so set_error() leaves the code alone.
thread_local openrar::api::ThreadError g_err;
void set_error(const std::string& msg) {
    g_err.set_message(msg);
}

// Archive handle map — shared contract table (shared_ptr pinning, report L12).
struct ArchiveHandle {
    std::vector<uint8_t> data;
    openrar::archive::BufferArchive ba;
    std::vector<openrar::archive::BufferArchiveEntry> entries;
};
openrar::api::HandleTable<ArchiveHandle> g_handles;

// Adapter between the DLL callback convention (user, done, total) and the
// core convention (done, total, user). One shared user pointer for both
// callbacks, per the _ex listing contract. Thunks are passed to the core
// unconditionally; they no-op when the host did not register that callback.
struct ListCallbackCtx {
    openrar_progress_cb progress;
    openrar_cancel_cb cancel;
    void* user;
};
void list_progress_thunk(uint64_t done, uint64_t total, void* user) {
    auto* ctx = static_cast<ListCallbackCtx*>(user);
    if (ctx->progress) ctx->progress(ctx->user, done, total);
}
int list_cancel_thunk(void* user) {
    auto* ctx = static_cast<ListCallbackCtx*>(user);
    return ctx->cancel ? ctx->cancel(ctx->user) : 0;
}

// Human-readable detail for the listing failure codes hosts act on.
const char* list_error_message(int rc) {
    switch (rc) {
    case RAR_ERR_ENCRYPTED:
        return "archive headers are encrypted (password required to list)";
    case RAR_ERR_BAD_PASSWORD:
        return "wrong password for encrypted headers";
    case RAR_ERR_ABORTED:
        return "listing aborted";
    default:
        return "list failed";
    }
}

// Pack parsed entries into the flat host buffers (malloc'd; freed with
// openrar_archive_list_free). Shared by every list export. Returns RAR_OK or
// RAR_ERR_NOMEM with the thread-local error set.
int pack_list_outputs(const std::vector<openrar::archive::BufferArchiveEntry>& parsed,
                      uint32_t* count, void** entries_out, void** paths_out,
                      size_t* paths_size_out) {
    std::vector<uint8_t> entries, paths;
    if (!openrar::api::pack_entries(parsed, entries, paths)) {
        set_error("pack failed");
        return RAR_ERR_NOMEM;
    }
    uint8_t* he = static_cast<uint8_t*>(std::malloc(entries.size() ? entries.size() : 1));
    if (!he) {
        set_error("oom entries");
        return RAR_ERR_NOMEM;
    }
    if (!entries.empty()) std::memcpy(he, entries.data(), entries.size());
    uint8_t* hp = static_cast<uint8_t*>(std::malloc(paths.size() ? paths.size() : 1));
    if (!hp) {
        std::free(he);
        set_error("oom paths");
        return RAR_ERR_NOMEM;
    }
    if (!paths.empty()) std::memcpy(hp, paths.data(), paths.size());
    *count = static_cast<uint32_t>(parsed.size());
    *entries_out = he;
    *paths_out = hp;
    *paths_size_out = paths.size();
    return RAR_OK;
}
} // namespace

extern "C" {

int OPENRAR_DLL_CALL openrar_version(void) {
    return OPENRAR_DLL_API_VERSION;
}
int OPENRAR_DLL_CALL openrar_archive_version(void) {
    return 1;
}
uint64_t OPENRAR_DLL_CALL openrar_abi_features(void) {
    return OPENRAR_ABI_FEATURE_LIST_PROGRESS | OPENRAR_ABI_FEATURE_LIST_PASSWORD |
           OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS;
}

void* OPENRAR_DLL_CALL openrar_alloc(size_t bytes) {
    return std::malloc(bytes);
}
void OPENRAR_DLL_CALL openrar_free(void* ptr) {
    std::free(ptr);
}
void* OPENRAR_DLL_CALL openrar_archive_alloc(size_t bytes) {
    return std::malloc(bytes);
}
void OPENRAR_DLL_CALL openrar_archive_free(void* ptr) {
    std::free(ptr);
}

int OPENRAR_DLL_CALL openrar_archive_get_error(char* buf, int buf_len) {
    return g_err.copy_message_to(buf, buf_len);
}
int OPENRAR_DLL_CALL openrar_last_error(char* buf, int buf_len) {
    return openrar_archive_get_error(buf, buf_len);
}

// ── Block codec ──────────────────────────────────────────────────────────────
int OPENRAR_DLL_CALL openrar_compress(const uint8_t* src, size_t src_len, uint8_t** out_ptr,
                                      size_t* out_len, int method) {
    return openrar_compress2(src, src_len, out_ptr, out_len, method, 2 * 1024 * 1024);
}
int OPENRAR_DLL_CALL openrar_compress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr,
                                       size_t* out_len, int method, size_t win_size) {
    try {
        if (!out_ptr || !out_len) return 0;
        if (win_size > openrar::api::MAX_WIN_SIZE) return 0;
        *out_ptr = nullptr;
        *out_len = 0;
        if (src_len == 0) return 1;
        if (!src && src_len != 0) return 0;
        std::vector<openrar::core::byte> out;
        // clamp method
        if (method < 0) method = 3;
        if (method > 5) method = 5;
        if (method == 0) {
            out.assign(src, src + src_len);
        } else {
            if (!openrar::compress::Compressor50::compress_buffer(src, src_len, out, method,
                                                                  win_size))
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
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_decompress(const uint8_t* src, size_t src_len, uint8_t** out_ptr,
                                        size_t* out_len) {
    return openrar_decompress2(src, src_len, out_ptr, out_len, 1024 * 1024);
}
int OPENRAR_DLL_CALL openrar_decompress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr,
                                         size_t* out_len, size_t win_size) {
    try {
        if (!out_ptr || !out_len) return 0;
        constexpr size_t MAX_WIN = 4ULL * 1024 * 1024 * 1024;
        if (win_size > MAX_WIN) return 0;
        *out_ptr = nullptr;
        *out_len = 0;
        if (src_len == 0) return 1;
        if (!src) return 0;
        openrar::compress::Decompressor50 dec(win_size ? win_size : 1024 * 1024);
        std::vector<openrar::core::byte> out;
        size_t prior = out.size();
        bool ok = dec.decompress_to_vector(src, src_len, out);
        if (!ok) {
            out.resize(prior);
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
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

// ── Archive — buffer API ────────────────────────────────────────────────────
int OPENRAR_DLL_CALL openrar_archive_list(const uint8_t* data, size_t size, uint32_t* count,
                                          void** entries_out, void** paths_out,
                                          size_t* paths_size_out) {
    try {
        if (!data || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        openrar::archive::BufferArchive ba;
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = ba.list(data, size, parsed);
        if (rc != RAR_OK) {
            set_error("list failed");
            return rc;
        }
        return pack_list_outputs(parsed, count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
void OPENRAR_DLL_CALL openrar_archive_list_free(void* entries, void* paths, size_t) {
    std::free(entries);
    std::free(paths);
}

int OPENRAR_DLL_CALL openrar_archive_extract(const uint8_t* data, size_t size, uint32_t entry_index,
                                             uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!data || !out_ptr || !out_len) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;
        openrar::archive::BufferArchive ba;
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = ba.list(data, size, parsed);
        if (rc != RAR_OK) return rc;
        if (entry_index >= parsed.size()) {
            set_error("entry_index out of range");
            return RAR_ERR_INVALID_ARG;
        }
        std::vector<uint8_t> out;
        rc = ba.extract(data, size, entry_index, out);
        if (rc != RAR_OK) return rc;
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size() ? out.size() : 1));
        if (!buf && !out.empty()) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        if (!out.empty()) std::memcpy(buf, out.data(), out.size());
        *out_ptr = out.empty() ? nullptr : buf;
        *out_len = out.size();
        if (out.empty()) std::free(buf);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_archive_extract_all(const uint8_t* data, size_t size,
                                                 uint8_t** buf_out_ptr, size_t* buf_size_out,
                                                 uint64_t** offsets_out_ptr,
                                                 uint32_t* offsets_count_out) {
    try {
        if (!data || !buf_out_ptr || !buf_size_out || !offsets_out_ptr || !offsets_count_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *buf_out_ptr = nullptr;
        *buf_size_out = 0;
        *offsets_out_ptr = nullptr;
        *offsets_count_out = 0;
        openrar::archive::BufferArchive ba;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        int rc = ba.extract_all(data, size, files);
        if (rc != RAR_OK) return rc;
        size_t total = 0;
        for (auto& f : files) total += f.second.size();
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(total ? total : 1));
        if (!buf) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        uint64_t* offs = static_cast<uint64_t*>(std::malloc(files.size() * sizeof(uint64_t) * 2));
        if (!offs) {
            std::free(buf);
            set_error("oom offs");
            return RAR_ERR_NOMEM;
        }
        size_t off = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            auto& p = files[i].second;
            if (!p.empty()) std::memcpy(buf + off, p.data(), p.size());
            offs[i * 2] = off;
            offs[i * 2 + 1] = p.size();
            off += p.size();
        }
        *buf_out_ptr = buf;
        *buf_size_out = total;
        *offsets_out_ptr = offs;
        *offsets_count_out = static_cast<uint32_t>(files.size());
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_archive_create(const uint8_t* const* paths_arr,
                                            const uint8_t* const* data_arr, const size_t* sizes_arr,
                                            uint32_t file_count, int method, uint32_t window_log2,
                                            uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!paths_arr || !data_arr || !sizes_arr || !out_ptr || !out_len) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;
        if (method != 0 && method != 3 && method != 5) {
            set_error("method must be 0,3,5");
            return RAR_ERR_INVALID_ARG;
        }
        if (window_log2 < 1 || window_log2 > 4) {
            set_error("window_log2 must be 1..4");
            return RAR_ERR_INVALID_ARG;
        }
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        files.reserve(file_count);
        for (uint32_t i = 0; i < file_count; ++i) {
            const uint8_t* p = paths_arr[i];
            const uint8_t* d = data_arr[i];
            size_t s = sizes_arr[i];
            if (!p || (!d && s != 0)) {
                set_error("null path/data");
                return RAR_ERR_INVALID_ARG;
            }
            if (!d) d = reinterpret_cast<const uint8_t*>("");
            size_t plen = 0;
            while (plen < 4096 && p[plen] != 0) ++plen;
            std::string path(reinterpret_cast<const char*>(p), plen);
            bool is_dir = !path.empty() && path.back() == '/';
            std::string err;
            if (!openrar::archive::validate_archive_path(path, is_dir, err)) {
                set_error("path " + path + ": " + err);
                return RAR_ERR_INVALID_ARG;
            }
            files.emplace_back(std::move(path), std::vector<uint8_t>(d, d + s));
        }
        std::vector<uint8_t> out;
        int rc = openrar::archive::create_archive(files, out, method, window_log2);
        if (rc != RAR_OK) return rc;
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size() ? out.size() : 1));
        if (!buf) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        if (!out.empty()) std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

// ── Handle API ───────────────────────────────────────────────────────────────
uint32_t OPENRAR_DLL_CALL openrar_archive_open(const uint8_t* data, size_t size) {
    try {
        if (!data || size == 0) {
            set_error("null argument");
            return 0;
        }
        auto h = std::make_shared<ArchiveHandle>();
        h->data.assign(data, data + size);
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = h->ba.list(h->data.data(), h->data.size(), parsed);
        if (rc != RAR_OK) {
            set_error("open failed");
            return 0;
        }
        h->entries = std::move(parsed);
        return g_handles.insert(h);
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("unknown C++ exception");
        return 0;
    }
}
void OPENRAR_DLL_CALL openrar_archive_close(uint32_t handle) {
    g_handles.erase(handle);
}
uint32_t OPENRAR_DLL_CALL openrar_archive_open_ex(const uint8_t* data, size_t size,
                                                  openrar_progress_cb progress,
                                                  openrar_cancel_cb cancel, void* user) {
    try {
        if (!data || size == 0) {
            set_error("null argument");
            return 0;
        }
        auto h = std::make_shared<ArchiveHandle>();
        h->data.assign(data, data + size);
        ListCallbackCtx ctx{progress, cancel, user};
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = h->ba.list(h->data.data(), h->data.size(), parsed, list_progress_thunk, &ctx,
                            list_cancel_thunk, &ctx);
        if (rc != RAR_OK) {
            set_error(rc == RAR_ERR_ABORTED ? "open aborted" : "open failed");
            return 0;
        }
        h->entries = std::move(parsed);
        return g_handles.insert(h);
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("unknown C++ exception");
        return 0;
    }
}
int OPENRAR_DLL_CALL openrar_archive_handle_list(uint32_t handle, uint32_t* count,
                                                 void** entries_out, void** paths_out,
                                                 size_t* paths_size_out) {
    try {
        if (!count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        std::vector<uint8_t> entries, paths;
        if (!openrar::api::pack_entries(h->entries, entries, paths)) {
            set_error("pack failed");
            return RAR_ERR_NOMEM;
        }
        uint8_t* he = static_cast<uint8_t*>(std::malloc(entries.size() ? entries.size() : 1));
        if (!he) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        if (!entries.empty()) std::memcpy(he, entries.data(), entries.size());
        uint8_t* hp = static_cast<uint8_t*>(std::malloc(paths.size() ? paths.size() : 1));
        if (!hp) {
            std::free(he);
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        if (!paths.empty()) std::memcpy(hp, paths.data(), paths.size());
        *count = static_cast<uint32_t>(h->entries.size());
        *entries_out = he;
        *paths_out = hp;
        *paths_size_out = paths.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_archive_handle_extract(uint32_t handle, uint32_t entry_index,
                                                    uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!out_ptr || !out_len) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        if (entry_index >= h->entries.size()) {
            set_error("entry_index OOR");
            return RAR_ERR_INVALID_ARG;
        }
        std::vector<uint8_t> out;
        int rc = h->ba.extract(h->data.data(), h->data.size(), entry_index, out);
        if (rc != RAR_OK) return rc;
        *out_ptr = openrar::api::heap_dup(out.data(), out.size());
        if (!*out_ptr && !out.empty()) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        *out_len = out.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
int OPENRAR_DLL_CALL openrar_archive_handle_extract_all(uint32_t handle, uint8_t** buf_out_ptr,
                                                        size_t* buf_size_out,
                                                        uint64_t** offsets_out_ptr,
                                                        uint32_t* offsets_count_out) {
    try {
        if (!buf_out_ptr || !buf_size_out || !offsets_out_ptr || !offsets_count_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *buf_out_ptr = nullptr;
        *buf_size_out = 0;
        *offsets_out_ptr = nullptr;
        *offsets_count_out = 0;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error("invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        int rc = h->ba.extract_all(h->data.data(), h->data.size(), files);
        if (rc != RAR_OK) return rc;
        size_t total = 0;
        for (auto& f : files) total += f.second.size();
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(total ? total : 1));
        if (!buf) {
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        uint64_t* offs = static_cast<uint64_t*>(std::malloc(files.size() * sizeof(uint64_t) * 2));
        if (!offs) {
            std::free(buf);
            set_error("oom");
            return RAR_ERR_NOMEM;
        }
        size_t off = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            auto& p = files[i].second;
            if (!p.empty()) std::memcpy(buf + off, p.data(), p.size());
            offs[i * 2] = off;
            offs[i * 2 + 1] = p.size();
            off += p.size();
        }
        *buf_out_ptr = buf;
        *buf_size_out = total;
        *offsets_out_ptr = offs;
        *offsets_count_out = static_cast<uint32_t>(files.size());
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

} // extern "C" helpers C++ linkage

static std::vector<uint8_t> read_file_bytes(const char* path, bool& ok) {
    ok = false;
    std::vector<uint8_t> out;
    if (!path) return out;
    std::filesystem::path p = std::filesystem::u8path(path);
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) return out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz));
    if (!f && !f.eof()) {
        out.clear();
        return out;
    }
    ok = true;
    return out;
}

extern "C" {

int OPENRAR_DLL_CALL openrar_archive_list_file(const char* arc_path, uint32_t* count,
                                               void** entries_out, void** paths_out,
                                               size_t* paths_size_out) {
    try {
        bool ok = false;
        auto data = read_file_bytes(arc_path, ok);
        if (!ok) return RAR_ERR_IO;
        return openrar_archive_list(data.data(), data.size(), count, entries_out, paths_out,
                                    paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_extract_file(const char* arc_path, uint32_t entry_index,
                                                  uint8_t** out_ptr, size_t* out_len) {
    try {
        bool ok = false;
        auto data = read_file_bytes(arc_path, ok);
        if (!ok) return RAR_ERR_IO;
        return openrar_archive_extract(data.data(), data.size(), entry_index, out_ptr, out_len);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_extract_file_to_path(const char* arc_path,
                                                          uint32_t entry_index,
                                                          const char* dest_path) {
    try {
        if (!arc_path || !dest_path) return RAR_ERR_INVALID_ARG;
        uint8_t* buf = nullptr;
        size_t len = 0;
        int rc = openrar_archive_extract_file(arc_path, entry_index, &buf, &len);
        if (rc != RAR_OK) return rc;
        std::filesystem::path out = std::filesystem::u8path(dest_path);
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        if (!f) {
            openrar_free(buf);
            return RAR_ERR_IO;
        }
        if (len > 0) f.write(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(len));
        openrar_free(buf);
        return f ? RAR_OK : RAR_ERR_IO;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_create_from_paths(const char* const* src_paths,
                                                       const char* const* arc_names,
                                                       uint32_t file_count, int method,
                                                       uint32_t window_log2, uint8_t** out_ptr,
                                                       size_t* out_len) {
    try {
        if (!src_paths || !arc_names || !out_ptr || !out_len) return RAR_ERR_INVALID_ARG;
        if (file_count == 0) return RAR_ERR_INVALID_ARG;
        std::vector<std::vector<uint8_t>> file_datas;
        std::vector<const uint8_t*> data_ptrs;
        std::vector<const uint8_t*> path_ptrs;
        std::vector<size_t> sizes;
        std::vector<std::string> arc_str;
        file_datas.reserve(file_count);
        data_ptrs.reserve(file_count);
        path_ptrs.reserve(file_count);
        sizes.reserve(file_count);
        arc_str.reserve(file_count);
        for (uint32_t i = 0; i < file_count; ++i) {
            if (!src_paths[i] || !arc_names[i]) return RAR_ERR_INVALID_ARG;
            bool ok = false;
            auto b = read_file_bytes(src_paths[i], ok);
            if (!ok) return RAR_ERR_IO;
            arc_str.emplace_back(arc_names[i]);
            file_datas.emplace_back(std::move(b));
        }
        for (uint32_t i = 0; i < file_count; ++i) {
            path_ptrs.push_back(reinterpret_cast<const uint8_t*>(arc_str[i].c_str()));
            data_ptrs.push_back(file_datas[i].data());
            sizes.push_back(file_datas[i].size());
        }
        return openrar_archive_create(path_ptrs.data(), data_ptrs.data(), sizes.data(), file_count,
                                      method, window_log2, out_ptr, out_len);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_create_to_file(const char* const* src_paths,
                                                    const char* const* arc_names,
                                                    uint32_t file_count, int method,
                                                    uint32_t window_log2, const char* out_path) {
    try {
        if (!out_path) return RAR_ERR_INVALID_ARG;
        uint8_t* buf = nullptr;
        size_t len = 0;
        int rc = openrar_archive_create_from_paths(src_paths, arc_names, file_count, method,
                                                   window_log2, &buf, &len);
        if (rc != RAR_OK) return rc;
        std::filesystem::path out = std::filesystem::u8path(out_path);
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        if (!f) {
            openrar_free(buf);
            return RAR_ERR_IO;
        }
        if (len > 0) f.write(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(len));
        openrar_free(buf);
        return f ? RAR_OK : RAR_ERR_IO;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}

// ── Streaming ────────────────────────────────────────────────────────────────
namespace {
struct StreamHandle {
    std::unique_ptr<openrar::compress::StreamEncoder> enc;
    openrar_progress_cb prog_cb{nullptr};
    void* prog_user{nullptr};
    openrar_cancel_cb cancel_cb{nullptr};
    void* cancel_user{nullptr};
};
// Shared contract table: pin() returns a shared_ptr so feed/finish can work
// with the table lock released (L12: host callbacks must never run under the
// map mutex, and a callback may close the handle concurrently).
openrar::api::HandleTable<StreamHandle> g_stream_handles;
} // namespace
uint32_t OPENRAR_DLL_CALL openrar_stream_create(int method, uint32_t window_log2) {
    try {
        size_t win = 0;
        switch (window_log2) {
        case 1:
            win = 128 * 1024;
            break;
        case 2:
            win = 256 * 1024;
            break;
        case 3:
            win = 512 * 1024;
            break;
        case 4:
            win = 1024 * 1024;
            break;
        case 5:
            win = 4 * 1024 * 1024;
            break;
        default:
            return 0;
        }
        auto h = std::make_shared<StreamHandle>();
        h->enc = std::make_unique<openrar::compress::StreamEncoder>(method, win);
        return g_stream_handles.insert(h);
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("unknown C++ exception");
        return 0;
    }
}
int OPENRAR_DLL_CALL openrar_stream_feed(uint32_t handle, const uint8_t* src, size_t n) {
    try {
        auto h = g_stream_handles.pin(handle);
        if (!h) return RAR_ERR_INVALID_ARG;
        // L12 (via HandleTable::pin): no host callback runs under the table
        // mutex — the shared_ptr pins the entry's lifetime, so the cancel poll
        // and enc->feed() below run with no lock held.
        if (h->cancel_cb && h->cancel_cb(h->cancel_user)) return RAR_ERR_ABORTED;
        bool ok = h->enc->feed(src, n);
        return ok ? RAR_OK : RAR_ERR_INVALID_ARG;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_INVALID_ARG;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_INVALID_ARG;
    }
}
int OPENRAR_DLL_CALL openrar_stream_finish(uint32_t handle, uint8_t** out_ptr, size_t* out_len) {
    try {
        if (!out_ptr || !out_len) return RAR_ERR_INVALID_ARG;
        *out_ptr = nullptr;
        *out_len = 0;
        auto h = g_stream_handles.pin(handle);
        if (!h) return RAR_ERR_INVALID_ARG;
        // L12 (via HandleTable::pin): cancel poll and enc->finish() run with
        // no table lock held (see feed).
        if (h->cancel_cb && h->cancel_cb(h->cancel_user)) return RAR_ERR_ABORTED;
        std::vector<uint8_t> out;
        bool ok = h->enc->finish(out);
        if (!ok) return RAR_ERR_NOMEM;
        if (out.empty()) {
            *out_ptr = nullptr;
            *out_len = 0;
            return RAR_OK;
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) return RAR_ERR_NOMEM;
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}
void OPENRAR_DLL_CALL openrar_stream_free(uint32_t handle) {
    g_stream_handles.erase(handle);
}
// openrar_set_progress/openrar_set_cancel removed (L12/I19): they were silent
// no-ops and there is no per-block hook in the one-shot archive handle API to
// drive them honestly. Use the stream-level openrar_stream_set_progress /
// openrar_stream_set_cancel instead.
int OPENRAR_DLL_CALL openrar_stream_set_progress(uint32_t handle, openrar_progress_cb cb,
                                                 void* user) {
    try {
        auto h = g_stream_handles.pin(handle);
        if (!h) return RAR_ERR_INVALID_ARG;
        h->prog_cb = cb;
        h->prog_user = user;
        if (cb)
            h->enc->set_progress(cb, user);
        else
            h->enc->set_progress(nullptr, nullptr);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_INVALID_ARG;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_INVALID_ARG;
    }
}
int OPENRAR_DLL_CALL openrar_stream_set_cancel(uint32_t handle, openrar_cancel_cb cb, void* user) {
    try {
        auto h = g_stream_handles.pin(handle);
        if (!h) return RAR_ERR_INVALID_ARG;
        h->cancel_cb = cb;
        h->cancel_user = user;
        h->enc->set_cancel(cb, user);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_INVALID_ARG;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_INVALID_ARG;
    }
}

// ── Listing with progress / cancel ───────────────────────────────────────────
int OPENRAR_DLL_CALL openrar_archive_list_file_ex(const char* arc_path, uint32_t* count,
                                                  void** entries_out, void** paths_out,
                                                  size_t* paths_size_out,
                                                  openrar_progress_cb progress,
                                                  openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        ListCallbackCtx ctx{progress, cancel, user};
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = openrar::archive::list_file_stream(
            std::filesystem::u8path(arc_path), parsed,
            /*password=*/nullptr,
            /*emit_encrypted_entries=*/false, list_progress_thunk, &ctx, list_cancel_thunk, &ctx);
        if (rc != RAR_OK) {
            set_error(list_error_message(rc));
            return rc;
        }
        return pack_list_outputs(parsed, count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_list_file_pw(const char* arc_path, const char* password,
                                                  uint32_t* count, void** entries_out,
                                                  void** paths_out, size_t* paths_size_out,
                                                  openrar_progress_cb progress,
                                                  openrar_cancel_cb cancel, void* user) {
    try {
        if (!arc_path || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        ListCallbackCtx ctx{progress, cancel, user};
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        // The streaming path owns header decryption; encrypted file entries
        // are reported with is_encrypted = 1 (this is what makes -hp listing
        // useful — see the header contract).
        int rc = openrar::archive::list_file_stream(
            std::filesystem::u8path(arc_path), parsed, password, /*emit_encrypted_entries=*/true,
            list_progress_thunk, &ctx, list_cancel_thunk, &ctx);
        if (rc != RAR_OK) {
            set_error(list_error_message(rc));
            return rc;
        }
        return pack_list_outputs(parsed, count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_IO;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_IO;
    }
}
int OPENRAR_DLL_CALL openrar_archive_list_ex(const uint8_t* data, size_t size, uint32_t* count,
                                             void** entries_out, void** paths_out,
                                             size_t* paths_size_out, openrar_progress_cb progress,
                                             openrar_cancel_cb cancel, void* user) {
    try {
        if (!data || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error("null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        ListCallbackCtx ctx{progress, cancel, user};
        openrar::archive::BufferArchive ba;
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = ba.list(data, size, parsed, list_progress_thunk, &ctx, list_cancel_thunk, &ctx);
        if (rc != RAR_OK) {
            set_error(list_error_message(rc));
            return rc;
        }
        return pack_list_outputs(parsed, count, entries_out, paths_out, paths_size_out);
    } catch (const std::exception& e) {
        set_error(e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error("unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

} // extern "C"
