#ifndef OPENRAR_ARCHIVE_ARCHIVE_ENTRY_HPP
#define OPENRAR_ARCHIVE_ARCHIVE_ENTRY_HPP

#include "../core/types.hpp"
#include "../format/headers.hpp"
#include <filesystem>

namespace openrar::archive {

struct VolumeExtent {
    std::filesystem::path volume_path;
    core::uint64 offset{0};
    core::uint64 size{0};
};

struct ArchiveEntry {
    format::FileBlock header;
    core::uint64 header_offset{0}; // offset of header in first volume (compat)
    core::uint64 header_size{0};
    core::uint64 data_offset{0}; // legacy single-extent offset (first extent)
    core::uint64 data_size{0};   // legacy single-extent size (sum for split, first for compat)
    bool in_memory{false};
    std::vector<core::byte> memory_data;
    // Multivolume extents: for non-split single entry, one extent equal to data_offset/size
    // For split files, multiple extents stitched in order
    std::vector<VolumeExtent> extents;
    // Split flags of the logical file (derived from first/last chunk)
    bool split_before{false};
    bool split_after{false};
};

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_ARCHIVE_ENTRY_HPP
