#ifndef OPENRAR_OPENRAR_HPP
#define OPENRAR_OPENRAR_HPP

#include "openrar_dll.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace openrar {

inline void check(int rc) {
    if (rc == RAR_OK || rc == RAR_ERR_PARTIAL_OK) return;
    char buf[512] = {};
    openrar_archive_get_error(buf, sizeof(buf));
    std::string msg = buf[0] ? buf : "openrar error";
    throw std::runtime_error(msg + " (code " + std::to_string(rc) + ")");
}

struct Entry {
    std::string path;
    uint64_t size{0};
    uint64_t packed_size{0};
    uint64_t mtime{0};
    uint32_t crc32{0};
    uint32_t method{0};
    bool is_dir{false};
    bool is_encrypted{false};
    uint32_t index{0};
};

struct InputFile {
    std::string path;
    std::vector<uint8_t> data;
    uint64_t mtime{0};
};

struct CreateOptions {
    int method{3};
    uint32_t window_log2{4};
};

// ── Block codec ──────────────────────────────────────────────────────────────
inline std::vector<uint8_t> compress_block(const uint8_t* data, size_t size, int method = 3,
                                           size_t win_size = 2 * 1024 * 1024) {
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_compress2(data, size, &out, &out_len, method, win_size);
    if (rc != 1) throw std::runtime_error("compress failed");
    std::vector<uint8_t> ret;
    if (out && out_len) {
        ret.assign(out, out + out_len);
        openrar_free(out);
    }
    return ret;
}
inline std::vector<uint8_t> compress_block(const std::vector<uint8_t>& src, int method = 3,
                                           size_t win_size = 2 * 1024 * 1024) {
    return compress_block(src.data(), src.size(), method, win_size);
}
inline std::vector<uint8_t> decompress_block(const uint8_t* data, size_t size,
                                             size_t win_size = 1024 * 1024) {
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_decompress2(data, size, &out, &out_len, win_size);
    if (rc != 1) throw std::runtime_error("decompress failed");
    std::vector<uint8_t> ret;
    if (out && out_len) {
        ret.assign(out, out + out_len);
        openrar_free(out);
    }
    return ret;
}
inline std::vector<uint8_t> decompress_block(const std::vector<uint8_t>& src,
                                             size_t win_size = 1024 * 1024) {
    return decompress_block(src.data(), src.size(), win_size);
}

// ── Archive helpers ──────────────────────────────────────────────────────────
// Unpack a malloc'd entries array + contiguous paths blob (as returned by the
// list exports) into Entry values. Pair with openrar_archive_list_free.
inline std::vector<Entry> unpack_entries(uint32_t count, void* entries, void* paths) {
    auto* ents = static_cast<openrar_archive_entry_t*>(entries);
    const char* pstr = static_cast<const char*>(paths);
    std::vector<Entry> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        e.path = std::string(pstr + ents[i].path_offset, ents[i].path_len);
        e.is_dir = ents[i].is_dir != 0;
        e.method = ents[i].method;
        e.is_encrypted = ents[i].is_encrypted != 0;
        e.crc32 = ents[i].crc32;
        e.size = ents[i].size;
        e.packed_size = ents[i].packed_size;
        e.mtime = ents[i].mtime;
        e.index = i;
        out.push_back(std::move(e));
    }
    return out;
}

inline std::vector<Entry> list_archive(const uint8_t* data, size_t size) {
    uint32_t count = 0;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_sz = 0;
    int rc = openrar_archive_list(data, size, &count, &entries, &paths, &paths_sz);
    check(rc);
    std::vector<Entry> out = unpack_entries(count, entries, paths);
    openrar_archive_list_free(entries, paths, paths_sz);
    return out;
}
inline std::vector<Entry> list_archive(const std::vector<uint8_t>& rar) {
    return list_archive(rar.data(), rar.size());
}

inline std::vector<Entry> list_archive_file(const std::filesystem::path& arc) {
    uint32_t count = 0;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_sz = 0;
    std::string u8 = arc.u8string();
    int rc = openrar_archive_list_file(u8.c_str(), &count, &entries, &paths, &paths_sz);
    check(rc);
    std::vector<Entry> out = unpack_entries(count, entries, paths);
    openrar_archive_list_free(entries, paths, paths_sz);
    return out;
}

