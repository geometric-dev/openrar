#ifndef OPENRAR_ARCHIVE_BUFFER_ARCHIVE_HPP
#define OPENRAR_ARCHIVE_BUFFER_ARCHIVE_HPP

#include "../core/types.hpp"
#include "extraction_limits.hpp"
#include "rar_errors.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace openrar::archive {

// (Error codes live in rar_errors.hpp — canonical shared ABI contract.)

// ── Callback conventions (mirror src/dll/openrar_dll.h) ─────────────────────
// progress: (done, total) cumulative and monotonic; total never negative.
// cancel:   polled once per entry; return non-zero to abort with
//           RAR_ERR_ABORTED. Never called under a lock.
using progress_cb = void (*)(uint64_t done, uint64_t total, void* user);
using cancel_cb = int (*)(void* user);

// ── One entry parsed from a RAR5 archive ─────────────────────────────────────
struct BufferArchiveEntry {
    std::string path; // forward-slash separated; trailing '/' iff is_dir
    bool is_dir{false};
    uint64_t size{0};        // uncompressed size
    uint64_t packed_size{0}; // compressed (or stored) size; 0 for dirs
    uint64_t mtime{0};       // UNIX seconds (converted from DOS time on disk)
    uint32_t crc32{0};       // CRC32 if has_crc32; 0 when absent
    int method{0};           // 0/1/2/3/4/5
    uint64_t win_size{0};    // compression window size
    bool is_encrypted{false};
    uint64_t header_offset{0}; // absolute byte offset of header in source buffer
    uint64_t data_offset{0};   // absolute byte offset of payload start in source buffer
    uint64_t data_size{0};     // byte size of packed payload (single extent in MVP)
    bool has_crc32{false};
    bool has_blake2sp{false};
    std::array<uint8_t, 32> blake2sp{};
};

// ── Validate an archive entry path. Returns true on success. ─────────────────
// Path contract:
//   - UTF-8, forward slashes only; backslash rejected.
//   - Non-empty; max 2048 bytes.
//   - No leading '/'; no control chars (<0x20) or NUL.
//   - No '..' path segments.
//   - For directories: trailing '/' required.
//   - For files: trailing '/' forbidden.
bool validate_archive_path(const std::string& path, bool is_dir, std::string& err_out);

// DOS-time (as stored in the 32-bit file-header mtime field) → UNIX seconds,
// UTC-based. 0 maps to 0 (no FHFL_UTIME field). Shared by the buffer walk and
// the DLL's file-handle entry mapping so both surfaces report identical
// mtimes for the same archive.
uint64_t dos_time_to_unix(uint32_t dos);

// ── In-memory RAR5 archive reader ────────────────────────────────────────────
class BufferArchive {
public:
    BufferArchive() = default;
    ~BufferArchive() = default;

    // Parse a single-volume RAR5 archive from a buffer.
    // On RAR_OK, out_entries contains the parsed entries and the archive
    // state caches them so subsequent extract() / extract_all() calls can
    // identify entries by index.
    // Returns RAR_ERR_NOT_RAR if no RAR5 signature is found (incl. SFX scan up
    // to 4 MiB), RAR_ERR_TRUNCATED on premature EOF, RAR_ERR_UNSUPPORTED_FEATURE
    // on multi-volume / recovery / encrypted headers, RAR_ERR_IO on read fail.
    // The optional hooks follow the progress_cb / cancel_cb convention above,
    // with byte semantics: (done, total) = (walk offset, buffer size), polled
    // between header blocks. Defaults keep the 3-argument form unchanged.
    int list(const uint8_t* data, size_t size, std::vector<BufferArchiveEntry>& out_entries,
             progress_cb on_progress = nullptr, void* progress_user = nullptr,
             cancel_cb on_cancel = nullptr, void* cancel_user = nullptr);

    // Decompress a single entry to `out`. `entry_index` is into the most
    // recent list() result on this instance. Returns RAR_ERR_INVALID_ARG
    // if entry_index is out of range.
    int extract(const uint8_t* data, size_t size, size_t entry_index, std::vector<uint8_t>& out,
                const ExtractionLimits* limits = nullptr,
                LimitState* state = nullptr);

