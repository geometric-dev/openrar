#ifndef OPENRAR_IO_PATH_UTIL_HPP
#define OPENRAR_IO_PATH_UTIL_HPP

#include <string>
#include <vector>

namespace openrar::io {

enum class ExcludePathMode {
    None = 0,                // Store path relative to scan root
    SkipWholePath = 1,       // -ep: store only filename, strip all directories
    BasePath = 2,            // -ep1: exclude the base part of path
    SaveFullPathNoDrive = 3, // -ep2: full path without disk letter
    AbsPath = 4              // -ep3: full path with disk letter
};

// Normalizes path separators to '/' (or specified sep)
std::string normalize_separators(const std::string& path, char sep = '/');

// Sanitizes path to prevent directory traversal vulnerabilities (strips leading '/', resolves '..')
std::string sanitize_archive_path(const std::string& path);

// Formats file path according to -ep switch rules
std::string format_archive_path(const std::string& full_path, const std::string& base_path,
                                ExcludePathMode mode);

#ifdef _WIN32
inline constexpr bool DEFAULT_CASE_SENSITIVE = false;
#else
inline constexpr bool DEFAULT_CASE_SENSITIVE = true;
#endif

// Simple wildcard match for masks (supports '*' and '?')
bool wildcard_match(const std::string& pattern, const std::string& text,
                    bool case_sensitive = DEFAULT_CASE_SENSITIVE);

} // namespace openrar::io

#endif // OPENRAR_IO_PATH_UTIL_HPP
