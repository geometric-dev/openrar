#ifndef OPENRAR_CLI_JSON_OUTPUT_HPP
#define OPENRAR_CLI_JSON_OUTPUT_HPP

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace openrar::cli {

// RFC 8259 compliant string serializer.
// Escapes quotes, backslashes, control characters (< 0x20) as \u00XX, and standard escapes.
inline std::string escape_json_string(const std::string& input) {
    std::ostringstream ss;
    for (unsigned char c : input) {
        switch (c) {
        case '"':
            ss << "\\\"";
            break;
        case '\\':
            ss << "\\\\";
            break;
        case '\b':
            ss << "\\b";
            break;
        case '\f':
            ss << "\\f";
            break;
        case '\n':
            ss << "\\n";
            break;
        case '\r':
            ss << "\\r";
            break;
        case '\t':
            ss << "\\t";
            break;
        default:
            if (c < 0x20) {
                // Control character -> \u00XX
                ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
                   << std::dec;
            } else {
                ss << static_cast<char>(c);
            }
            break;
        }
    }
    return ss.str();
}

} // namespace openrar::cli

#endif // OPENRAR_CLI_JSON_OUTPUT_HPP
