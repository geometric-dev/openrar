#include "containment.hpp"
#include "file_stream.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/syslimits.h>
#endif
#endif

namespace openrar::io {

namespace {

std::vector<std::string> split_components(const std::string& rel) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : rel) {
        if (c == '/') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

#if defined(_WIN32)

// ── NT runtime plumbing (dynamically loaded; no ntdll link dependency) ──────
constexpr ULONG kObjCaseInsensitive = 0x00000040;

constexpr ACCESS_MASK kDirAccess = FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
                                   FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE;
// Full GENERIC_WRITE|DELETE mapping (as CreateFileW would grant): a narrower
// mask changes rename-commit semantics (deferral) for POSIX-semantics
// renames — found by the gate-1 suite.
constexpr ACCESS_MASK kFileWriteAccess = FILE_GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES;
constexpr ACCESS_MASK kJournalAccess =
    FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE;

// Directories are shared like kernel32's FILE_FLAG_BACKUP_SEMANTICS opens:
// a too-restrictive share mode here would block every later write-intent
// open of the same directory (found by the gate-1 suite).
constexpr ULONG kDirShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

constexpr ULONG kFileOpen = 0x00000001;   // disposition: fail if missing
constexpr ULONG kFileCreate = 0x00000002; // disposition: fail if exists
constexpr ULONG kFileDirectoryFile = 0x00000001;
constexpr ULONG kFileNonDirectoryFile = 0x00000040;
constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020;
constexpr ULONG kFileOpenReparsePoint = 0x00200000;

constexpr LONG kStatusSuccess = 0;
constexpr LONG kStatusObjectNameCollision = static_cast<LONG>(0xC0000035u);
constexpr LONG kStatusObjectNameNotFound = static_cast<LONG>(0xC0000034u);

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct NtObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    NtUnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct NtIoStatusBlock {
    LONG Status;
    ULONG_PTR Information;
};

using NtCreateFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, NtObjectAttributes*, NtIoStatusBlock*,
                                    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

NtCreateFileFn nt_create_file() {
    static NtCreateFileFn fn = []() -> NtCreateFileFn {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtCreateFileFn>(
                           static_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")))
                     : nullptr;
    }();
    return fn;
}

// Anchored open of ONE component under `root`. `name` is a leaf (no path
// separators). When `open_reparse` is set the component is opened WITHOUT
// traversing a reparse point (so the caller can inspect and reject it).
// Directory opens share everything (matching kernel32's backup-semantics
// opens); file opens share read only, which is what makes the journal's
// exclusive lock work.
LONG nt_open_anchored(HANDLE root, const std::wstring& name, ACCESS_MASK access,
                      ULONG create_options, ULONG disposition, HANDLE* out) {
    const NtCreateFileFn create = nt_create_file();
    if (!create) return static_cast<LONG>(0xC0000001u); // STATUS_UNSUCCESSFUL
    NtUnicodeString us;
    us.Length = static_cast<USHORT>(name.size() * sizeof(WCHAR));
    us.MaximumLength = us.Length;
    us.Buffer = const_cast<PWSTR>(name.c_str());
    NtObjectAttributes oa;
    oa.Length = sizeof(oa);
    oa.RootDirectory = root;
    oa.ObjectName = &us;
    oa.Attributes = kObjCaseInsensitive;
    oa.SecurityDescriptor = nullptr;
    oa.SecurityQualityOfService = nullptr;
    NtIoStatusBlock iosb{};
    const ULONG share = (create_options & kFileDirectoryFile) ? kDirShare : FILE_SHARE_READ;
    return create(out, access, &oa, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
                  create_options, nullptr, 0);
}

void close_handle(void*& h) {
    if (h != nullptr) {
        CloseHandle(static_cast<HANDLE>(h));
        h = nullptr;
    }
}

void close_os_handle(void*& h) {
    close_handle(h);
}

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), n);
    }
    return out;
}

#else // POSIX

