#ifndef OPENRAR_ARCHIVE_VOLUME_HPP
#define OPENRAR_ARCHIVE_VOLUME_HPP

#include "../core/types.hpp"
#include <filesystem>
#include <string>

namespace openrar::archive::volume {

inline constexpr size_t MAX_HEADER_SIZE_MARGIN = 80;
inline constexpr size_t ENDARC_SIZE = 7;
inline constexpr core::uint64 VOLSIZE_AUTO = static_cast<core::uint64>(~0ULL);

// Naming helpers (RAR5 new numbering + legacy .r00)
std::filesystem::path next_volume_name(const std::filesystem::path& cur, bool old_numbering);
std::filesystem::path vol_name_to_first_name(const std::filesystem::path& cur, bool old_numbering);
std::filesystem::path first_volume_name(const std::filesystem::path& arc_path,
                                        bool old_numbering = false);

// Parse -v<size>[k|m|g|t] ; supports float, k=1024 ; sets ok false on error
// bare "-v" => VOLSIZE_AUTO, "-v-" => 0 (clear)
core::uint64 parse_vol_size_str(const std::string& vol_arg, bool& ok);

} // namespace openrar::archive::volume

#endif // OPENRAR_ARCHIVE_VOLUME_HPP
