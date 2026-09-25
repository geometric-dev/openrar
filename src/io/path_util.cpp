#include "path_util.hpp"
#include "../core/types.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace openrar::io {

std::string normalize_separators(const std::string& path, char sep) {
    std::string res = path;
    for (char& c : res) {
        if (c == '/' || c == '\\') {
            c = sep;
        }
    }
    return res;
}

namespace {

// Windows-specific component hardening for names coming from untrusted
// archives (report M10): Win32 silently reinterprets several harmless-looking
// names, so neutralize them before the caller joins the result onto the
// extraction root.
//  - Reserved DOS device names (CON, NUL, COM1, "aux.txt", ...): the stem
//    before the first dot decides, case-insensitively. Prefixing with '_'
//    keeps the entry on disk instead of opening a device.
//  - Colons: NTFS alternate data streams ("file:stream"). Any colon that
//    survived drive stripping is replaced with '_'.
//  - Trailing dots/spaces: Win32 strips them at open time, letting two
//    distinct entry names collapse onto one file. Trimmed; a component that
//    trims to nothing becomes '_'.
std::string make_safe_component(std::string item) {
    static const char* RESERVED_NAMES[] = {
        "con",  "prn",  "aux",  "nul",  "com0", "com1", "com2", "com3",
        "com4", "com5", "com6", "com7", "com8", "com9", "lpt0", "lpt1",
        "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };

    // Pass 1: Map null byte \0, control characters \x01..\x1F, and forbidden
    // Windows characters (< > " | ? * :) to '_' to prevent invalid name
    // errors and null-byte C-string truncation attacks.
    for (char& c : item) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' ||
            c == ':') {
            c = '_';
        }
    }

    // Pass 2: Trim trailing dots and spaces from the whole component. Win32
    // strips them at open time, which would cause distinct entries to collide.
    size_t end = item.size();
    while (end > 0 && (item[end - 1] == '.' || item[end - 1] == ' ')) {
        --end;
    }
    if (end == 0) return "_";
    item.resize(end);

    // Pass 3: Extract the stem before the first dot, trim trailing dots/spaces
    // from the stem, and check against DOS device names (e.g., "CON .txt" or
    // "aux.tar.gz"). Prefixing with '_' ensures regular file creation.
    std::string stem = item;
    size_t dot = stem.find('.');
    if (dot != std::string::npos) stem.resize(dot);
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) {
        stem.pop_back();
    }
    for (char& c : stem) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const char* r : RESERVED_NAMES) {
        if (stem == r) {
            item.insert(item.begin(), '_');
            break;
        }
    }

    return item;
}

} // namespace

