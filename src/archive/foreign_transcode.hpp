#ifndef OPENRAR_ARCHIVE_FOREIGN_TRANSCODE_HPP
#define OPENRAR_ARCHIVE_FOREIGN_TRANSCODE_HPP

// cv transcoder (v1.29.0) — foreign archive in, RAR 5.0 out. The output is
// produced ENTIRELY by the shipped ArchiveMutator pipeline (stage-to-temp +
// prepare_add_* + write_batch_add, pre-analysis CV-A1): zero emission code
// is added by this arc. Normative references: docs/spec/11-foreign-formats.md,
// docs/v1.29-pre-analysis.md §2 (security mapping), implementation plan §1 M5.

#include "archive_reader.hpp"
#include "extraction_report.hpp"
#include "foreign_reader.hpp"

#include <filesystem>

namespace openrar::archive::foreign {

struct TranscodeOptions {
    int method = 3;             // output compression (-m0..-m5)
    core::uint64 dict_size = 0; // -md pass-through
    bool solid = false;         // -s
    std::string password;       // OUTPUT encryption only (no foreign decryption)
    bool encrypt_headers = false;
    bool want_rr = false;
    core::uint32 rr_percent = 0; // -rr[N%]
    bool delete_source = false;  // -df: only after a 100%-verified roundtrip
    unsigned threads = 1;
    std::string comment_override; // -z override; empty = migrate source comment

    // §5.4 caps (plan §7 defaults; non-disableable for cv reads — the caller
    // passes UNLIMITED fields at its own peril, the cv CLI pins defaults).
    ExtractionLimits limits;
};

struct TranscodeResult {
    ForeignStatus status = ForeignStatus::NotOpened;
    bool usage_refused = false; // RAR5/legacy-RAR source: wrong KIND, exit 7
    std::string detail;
    std::string format;    // "zip" | "tar" | "gzip"
    bool verified = false; // roundtrip verify outcome
    bool source_deleted = false;
    core::uint64 migrated_files = 0;
    core::uint64 migrated_dirs = 0;
    core::uint64 skipped = 0;
    core::uint64 failed = 0;
    bool encrypted_refused_present = false;
    bool archive_comment_migrated = false;
    bool archive_comment_skipped = false; // hostile bytes neutralized
};

// Executes the migration. `report` receives the per-entry records (schema v2
// fields). `hooks` drive progress + cooperative cancel on every loop.
ForeignStatus transcode(const std::filesystem::path& source, const std::filesystem::path& dest,
                        const TranscodeOptions& options, TranscodeResult& result,
                        ExtractionReport& report, ReaderHooks hooks);

} // namespace openrar::archive::foreign

#endif // OPENRAR_ARCHIVE_FOREIGN_TRANSCODE_HPP