    // Extract every entry from the most recent list(). Cumulative monotonic
    // progress from 0 to `total` (sum of entry sizes); ~50% is the size-probe
    // (list) phase; the remainder is payload materialisation. done === total
    // is emitted exactly once at completion. on_progress may be null.
    int extract_all(const uint8_t* data, size_t size,
                    std::vector<std::pair<std::string, std::vector<uint8_t>>>& out_files,
                    progress_cb on_progress = nullptr, void* user = nullptr,
                    cancel_cb on_cancel = nullptr, void* cancel_user = nullptr,
                    const ExtractionLimits* limits = nullptr,
                    LimitState* state = nullptr);

private:
    // Cached result from the most recent list() call.
    std::vector<BufferArchiveEntry> cached_entries_;
    // Byte offset of the 8-byte signature in the most recent list() call.
    // extract()/extract_all() recompute by re-scanning for the signature.
    size_t cached_signature_offset_{0};
    size_t cached_buffer_size_{0};
    // Identity of the buffer list() parsed: a different buffer of the same
    // size must not silently reuse stale offsets.
    const uint8_t* cached_buffer_{nullptr};
};

// ── Streaming list of a RAR5 archive directly from a file ────────────────────
// Sequential header walk over the file (no whole-file buffering), so listing
// multi-GB archives does not materialise them in RAM and progress/cancel stay
// responsive while the walk seeks across slow (e.g. network) storage. Shares
// the per-block state machine with BufferArchive::list, so entry output and
// error mapping match — with documented deltas:
//   - RAR_ERR_ENCRYPTED as soon as a HEAD_CRYPT block is reached without a
//     password (headers encrypted; a password would be required to read them)
//     instead of the historical RAR_ERR_UNSUPPORTED_FEATURE;
//   - RAR_ERR_BAD_PASSWORD when a password was supplied but does not decrypt
//     the headers (PswCheck mismatch or header CRC failure);
//   - RAR_ERR_UNSUPPORTED_FEATURE for an unknown HEAD_CRYPT crypto version;
//   - RAR_ERR_IO when the file cannot be opened.
//
// `password` (empty or null = none) decrypts header-encrypted archives
// (RAR5 -hp): the HEAD_CRYPT block is read in the clear, keys are derived via
// PBKDF2, and every following header is read through AES-256-CBC. A password
// on an archive without header encryption is ignored.
//
// `emit_encrypted_entries`: when false (frozen v1.1.0 semantics), the walk
// stops with RAR_ERR_UNSUPPORTED_FEATURE at the first file header carrying
// the encrypted-data flag, like the historical paths. When true, such
// entries are reported with is_encrypted = 1 and the walk continues — the
// password-listing path depends on this, because -hp implies encrypted file
// data for every entry. Only the DLL's password export opts in; solid,
// multi-volume and recovery archives remain rejected in every mode (the
// 64-byte entry layout cannot represent them).
//
// Progress is byte-based: (done, total) = (absolute walk offset incl. any SFX
// prefix, file size), monotonic, emitted per SFX-scan chunk and between
// header blocks, with exactly one final (total, total) on success. Callbacks
// may fire before a failing return (e.g. mid-SFX-scan). Cancel is polled per
// scan chunk and between header blocks; a non-zero return produces
// RAR_ERR_ABORTED. out_entries is cleared on every non-OK return.
int list_file_stream(const std::filesystem::path& arc_path,
                     std::vector<BufferArchiveEntry>& out_entries, const char* password,
                     bool emit_encrypted_entries, progress_cb on_progress = nullptr,
                     void* progress_user = nullptr, cancel_cb on_cancel = nullptr,
                     void* cancel_user = nullptr);

// ── Write a single-volume RAR5 archive to `out` ──────────────────────────────
// `method` ∈ {0, 3, 5}. `window_log2` ∈ {1, 2, 3, 4} → win_size 128KB..1MB
// (doubling: 128KB / 256KB / 512KB / 1MB). Default 4 MiB in the spec was
// inconsistent with the {1..4} range; we double cleanly. See implementation
// notes for details.
// MVP: no solid, no encryption, no recovery, no multi-volume, no mtime_ns.
// DOS mtime encoded on disk per spec; in-memory `mtime` is UNIX seconds.
// Directory entries have path with trailing '/', method=0, size=0.
int create_archive(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
                   std::vector<uint8_t>& out, int method = 3, unsigned window_log2 = 4,
                   progress_cb on_progress = nullptr, void* user = nullptr,
                   cancel_cb on_cancel = nullptr, void* cancel_user = nullptr);

// ── Input file (struct-based overload) ───────────────────────────────────────
struct ArchiveFileInput {
    std::string path;          // forward-slash separated; trailing '/' iff dir
    std::vector<uint8_t> data; // empty for dirs / empty files
    uint64_t mtime_unix{0};    // 0 ⇒ now() (this overload only)
};

int create_archive(const std::vector<ArchiveFileInput>& files, std::vector<uint8_t>& out,
                   int method = 3, unsigned window_log2 = 4, progress_cb on_progress = nullptr,
                   void* user = nullptr, cancel_cb on_cancel = nullptr,
                   void* cancel_user = nullptr);

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_BUFFER_ARCHIVE_HPP
