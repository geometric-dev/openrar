#ifndef OPENRAR_ARCHIVE_FOREIGN_GZIP_HPP
#define OPENRAR_ARCHIVE_FOREIGN_GZIP_HPP

// GZIP reader (v1.29.0) — docs/spec/11-foreign-formats.md §5 is normative.
// Multi-member files are ONE logical entry: concatenated member payloads
// (the RFC 1952 interoperable concatenation reading). Declared
// uncompressed size does not exist until a member ends (ISIZE mod 2^32) —
// the in-flight cap is the ONLY bound (plan D5).

#include "foreign_reader.hpp"

namespace openrar::archive::foreign {

class GzipReader : public ForeignReader {
public:
    ForeignStatus open(const std::filesystem::path& path, const ExtractionLimits& limits,
                       ReaderHooks hooks, std::string& detail) override;
    size_t entry_count() const override { return opened_ ? 1 : 0; }
    const ForeignEntry& entry(size_t index) const override;
    ForeignStatus decode(size_t index, const SinkFn& sink, const ExtractionLimits& limits,
                         LimitState& state, ReaderHooks hooks) override;
    std::string format_name() const override { return "gzip"; }

private:
    ForeignEntry entry_;
    std::filesystem::path path_;
    io::FileStream file_;
    bool opened_ = false;
    // Logical member map (member index → data start offset; payload extends
    // to the next member header or EOF).
    struct Member {
        core::uint64 data_offset = 0;
        core::uint32 crc = 0;   // verified at decode
        core::uint32 isize = 0; // mod 2^32
    };
    std::vector<Member> members_;
};

} // namespace openrar::archive::foreign

#endif // OPENRAR_ARCHIVE_FOREIGN_GZIP_HPP
