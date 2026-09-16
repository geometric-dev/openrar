#include "archive_api.hpp"
#include "../archive/buffer_archive.hpp"

#include <exception>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <utility>

namespace openrar::wasm {

// Thread-local last-error state, from the shared ABI contract. Set by the
// entry points below when they return a negative RarError code; read by
// openrar_archive_get_error() (message) and
// openrar_archive_last_error_code() (numeric code).
static thread_local api::ThreadError g_err;

static void set_error(int code, const std::string& msg) {
    g_err.set(code, msg);
}

// For failure paths that propagate an inner rc without their own message.
static int fail_code(int rc) {
    return g_err.fail(rc);
}

int validate_archive_path_c(const char* path, size_t path_len, int is_dir, char* err_buf,
                            size_t err_buf_len) {
    if (!path) return RAR_ERR_INVALID_ARG;
    std::string s(path, path_len);
    std::string err;
    if (!openrar::archive::validate_archive_path(s, is_dir != 0, err)) {
        if (err_buf && err_buf_len > 0) {
            size_t n = std::min(err.size(), err_buf_len - 1);
            std::memcpy(err_buf, err.data(), n);
            err_buf[n] = '\0';
        }
        return RAR_ERR_INVALID_ARG;
    }
    return RAR_OK;
}

} // namespace openrar::wasm

// ── C ABI ────────────────────────────────────────────────────────────────────
extern "C" {

using namespace openrar::wasm;

int openrar_archive_version(void) {
    return ARCHIVE_WASM_API_VERSION;
}

// Concatenate per-entry payloads into one malloc'd buffer plus a
// {offset, size}* table. Shared by extract_all, extract_all2 and the handle
// variants. Caller owns *buf_out_ptr / *offsets_out_ptr (openrar_archive_free).
static int pack_extract_all(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
                            uint8_t** buf_out_ptr, size_t* buf_size_out, uint64_t** offsets_out_ptr,
                            uint32_t* offsets_count_out) {
    if (files.empty()) {
        *buf_out_ptr = nullptr;
        *buf_size_out = 0;
        *offsets_out_ptr = nullptr;
        *offsets_count_out = 0;
        return RAR_OK;
    }
    if (files.size() > SIZE_MAX / (sizeof(uint64_t) * 2)) {
        set_error(RAR_ERR_NOMEM, "out of memory (offsets overflow)");
        return RAR_ERR_NOMEM;
    }
    size_t total = 0;
    for (const auto& f : files) {
        if (f.second.size() > SIZE_MAX - total) {
            set_error(RAR_ERR_NOMEM, "out of memory (size overflow)");
            return RAR_ERR_NOMEM;
        }
        total += f.second.size();
    }
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(total > 0 ? total : 1));
    if (!buf) {
        set_error(RAR_ERR_NOMEM, "out of memory");
        return RAR_ERR_NOMEM;
    }
    uint64_t* offsets = static_cast<uint64_t*>(std::malloc(files.size() * sizeof(uint64_t) * 2));
    if (!offsets) {
        std::free(buf);
        set_error(RAR_ERR_NOMEM, "out of memory (offsets)");
        return RAR_ERR_NOMEM;
    }
    size_t off = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& payload = files[i].second;
        if (!payload.empty()) std::memcpy(buf + off, payload.data(), payload.size());
        offsets[i * 2 + 0] = static_cast<uint64_t>(off);
        offsets[i * 2 + 1] = static_cast<uint64_t>(payload.size());
        off += payload.size();
    }
    *buf_out_ptr = buf;
    *buf_size_out = total;
    *offsets_out_ptr = offsets;
    *offsets_count_out = static_cast<uint32_t>(files.size());
    return RAR_OK;
}

