#include "file_stream.hpp"
#include <algorithm>
#include <cerrno>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace openrar::io {

FileStream::FileStream() : handle_(nullptr), mode_(FileMode::ReadOnly) {}

FileStream::~FileStream() {
    close();
}

FileStream::FileStream(FileStream&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)), mode_(other.mode_) {
    other.handle_ = nullptr;
}

FileStream& FileStream::operator=(FileStream&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        path_ = std::move(other.path_);
        mode_ = other.mode_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool FileStream::open(const std::filesystem::path& path, FileMode mode) {
    close();
    path_ = path;
    mode_ = mode;

#ifdef _WIN32
    DWORD access = 0;
    DWORD share = FILE_SHARE_READ;
    DWORD disposition = 0;

    switch (mode) {
    case FileMode::ReadOnly:
        access = GENERIC_READ;
        // INTENDED: READ is deliberately shared with WRITE so scans and
        // extractions of a volume set can proceed while another process
        // appends to it. A short/partial read of the growing tail is
        // caught by CRC checks, not by exclusive opens; do not drop
        // FILE_SHARE_WRITE.
        share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        disposition = OPEN_EXISTING;
        break;
    case FileMode::WriteOnly:
    case FileMode::CreateAlways:
        access = GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        break;
    case FileMode::CreateNew:
        // CREATE_NEW fails when the name already exists (including a
        // pre-planted symlink/hardlink), so it never truncates a victim
        // file through a planted link.
        access = GENERIC_WRITE;
        disposition = CREATE_NEW;
        break;
    case FileMode::ReadWrite:
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = OPEN_ALWAYS;
        break;
    case FileMode::OpenExisting:
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = OPEN_EXISTING;
        break;
    }

    HANDLE h = CreateFileW(path.wstring().c_str(), access, share, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        last_error_ = static_cast<int>(GetLastError());
        return false;
    }
    last_error_ = 0;
    handle_ = h;
    return true;
#else
    int flags = 0;
    mode_t perm = 0644;

    switch (mode) {
    case FileMode::ReadOnly:
        flags = O_RDONLY;
        break;
    case FileMode::WriteOnly:
    case FileMode::CreateAlways:
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case FileMode::CreateNew:
        // O_CREAT|O_EXCL fails when the name already exists, even if it is
        // a dangling symlink, so a planted link cannot be followed.
        flags = O_WRONLY | O_CREAT | O_EXCL;
        break;
    case FileMode::ReadWrite:
        flags = O_RDWR | O_CREAT;
        break;
    case FileMode::OpenExisting:
        flags = O_RDWR;
        break;
    }

#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif

    int fd = ::open(path.c_str(), flags, perm);
    if (fd < 0) {
        handle_ = nullptr;
        last_error_ = errno;
        return false;
    }
    last_error_ = 0;
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    return true;
#endif
}

bool FileStream::is_collision_error() const {
#ifdef _WIN32
    return last_error_ == ERROR_FILE_EXISTS || last_error_ == ERROR_ALREADY_EXISTS;
#else
    return last_error_ == EEXIST;
#endif
}

void FileStream::close() {
    if (handle_ != nullptr) {
#ifdef _WIN32
        CloseHandle(static_cast<HANDLE>(handle_));
#else
        ::close(static_cast<int>(reinterpret_cast<intptr_t>(handle_)));
#endif
        handle_ = nullptr;
    }
}

bool FileStream::is_open() const {
    return handle_ != nullptr;
}

core::uint64 FileStream::size() const {
    if (!is_open()) return 0;

#ifdef _WIN32
    LARGE_INTEGER li;
    if (GetFileSizeEx(static_cast<HANDLE>(handle_), &li)) {
        return static_cast<core::uint64>(li.QuadPart);
    }
    return 0;
#else
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    struct stat st;
    if (fstat(fd, &st) == 0) {
        return static_cast<core::uint64>(st.st_size);
    }
    return 0;
#endif
}

size_t FileStream::read(void* dest, size_t bytes) {
    if (!is_open() || bytes == 0) return 0;

#ifdef _WIN32
    size_t total_read = 0;
    auto* ptr = static_cast<core::byte*>(dest);

    while (bytes > 0) {
        DWORD to_read = static_cast<DWORD>(std::min<size_t>(bytes, 0x40000000));
        DWORD bytes_read = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), ptr, to_read, &bytes_read, nullptr) ||
            bytes_read == 0) {
            break;
        }
        total_read += bytes_read;
        ptr += bytes_read;
        bytes -= bytes_read;
        if (bytes_read < to_read) {
            break;
        }
    }
    return total_read;