// ── Listing with progress / cancel (_ex ABI; callbacks may be null) ──────────
// Byte-based progress (done = archive bytes consumed, total = archive size)
// and a cancel poll between header blocks; the file variant streams from disk
// instead of slurping. See openrar_dll.h for the full contract. RAR_ERR_
// ENCRYPTED (header-encrypted archive) and RAR_ERR_ABORTED throw via check()
// like any other non-OK code.
inline std::vector<Entry> list_archive(const uint8_t* data, size_t size,
                                       openrar_progress_cb progress, openrar_cancel_cb cancel,
                                       void* user) {
    uint32_t count = 0;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_sz = 0;
    int rc = openrar_archive_list_ex(data, size, &count, &entries, &paths, &paths_sz, progress,
                                     cancel, user);
    check(rc);
    std::vector<Entry> out = unpack_entries(count, entries, paths);
    openrar_archive_list_free(entries, paths, paths_sz);
    return out;
}
inline std::vector<Entry> list_archive(const std::vector<uint8_t>& rar,
                                       openrar_progress_cb progress,
                                       openrar_cancel_cb cancel = nullptr, void* user = nullptr) {
    return list_archive(rar.data(), rar.size(), progress, cancel, user);
}
inline std::vector<Entry> list_archive_file(const std::filesystem::path& arc,
                                            openrar_progress_cb progress,
                                            openrar_cancel_cb cancel = nullptr,
                                            void* user = nullptr) {
    uint32_t count = 0;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_sz = 0;
    std::string u8 = arc.u8string();
    int rc = openrar_archive_list_file_ex(u8.c_str(), &count, &entries, &paths, &paths_sz, progress,
                                          cancel, user);
    check(rc);
    std::vector<Entry> out = unpack_entries(count, entries, paths);
    openrar_archive_list_free(entries, paths, paths_sz);
    return out;
}

// ── Password listing (_pw ABI) ───────────────────────────────────────────────
// Streams the archive from disk, decrypting a header-encrypted (-hp) archive
// with `password`; file entries with encrypted payloads are reported with
// is_encrypted = 1 and the walk continues. Wrong password throws with
// RAR_ERR_BAD_PASSWORD; NULL/empty password on a header-encrypted archive
// throws with RAR_ERR_ENCRYPTED. See openrar_dll.h for the full contract.
// (Passing nullptr as the second argument is ambiguous with the callback
// overload above — pass "" for "no password" or use the callback overload.)
inline std::vector<Entry> list_archive_file(const std::filesystem::path& arc, const char* password,
                                            openrar_progress_cb progress = nullptr,
                                            openrar_cancel_cb cancel = nullptr,
                                            void* user = nullptr) {
    uint32_t count = 0;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_sz = 0;
    std::string u8 = arc.u8string();
    int rc = openrar_archive_list_file_pw(u8.c_str(), password, &count, &entries, &paths, &paths_sz,
                                          progress, cancel, user);
    check(rc);
    std::vector<Entry> out = unpack_entries(count, entries, paths);
    openrar_archive_list_free(entries, paths, paths_sz);
    return out;
}

inline std::vector<uint8_t> extract_file(const uint8_t* data, size_t size, uint32_t index) {
    uint8_t* out = nullptr;
    size_t len = 0;
    int rc = openrar_archive_extract(data, size, index, &out, &len);
    check(rc);
    std::vector<uint8_t> ret;
    if (out && len) ret.assign(out, out + len);
    if (out) openrar_free(out);
    return ret;
}
inline std::vector<uint8_t> extract_file(const std::vector<uint8_t>& rar, uint32_t index) {
    return extract_file(rar.data(), rar.size(), index);
}

inline std::map<std::string, std::vector<uint8_t>> extract_all(const uint8_t* data, size_t size) {
    uint8_t* buf = nullptr;
    size_t buf_sz = 0;
    uint64_t* offs = nullptr;
    uint32_t cnt = 0;
    int rc = openrar_archive_extract_all(data, size, &buf, &buf_sz, &offs, &cnt);
    check(rc);
    auto entries = list_archive(data, size);
    std::map<std::string, std::vector<uint8_t>> out;
    for (uint32_t i = 0; i < cnt && i < entries.size(); ++i) {
        uint64_t off = offs[i * 2], sz = offs[i * 2 + 1];
        std::vector<uint8_t> v;
        if (sz && off + sz <= buf_sz) v.assign(buf + off, buf + off + sz);
        out[entries[i].path] = std::move(v);
    }
    if (buf) openrar_free(buf);
    if (offs) openrar_free(offs);
    return out;
}
inline std::map<std::string, std::vector<uint8_t>> extract_all(const std::vector<uint8_t>& rar) {
    return extract_all(rar.data(), rar.size());
}

