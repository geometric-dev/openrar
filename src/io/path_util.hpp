#ifndef OPENRAR_IO_PATH_UTIL_HPP
#define OPENRAR_IO_PATH_UTIL_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace openrar::io {

// path::u8string() returns std::u8string (char8_t) in C++20 but std::string in
// C++17, and only the latter converts or concatenates with std::string. All
// internal paths crossing into std::string go through this bridge.
inline std::string u8_str(const std::filesystem::path& p) {
#ifdef __cpp_char8_t
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return p.u8string();
#endif
}

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

// Validates that target is strictly contained within base_dir using purely lexical resolution
// (zero filesystem roundtrips or disk syscalls). Returns false if target escapes base_dir.
bool is_lexically_contained(const std::filesystem::path& target,
                            const std::filesystem::path& base_dir);

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