#if defined(__linux__)
#if !defined(SYS_openat2)
// openat2 landed in kernel 5.6 with syscall slot 437 on every architecture
// OpenRAR builds for; glibc only grew the wrapper in 2.36, so call it
// directly (v1.24 plan §1.2).
#define SYS_openat2 437
#endif
#if defined(SYS_openat2)
#define OPENRAR_HAS_OPENAT2 1
constexpr uint64_t kResolveBeneath = 0x02;
constexpr uint64_t kResolveNoSymlinks = 0x04;
struct OpenHow {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};
#endif
#if !defined(SYS_renameat2)
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
#endif // __linux__

#if defined(__APPLE__)
#define OPENRAR_HAS_RENAMEX_NP 1
#ifndef RENAME_EXCL
#define RENAME_EXCL (1 << 0)
#endif
#endif

// openat2 availability: probed once per process (plan gap 5). ENOSYS (old
// kernel) and EPERM (seccomp) both mean "unavailable" — the fallback walk is
// always correct, just slower.
bool openat2_available() {
#if defined(OPENRAR_HAS_OPENAT2)
    static int cached = -1;
    if (cached < 0) {
        OpenHow how{O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, kResolveBeneath | kResolveNoSymlinks};
        const long rv = ::syscall(SYS_openat2, AT_FDCWD, ".", &how, sizeof(how));
        cached = (rv >= 0 || (errno != ENOSYS && errno != EPERM)) ? 1 : 0;
    }
    return cached == 1;
#else
    return false;
#endif
}

bool openat2_resolve(int root_fd, const char* rel, bool directory, int& out_fd) {
#if defined(OPENRAR_HAS_OPENAT2)
    const int flags = (directory ? O_RDONLY | O_DIRECTORY : O_RDONLY) | O_CLOEXEC;
    OpenHow how{static_cast<uint64_t>(flags), 0, kResolveBeneath | kResolveNoSymlinks};
    const long rv = ::syscall(SYS_openat2, root_fd, rel, &how, sizeof(how));
    if (rv < 0) return false;
    out_fd = static_cast<int>(rv);
    return true;
#else
    (void)root_fd;
    (void)rel;
    (void)directory;
    (void)out_fd;
    return false;
#endif
}

