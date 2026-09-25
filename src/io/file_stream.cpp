#include "file_stream.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#if !defined(SYS_renameat2)
// glibc < 2.26 does not define the slot; pin the known arch numbers so the
// syscall can still be issued directly (the glibc wrapper only exists since
// 2.36 — v1.24 plan §1.2; CI/WSL run older glibc).
#if defined(__x86_64__)
#define SYS_renameat2 316
#elif defined(__aarch64__)
#define SYS_renameat2 276
#elif defined(__i386__)
#define SYS_renameat2 353
#elif defined(__arm__)
#define SYS_renameat2 382
#endif
#endif
#if defined(SYS_renameat2)
#define OPENRAR_HAS_RENAMEAT2 1
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#endif

#if defined(__APPLE__)
#if defined(__has_include)
#if __has_include(<sys/renameat.h>)
#include <sys/renameat.h>
#endif
#endif
#ifndef RENAME_EXCL
#define RENAME_EXCL (1 << 0)
#endif
#define OPENRAR_HAS_RENAMEX_NP 1
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
        // file through a planted link. DELETE is required to commit such a
        // temp via FileRenameInformationEx (v1.24 plan §2.2); safe to
        // request only here — CreateNew has no concurrent openers by
        // definition, and wider DELETE on CreateAlways/WriteOnly would
        // conflict with read streams' share modes (regression found by
        // test_compressor50_truncated_source).
        access = GENERIC_WRITE | DELETE;
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