int openrar_archive_list(const uint8_t* data, size_t size, uint32_t* count, void** entries_out,
                         void** paths_out, size_t* paths_size_out) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!data || !count || !entries_out || !paths_out || !paths_size_out) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
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
            set_error(rc, "list failed");
            return rc;
        }

        std::vector<uint8_t> entries;
        std::vector<uint8_t> paths;
        if (!openrar::api::pack_entries(parsed, entries, paths)) {
            set_error(RAR_ERR_NOMEM, "pack_entries failed");
            return RAR_ERR_NOMEM;
        }

        // Hand the bytes to the WASM heap so the caller can read them after
        // we return. They will be freed by openrar_archive_list_free().
        // heap_dup returns nullptr for empty buffers by contract (report M12):
        // malloc(0) may legally return null, which must not be misread as OOM
        // for a successfully parsed archive with zero entries.
        uint8_t* heap_entries = openrar::api::heap_dup(entries.data(), entries.size());
        if (!heap_entries && !entries.empty()) {
            set_error(RAR_ERR_NOMEM, "out of memory (entries)");
            return RAR_ERR_NOMEM;
        }

        uint8_t* heap_paths = openrar::api::heap_dup(paths.data(), paths.size());
        if (!heap_paths && !paths.empty()) {
            std::free(heap_entries);
            set_error(RAR_ERR_NOMEM, "out of memory (paths)");
            return RAR_ERR_NOMEM;
        }

        *count = static_cast<uint32_t>(parsed.size());
        *entries_out = heap_entries;
        *paths_out = heap_paths;
        *paths_size_out = paths.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

void openrar_archive_list_free(void* entries, void* paths, size_t /*paths_size*/) {
    std::free(entries);
    std::free(paths);
}

int openrar_archive_extract(const uint8_t* data, size_t size, uint32_t entry_index,
                            uint8_t** out_ptr, size_t* out_len) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!data || !out_ptr || !out_len) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;

        openrar::archive::BufferArchive ba;
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = ba.list(data, size, parsed);
        if (rc != RAR_OK) return fail_code(rc);
        if (entry_index >= parsed.size()) {
            set_error(RAR_ERR_INVALID_ARG, "entry_index out of range");
            return RAR_ERR_INVALID_ARG;
        }

        std::vector<uint8_t> out;
        rc = ba.extract(data, size, entry_index, out);
        if (rc != RAR_OK) return fail_code(rc);

        // malloc(0) may legally return NULL, which the check below would
        // misreport as OOM for a successfully extracted empty entry (report M12);
        // mirror the dll handler's size?size:1 convention.
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size() ? out.size() : 1));
        if (!buf && !out.empty()) {
            set_error(RAR_ERR_NOMEM, "out of memory");
            return RAR_ERR_NOMEM;
        }
        if (!out.empty()) std::memcpy(buf, out.data(), out.size());
        *out_ptr = out.empty() ? nullptr : buf;
        *out_len = out.size();
        if (out.empty()) std::free(buf);
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

int openrar_archive_extract_all(const uint8_t* data, size_t size, uint8_t** buf_out_ptr,
                                size_t* buf_size_out, uint64_t** offsets_out_ptr,
                                uint32_t* offsets_count_out) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!data || !buf_out_ptr || !buf_size_out || !offsets_out_ptr || !offsets_count_out) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *buf_out_ptr = nullptr;
        *buf_size_out = 0;
        *offsets_out_ptr = nullptr;
        *offsets_count_out = 0;

        openrar::archive::BufferArchive ba;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        int rc = ba.extract_all(data, size, files);
        if (rc != RAR_OK) return fail_code(rc);

        return pack_extract_all(files, buf_out_ptr, buf_size_out, offsets_out_ptr,
                                offsets_count_out);
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