// Kernel-resolved absolute path of an fd: /proc/self/fd on Linux, F_GETPATH
// on macOS. Empty string when unavailable (then the caller must fail closed).
std::string fd_kernel_path(int fd) {
#if defined(__linux__)
    std::string link = "/proc/self/fd/" + std::to_string(fd);
    std::string buf(512, '\0');
    for (;;) {
        const ssize_t n = ::readlink(link.c_str(), buf.data(), buf.size());
        if (n < 0) return "";
        if (static_cast<size_t>(n) < buf.size()) {
            buf.resize(static_cast<size_t>(n));
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    char buf[MAXPATHLEN] = {};
    if (::fcntl(fd, F_GETPATH, buf) != 0) return "";
    return std::string(buf);
#else
    (void)fd;
    return "";
#endif
}

void close_fd(void*& h) {
    if (h != nullptr) {
        ::close(static_cast<int>(reinterpret_cast<intptr_t>(h)));
        h = nullptr;
    }
}

void close_os_handle(void*& h) {
    close_fd(h);
}

#endif // _WIN32

bool component_ok(const std::string& c) {
    return !c.empty() && c != "." && c != ".." && !is_83_alias_component(c);
}

} // namespace

// ── 8.3 alias detection ──────────────────────────────────────────────────────

bool is_83_alias_component(const std::string& component) {
    // `NAME~X.ext` shape: a '~' followed by at least one digit in the stem
    // (the part before the first dot). Win32 resolves such names through
    // short-name alias space, so they are rejected before any resolution.
    const size_t dot = component.find('.');
    const std::string stem = component.substr(0, dot == std::string::npos ? component.size() : dot);
    for (size_t i = 0; i < stem.size(); ++i) {
        if (stem[i] == '~' && i + 1 < stem.size() && stem[i + 1] >= '0' && stem[i + 1] <= '9') {
            return true;
        }
    }
    return false;
}

bool path_has_83_component(const std::string& rel) {
    for (const std::string& c : split_components(rel)) {
        if (is_83_alias_component(c)) return true;
    }
    return false;
}

void split_archive_relpath(const std::string& rel, std::string& dir_out, std::string& leaf_out) {
    const size_t slash = rel.find_last_of('/');
    if (slash == std::string::npos) {
        dir_out.clear();
        leaf_out = rel;
    } else {
        dir_out = rel.substr(0, slash);
        leaf_out = rel.substr(slash + 1);
    }
}

// ── ContainmentRoot ──────────────────────────────────────────────────────────

ContainmentRoot::~ContainmentRoot() {
    cache_clear();
#if defined(_WIN32)
    close_handle(pinned_);
#else
    close_fd(pinned_);
#endif
}

bool ContainmentRoot::attach(const std::filesystem::path& root) {
    // The extraction destination may not exist yet (CLI `x` into a new
    // directory) — the caller-chosen root is created, never the walk's
    // archive-controlled components (those go through resolve_dir).
    {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) {
            std::filesystem::create_directories(root, ec);
            if (ec) return false;
        }
    }
#if defined(_WIN32)
    close_handle(pinned_);
    // The root doubles as a rename-target parent for entries directly in it
    // (rel_dir == ""), so it needs FILE_ADD_FILE / FILE_ADD_SUBDIRECTORY
    // like any walked directory handle.
    HANDLE h = CreateFileW(root.wstring().c_str(),
                           FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
                               FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    pinned_ = h;
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(root, ec);
    canonical_root_ = ec ? root : abs.lexically_normal();
    // \\?\-canonical form of the pinned root (the same form
    // GetFinalPathNameByHandleW returns for files under it) — computed once
    // so the final assertion compares like with like (plan §1.1.2).
    WCHAR buf[32768];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 32768, FILE_NAME_NORMALIZED);
    if (n > 0 && n < 32768) {
        canonical_root_nt_ = std::wstring(buf, n);
    }
    return true;
#else
    close_fd(pinned_);
    const int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    pinned_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(root, ec);
    canonical_root_ = ec ? root : abs.lexically_normal();
    return true;
#endif
}

void ContainmentRoot::cache_put(const std::string& key, void* handle) {
    // Duplicate: the cache owns its own copy; the caller's anchor stays
    // independently owned.
#if defined(_WIN32)
    void* dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(handle), GetCurrentProcess(),
                         &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return; // cache is best-effort
    }
    handle = dup;
#else
    const int dup = ::dup(static_cast<int>(reinterpret_cast<intptr_t>(handle)));
    if (dup < 0) return;
    handle = reinterpret_cast<void*>(static_cast<intptr_t>(dup));
#endif
    cache_.push_front(CacheEntry{key, handle});
    if (cache_.size() > kCacheCapacity) {
        close_os_handle(cache_.back().handle);
        cache_.pop_back();
    }
}

void* ContainmentRoot::cache_get(const std::string& key) {
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->key == key) {
            cache_.splice(cache_.begin(), cache_, it); // LRU touch
#if defined(_WIN32)
            void* dup = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(it->handle),
                                 GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                return nullptr;
            }
            return dup;
#else
            const int dup = ::dup(static_cast<int>(reinterpret_cast<intptr_t>(it->handle)));
            if (dup < 0) return nullptr;
            return reinterpret_cast<void*>(static_cast<intptr_t>(dup));
#endif
        }
    }
    return nullptr;
}

void ContainmentRoot::cache_clear() {
    for (CacheEntry& e : cache_) {
        close_os_handle(e.handle);
    }
    cache_.clear();
}