bool FileStream::attach_os_handle(void* os_handle, const std::filesystem::path& display_path) {
    close();
    if (os_handle == nullptr) {
        last_error_ = EBADF;
        return false;
    }
    handle_ = os_handle;
    path_ = display_path;
    mode_ = FileMode::WriteOnly;
    last_error_ = 0;
    return true;
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

#ifdef _WIN32

namespace {
// FileRenameInformationEx (FileRenameInfoEx, class 13) — defined locally so
// the build does not depend on _WIN32_WINNT being pinned at 0x0602+ or on a
// particular SDK vintage (the public FILE_RENAME_INFO + constants are only
// declared for newer SDKs).
constexpr DWORD kFileRenameInfoExClass = 13;
constexpr DWORD kFileRenameFlagReplaceIfExists = 0x00000001;
constexpr DWORD kFileRenameFlagPosixSemantics = 0x00000002;
constexpr DWORD kFileRenameFlagIgnoreReadonlyAttribute = 0x00000004;

struct OpenrarRenameInfo {
    DWORD Flags;
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[1];
};

// True for OS/FS capability failures that should fall back to MoveFileExW;
// anything else (ACCESS_DENIED on a read-only target, sharing violations,
// missing paths) is a real error the caller must see.
bool rename_ex_unsupported(DWORD e) {
    return e == ERROR_INVALID_PARAMETER || e == ERROR_NOT_SUPPORTED ||
           e == ERROR_CALL_NOT_IMPLEMENTED || e == ERROR_INVALID_FUNCTION;
}
} // namespace

bool FileStream::commit_rename(const std::filesystem::path& dest, CommitMode mode) {
    if (!is_open()) {
        last_error_ = static_cast<int>(ERROR_INVALID_HANDLE);
        return false;
    }
    if (!flush()) {
        last_error_ = static_cast<int>(GetLastError());
        return false;
    }

    const bool replace = mode == CommitMode::ReplaceExisting;
    // NOTE: kFileRenameFlagIgnoreReadonlyAttribute is deliberately NOT set —
    // a READONLY destination must fail the commit (v1.24 plan §9 row R:
    // "readonly respected"), matching the pre-atomic CreateAlways behavior.

    // Anchor the new name to the destination's parent directory: with a
    // RootDirectory handle the leaf name is unambiguous, no NT-path
    // canonicalization is involved, and the temp/dest same-directory rule
    // makes the rename same-volume by construction. (M2's containment walk
    // supplies this handle from the verified parent instead of reopening.)
    HANDLE parent = CreateFileW(dest.parent_path().wstring().c_str(),
                                FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (parent != INVALID_HANDLE_VALUE) {
        const std::wstring leaf = dest.filename().wstring();
        std::vector<BYTE> buf(sizeof(OpenrarRenameInfo) + leaf.size() * sizeof(WCHAR));
        auto* ri = reinterpret_cast<OpenrarRenameInfo*>(buf.data());
        ri->Flags = kFileRenameFlagPosixSemantics | (replace ? kFileRenameFlagReplaceIfExists : 0);
        ri->RootDirectory = parent;
        ri->FileNameLength = static_cast<DWORD>(leaf.size() * sizeof(WCHAR));
        std::memcpy(ri->FileName, leaf.c_str(), (leaf.size() + 1) * sizeof(WCHAR));
        const BOOL ok = SetFileInformationByHandle(
            static_cast<HANDLE>(handle_),
            static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoExClass), ri,
            static_cast<DWORD>(buf.size()));
        const DWORD rename_err = ok ? 0 : GetLastError();
        CloseHandle(parent);
        if (ok) {
            last_error_ = 0;
            return true; // handle stays open on the renamed file; caller closes
        }
        if (!rename_ex_unsupported(rename_err)) {
            last_error_ = static_cast<int>(rename_err);
            return false;
        }
    } else {
        const DWORD parent_err = GetLastError();
        if (!rename_ex_unsupported(parent_err)) {
            // Cannot even anchor the parent (real IO problem) — but a missing
            // parent directory for a destination we are about to create is a
            // genuine failure too: report it.
            last_error_ = static_cast<int>(parent_err);
            return false;
        }
    }

    // Fallback (pre-1709 / FAT / exFAT / some SMB): MoveFileExW. Not truly
    // atomic — a crash between delete-old and rename-new loses the file
    // (documented, plan §2.2). The source handle must be closed first so
    // MoveFileExW can open it for delete.
    close();
    if (MoveFileExW(path_.wstring().c_str(), dest.wstring().c_str(),
                    replace ? MOVEFILE_REPLACE_EXISTING : 0)) {
        last_error_ = 0;
        return true;
    }
    last_error_ = static_cast<int>(GetLastError());
    return false;
}

bool FileStream::commit_rename_in(void* parent_dir_handle, const std::string& utf8_leaf,
                                  CommitMode mode) {
    if (!is_open() || parent_dir_handle == nullptr) {
        last_error_ = static_cast<int>(ERROR_INVALID_HANDLE);
        return false;
    }
    if (!flush()) {
        last_error_ = static_cast<int>(GetLastError());
        return false;
    }
    const auto to_wide = [](const std::string& u8) {
        if (u8.empty()) return std::wstring();
        const int n =
            MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        if (n > 0) {
            MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), w.data(), n);
        }
        return w;
    };
    const std::wstring leaf = to_wide(utf8_leaf);
    std::vector<BYTE> buf(sizeof(OpenrarRenameInfo) + leaf.size() * sizeof(WCHAR));
    auto* ri = reinterpret_cast<OpenrarRenameInfo*>(buf.data());
    ri->Flags = kFileRenameFlagPosixSemantics |
                (mode == CommitMode::ReplaceExisting ? kFileRenameFlagReplaceIfExists : 0);
    // The parent handle comes from the verified containment walk — no path
    // resolution of any archive-controlled component happens after it.
    ri->RootDirectory = static_cast<HANDLE>(parent_dir_handle);
    ri->FileNameLength = static_cast<DWORD>(leaf.size() * sizeof(WCHAR));
    std::memcpy(ri->FileName, leaf.c_str(), (leaf.size() + 1) * sizeof(WCHAR));
    // NtSetInformationFile directly: SetFileInformationByHandle rejects
    // handles it did not create itself (ERROR_INVALID_PARAMETER for
    // NtCreateFile-produced handles — found by the gate-1 suite), and the
    // containment writer's handle is always NtCreateFile-produced.
    using NtSetInfoFn = LONG(NTAPI*)(HANDLE, PVOID, PVOID, ULONG, ULONG);
    static NtSetInfoFn set_info = []() -> NtSetInfoFn {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtSetInfoFn>(
                           static_cast<void*>(GetProcAddress(ntdll, "NtSetInformationFile")))
                     : nullptr;
    }();
    if (set_info == nullptr) {
        last_error_ = static_cast<int>(ERROR_NOT_SUPPORTED);
        return false;
    }
    struct {
        LONG Status;
        ULONG_PTR Information;
    } iosb{0, 0};
    // NT FILE_INFORMATION_CLASS values differ from kernel32's
    // FILE_INFO_BY_HANDLE_CLASS: FileRenameInformationEx = 65 on the NT
    // side (13 there is FileDispositionInformation — passing it deletes the
    // file! — found by the gate-1 suite).
    constexpr ULONG kNtFileRenameInformationEx = 65;
    const LONG st = set_info(static_cast<HANDLE>(handle_), &iosb, ri,
                             static_cast<ULONG>(buf.size()), kNtFileRenameInformationEx);
    if (st == 0) {
        last_error_ = 0;
        return true;
    }
    // Capability failures (pre-1709 / FAT / SMB) are reported to the caller —
    // the containment layer decides whether a path-based fallback is
    // acceptable (it never silently re-resolves archive-controlled paths).
    static constexpr LONG kStatusInvalidParameter = static_cast<LONG>(0xC000000Du);
    static constexpr LONG kStatusNotSupported = static_cast<LONG>(0xC00000BBu);
    static constexpr LONG kStatusInvalidDeviceRequest = static_cast<LONG>(0xC0000010u);
    if (st == kStatusInvalidParameter || st == kStatusNotSupported ||
        st == kStatusInvalidDeviceRequest) {
        last_error_ = static_cast<int>(ERROR_NOT_SUPPORTED);
    } else {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            using RtlFn = ULONG(NTAPI*)(LONG);
            auto to_dos = reinterpret_cast<RtlFn>(
                static_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
            if (to_dos) last_error_ = static_cast<int>(to_dos(st));
        }
    }
    return false;
}