inline std::vector<uint8_t> create_archive(const std::vector<InputFile>& files,
                                           CreateOptions opts = {}) {
    std::vector<const uint8_t*> path_ptrs;
    path_ptrs.reserve(files.size());
    std::vector<const uint8_t*> data_ptrs;
    data_ptrs.reserve(files.size());
    std::vector<size_t> sizes;
    sizes.reserve(files.size());
    std::vector<std::string> paths;
    paths.reserve(files.size());
    for (auto& f : files) paths.push_back(f.path);
    for (size_t i = 0; i < files.size(); ++i) {
        path_ptrs.push_back(reinterpret_cast<const uint8_t*>(paths[i].c_str()));
        data_ptrs.push_back(files[i].data.data());
        sizes.push_back(files[i].data.size());
    }
    uint8_t* out = nullptr;
    size_t out_len = 0;
    int rc = openrar_archive_create(path_ptrs.data(), data_ptrs.data(), sizes.data(),
                                    static_cast<uint32_t>(files.size()), opts.method,
                                    opts.window_log2, &out, &out_len);
    check(rc);
    std::vector<uint8_t> ret;
    if (out && out_len) ret.assign(out, out + out_len);
    if (out) openrar_free(out);
    return ret;
}

// ── Archive mutation (v1.4.0; atomic write-path over an existing archive) ───
// Free functions over the same engine the CLI uses (temp + flush + atomic
// replace; the original is untouched on any failure). Deliberately not
// ArchiveHandle methods: a mutation rewrites the archive and invalidates
// every cached walk, so re-open afterwards (indices shift, handles go
// stale). Both refuse to run while a file-mode ArchiveHandle still holds
// the archive open (RAR_ERR_BUSY) — close it first.

// Delete entries by index. Indices are in the FILE-HANDLE LISTING ORDER —
// the sequence ArchiveHandle::list() reports on a file-mode archive (file
// entries only; service headers like CMT/RR are never exposed). Deleting a
// member of a solid run while a later member is retained throws
// UNSUPPORTED_FEATURE (suffix-only delete).
inline void delete_entries(const std::filesystem::path& arc,
                           const std::vector<uint32_t>& indices) {
    std::string u8 = arc.u8string();
    int rc = openrar_archive_delete_entries_file(u8.c_str(), indices.data(),
                                                 static_cast<uint32_t>(indices.size()));
    check(rc);
}

// Batch add/replace with 'u' semantics: every existing entry whose name
// equals an incoming archive name is replaced by it. files pairs a source
// path on disk with the archive name it should get ('/' separators;
// '\' is normalized). Directory sources become directory records
// (non-recursive). Added files are written unencrypted.
struct AddOptions {
    int method{3};
    uint32_t window_log2{4};
};

inline void add_files(const std::filesystem::path& arc,
                      const std::vector<std::pair<std::filesystem::path, std::string>>& files,
                      AddOptions opts = {}) {
    std::vector<std::string> srcs;
    std::vector<std::string> names;
    srcs.reserve(files.size());
    names.reserve(files.size());
    for (const auto& f : files) {
        srcs.push_back(f.first.u8string());
        names.push_back(f.second);
    }
    std::vector<const char*> src_ptrs;
    std::vector<const char*> name_ptrs;
    src_ptrs.reserve(files.size());
    name_ptrs.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        src_ptrs.push_back(srcs[i].c_str());
        name_ptrs.push_back(names[i].c_str());
    }
    std::string u8 = arc.u8string();
    int rc = openrar_archive_add_files_file(u8.c_str(), src_ptrs.data(), name_ptrs.data(),
                                            static_cast<uint32_t>(files.size()), opts.method,
                                            opts.window_log2);
    check(rc);
}