bool ContainmentRoot::resolve_dir(const std::string& rel_dir, bool create, VerifiedDir& out) {
    if (!attached()) return false;

    // Reject malformed components before touching the filesystem.
    if (!rel_dir.empty()) {
        for (const std::string& c : split_components(rel_dir)) {
            if (!component_ok(c)) return false;
        }
    }

    // LRU hit: the handle pins the verified inode; the final containment
    // assertion still runs at the leaf (never skipped).
    if (void* cached = cache_get(rel_dir)) {
        out = VerifiedDir();
        out.handle_ = cached;
        return true;
    }

#if defined(_WIN32)
    // Windows walk: component-by-component anchored NtCreateFile; every
    // component opened WITH FILE_OPEN_REPARSE_POINT (not traversed) and
    // verified reparse-free (plan §1.1.1).
    void* current = nullptr; // owned handle for the current component
    void* parent = pinned_;
    std::string walked; // components resolved so far (for the cache)
    for (const std::string& c :
         rel_dir.empty() ? std::vector<std::string>{} : split_components(rel_dir)) {
        HANDLE next = INVALID_HANDLE_VALUE;
        LONG st = nt_open_anchored(static_cast<HANDLE>(parent), to_wide(c), kDirAccess,
                                   kFileDirectoryFile | kFileOpenReparsePoint |
                                       kFileSynchronousIoNonAlert,
                                   kFileOpen, &next);
        if (st != kStatusSuccess && create && st == kStatusObjectNameNotFound) {
            // Directory-creation race protocol (plan §1.1.4): anchored
            // create-exclusive; if a concurrent creator wins the race
            // (COLLISION), open the winner's directory no-follow instead —
            // the reparse/dir verification below applies either way.
            st = nt_open_anchored(static_cast<HANDLE>(parent), to_wide(c), kDirAccess,
                                  kFileDirectoryFile | kFileSynchronousIoNonAlert, kFileCreate,
                                  &next);
            if (st == kStatusObjectNameCollision) {
                st = nt_open_anchored(static_cast<HANDLE>(parent), to_wide(c), kDirAccess,
                                      kFileDirectoryFile | kFileOpenReparsePoint |
                                          kFileSynchronousIoNonAlert,
                                      kFileOpen, &next);
            }
        }
        if (st != kStatusSuccess) {
            close_handle(current);
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(next, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            CloseHandle(next);
            close_handle(current);
            return false; // planted reparse point or not a directory
        }
        close_handle(current);
        current = next;
        parent = current;
        walked = walked.empty() ? c : walked + "/" + c;
    }
    if (current == nullptr) {
        // rel_dir empty: anchor to the root itself (duplicated for ownership)
        void* dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(pinned_), GetCurrentProcess(),
                             &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return false;
        }
        current = dup;
    }
    cache_put(rel_dir, current);
    out = VerifiedDir();
    out.handle_ = current;
    return true;
#else
    const int root_fd = static_cast<int>(reinterpret_cast<intptr_t>(pinned_));

    // rel_dir "": anchor to a dup of the pinned root.
    if (rel_dir.empty()) {
        const int dup = ::dup(root_fd);
        if (dup < 0) return false;
        void* h = reinterpret_cast<void*>(static_cast<intptr_t>(dup));
        cache_put(rel_dir, h);
        out = VerifiedDir();
        out.handle_ = h;
        return true;
    }

    // openat2 primary path (Linux 5.6+): one atomic kernel resolution —
    // any escaping component or traversed symlink fails the call (§1.2.1).
    if (openat2_available() && !force_fallback_) {
        int fd = -1;
        if (openat2_resolve(root_fd, rel_dir.c_str(), true, fd)) {
            void* h = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
            cache_put(rel_dir, h);
            out = VerifiedDir();
            out.handle_ = h;
            return true;
        }
        if (errno != ENOENT || !create) return false;
        // Missing tail: create components, then resolve again (the final
        // openat2 re-verifies the whole chain atomically).
    }

    // Fallback walk (pre-5.6 kernels, macOS, openat2 disabled): anchored
    // openat with O_NOFOLLOW on every component + mkdirat race protocol
    // (§1.2.2/§1.2.3).
    int current = ::dup(root_fd); // owned; walks down
    if (current < 0) return false;
    std::string walked;
    bool created_any = false;
    for (const std::string& c : split_components(rel_dir)) {
        int next = ::openat(current, c.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && create && errno == ENOENT) {
            if (::mkdirat(current, c.c_str(), 0700) == 0) created_any = true;
            // Race protocol: whether we created it or lost a race, open it
            // no-follow and fstat-verify it is a real directory before use.
            next = ::openat(current, c.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) {
            ::close(current);
            return false; // symlink (ELOOP), non-directory (ENOTDIR), IO error
        }
        struct stat st;
        if (::fstat(next, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ::close(next);
            ::close(current);
            return false;
        }
        ::close(current);
        current = next;
        walked = walked.empty() ? c : walked + "/" + c;
    }

    // Residual-race mitigation (§1.2.2): the kernel-resolved path of the
    // final anchor must be a prefix-path of the root's own kernel-resolved
    // path. Fail closed when neither /proc nor F_GETPATH can answer.
    if (!fd_prefix_verified(reinterpret_cast<void*>(static_cast<intptr_t>(current)))) {
        ::close(current);
        return false;
    }

    // When openat2 exists we deliberately finish with one atomic re-open so
    // the created chain is re-resolved under RESOLVE_BENEATH|NO_SYMLINKS.
#if defined(OPENRAR_HAS_OPENAT2)
    if (openat2_available() && !force_fallback_ && created_any) {
        int reopened = -1;
        if (openat2_resolve(root_fd, walked.c_str(), true, reopened)) {
            ::close(current);
            current = reopened;
        } else {
            ::close(current);
            return false;
        }
    }
#endif

    void* h = reinterpret_cast<void*>(static_cast<intptr_t>(current));
    cache_put(walked, h);
    out = VerifiedDir();
    out.handle_ = h;
    return true;
#endif
}

bool ContainmentRoot::anchored_create(const VerifiedDir& dir, const std::string& leaf,
                                      void*& out_handle) {
    if (!dir.valid() || !component_ok(leaf)) return false;
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
    const LONG st =
        nt_open_anchored(static_cast<HANDLE>(dir.raw()), to_wide(leaf), kFileWriteAccess,
                         kFileNonDirectoryFile | kFileSynchronousIoNonAlert, kFileCreate, &h);
    if (st != kStatusSuccess) return false;
    out_handle = h;
    return true;
#else
    const int fd = ::openat(static_cast<int>(reinterpret_cast<intptr_t>(dir.raw())), leaf.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    out_handle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    return true;
#endif
}

bool ContainmentRoot::anchored_create_journal(const VerifiedDir& dir, const std::string& leaf,
                                              void*& out_handle) {
    if (!dir.valid() || !component_ok(leaf)) return false;
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
    const LONG st = nt_open_anchored(static_cast<HANDLE>(dir.raw()), to_wide(leaf), kJournalAccess,
                                     kFileSynchronousIoNonAlert, kFileCreate, &h);
    if (st != kStatusSuccess) return false;
    out_handle = h;
    return true;
#else
    const int fd = ::openat(static_cast<int>(reinterpret_cast<intptr_t>(dir.raw())), leaf.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    out_handle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    return true;
#endif
}

bool ContainmentRoot::anchored_rename(const VerifiedDir& dir, const std::string& from_leaf,
                                      const std::string& to_leaf, CommitMode mode,
                                      int& last_error) {
    if (!dir.valid() || !component_ok(from_leaf) || !component_ok(to_leaf)) {
        last_error = EINVAL;
        return false;
    }
#if defined(_WIN32)
    // Windows commits go through the writer's own open handle
    // (FileStream::commit_rename_in): FileRenameInformation renames via the
    // SOURCE handle, which AtomicWriter holds exclusively.
    (void)from_leaf;
    (void)to_leaf;
    (void)mode;
    last_error = static_cast<int>(ERROR_NOT_SUPPORTED);
    return false;
#else
    const int dirfd = static_cast<int>(reinterpret_cast<intptr_t>(dir.raw()));
    if (mode == CommitMode::ReplaceExisting) {
        // READONLY respected (plan §10 test 14), mirroring
        // FileStream::commit_rename's path-based pre-check.
        if (::faccessat(dirfd, to_leaf.c_str(), W_OK, 0) != 0 && errno == EACCES) {
            last_error = EACCES;
            return false;
        }
        if (::renameat(dirfd, from_leaf.c_str(), dirfd, to_leaf.c_str()) != 0) {
            last_error = errno;
            return false;
        }
        last_error = 0;
        return true;
    }
#if defined(OPENRAR_HAS_RENAMEAT2)
    if (::syscall(SYS_renameat2, dirfd, from_leaf.c_str(), dirfd, to_leaf.c_str(),
                  RENAME_NOREPLACE) == 0) {
        last_error = 0;
        return true;
    }
    last_error = errno;
    if (last_error == EEXIST) return false;
    if (last_error != ENOSYS && last_error != EINVAL && last_error != EOPNOTSUPP &&
        last_error != EPERM) {
        return false;
    }
#endif
    // link()+unlink() cascade — anchored, atomic no-clobber (macOS included;
    // renamex_np has no dirfd form, and linkat does not need one).
    if (::linkat(dirfd, from_leaf.c_str(), dirfd, to_leaf.c_str(), 0) == 0) {
        if (::unlinkat(dirfd, from_leaf.c_str(), 0) != 0) {
            last_error = errno;
        } else {
            last_error = 0;
        }
        return true;
    }
    last_error = errno;
    if (last_error == EEXIST) return false;
    if (last_error == EXDEV || last_error == EPERM || last_error == EMLINK ||
        last_error == ENOSYS || last_error == EOPNOTSUPP) {
        // Filesystem without hardlinks: pre-checked renameat — the small
        // documented no-clobber TOCTOU window (plan §2.2).
        struct stat st;
        if (::fstatat(dirfd, to_leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
            last_error = EEXIST;
            return false;
        }
        if (::renameat(dirfd, from_leaf.c_str(), dirfd, to_leaf.c_str()) != 0) {
            last_error = errno;
            return false;
        }
        last_error = 0;
        return true;
    }
    return false;
#endif
}

bool ContainmentRoot::leaf_is_reparse(const VerifiedDir& dir, const std::string& leaf, bool& out) {
    out = false;
    if (!dir.valid() || !component_ok(leaf)) return false;
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
    // FILE_OPEN_REPARSE_POINT: open the leaf itself, never its target.
    // Attributes-only open: no synchronous-IO option (it would require
    // SYNCHRONIZE access, which a read-attributes open does not carry).
    const LONG st = nt_open_anchored(static_cast<HANDLE>(dir.raw()), to_wide(leaf),
                                     FILE_READ_ATTRIBUTES, kFileOpenReparsePoint, kFileOpen, &h);
    if (st != kStatusSuccess) return false; // not there / IO error: out stays false
    BY_HANDLE_FILE_INFORMATION info;
    const bool ok = GetFileInformationByHandle(h, &info) != FALSE;
    CloseHandle(h);
    if (!ok) return false;
    out = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
#else
    const int dirfd = static_cast<int>(reinterpret_cast<intptr_t>(dir.raw()));
    struct stat st;
    if (::fstatat(dirfd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return false;
    out = S_ISLNK(st.st_mode) != 0;
    return true;
#endif
}

bool ContainmentRoot::anchored_unlink_leaf(const VerifiedDir& dir, const std::string& leaf) {
    if (!dir.valid() || !component_ok(leaf)) return false;
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
    // Open the link itself (no traverse) and delete THAT object on close.
    // DELETE covers the disposition; no synchronous-IO option needed.
    const LONG st =
        nt_open_anchored(static_cast<HANDLE>(dir.raw()), to_wide(leaf),
                         DELETE | FILE_READ_ATTRIBUTES, kFileOpenReparsePoint, kFileOpen, &h);
    if (st != kStatusSuccess) return false;
    using NtSetInfoFn = LONG(NTAPI*)(HANDLE, PVOID, PVOID, ULONG, ULONG);
    static NtSetInfoFn set_info = []() -> NtSetInfoFn {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtSetInfoFn>(
                           static_cast<void*>(GetProcAddress(ntdll, "NtSetInformationFile")))
                     : nullptr;
    }();
    if (set_info == nullptr) {
        CloseHandle(h);
        return false;
    }
    struct {
        LONG Status;
        ULONG_PTR Information;
    } iosb{0, 0};
    BOOLEAN do_delete = TRUE;
    // FileDispositionInformationEx (21) with POSIX semantics: unlinks the
    // object immediately, immune to sharing conflicts on the link object
    // (plain FileDispositionInformation fails with SHARING_VIOLATION for
    // symlink leaves whose target exists).
    struct {
        ULONG Flags;
    } disp{0x1 /*DELETE*/ | 0x2 /*POSIX_SEMANTICS*/};
    const LONG dst = set_info(h, &iosb, &disp, sizeof(disp), 64 /*FileDispositionInformationEx*/);
    CloseHandle(h);
    return dst == 0;
#else
    const int dirfd = static_cast<int>(reinterpret_cast<intptr_t>(dir.raw()));
    return ::unlinkat(dirfd, leaf.c_str(), 0) == 0;
#endif
}

bool ContainmentRoot::final_path_inside(const void* handle, const std::string& rel_dir,
                                        const std::string& leaf) const {
#if defined(_WIN32)
    if (canonical_root_nt_.empty()) return false; // cannot assert → fail closed
    WCHAR buf[32768];
    const DWORD n = GetFinalPathNameByHandleW(static_cast<HANDLE>(const_cast<void*>(handle)), buf,
                                              32768, FILE_NAME_NORMALIZED);
    if (n == 0 || n >= 32768) return false;
    std::wstring actual(buf, n);
    std::wstring expected = canonical_root_nt_;
    if (!rel_dir.empty() || !leaf.empty()) {
        std::wstring tail = to_wide(rel_dir.empty() ? leaf : rel_dir + "/" + leaf);
        for (wchar_t& c : tail) {
            if (c == L'/') c = L'\\';
        }
        if (expected.back() != L'\\') expected.push_back(L'\\');
        expected += tail;
    }
    const auto lower = [](std::wstring& s) {
        for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    };
    lower(actual);
    lower(expected);
    return actual == expected;
#else
    (void)handle;
    (void)rel_dir;
    (void)leaf;
    return true; // POSIX uses fd_prefix_verified
#endif
}

bool ContainmentRoot::fd_prefix_verified(const void* handle) const {
#if defined(_WIN32)
    (void)handle;
    return true; // Windows uses final_path_inside
#else
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(const_cast<void*>(handle)));
    const int root_fd = static_cast<int>(reinterpret_cast<intptr_t>(pinned_));
    const std::string root_path = fd_kernel_path(root_fd);
    if (root_path.empty()) return false;
    const std::string fd_path = fd_kernel_path(fd);
    if (fd_path.empty()) return false;
    if (fd_path == root_path) return true; // the root itself
    return fd_path.size() > root_path.size() &&
           fd_path.compare(0, root_path.size(), root_path) == 0 && fd_path[root_path.size()] == '/';
#endif
}

// ── VerifiedDir ──────────────────────────────────────────────────────────────

ContainmentRoot::VerifiedDir::VerifiedDir() = default;

ContainmentRoot::VerifiedDir::~VerifiedDir() {
    if (handle_ != nullptr) {
#if defined(_WIN32)
        CloseHandle(static_cast<HANDLE>(handle_));
#else
        ::close(static_cast<int>(reinterpret_cast<intptr_t>(handle_)));
#endif
        handle_ = nullptr;
    }
}

ContainmentRoot::VerifiedDir::VerifiedDir(VerifiedDir&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

ContainmentRoot::VerifiedDir&
ContainmentRoot::VerifiedDir::operator=(VerifiedDir&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
#if defined(_WIN32)
            CloseHandle(static_cast<HANDLE>(handle_));
#else
            ::close(static_cast<int>(reinterpret_cast<intptr_t>(handle_)));
#endif
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

} // namespace openrar::io
