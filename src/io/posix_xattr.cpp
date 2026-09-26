#include "posix_xattr.hpp"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__wasm__)

#include <cerrno>
#include <cstddef>
#include <sys/xattr.h>

namespace openrar::io {

namespace {

// The no-follow call shapes differ: Linux exposes the l-variants directly;
// macOS gates those declarations behind _DARWIN_C_SOURCE, so it uses the
// canonical option-flag API (XATTR_NOFOLLOW == the l- semantics).
#if defined(__APPLE__)
ssize_t llist(const std::filesystem::path& p, char* buf, size_t size) {
    return listxattr(p.c_str(), buf, size, XATTR_NOFOLLOW);
}
ssize_t lget(const std::filesystem::path& p, const char* name, void* buf, size_t size) {
    return getxattr(p.c_str(), name, buf, size, 0, XATTR_NOFOLLOW);
}
int lset(const std::filesystem::path& p, const char* name, const void* buf, size_t size) {
    return setxattr(p.c_str(), name, buf, size, 0, XATTR_NOFOLLOW);
}
#else
ssize_t llist(const std::filesystem::path& p, char* buf, size_t size) {
    return llistxattr(p.c_str(), buf, size);
}
ssize_t lget(const std::filesystem::path& p, const char* name, void* buf, size_t size) {
    return lgetxattr(p.c_str(), name, buf, size);
}
int lset(const std::filesystem::path& p, const char* name, const void* buf, size_t size) {
    return lsetxattr(p.c_str(), name, buf, size, 0);
}
#endif

// Probe cap: the name list and any single value are bounded well above
// every legal size (Linux XATTR_LIST_MAX / XATTR_SIZE_MAX are 64 KiB each);
// a source beyond the cap is treated as unreadable rather than growing
// without bound.
constexpr size_t kProbeMax = 1u << 20;

} // namespace

bool list_xattrs(const std::filesystem::path& path, std::vector<std::string>& out_names) {
    out_names.clear();
    // Probe-then-read with a bounded grow loop: the list can change between
    // the two calls, so ERANGE means "grow and retry" — beyond the cap the
    // attribute list is treated as unreadable (fail-soft, plan FMM row C).
    std::vector<char> buf;
    for (size_t size = 512; size <= kProbeMax; size *= 2) {
        buf.assign(size, '\0');
        const ssize_t got = llist(path, buf.data(), size);
        if (got < 0) {
            if (errno == ERANGE) continue;
            // ENOTSUP/ENOSYS/EOPNOTSUPP: the filesystem carries no xattrs.
            // EPERM-class list failures: unreadable. Both -> "no attributes".
            return false;
        }
        size_t off = 0;
        while (off < static_cast<size_t>(got)) {
            const char* start = buf.data() + off;
            size_t len = strnlen(start, static_cast<size_t>(got) - off);
            if (len == 0 || off + len >= static_cast<size_t>(got)) {
                out_names.clear(); // malformed list — fail-soft to empty
                return false;
            }
            out_names.emplace_back(start, len);
            off += len + 1;
        }
        return true;
    }
    return false;
}

bool get_xattr(const std::filesystem::path& path, const std::string& name,
               std::vector<core::byte>& out_value) {
    out_value.clear();
    // Size probe, then read with one grow retry (the value can change
    // between calls).
    for (size_t size = 256; size <= kProbeMax; size *= 2) {
        std::vector<char> buf(size);
        const ssize_t got = lget(path, name.c_str(), buf.data(), size);
        if (got < 0) {
            if (errno == ERANGE) continue;
            return false;
        }
        out_value.assign(reinterpret_cast<core::byte*>(buf.data()),
                         reinterpret_cast<core::byte*>(buf.data()) + static_cast<size_t>(got));
        return true;
    }
    return false;
}

bool set_xattr(const std::filesystem::path& path, const std::string& name, const core::byte* data,
               size_t size) {
    return lset(path, name.c_str(), data, size) == 0;
}

} // namespace openrar::io

#else // _WIN32 / WASM: platform-gated — capture and restore are no-ops

namespace openrar::io {

bool list_xattrs(const std::filesystem::path&, std::vector<std::string>& out_names) {
    out_names.clear();
    return false;
}

bool get_xattr(const std::filesystem::path&, const std::string&, std::vector<core::byte>&) {
    return false;
}

bool set_xattr(const std::filesystem::path&, const std::string&, const core::byte*, size_t) {
    return false;
}

} // namespace openrar::io

#endif
