#ifndef OPENRAR_CLI_JSON_OUTPUT_HPP
#define OPENRAR_CLI_JSON_OUTPUT_HPP

#include <cstdint>
#include <string>

namespace openrar::cli {

// RFC 8259 compliant string serializer.
// Escapes quotes, backslashes, control characters (< 0x20) as \u00XX, and standard escapes.
// Replaces invalid UTF-8 byte sequences with \uFFFD.
inline std::string escape_json_string(const std::string& s) {
    std::string out;
    static const char hex[] = "0123456789abcdef";
    out.reserve(s.size() + 8);
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
            }
            ++i;
            continue;
        }
        // v1.28 §7.1 release gate: RFC 8259 requires JSON text to be valid
        // UTF-8. Valid multi-byte sequences pass through; every byte of an
        // invalid sequence becomes the ASCII \uFFFD escape (never the raw
        // character), so the document stays parseable regardless of input.
        int len = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        }
        bool ok = len > 0 && i + static_cast<size_t>(len) <= n;
        for (int k = 1; ok && k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0xC0) != 0x80) {
                ok = false;
            }
        }
        if (ok) {
            uint32_t cp = 0;
            for (int k = 0; k < len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
                cp = (k == 0) ? (cc & (len == 2   ? 0x1Fu
                                       : len == 3 ? 0x0Fu
                                                  : 0x07u))
                              : ((cp << 6) | (cc & 0x3Fu));
            }
            static const uint32_t min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
            if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu) || cp < min_cp[len]) {
                ok = false;
            }
        }
        if (ok) {
            out.append(s, i, static_cast<size_t>(len));
            i += static_cast<size_t>(len);
        } else {
            out += "\\uFFFD";
            ++i;
        }
    }
    return out;
}
} // namespace openrar::cli

#endif // OPENRAR_CLI_JSON_OUTPUT_HPP