int openrar_archive_create(const uint8_t* const* paths_arr, const uint8_t* const* data_arr,
                           const size_t* sizes_arr, uint32_t file_count, int method,
                           uint32_t window_log2, uint8_t** out_ptr, size_t* out_len) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!paths_arr || !data_arr || !sizes_arr || !out_ptr || !out_len) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;

        // Validate method + window_log2 before building anything.
        if (method != 0 && method != 3 && method != 5) {
            set_error(RAR_ERR_INVALID_ARG, "method must be 0, 3, or 5");
            return RAR_ERR_INVALID_ARG;
        }
        if (window_log2 < 1 || window_log2 > 4) {
            set_error(RAR_ERR_INVALID_ARG, "window_log2 must be in [1, 4]");
            return RAR_ERR_INVALID_ARG;
        }

        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        files.reserve(file_count);
        for (uint32_t i = 0; i < file_count; ++i) {
            const uint8_t* p = paths_arr[i];
            const uint8_t* d = data_arr[i];
            size_t s = sizes_arr[i];
            if (!p || (!d && s != 0)) {
                set_error(RAR_ERR_INVALID_ARG, "null path or data pointer");
                return RAR_ERR_INVALID_ARG;
            }
            if (!d) d = reinterpret_cast<const uint8_t*>("");
            // Find NUL-terminator; if not found, take up to 4 KiB and let
            // validate_archive_path handle the rest.
            size_t plen = 0;
            while (plen < 4096 && p[plen] != 0) ++plen;
            std::string path(reinterpret_cast<const char*>(p), plen);
            bool is_dir = !path.empty() && path.back() == '/';
            std::string err;
            if (!openrar::archive::validate_archive_path(path, is_dir, err)) {
                set_error(RAR_ERR_INVALID_ARG, "path " + path + ": " + err);
                return RAR_ERR_INVALID_ARG;
            }
            files.emplace_back(std::move(path), std::vector<uint8_t>(d, d + s));
        }

        std::vector<uint8_t> out;
        int rc = openrar::archive::create_archive(files, out, method, window_log2);
        if (rc != RAR_OK) return fail_code(rc);

        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) {
            set_error(RAR_ERR_NOMEM, "out of memory");
            return RAR_ERR_NOMEM;
        }
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

// ── v2 entry points ──────────────────────────────────────────────────────────

int openrar_archive_create2(const ArchiveInputFile* files_arr, uint32_t file_count,
                            const ArchiveCreateOpts* opts, const ArchiveHooks* hooks,
                            uint8_t** out_ptr, size_t* out_len) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!out_ptr || !out_len) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;
        if (!files_arr && file_count != 0) {
            set_error(RAR_ERR_INVALID_ARG, "null files array");
            return RAR_ERR_INVALID_ARG;
        }

        const int method = opts ? opts->method : 3;
        const uint32_t window_log2 = opts ? opts->window_log2 : 4;
        if (method != 0 && method != 3 && method != 5) {
            set_error(RAR_ERR_INVALID_ARG, "method must be 0, 3, or 5");
            return RAR_ERR_INVALID_ARG;
        }
        if (window_log2 < 1 || window_log2 > 4) {
            set_error(RAR_ERR_INVALID_ARG, "window_log2 must be in [1, 4]");
            return RAR_ERR_INVALID_ARG;
        }

        std::vector<openrar::archive::ArchiveFileInput> files;
        files.reserve(file_count);
        for (uint32_t i = 0; i < file_count; ++i) {
            const ArchiveInputFile& f = files_arr[i];
            if (!f.path) {
                set_error(RAR_ERR_INVALID_ARG, "null path");
                return RAR_ERR_INVALID_ARG;
            }
            std::string path(f.path); // NUL-terminated by contract
            const bool slash_dir = !path.empty() && path.back() == '/';
            if (f.is_dir && !slash_dir) {
                set_error(RAR_ERR_INVALID_ARG, "is_dir set but path does not end in '/': " + path);
                return RAR_ERR_INVALID_ARG;
            }
            std::string err;
            if (!openrar::archive::validate_archive_path(path, slash_dir, err)) {
                set_error(RAR_ERR_INVALID_ARG, "path " + path + ": " + err);
                return RAR_ERR_INVALID_ARG;
            }
            openrar::archive::ArchiveFileInput fi;
            fi.path = std::move(path);
            if (f.data && f.data_len > 0) fi.data.assign(f.data, f.data + f.data_len);
            fi.mtime_unix = f.mtime_unix; // 0 ⇒ now() in the core overload
            files.push_back(std::move(fi));
        }

        std::vector<uint8_t> out;
        int rc = openrar::archive::create_archive(
            files, out, method, window_log2, hooks ? hooks->progress : nullptr,
            hooks ? hooks->progress_user : nullptr, hooks ? hooks->cancel : nullptr,
            hooks ? hooks->cancel_user : nullptr);
        if (rc != RAR_OK) return fail_code(rc);

        uint8_t* buf = static_cast<uint8_t*>(std::malloc(out.size()));
        if (!buf) {
            set_error(RAR_ERR_NOMEM, "out of memory");
            return RAR_ERR_NOMEM;
        }
        std::memcpy(buf, out.data(), out.size());
        *out_ptr = buf;
        *out_len = out.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

