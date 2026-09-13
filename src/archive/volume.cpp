#include "volume.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace openrar::archive::volume {

std::filesystem::path next_volume_name(const std::filesystem::path& cur, bool old_numbering) {
    std::string fname = cur.filename().string();
    std::string dir = cur.parent_path().string();
    if (!old_numbering) {
        std::string ext = cur.extension().string();
        std::string lower_ext = ext;
        std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext.empty()) {
            fname += ".rar";
        } else if (!fname.empty() && fname.back() == '.') {
            fname += "rar";
        } else if (lower_ext == ".exe" || lower_ext == ".sfx") {
            fname = cur.stem().string() + ".rar";
        }
        int r = -1, l = -1;
        for (int i = static_cast<int>(fname.size()) - 1; i >= 0; --i) {
            if (std::isdigit(static_cast<unsigned char>(fname[i]))) {
                r = i;
                break;
            }
        }
        if (r == -1) {
            size_t dot = fname.rfind('.');
            std::string stem = (dot == std::string::npos) ? fname : fname.substr(0, dot);
            std::string e = (dot == std::string::npos) ? "" : fname.substr(dot);
            fname = stem + ".part01" + e;
        } else {
            l = r;
            while (l > 0 && std::isdigit(static_cast<unsigned char>(fname[l - 1]))) --l;
            std::string digits = fname.substr(l, r - l + 1);
            int carry = 1;
            for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
                int d = (digits[i] - '0') + carry;
                carry = d / 10;
                digits[i] = char('0' + (d % 10));
            }
            if (carry) {
                fname.insert(l, "1");
                fname.replace(l + 1, digits.size(), digits);
            } else {
                fname.replace(l, digits.size(), digits);
            }
        }
        std::filesystem::path out = fname;
        if (!dir.empty()) out = std::filesystem::path(dir) / out;
        return out;
    } else {
        std::string f = fname;
        size_t dot = f.rfind('.');
        std::string ext = (dot == std::string::npos) ? "" : f.substr(dot + 1);
        if (ext.size() < 2 || !std::isdigit((unsigned char)ext[ext.size() - 2]) ||
            !std::isdigit((unsigned char)ext[ext.size() - 1])) {
            if (dot == std::string::npos)
                f += ".r00";
            else
                f = f.substr(0, dot) + ".r00";
            std::filesystem::path out = f;
            if (!dir.empty()) out = std::filesystem::path(dir) / out;
            return out;
        } else {
            int i = static_cast<int>(f.size()) - 1;
            int carry = 1;
            while (i > static_cast<int>(dot) && carry) {
                char c = f[i];
                if (c >= '0' && c <= '9') {
                    int d = (c - '0') + carry;
                    carry = d / 10;
                    f[i] = char('0' + (d % 10));
                }
                --i;
            }
            if (carry) {
                // Tail digits wrapped (e.g. "99" -> carry with "00"); advance
                // the leading extension letter. Old-style volume names are
                // exactly one letter followed by two digits (`.r00`..`.r99`,
                // `.s00`..`.s99`, ..., `.z00`..`.z99`) -- the scheme defines no volume
                // past `.z99`. Refuse to advance rather than incrementing 'z'
                // into a non-alphabetic byte with no upper bound.
                if (dot == std::string::npos || dot + 1 >= f.size()) {
                    return cur; // malformed extension; nothing sane to advance
                }
                char letter = f[dot + 1];
                if (letter < 'a' || letter >= 'z') {
                    // Already at 'z' (or an unexpected non-letter) -- no
                    // further volume name exists in the legacy scheme
                    // (`.z99` is the last name defined by this convention). Returning
                    // `cur` UNCHANGED is the deliberate "no next volume"
                    // signal, not a loop bug: callers must compare names and
                    // stop (scan_archive does since fix L7, commit 7e454ee).
                    return cur;
                }
                f[dot + 1] = static_cast<char>(letter + 1);
            }
            std::filesystem::path out = f;
            if (!dir.empty()) out = std::filesystem::path(dir) / out;
            return out;
        }
    }
}

