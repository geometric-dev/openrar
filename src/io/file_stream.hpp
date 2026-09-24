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

class FileStream {
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

    bool is_open() const;
    core::uint64 size() const;

    size_t read(void* dest, size_t bytes);
    size_t write(const void* src, size_t bytes);

    bool seek(core::int64 offset, SeekOrigin origin);
    core::uint64 tell() const;

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

} // namespace openrar::io

#endif // OPENRAR_IO_FILE_STREAM_HPP