int openrar_archive_extract_all2(const uint8_t* data, size_t size, const ArchiveHooks* hooks,
                                 uint8_t** buf_out_ptr, size_t* buf_size_out,
                                 uint64_t** offsets_out_ptr, uint32_t* offsets_count_out) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!data || !buf_out_ptr || !buf_size_out || !offsets_out_ptr || !offsets_count_out) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *buf_out_ptr = nullptr;
        *buf_size_out = 0;
        *offsets_out_ptr = nullptr;
        *offsets_count_out = 0;

        openrar::archive::BufferArchive ba;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        int rc =
            ba.extract_all(data, size, files, hooks ? hooks->progress : nullptr,
                           hooks ? hooks->progress_user : nullptr, hooks ? hooks->cancel : nullptr,
                           hooks ? hooks->cancel_user : nullptr);
        if (rc != RAR_OK) return fail_code(rc);

        return pack_extract_all(files, buf_out_ptr, buf_size_out, offsets_out_ptr,
                                offsets_count_out);
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

int openrar_archive_get_error(char* buf, int buf_len) {
    return g_err.copy_message_to(buf, buf_len);
}

int openrar_archive_last_error_code(void) {
    return g_err.code();
}

void* openrar_archive_alloc(size_t bytes) {
    return std::malloc(bytes);
}
void openrar_archive_free(void* ptr) {
    std::free(ptr);
}

} // extern "C"

// ── Handle API ───────────────────────────────────────────────────────────────
#include <unordered_map>
#include <mutex>

namespace {
struct ArchiveHandle {
    std::vector<uint8_t> data;
    openrar::archive::BufferArchive ba;
    std::vector<openrar::archive::BufferArchiveEntry> entries;
};
// Shared contract table: pin() hands out a shared_ptr so work and host
// callbacks run with NO table lock held (report L12).
openrar::api::HandleTable<ArchiveHandle> g_handles;
} // namespace

extern "C" {

uint32_t openrar_archive_open(const uint8_t* data, size_t size) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!data || size == 0) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return 0;
        }
        auto h = std::make_shared<ArchiveHandle>();
        h->data.assign(data, data + size);
        std::vector<openrar::archive::BufferArchiveEntry> parsed;
        int rc = h->ba.list(h->data.data(), h->data.size(), parsed);
        if (rc != RAR_OK) {
            // Record the real code — the JS wrapper reads it back instead of
            // guessing NOT_RAR for every open failure.
            set_error(rc, "open failed");
            return 0;
        }
        h->entries = std::move(parsed);
        return g_handles.insert(h);
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return 0;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return 0;
    }
}

void openrar_archive_close(uint32_t handle) {
    try {
        g_handles.erase(handle);
    } catch (const std::exception& e) {
        set_error(RAR_ERR_IO, e.what());
    } catch (...) {
        set_error(RAR_ERR_IO, "unknown C++ exception");
    }
}

