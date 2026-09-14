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

// Archive handle RAII
class ArchiveHandle {
public:
    explicit ArchiveHandle(const std::vector<uint8_t>& rar)
        : ArchiveHandle(rar.data(), rar.size()) {}
    ArchiveHandle(const uint8_t* data, size_t size) {
        h_ = openrar_archive_open(data, size);
        if (h_ == 0) {
            char b[512] = {};
            openrar_archive_get_error(b, sizeof(b));
            throw std::runtime_error(b[0] ? b : "open failed");
        }
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
    uint32_t native_handle() const { return h_; }

private:
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