// ── Extended metadata (v1.5.0; file-mode handles only) ──────────────────────
// Fields the frozen 64-byte entry struct drops. Timestamps are Windows
// FILETIMEs (UTC, 100ns); 0 = not stored — cross-check the HAS_* bits of
// flags. Buffer handles throw UNSUPPORTED_FEATURE.
struct EntryEx {
    uint32_t attrs{0};
    uint32_t host_os{0}; // 0 = Windows, 1 = Unix
    uint32_t flags{0};   // OPENRAR_ENTRY_FLAG_* bitmask
    uint32_t win_size{0};
    uint32_t redir_type{0}; // 0 none / 1 unixsymlink / 2 winsymlink / 3 junction /
                            // 4 hardlink / 5 filecopy
    uint32_t version_needed{0};
    uint64_t mtime_ft{0};
    uint64_t ctime_ft{0};
    uint64_t atime_ft{0};
    std::string redir_target; // empty when redir_type == 0
};

struct ArchiveInfo {
    uint32_t flags{0};        // main-header MHFL_* flags
    uint32_t volume_index{0}; // 0-based volume number
    uint32_t volume_count{0}; // 1 for single-volume (file-mode opens are strict)
    uint64_t recovery_size{0};
    std::string comment; // empty when the archive has none
};

// Archive handle RAII
class ArchiveHandle {
public:
    explicit ArchiveHandle(const std::vector<uint8_t>& rar)
        : ArchiveHandle(rar.data(), rar.size()) {}
    ArchiveHandle(const uint8_t* data, size_t size) {
        h_ = openrar_archive_open(data, size);
        fail_if_null();
    }
    // Progress/cancel over the scan that happens at open time (open_ex ABI).
    // A cancelled scan throws with the "open aborted" detail.
    ArchiveHandle(const uint8_t* data, size_t size, openrar_progress_cb progress,
                  openrar_cancel_cb cancel, void* user = nullptr) {
        h_ = openrar_archive_open_ex(data, size, progress, cancel, user);
        fail_if_null();
    }
    ArchiveHandle(const std::vector<uint8_t>& rar, openrar_progress_cb progress,
                  openrar_cancel_cb cancel = nullptr, void* user = nullptr)
        : ArchiveHandle(rar.data(), rar.size(), progress, cancel, user) {}
    // File-mode handle (openrar_archive_open_file): scan-once handle over an
    // archive on disk, with password (NULL/"" = none; required up front for
    // -hp archives), progress/cancel over the open-time scan, extract_to_path
    // and test below. Encrypted/solid/multi-volume archives are supported on
    // this handle kind (rejected by the buffer constructors).
    ArchiveHandle(const std::filesystem::path& arc, const char* password = nullptr,
                  openrar_progress_cb progress = nullptr, openrar_cancel_cb cancel = nullptr,
                  void* user = nullptr) {
        std::string u8 = arc.u8string();
        h_ = openrar_archive_open_file(u8.c_str(), password, progress, cancel, user);
        fail_if_null();
    }
    ~ArchiveHandle() {
        if (h_) openrar_archive_close(h_);
    }
    ArchiveHandle(const ArchiveHandle&) = delete;
    ArchiveHandle& operator=(const ArchiveHandle&) = delete;
    ArchiveHandle(ArchiveHandle&& o) noexcept : h_(o.h_) { o.h_ = 0; }
    ArchiveHandle& operator=(ArchiveHandle&& o) noexcept {
        if (this != &o) {
            if (h_) openrar_archive_close(h_);
            h_ = o.h_;
            o.h_ = 0;
        }
        return *this;
    }
    std::vector<Entry> list() const {
        uint32_t count = 0;
        void* e = nullptr;
        void* p = nullptr;
        size_t ps = 0;
        int rc = openrar_archive_handle_list(h_, &count, &e, &p, &ps);
        check(rc);
        std::vector<Entry> out = unpack_entries(count, e, p);
        openrar_archive_list_free(e, p, ps);
        return out;
    }
    std::vector<uint8_t> extract(uint32_t idx) const {
        uint8_t* out = nullptr;
        size_t len = 0;
        int rc = openrar_archive_handle_extract(h_, idx, &out, &len);
        check(rc);
        std::vector<uint8_t> ret;
        if (out && len) ret.assign(out, out + len);
        if (out) openrar_free(out);
        return ret;
    }
    // Direct-to-disk extraction with the DLL-owned durability contract (temp
    // file + atomic rename; no partial destination on abort/failure) and
    // byte progress against entry.size. File handles: streaming; buffer
    // handles: in-memory extract then write.
    void extract_to_path(uint32_t idx, const std::filesystem::path& dest,
                         openrar_progress_cb progress = nullptr,
                         openrar_cancel_cb cancel = nullptr, void* user = nullptr) const {
        std::string u8 = dest.u8string();
        int rc = openrar_archive_handle_extract_to_path(h_, idx, u8.c_str(), progress, cancel,
                                                        user);
        check(rc);
    }
    // Streaming integrity test (file handles; buffer handles throw
    // UNSUPPORTED_FEATURE). Verifies CRC32 / BLAKE2sp without retaining output.
    void test(uint32_t idx, openrar_progress_cb progress = nullptr,
              openrar_cancel_cb cancel = nullptr, void* user = nullptr) const {
        int rc = openrar_archive_handle_test(h_, idx, progress, cancel, user);
        check(rc);
    }
    // Extended metadata query (file handles only; buffer handles throw
    // UNSUPPORTED_FEATURE). extra_out lands in redir_target.
    EntryEx entry_ex(uint32_t idx) const {
        openrar_entry_ex_t raw{};
        void* extra = nullptr;
        size_t extra_sz = 0;
        int rc = openrar_archive_handle_entry_ex(h_, idx, &raw, &extra, &extra_sz);
        check(rc);
        EntryEx e;
        e.attrs = raw.attrs;
        e.host_os = raw.host_os;
        e.flags = raw.flags;
        e.win_size = raw.win_size;
        e.redir_type = raw.redir_type;
        e.version_needed = raw.version_needed;
        e.mtime_ft = raw.mtime_ft;
        e.ctime_ft = raw.ctime_ft;
        e.atime_ft = raw.atime_ft;
        if (extra && extra_sz) e.redir_target.assign(static_cast<const char*>(extra), extra_sz);
        openrar_archive_entry_ex_free(extra);
        return e;
    }
    // Archive-level properties (file handles only; buffer handles throw).
    ArchiveInfo info() const {
        openrar_archive_info_t raw{};
        void* cmt = nullptr;
        size_t cmt_sz = 0;
        int rc = openrar_archive_handle_info(h_, &raw, &cmt, &cmt_sz);
        check(rc);
        ArchiveInfo ai;
        ai.flags = raw.flags;
        ai.volume_index = raw.volume_index;
        ai.volume_count = raw.volume_count;
        ai.recovery_size = raw.recovery_size;
        if (cmt && cmt_sz) ai.comment.assign(static_cast<const char*>(cmt), cmt_sz);
        openrar_free(cmt);
        return ai;
    }
    uint32_t native_handle() const { return h_; }

private:
    void fail_if_null() {
        if (h_ == 0) {
            char b[512] = {};
            openrar_archive_get_error(b, sizeof(b));
            throw std::runtime_error(b[0] ? b : "open failed");
        }
    }
    uint32_t h_{0};
};