std::filesystem::path vol_name_to_first_name(const std::filesystem::path& cur, bool old_numbering) {
    if (old_numbering) {
        std::string f = cur.filename().string();
        size_t dot = f.rfind('.');
        std::string out = (dot == std::string::npos) ? f + ".rar" : f.substr(0, dot) + ".rar";
        if (!cur.parent_path().empty()) return cur.parent_path() / out;
        return std::filesystem::path(out);
    }
    std::string fname = cur.filename().string();
    bool has_digit = false;
    for (char c : fname)
        if (std::isdigit((unsigned char)c)) has_digit = true;
    if (!has_digit) {
        // No digits -> per next_volume logic would become *.part01.rar, treat same as next's no-digit case
        size_t dot = fname.rfind('.');
        // Apply extension handling as in next_volume_name first step
        std::string ext = cur.extension().string();
        std::string lower_ext = ext;
        std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext.empty()) {
            fname += ".rar";
            dot = fname.rfind('.');
        } else if (!fname.empty() && fname.back() == '.') {
            fname += "rar";
            dot = fname.rfind('.');
        } else if (lower_ext == ".exe" || lower_ext == ".sfx") {
            fname = cur.stem().string() + ".rar";
            dot = fname.rfind('.');
        }
        // Re-check has_digit after ext fix (still none)
        std::string stem = (dot == std::string::npos) ? fname : fname.substr(0, dot);
        std::string e = (dot == std::string::npos) ? "" : fname.substr(dot);
        fname = stem + ".part01" + e;
        if (!cur.parent_path().empty()) return cur.parent_path() / fname;
        return std::filesystem::path(fname);
    }
    // Rewrite ONLY the last digit run to "0...01" (first volume). Zeroing
    // every digit in the stem ("backup2024" -> "backup0000") produces a
    // nonexistent filename for the common digit-in-stem case.
    int r = -1;
    for (int i = static_cast<int>(fname.size()) - 1; i >= 0; --i) {
        if (std::isdigit(static_cast<unsigned char>(fname[i]))) {
            r = i;
            break;
        }
    }
    int l = r;
    while (l > 0 && std::isdigit(static_cast<unsigned char>(fname[l - 1]))) --l;
    for (int i = l; i <= r; ++i) fname[i] = '0';
    fname[r] = '1';
    if (!cur.parent_path().empty()) return cur.parent_path() / fname;
    return std::filesystem::path(fname);
}

std::filesystem::path first_volume_name(const std::filesystem::path& arc_path, bool old_numbering) {
    return vol_name_to_first_name(arc_path, old_numbering);
}

core::uint64 parse_vol_size_str(const std::string& vol_arg, bool& ok) {
    ok = true;
    if (vol_arg.empty()) {
        // bare -v
        return VOLSIZE_AUTO;
    }
    if (vol_arg == "-") {
        return 0; // clear
    }
    // vol_arg is like "100k", "10M", "1.5g" etc without leading "-v"
    // support optional suffix b/k/m/g/t (case insensitive)
    // find where numeric part ends
    // allow floating point
    size_t num_end = 0;
    bool dot_seen = false;
    while (num_end < vol_arg.size()) {
        char c = vol_arg[num_end];
        if (std::isdigit((unsigned char)c)) {
            ++num_end;
            continue;
        }
        if (c == '.' && !dot_seen) {
            dot_seen = true;
            ++num_end;
            continue;
        }
        break;
    }
    if (num_end == 0) {
        ok = false;
        return 0;
    }
    std::string num_str = vol_arg.substr(0, num_end);
    std::string suf = vol_arg.substr(num_end);
    double num = 0;
    try {
        num = std::stod(num_str);
    } catch (...) {
        ok = false;
        return 0;
    }
    core::uint64 mul = 1;
    if (!suf.empty()) {
        const int c = std::tolower((unsigned char)suf[0]);
        if (c == 'b')
            mul = 1;
        else if (c == 'k')
            mul = 1024ULL;
        else if (c == 'm')
            mul = 1024ULL * 1024;
        else if (c == 'g')
            mul = 1024ULL * 1024 * 1024;
        else if (c == 't')
            mul = 1024ULL * 1024 * 1024 * 1024;
        else {
            ok = false;
            return 0;
        }
        //allow second char? ignore
    }
    double res = num * (double)mul;
    if (res < 1) res = 1;
    if (res > (double)std::numeric_limits<core::uint64>::max()) {
        ok = false;
        return 0;
    }
    return static_cast<core::uint64>(res);
}

} // namespace openrar::archive::volume
