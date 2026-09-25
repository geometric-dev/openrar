#include "mapped_file.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace openrar::io {

// ── Windows fault-guard leaf ────────────────────────────────────────────────
// SEH (__try/__except) must live in a function without objects requiring
// unwinding, so the copy is a static leaf over raw pointers. An access
// violation or in-page error (truncation after map) yields a short read.
#ifdef _WIN32

namespace {
// Copies through the mapping under SEH; returns bytes copied (0 on fault).
size_t guarded_copy(const core::byte* base, size_t map_len, core::uint64 off, void* dst,
                    size_t bytes) {
    __try {
        std::memcpy(dst, base + off, bytes);
        return bytes;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                        GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return 0; // mapping fault: clean short read, never a crash
    }
}
} // namespace

struct MappedFile::Impl {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    void* base = nullptr;
    size_t length = 0;
};

MappedFile::MappedFile() : impl_(new Impl) {}

MappedFile::~MappedFile() {
    close();
    delete impl_;
}

bool MappedFile::open(const std::filesystem::path& path) {
    close();
    impl_->file = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(impl_->file, &li) || li.QuadPart <= 0) {
        close();
        return false; // empty files need no mapping; callers use buffered reads
    }
    const size_t len = static_cast<size_t>(li.QuadPart);
    impl_->mapping = CreateFileMappingW(impl_->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (impl_->mapping == nullptr) {
        close();
        return false;
    }
    impl_->base = MapViewOfFile(impl_->mapping, FILE_MAP_READ, 0, 0, 0);
    if (impl_->base == nullptr) {
        close();
        return false;
    }
    impl_->length = len;
    path_ = path;
    size_ = len;
    cursor_ = 0;
    return true;
}

void MappedFile::close() {
    if (impl_->base != nullptr) UnmapViewOfFile(impl_->base);
    if (impl_->mapping != nullptr) CloseHandle(impl_->mapping);
    if (impl_->file != INVALID_HANDLE_VALUE) CloseHandle(impl_->file);
    impl_->base = nullptr;
    impl_->mapping = nullptr;
    impl_->file = INVALID_HANDLE_VALUE;
    impl_->length = 0;
    path_.clear();
    size_ = 0;
    cursor_ = 0;
}

bool MappedFile::is_open() const {
    return impl_->base != nullptr;
}
core::uint64 MappedFile::size() const {
    return size_;
}

bool MappedFile::seek(core::int64 offset, SeekOrigin origin) {
    core::uint64 next = cursor_;
    if (origin == SeekOrigin::Begin) {
        next = offset < 0 ? 0 : static_cast<core::uint64>(offset);
    } else if (origin == SeekOrigin::Current) {
        next = static_cast<core::uint64>(static_cast<core::int64>(cursor_) + offset);
    } else {
        next = static_cast<core::uint64>(static_cast<core::int64>(size_) + offset);
    }
    if (next > size_) return false;
    cursor_ = next;
    return true;
}

core::uint64 MappedFile::tell() const {
    return cursor_;
}

size_t MappedFile::read(void* dest, size_t bytes) {
    const size_t n = read_at(cursor_, dest, bytes);
    cursor_ += n;
    return n;
}

size_t MappedFile::read_at(core::uint64 offset, void* dest, size_t bytes) {
    if (impl_->base == nullptr || bytes == 0) return 0;
    if (offset >= size_) return 0;
    const size_t take = static_cast<size_t>(std::min<core::uint64>(bytes, size_ - offset));
    return guarded_copy(static_cast<const core::byte*>(impl_->base), impl_->length, offset, dest,
                        take);
}

#else // POSIX

struct MappedFile::Impl {
    int fd = -1;
    void* base = nullptr;
    size_t length = 0;
};

MappedFile::MappedFile() : impl_(new Impl) {}

MappedFile::~MappedFile() {
    close();
    delete impl_;
}

bool MappedFile::open(const std::filesystem::path& path) {
    close();
    impl_->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (impl_->fd < 0) return false;
    struct stat st;
    if (::fstat(impl_->fd, &st) != 0 || st.st_size <= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
        return false; // empty files need no mapping; callers use buffered reads
    }
    const size_t len = static_cast<size_t>(st.st_size);
    void* base = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, impl_->fd, 0);
    if (base == MAP_FAILED) {
        ::close(impl_->fd);
        impl_->fd = -1;
        return false;
    }
    impl_->base = base;
    impl_->length = len;
    path_ = path;
    size_ = len;
    cursor_ = 0;
    return true;
}

void MappedFile::close() {
    if (impl_->base != nullptr) {
        ::munmap(impl_->base, impl_->length);
        impl_->base = nullptr;
        impl_->length = 0;
    }
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    path_.clear();
    size_ = 0;
    cursor_ = 0;
}

bool MappedFile::is_open() const {
    return impl_->base != nullptr;
}
core::uint64 MappedFile::size() const {
    return size_;
}

bool MappedFile::seek(core::int64 offset, SeekOrigin origin) {
    core::uint64 next = cursor_;
    if (origin == SeekOrigin::Begin) {
        next = offset < 0 ? 0 : static_cast<core::uint64>(offset);
    } else if (origin == SeekOrigin::Current) {
        next = static_cast<core::uint64>(static_cast<core::int64>(cursor_) + offset);
    } else {
        next = static_cast<core::uint64>(static_cast<core::int64>(size_) + offset);
    }
    if (next > size_) return false;
    cursor_ = next;
    return true;
}

core::uint64 MappedFile::tell() const {
    return cursor_;
}

size_t MappedFile::read(void* dest, size_t bytes) {
    const size_t n = read_at(cursor_, dest, bytes);
    cursor_ += n;
    return n;
}

size_t MappedFile::read_at(core::uint64 offset, void* dest, size_t bytes) {
    if (impl_->base == nullptr || impl_->fd < 0 || bytes == 0) return 0;
    if (offset >= size_) return 0;
    size_t take = static_cast<size_t>(std::min<core::uint64>(bytes, size_ - offset));
    // Truncation-aware pre-flight (plan §0): the file may have shrunk since
    // the map. Clamp to the CURRENT size so we never touch pages beyond the
    // new EOF; a short result signals truncation to the caller, which falls
    // back to buffered reads. (Residual: a truncation landing between this
    // check and the copy can still SIGBUS — accepted, no signal handlers.)
    struct stat st;
    if (::fstat(impl_->fd, &st) != 0 || st.st_size <= 0) return 0;
    if (offset >= static_cast<core::uint64>(st.st_size)) return 0;
    const size_t valid = static_cast<size_t>(
        std::min<core::uint64>(take, static_cast<core::uint64>(st.st_size) - offset));
    if (valid == 0) return 0;
    std::memcpy(dest, static_cast<const core::byte*>(impl_->base) + offset, valid);
    return valid;
}

#endif // _WIN32

} // namespace openrar::io