int openrar_archive_handle_list(uint32_t handle, uint32_t* count, void** entries_out,
                                void** paths_out, size_t* paths_size_out) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!count || !entries_out || !paths_out || !paths_size_out) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return RAR_ERR_INVALID_ARG;
        }
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error(RAR_ERR_INVALID_ARG, "invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        *count = 0;
        *entries_out = nullptr;
        *paths_out = nullptr;
        *paths_size_out = 0;
        std::vector<uint8_t> entries;
        std::vector<uint8_t> paths;
        if (!openrar::api::pack_entries(h->entries, entries, paths)) {
            set_error(RAR_ERR_NOMEM, "pack failed");
            return RAR_ERR_NOMEM;
        }
        // heap_dup contract: empty buffers yield nullptr, not a malloc(0)
        // quirk that could be misread as OOM (report M12).
        uint8_t* heap_entries = openrar::api::heap_dup(entries.data(), entries.size());
        if (!heap_entries && !entries.empty()) {
            set_error(RAR_ERR_NOMEM, "out of memory (entries)");
            return RAR_ERR_NOMEM;
        }
        uint8_t* heap_paths = openrar::api::heap_dup(paths.data(), paths.size());
        if (!heap_paths && !paths.empty()) {
            std::free(heap_entries);
            set_error(RAR_ERR_NOMEM, "out of memory (paths)");
            return RAR_ERR_NOMEM;
        }
        *count = static_cast<uint32_t>(h->entries.size());
        *entries_out = heap_entries;
        *paths_out = heap_paths;
        *paths_size_out = paths.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

int openrar_archive_handle_extract(uint32_t handle, uint32_t entry_index, uint8_t** out_ptr,
                                   size_t* out_len) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!out_ptr || !out_len) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *out_ptr = nullptr;
        *out_len = 0;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error(RAR_ERR_INVALID_ARG, "invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        if (entry_index >= h->entries.size()) {
            set_error(RAR_ERR_INVALID_ARG, "entry_index out of range");
            return RAR_ERR_INVALID_ARG;
        }
        std::vector<uint8_t> out;
        int rc = h->ba.extract(h->data.data(), h->data.size(), entry_index, out);
        if (rc != RAR_OK) return fail_code(rc);
        *out_ptr = openrar::api::heap_dup(out.data(), out.size());
        if (!*out_ptr && !out.empty()) {
            set_error(RAR_ERR_NOMEM, "out of memory");
            return RAR_ERR_NOMEM;
        }
        *out_len = out.size();
        return RAR_OK;
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

int openrar_archive_handle_extract_all(uint32_t handle, uint8_t** buf_out_ptr, size_t* buf_size_out,
                                       uint64_t** offsets_out_ptr, uint32_t* offsets_count_out) {
    return openrar_archive_handle_extract_all2(handle, nullptr, buf_out_ptr, buf_size_out,
                                               offsets_out_ptr, offsets_count_out);
}

int openrar_archive_handle_extract_all2(uint32_t handle, const ArchiveHooks* hooks,
                                        uint8_t** buf_out_ptr, size_t* buf_size_out,
                                        uint64_t** offsets_out_ptr, uint32_t* offsets_count_out) {
    g_err.reset_code(); // contract: code of the most recent call
    try {
        if (!buf_out_ptr || !buf_size_out || !offsets_out_ptr || !offsets_count_out) {
            set_error(RAR_ERR_INVALID_ARG, "null argument");
            return RAR_ERR_INVALID_ARG;
        }
        *buf_out_ptr = nullptr;
        *buf_size_out = 0;
        *offsets_out_ptr = nullptr;
        *offsets_count_out = 0;
        auto h = g_handles.pin(handle);
        if (!h) {
            set_error(RAR_ERR_INVALID_ARG, "invalid handle");
            return RAR_ERR_INVALID_ARG;
        }
        // pin() releases the table lock before this point: progress/cancel
        // callbacks run with no handle-table lock held (report L12), and the
        // shared_ptr keeps the handle alive even if another thread closes it.
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        int rc = h->ba.extract_all(
            h->data.data(), h->data.size(), files, hooks ? hooks->progress : nullptr,
            hooks ? hooks->progress_user : nullptr, hooks ? hooks->cancel : nullptr,
            hooks ? hooks->cancel_user : nullptr);
        if (rc != RAR_OK) return fail_code(rc);
        return pack_extract_all(files, buf_out_ptr, buf_size_out, offsets_out_ptr,
                                offsets_count_out);
    } catch (const std::exception& e) {
        set_error(RAR_ERR_NOMEM, e.what());
        return RAR_ERR_NOMEM;
    } catch (...) {
        set_error(RAR_ERR_NOMEM, "unknown C++ exception");
        return RAR_ERR_NOMEM;
    }
}

} // extern "C"
