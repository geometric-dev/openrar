#include "sfx_config.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace openrar::sfx {

namespace {

// Strict UTF-8 validation (rejects overlongs, surrogates, > U+10FFFF,
// bad leads/continuations). The decoder in cli/progress.hpp cannot be
// reused from here without a CLI-layer dependency; this validator shares
// its acceptance rules.
bool is_valid_utf8(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int len = 0;
        core::uint32 cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07u;
        } else {
            return false; // C0/C1 leads, continuation as lead, F5-FF
        }
        if (i + static_cast<size_t>(len) > n) return false;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        static constexpr core::uint32 kMinCp[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMinCp[len] || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
        i += static_cast<size_t>(len);
    }
    return true;
}

// Splits the comment into lines on LF, CRLF, or lone CR. Line splits only —
// values keep their own bytes verbatim (no trimming).
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            size_t end = i;
            if (end > start && s[end - 1] == '\r') --end;
            lines.emplace_back(s, start, end - start);
            start = i + 1;
        } else if (s[i] == '\r') {
            // Lone CR (old-Mac style or a crafted separator): also a line end.
            // CRLF pairs were already handled above only when followed by LF —
            // treat every CR as a terminator and let the LF branch skip the
            // empty line it produces.
            lines.emplace_back(s, start, i - start);
            start = i + 1;
        }
    }
    lines.emplace_back(s, start, s.size() - start);
    return lines;
}

std::string ascii_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
    });
    return out;
}

std::string trim_spaces_tabs(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

} // namespace

SfxConfig parse_sfx_config(const std::vector<core::byte>& comment) {
    SfxConfig cfg;

    std::string text(reinterpret_cast<const char*>(comment.data()), comment.size());
    // UTF-8 BOM tolerated and stripped.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    if (!is_valid_utf8(text)) {
        cfg.invalid_utf8 = true;
        return cfg;
    }

    size_t directive_count = 0;
    for (const std::string& line : split_lines(text)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue; // prose: ignored silently
        const std::string key = ascii_lower(trim_spaces_tabs(line.substr(0, eq)));
        const std::string value = line.substr(eq + 1);
        if (key.empty()) continue;

        const bool recognized = key == "setup" || key == "presetup" || key == "delete" ||
                                key == "shortcut" || key == "silent" || key == "path" ||
                                key == "overwrite" || key == "title" || key == "text" ||
                                key == "license" || key == "tempmode";
        if (!recognized) {
            cfg.ignored_unknown_keys++;
            continue;
        }

        if (++directive_count > kMaxDirectiveLines) {
            cfg.truncated = true;
            continue;
        }
        if (value.size() > kMaxValueBytes) {
            cfg.ignored_over_limit_lines++;
            continue;
        }

        if (key == "setup") {
            cfg.setup.push_back(value);
        } else if (key == "presetup") {
            cfg.presetup.push_back(value);
        } else if (key == "delete") {
            cfg.delete_patterns.push_back(value);
        } else if (key == "text") {
            cfg.text_lines.push_back(value);
        } else if (key == "license") {
            cfg.license_lines.push_back(value);
        } else if (key == "path") {
            cfg.path = value;
        } else if (key == "title") {
            cfg.title = value;
        } else if (key == "silent") {
            cfg.silent = value == "1"   ? SilentMode::HideStart
                         : value == "2" ? SilentMode::Headless
                                        : SilentMode::Off;
        } else if (key == "overwrite") {
            cfg.overwrite = value == "1"   ? OverwriteDirective::OverwriteAll
                            : value == "2" ? OverwriteDirective::SkipExisting
                                           : OverwriteDirective::Ask;
        } else if (key == "tempmode") {
            cfg.tempmode = true; // presence wins; the optional value is v1-ignored
        } else if (key == "shortcut") {
            // Comma-separated fields; at most kMaxShortcutFields, each at most
            // kMaxShortcutFieldBytes. More fields than allowed = malformed line.
            std::array<std::string, kMaxShortcutFields> fields;
            size_t count = 0;
            size_t start = 0;
            bool over = false;
            for (size_t i = 0; i <= value.size(); ++i) {
                if (i == value.size() || value[i] == ',') {
                    const std::string field = value.substr(start, i - start);
                    if (count >= kMaxShortcutFields || field.size() > kMaxShortcutFieldBytes) {
                        over = true;
                        break;
                    }
                    fields[count++] = field;
                    start = i + 1;
                }
            }
            if (over || count == 0) {
                cfg.ignored_over_limit_lines++;
            } else {
                cfg.shortcuts.push_back(ShortcutDirective{fields[0], fields[1], fields[2],
                                                          fields[3], fields[4], fields[5]});
            }
        }
    }
    return cfg;
}

} // namespace openrar::sfx