bool atomic_rename_commit(const std::filesystem::path& from, const std::filesystem::path& to,
                          CommitMode mode, int& last_error) {
    // Path-based form: MoveFileExW cascade (no open handle to rename through).
    if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                    mode == CommitMode::ReplaceExisting ? MOVEFILE_REPLACE_EXISTING : 0)) {
        last_error = 0;
        return true;
    }
    last_error = static_cast<int>(GetLastError());
    return false;
}

bool commit_is_collision_error(int last_error) {
    return last_error == ERROR_FILE_EXISTS || last_error == ERROR_ALREADY_EXISTS;
}

bool commit_error_is_unsupported(int last_error) {
    return last_error == ERROR_NOT_SUPPORTED || last_error == ERROR_INVALID_PARAMETER ||
           last_error == ERROR_CALL_NOT_IMPLEMENTED || last_error == ERROR_INVALID_FUNCTION;
}

#else // POSIX

bool FileStream::commit_rename(const std::filesystem::path& dest, CommitMode mode) {
    if (!is_open()) {
        last_error_ = EBADF;
        return false;
    }
    if (!flush()) {
        last_error_ = errno;
        return false;
    }
    // POSIX rename() silently replaces a destination its mode bits protect;
    // respect READONLY like Windows does (v1.24 test 14). faccessat uses the
    // real uid — as root this passes, matching the historical O_TRUNC
    // behavior for root.
    if (mode == CommitMode::ReplaceExisting) {
        struct stat st;
        if (::stat(dest.c_str(), &st) == 0 && ::faccessat(AT_FDCWD, dest.c_str(), W_OK, 0) != 0) {
            last_error_ = EACCES;
            return false;
        }
    }
    if (!atomic_rename_commit(path_, dest, mode, last_error_)) return false;
    close();
    return true;
}

bool atomic_rename_commit(const std::filesystem::path& from, const std::filesystem::path& to,
                          CommitMode mode, int& last_error) {
    if (mode == CommitMode::NoClobber) {
#if defined(OPENRAR_HAS_RENAMEAT2)
        long rc = ::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
                            RENAME_NOREPLACE);
        if (rc == 0) {
            last_error = 0;
            return true;
        }
        last_error = errno;
        if (last_error == EEXIST) return false; // clean atomic collision
        if (last_error != ENOSYS && last_error != EINVAL && last_error != EOPNOTSUPP &&
            last_error != EPERM) {
            return false;
        }
        // Kernel/FS without renameat2 support: fall through to the link
        // cascade below.
#elif defined(OPENRAR_HAS_RENAMEX_NP)
        if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) {
            last_error = 0;
            return true;
        }
        last_error = errno;
        if (last_error != ENOSYS && last_error != EOPNOTSUPP && last_error != EINVAL) return false;
#endif
        // link() fails atomically if the destination exists (no-clobber
        // without a rename-flag syscall); same-directory temp makes EXDEV
        // impossible on hardlink-capable filesystems.
        if (::link(from.c_str(), to.c_str()) == 0) {
            if (::unlink(from.c_str()) != 0) {
                last_error = errno; // temp lingers; sweep/class checks catch it
            } else {
                last_error = 0;
            }
            return true;
        }
        last_error = errno;
        if (last_error == EEXIST) return false;
        if (last_error == EXDEV || last_error == EPERM || last_error == EMLINK ||
            last_error == ENOSYS || last_error == EOPNOTSUPP) {
            // Filesystem without hardlink support (FAT/most SMB): pre-checked
            // rename — the small documented no-clobber TOCTOU window (plan
            // §2.2).
            struct stat st;
            if (::lstat(to.c_str(), &st) == 0) {
                last_error = EEXIST;
                return false;
            }
            if (::rename(from.c_str(), to.c_str()) != 0) {
                last_error = errno;
                return false;
            }
            last_error = 0;
            return true;
        }
        return false;
    }

    if (::rename(from.c_str(), to.c_str()) != 0) {
        last_error = errno;
        return false;
    }
    last_error = 0;
    return true;
}

bool commit_is_collision_error(int last_error) {
    return last_error == EEXIST;
}

bool commit_error_is_unsupported(int) {
    return false; // POSIX commits degrade to the link cascade, never "unsupported"
}

#endif // _WIN32

} // namespace openrar::io