// Streaming encoder RAII
class StreamEncoder {
public:
    explicit StreamEncoder(int method = 3, uint32_t window_log2 = 5) {
        h_ = openrar_stream_create(method, window_log2);
        if (h_ == 0) throw std::runtime_error("stream_create failed");
    }
    ~StreamEncoder() {
        if (h_) openrar_stream_free(h_);
    }
    StreamEncoder(const StreamEncoder&) = delete;
    StreamEncoder& operator=(const StreamEncoder&) = delete;
    StreamEncoder(StreamEncoder&& o) noexcept : h_(o.h_) { o.h_ = 0; }
    StreamEncoder& operator=(StreamEncoder&& o) noexcept {
        if (this != &o) {
            if (h_) openrar_stream_free(h_);
            h_ = o.h_;
            o.h_ = 0;
        }
        return *this;
    }
    void feed(const std::vector<uint8_t>& chunk) { feed(chunk.data(), chunk.size()); }
    void feed(const uint8_t* data, size_t size) {
        int rc = openrar_stream_feed(h_, data, size);
        check(rc);
    }
    std::vector<uint8_t> finish() {
        uint8_t* out = nullptr;
        size_t len = 0;
        int rc = openrar_stream_finish(h_, &out, &len);
        check(rc);
        std::vector<uint8_t> ret;
        if (out && len) ret.assign(out, out + len);
        if (out) openrar_free(out);
        return ret;
    }

private:
    uint32_t h_{0};
};

} // namespace openrar

#endif // OPENRAR_OPENRAR_HPP
