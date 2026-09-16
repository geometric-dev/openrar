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

    const std::filesystem::path& path() const { return path_; }
    int last_error() const { return last_error_; }
    bool is_collision_error() const;

private:
    void* handle_;
    std::filesystem::path path_;
    FileMode mode_;
    int last_error_ = 0;
};

} // namespace openrar::io

#endif // OPENRAR_IO_FILE_STREAM_HPP