#else
    size_t total_read = 0;
    auto* ptr = static_cast<core::byte*>(dest);

    while (bytes > 0) {
        size_t to_read = std::min<size_t>(bytes, 0x40000000);
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
        ssize_t res = ::read(fd, ptr, to_read);
        if (res < 0 && errno == EINTR) continue; // interrupted by a signal: retry (L9)
        if (res <= 0) break;
        total_read += static_cast<size_t>(res);
        ptr += res;
        bytes -= static_cast<size_t>(res);
        if (static_cast<size_t>(res) < to_read) break;
    }
    return total_read;
#endif
}

size_t FileStream::write(const void* src, size_t bytes) {
    if (!is_open() || bytes == 0) return 0;

#ifdef _WIN32
    size_t total_written = 0;
    const auto* ptr = static_cast<const core::byte*>(src);

    while (bytes > 0) {
        DWORD to_write = static_cast<DWORD>(std::min<size_t>(bytes, 0x40000000));
        DWORD bytes_written = 0;
        if (!WriteFile(static_cast<HANDLE>(handle_), ptr, to_write, &bytes_written, nullptr) ||
            bytes_written == 0) {
            break;
        }
        total_written += bytes_written;
        ptr += bytes_written;
        bytes -= bytes_written;
        if (bytes_written < to_write) {
            break;
        }
    }
    return total_written;
#else
    size_t total_written = 0;
    const auto* ptr = static_cast<const core::byte*>(src);

    while (bytes > 0) {
        size_t to_write = std::min<size_t>(bytes, 0x40000000);
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
        ssize_t res = ::write(fd, ptr, to_write);
        if (res < 0 && errno == EINTR) continue; // interrupted by a signal: retry (L9)
        if (res <= 0) break;
        total_written += static_cast<size_t>(res);
        ptr += res;
        bytes -= static_cast<size_t>(res);
        if (static_cast<size_t>(res) < to_write) break;
    }
    return total_written;
#endif
}

bool FileStream::seek(core::int64 offset, SeekOrigin origin) {
    if (!is_open()) return false;

#ifdef _WIN32
    DWORD method = FILE_BEGIN;
    switch (origin) {
    case SeekOrigin::Begin:
        method = FILE_BEGIN;
        break;
    case SeekOrigin::Current:
        method = FILE_CURRENT;
        break;
    case SeekOrigin::End:
        method = FILE_END;
        break;
    }
    LARGE_INTEGER dist, pos;
    dist.QuadPart = offset;
    return SetFilePointerEx(static_cast<HANDLE>(handle_), dist, &pos, method) != FALSE;
#else
    int whence = SEEK_SET;
    switch (origin) {
    case SeekOrigin::Begin:
        whence = SEEK_SET;
        break;
    case SeekOrigin::Current:
        whence = SEEK_CUR;
        break;
    case SeekOrigin::End:
        whence = SEEK_END;
        break;
    }
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    return lseek(fd, offset, whence) != -1;
#endif
}

core::uint64 FileStream::tell() const {
    if (!is_open()) return 0;

#ifdef _WIN32
    LARGE_INTEGER zero, pos;
    zero.QuadPart = 0;
    if (SetFilePointerEx(static_cast<HANDLE>(handle_), zero, &pos, FILE_CURRENT)) {
        return static_cast<core::uint64>(pos.QuadPart);
    }
    return 0;
#else
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    off_t pos = lseek(fd, 0, SEEK_CUR);
    return pos >= 0 ? static_cast<core::uint64>(pos) : 0;
#endif
}

bool FileStream::truncate(core::uint64 new_size) {
    if (!seek(static_cast<core::int64>(new_size), SeekOrigin::Begin)) return false;

#ifdef _WIN32
    return SetEndOfFile(static_cast<HANDLE>(handle_)) != FALSE;
#else
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    return ftruncate(fd, static_cast<off_t>(new_size)) == 0;
#endif
}

bool FileStream::flush() {
    if (!is_open()) return false;

#ifdef _WIN32
    return FlushFileBuffers(static_cast<HANDLE>(handle_)) != FALSE;
#else
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    return fsync(fd) == 0;
#endif
}

} // namespace openrar::io
