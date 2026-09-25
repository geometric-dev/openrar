#ifndef OPENRAR_IO_MAPPED_FILE_HPP
#define OPENRAR_IO_MAPPED_FILE_HPP

#include "file_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace openrar::io {

// ── Memory-mapped read engine (v1.25.0 plan §1) ─────────────────────────────
//
// Maps a file read-only into the address space and serves bounded,
// fault-guarded reads. SCOPE (SECURITY_ARCHITECTURE §5.2, normative):
// listing, header-scanning and random-read ONLY — never extraction inputs.
//
// Fault policy:
//   Windows: the copy leaf runs under SEH (__try/__except); an access
//     violation or in-page error (e.g. the file was truncated after
//     mapping) is converted to a short read, never a crash.
//   POSIX: no signal handlers (plan §0). Every access is bounds-checked
//     against the mapped length AND a fresh fstat of the current file size
//     (truncation-aware pre-flight); reads past the current EOF return
//     short so callers fall back to buffered reads. Residual: a hostile
//     truncation between the size check and the copy can still SIGBUS —
//     the documented posture is that this beats installing SIGBUS
//     handlers in a multithreaded engine.
//
// The engine pins the file to its open-time size: size() never changes
// after open, and End-relative seeks resolve against that size.

class MappedFile : public ReadSource {
public:
    MappedFile();
    ~MappedFile() override;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Opens and maps the file read-only. The mapping length is fixed to
    // the file size at open time (pre-flight). False when the file cannot
    // be opened or mapped — callers fall back to buffered reads (fail-open).
    bool open(const std::filesystem::path& path);
    void close();

    bool is_open() const override;
    core::uint64 size() const override; // open-time size (pinned)
    bool seek(core::int64 offset, SeekOrigin origin) override;
    core::uint64 tell() const override;
    size_t read(void* dest, size_t bytes) override;

    // Direct bounded read at an absolute offset (no cursor). Returns the
    // number of bytes actually copied — fewer than requested (including 0)
    // signals truncation or a mapping fault.
    size_t read_at(core::uint64 offset, void* dest, size_t bytes);

    const std::filesystem::path& path() const { return path_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::filesystem::path path_;
    core::uint64 size_ = 0; // pinned at open
    core::uint64 cursor_ = 0;
};

} // namespace openrar::io

#endif // OPENRAR_IO_MAPPED_FILE_HPP
