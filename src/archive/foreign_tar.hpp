#ifndef OPENRAR_ARCHIVE_FOREIGN_TAR_HPP
#define OPENRAR_ARCHIVE_FOREIGN_TAR_HPP

// TAR reader (v1.29.0) — docs/spec/11-foreign-formats.md §4 is normative:
// ustar + a documented GNU/pax subset. Sparse members are refused
// per-entry (documented subset boundary, never silent corruption).

#include "foreign_reader.hpp"

namespace openrar::archive::foreign {

class TarReader : public ForeignReader {
public:
    ForeignStatus open(const std::filesystem::path& path, const ExtractionLimits& limits,
                       ReaderHooks hooks, std::string& detail) override;
    size_t entry_count() const override { return entries_.size(); }
    const ForeignEntry& entry(size_t index) const override { return entries_[index].meta; }
    ForeignStatus decode(size_t index, const SinkFn& sink, const ExtractionLimits& limits,
                         LimitState& state, ReaderHooks hooks) override;
    std::string format_name() const override { return "tar"; }

private:
    struct Member {
        ForeignEntry meta;
        core::uint64 data_offset = 0;
        core::uint64 size = 0;
    };

    std::vector<Member> entries_;
    std::filesystem::path path_;
    io::FileStream file_; // opened by open(); held for decode
};

} // namespace openrar::archive::foreign

#endif // OPENRAR_ARCHIVE_FOREIGN_TAR_HPP
