#ifndef OPENRAR_ARCHIVE_FOREIGN_READER_HPP
#define OPENRAR_ARCHIVE_FOREIGN_READER_HPP

// Shared vocabulary for the v1.29.0 foreign-format readers (ZIP / TAR /
// GZIP) — docs/spec/11-foreign-formats.md is the normative reference.
// Archive-layer (rank 4): includes point downward only. NOT part of the C
// ABI surface (plan binding negative: zero new exports).

#include "../archive/extraction_limits.hpp"
#include "../archive/archive_reader.hpp" // ReaderHooks (progress/cancel)
#include "../core/types.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace openrar::archive::foreign {

// ── Dispatch (one detection function, plan D8) ──────────────────────────────
enum class SourceFormat {
    None,      // unrecognized → exit 13
    Rar5,      // not a cv source (usage refusal upstream)
    LegacyRar, // OUT OF SCOPE (user decision) → explicit refusal
    Zip,
    Gzip,
    Tar,
};

SourceFormat detect_foreign_format(const std::filesystem::path& path);

// ── Status vocabulary (maps to the §3.1 exit taxonomy at the cv layer; the
// frozen C-ABI enum RarError is untouched) ───────────────────────────────────
enum class ForeignStatus {
    Ok = 0,
    Unparseable = 1,        // signature/EOCD/CD/structure unreadable → 13
    StructuralMismatch = 2, // §2.1 CD-vs-LFH, §3.2 collision classes → 2
    Truncated = 3,          // member/payload ends early → 3
    CrcMismatch = 4,        // payload checksum → 3
    LimitExceeded = 5,      // §5.4 caps → 2
    Aborted = 6,            // cancel via hooks → 255
    IoError = 7,            // file open/read/staging IO → 2
    UnsupportedMethod = 8,  // per-entry skip (method-named)
    EncryptedRefused = 9,   // per-entry skip (ZipCrypto policy / AES-ZIP)
    SparseRefused = 10,     // per-entry skip (TAR sparse, documented subset)
    NameEmpty = 11,         // per-entry skip (name sanitizes to empty)
    NotOpened = 12,         // decode attempted before a successful open
};

const char* foreign_status_string(ForeignStatus s);

// ── Entry model (post-translation) ──────────────────────────────────────────
enum class ForeignType {
    File,
    Dir,
    Symlink,
    Hardlink,
    Other, // char/block/fifo and friends: skip-with-report
};

struct ForeignEntry {
    std::string name;     // composed name (translate_foreign_name)
    std::string name_raw; // bytes as stored (report/diagnostics only)
    bool name_escaped = false;
    ForeignType type = ForeignType::File;
    std::string link_target;        // composed (symlink/hardlink)
    core::uint64 unpacked_size = 0; // declared; NEVER a bound (plan D5)
    core::uint32 crc32 = 0;         // format-level payload integrity (when present)
    bool has_crc32 = false;
    core::uint32 dos_attrs = 0;
    bool has_posix_mode = false;
    core::uint32 posix_mode = 0; // honored only with real 0170000 type bits
    std::string owner_name;
    std::string group_name;
    bool has_owner_ids = false;
    core::uint64 owner_uid = 0;
    core::uint64 owner_gid = 0;
    int64_t mtime_sec = 0;  // epoch seconds, clamped through MtimeBounds
    int32_t mtime_nsec = 0; // sub-second when the source carries it
    bool timestamp_clamped = false;
    bool encrypted = false;
    std::string method_name; // human-readable method label for reports
    std::string comment;     // entry-level comment (no RAR5 home: reported)
};

// ── Name composition (plan D14 — ONE pipeline, all formats) ─────────────────
enum class ForeignNameEncoding { Utf8, Cp437, Raw };

// 1. decode per source rule → 2. '\'→'/' → 3. percent-encode invalid UTF-8
// → 4. io::sanitize_archive_path → 5. empty result = NameEmpty skip.
std::string translate_foreign_name(const std::string& raw, ForeignNameEncoding enc, bool& escaped);

// CP437 → UTF-8 (total 256-entry map; in-tree table, no dependencies).
std::string cp437_to_utf8(const std::string& raw);

// ── Reader interface ────────────────────────────────────────────────────────
class ForeignReader {
public:
    using SinkFn = std::function<bool(const core::byte* data, size_t size)>;

    virtual ~ForeignReader() = default;

    // Full structural pass (ZIP: CD walk + the §2.1 CD-vs-LFH pre-flight
    // gate). Caps apply to the walk; hooks are polled between headers.
    virtual ForeignStatus open(const std::filesystem::path& path, const ExtractionLimits& limits,
                               ReaderHooks hooks, std::string& detail) = 0;

    virtual size_t entry_count() const = 0;
    virtual const ForeignEntry& entry(size_t index) const = 0;

    // Decode entry payload into sink. The in-flight cap (plan D5) bounds
    // actual output independently of any declared size; cumulative debit
    // rides `state`. hooks drive progress + cooperative cancel.
    virtual ForeignStatus decode(size_t index, const SinkFn& sink, const ExtractionLimits& limits,
                                 LimitState& state, ReaderHooks hooks) = 0;

    // Count of members whose declared size exceeded the file (diagnostic).
    virtual std::string format_name() const = 0;
};

// Cumulative-output debit across a cv run rides the shipped
// archive::LimitState (extraction_limits.hpp) — same accumulator the RAR5
// reader uses; no parallel vocabulary.
inline bool debit_total_bytes(core::uint64 bytes, const ExtractionLimits& limits,
                              LimitState& state) {
    state.total_out += bytes;
    if (limits.total_limited() && state.total_out > limits.max_total_output_bytes) return false;
    return true;
}

} // namespace openrar::archive::foreign

#endif // OPENRAR_ARCHIVE_FOREIGN_READER_HPP
