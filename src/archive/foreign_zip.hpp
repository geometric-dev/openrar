#ifndef OPENRAR_ARCHIVE_FOREIGN_ZIP_HPP
#define OPENRAR_ARCHIVE_FOREIGN_ZIP_HPP

// ZIP reader (v1.29.0) — docs/spec/11-foreign-formats.md §2 is normative.
// The §2.1 CD-vs-LFH pre-flight gate is a RELEASE GATE: any mismatch
// outside the documented tolerated set aborts with StructuralMismatch
// BEFORE any output exists.

#include "../io/file_stream.hpp"
#include "foreign_reader.hpp"

namespace openrar::archive::foreign {

class ZipReader : public ForeignReader {
public:
    ForeignStatus open(const std::filesystem::path& path, const ExtractionLimits& limits,
                       ReaderHooks hooks, std::string& detail) override;
    size_t entry_count() const override { return entries_.size(); }
    const ForeignEntry& entry(size_t index) const override { return entries_[index].meta; }
    ForeignStatus decode(size_t index, const SinkFn& sink, const ExtractionLimits& limits,
                         LimitState& state, ReaderHooks hooks) override;
    std::string format_name() const override { return "zip"; }

private:
    struct Cdh {
        ForeignEntry meta;
        core::uint64 lfh_offset = 0;
        core::uint16 method = 0;
        core::uint16 gp_flags = 0;
        core::uint32 crc = 0;
        core::uint64 packed_size = 0;
        core::uint64 unpacked_size = 0;
        core::uint64 data_offset = 0; // resolved after pre-flight
    };

    ForeignStatus locate_eocd(const ExtractionLimits& limits, std::string& detail);
    ForeignStatus walk_central_directory(const ExtractionLimits& limits, ReaderHooks hooks,
                                         std::string& detail);
    ForeignStatus preflight_cd_vs_lfh(std::string& detail);
    bool parse_zip64_extra(const core::byte* p, size_t n, bool need_sizes, bool need_offset,
                           core::uint64& unp, core::uint64& pack, core::uint64& offset) const;

    std::filesystem::path path_;
    io::FileStream file_;
    std::vector<Cdh> entries_;
    core::uint64 cd_offset_ = 0;
    core::uint64 cd_size_ = 0;
    core::uint64 total_entries_ = 0;
    std::string archive_comment_; // raw bytes (sanitized by the transcoder)
};

} // namespace openrar::archive::foreign

#endif // OPENRAR_ARCHIVE_FOREIGN_ZIP_HPP