bool is_valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto lead = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if ((lead & 0xE0) == 0xC0)
            len = 2;
        else if ((lead & 0xF0) == 0xE0)
            len = 3;
        else if ((lead & 0xF8) == 0xF0)
            len = 4;
        else
            return false; // stray continuation byte or 0xF8+ lead
        if (i + len > s.size()) return false;
        unsigned v = lead & (len == 2 ? 0x1F : (len == 3 ? 0x0F : 0x07));
        for (size_t k = 1; k < len; ++k) {
            const auto cont = static_cast<unsigned char>(s[i + k]);
            if ((cont & 0xC0) != 0x80) return false;
            v = (v << 6) | (cont & 0x3F);
        }
        // overlong and surrogate rejection
        static const unsigned kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (v < kMin[len] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

std::string percent_encode_invalid_utf8(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    size_t i = 0;
    while (i < s.size()) {
        const auto lead = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        bool ok = false;
        if (lead < 0x80) {
            len = 1;
            ok = true;
        } else if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        if (len > 1 && i + len <= s.size()) {
            ok = true;
            unsigned v = lead & (len == 2 ? 0x1F : (len == 3 ? 0x0F : 0x07));
            for (size_t k = 1; k < len; ++k) {
                const auto cont = static_cast<unsigned char>(s[i + k]);
                if ((cont & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                v = (v << 6) | (cont & 0x3F);
            }
            if (ok) {
                static const unsigned kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
                if (v < kMin[len] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) ok = false;
            }
        }
        if (len == 0) len = 1; // invalid lead byte: escape it and move on
        if (ok) {
            out.append(s, i, len);
        } else {
            // each byte of the broken sequence becomes %XX
            for (size_t k = 0; k < len; ++k) {
                const auto b = static_cast<unsigned char>(s[i + k]);
                out += '%';
                out += kHex[(b >> 4) & 0xF];
                out += kHex[b & 0xF];
            }
        }
        i += len;
    }
    return out;
}

std::string sanitize_archive_path(const std::string& path) {
    std::string norm = normalize_separators(path, '/');

    // Strip drive letter (e.g. "C:")
    if (norm.size() >= 2 && std::isalpha(static_cast<unsigned char>(norm[0])) && norm[1] == ':') {
        norm = norm.substr(2);
    }

    // Strip leading slashes
    size_t start = norm.find_first_not_of('/');
    if (start == std::string::npos) return "";
    norm = norm.substr(start);

    // Tokenize and resolve '.' and '..'
    std::vector<std::string> parts;
    std::stringstream ss(norm);
    std::string item;

    while (std::getline(ss, item, '/')) {
        if (item.empty() || item == ".") {
            continue;
        }
        if (item == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else {
            // v1.24 plan §4.3: undecodable (invalid UTF-8) names are
            // percent-encoded losslessly BEFORE component hardening — the
            // escaped name is the on-disk name and displayed name alike.
            parts.push_back(make_safe_component(percent_encode_invalid_utf8(item)));
        }
    }

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += '/';
        result += parts[i];
    }
    return result;
}

std::string format_archive_path(const std::string& full_path, const std::string& base_path,
                                ExcludePathMode mode) {
    std::string norm_full = normalize_separators(full_path, '/');
    std::string norm_base = normalize_separators(base_path, '/');

    switch (mode) {
    case ExcludePathMode::SkipWholePath: {
        // -ep: return filename only
        size_t last_slash = norm_full.find_last_of('/');
        if (last_slash != std::string::npos) {
            return norm_full.substr(last_slash + 1);
        }
        return norm_full;
    }
    case ExcludePathMode::BasePath: {
        // -ep1: strip base directory prefix
        if (!norm_base.empty() && norm_full.rfind(norm_base, 0) == 0) {
            size_t offset = norm_base.size();
            while (offset < norm_full.size() && norm_full[offset] == '/') {
                offset++;
            }
            return norm_full.substr(offset);
        }
        return sanitize_archive_path(norm_full);
    }
    case ExcludePathMode::SaveFullPathNoDrive: {
        // -ep2: strip drive letter, retain path
        if (norm_full.size() >= 2 && std::isalpha(static_cast<unsigned char>(norm_full[0])) &&
            norm_full[1] == ':') {
            norm_full = norm_full.substr(2);
        }
        return sanitize_archive_path(norm_full);
    }
    case ExcludePathMode::AbsPath: {
        // -ep3: full absolute path with disk letter
        return norm_full;
    }
    case ExcludePathMode::None:
    default: {
        if (!norm_base.empty() && norm_full.rfind(norm_base, 0) == 0) {
            size_t offset = norm_base.size();
            while (offset < norm_full.size() && norm_full[offset] == '/') {
                offset++;
            }
            return norm_full.substr(offset);
        }
        return sanitize_archive_path(norm_full);
    }
    }
}

bool wildcard_match(const std::string& pattern, const std::string& text, bool case_sensitive) {
    size_t p = 0, t = 0;
    size_t star_p = std::string::npos, star_t = 0;

    auto eq = [case_sensitive](char a, char b) {
        if (case_sensitive) return a == b;
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], text[t]))) {
            p++;
            t++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p != std::string::npos) {
            p = star_p + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') {
        p++;
    }

    return p == pattern.size();
}

bool is_lexically_contained(const std::filesystem::path& target,
                            const std::filesystem::path& base_dir) {
    std::filesystem::path norm_target = target.lexically_normal();
    std::filesystem::path norm_base = base_dir.lexically_normal();

    std::filesystem::path rel = norm_target.lexically_relative(norm_base);
    if (rel.empty()) return false;
    // An absolute relative path indicates different root drives (e.g. C: vs D:)
    if (rel.is_absolute()) return false;

    // Check if relative path escapes via leading ".."
    auto it = rel.begin();
    if (it != rel.end() && *it == "..") {
        return false;
    }
    return true;
}

} // namespace openrar::io
