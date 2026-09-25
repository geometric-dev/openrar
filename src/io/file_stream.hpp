#ifndef OPENRAR_IO_FILE_STREAM_HPP
#define OPENRAR_IO_FILE_STREAM_HPP

#include "../core/types.hpp"
#include <filesystem>
#include <string>

namespace openrar::io {

enum class FileMode {
    ReadOnly,
    WriteOnly,
    ReadWrite,
    CreateAlways,
    CreateNew, // create-only: fails if the name already exists (fail-if-exists)
    OpenExisting
};

// Commit semantics for the atomic rename cascade (v1.24.0 plan §2.2).
enum class CommitMode {
    ReplaceExisting, // atomic replace; a symlink leaf is replaced, not followed
    NoClobber        // atomic fail-if-exists (RENAME_NOREPLACE / RENAME_EXCL)
};

enum class SeekOrigin { Begin, Current, End };

// Minimal random-access read interface (v1.25 plan §1). Implemented by
// FileStream (buffered syscall reads) and io::MappedFile (fault-guarded
// mapped reads) so consumers — the header scanner — run unchanged on
// either engine.
class ReadSource {
public:
    virtual ~ReadSource() = default;
    virtual bool is_open() const = 0;
    virtual core::uint64 size() const = 0;
    virtual bool seek(core::int64 offset, SeekOrigin origin) = 0;
    virtual core::uint64 tell() const = 0;
    virtual size_t read(void* dest, size_t bytes) = 0;
};

class FileStream : public ReadSource {
public:
    FileStream();
    ~FileStream();

    // Move semantics (non-copyable)
    FileStream(FileStream&& other) noexcept;
    FileStream& operator=(FileStream&& other) noexcept;

    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;

    bool open(const std::filesystem::path& path, FileMode mode);
    void close();

    // Adopt an externally created OS handle (Windows HANDLE / POSIX fd).
    // Ownership transfers to this stream; the path is display-only. Used by
    // the v1.24 containment writer: the handle produced by the anchored
    // containment walk IS the write handle (write-through-handle, plan
    // §1.1.3 / §4.1).
    bool attach_os_handle(void* os_handle, const std::filesystem::path& display_path);
    void* os_handle() { return handle_; }

    // v1.24 containment commit (Windows): rename THIS open file to `leaf`
    // inside the already-verified parent directory handle — no path
    // resolution of any archive-controlled component happens after the walk.
    // POSIX callers use ContainmentRoot::anchored_rename (name-anchored
    // renameat) instead.
    bool commit_rename_in(void* parent_dir_handle, const std::string& utf8_leaf, CommitMode mode);

    bool is_open() const override;
    core::uint64 size() const override;

    size_t read(void* dest, size_t bytes) override;
    size_t write(const void* src, size_t bytes);

    bool seek(core::int64 offset, SeekOrigin origin) override;
    core::uint64 tell() const override;

    bool truncate(core::uint64 new_size);
    bool flush();

    // Atomic rename of THIS open file to `dest` (same directory — temp and
    // destination must share the volume, which the AtomicWriter guarantees by
    // construction). Flushes first. Windows: POSIX-semantics
    // FileRenameInformationEx through the open handle (replaces a symlink
    // leaf without following it; pre-1709 / FAT falls back to MoveFileExW,
    // which requires closing the handle first and is not truly atomic —
    // documented per plan §2.2). POSIX: renameat2(RENAME_NOREPLACE) /
    // renamex_np(RENAME_EXCL) for NoClobber, rename() for ReplaceExisting,
    // link()+unlink() cascade when the no-clobber syscalls are unavailable.
    // A read-only destination fails the commit (never clobbered).
    bool commit_rename(const std::filesystem::path& dest, CommitMode mode);

    const std::filesystem::path& path() const { return path_; }
    int last_error() const { return last_error_; }
    bool is_collision_error() const;

private:
    void* handle_;
    std::filesystem::path path_;
    FileMode mode_;
    int last_error_ = 0;
};

// Path-based atomic rename cascade (the handle-anchored form is
// FileStream::commit_rename). `from` and `to` should share a directory so the
// commit is same-volume by construction. last_error receives errno (POSIX) or
// GetLastError (Windows); use commit_is_collision_error to classify a
// NoClobber failure as "destination exists" rather than a hard error.
bool atomic_rename_commit(const std::filesystem::path& from, const std::filesystem::path& to,
                          CommitMode mode, int& last_error);
bool commit_is_collision_error(int last_error);

// True when the commit failed because the filesystem/OS lacks the
// POSIX-semantics rename capability (pre-1709 Windows, FAT, SMB) — the
// documented MoveFileExW-fallback case — as opposed to a real error
// (read-only destination, sharing violation, missing path).
bool commit_error_is_unsupported(int last_error);

} // namespace openrar::io

#endif // OPENRAR_IO_FILE_STREAM_HPP
