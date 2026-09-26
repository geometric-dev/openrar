#include "extraction_report.hpp"

#include <cstdint>

namespace openrar::archive {

std::string json_escape(const std::string& s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
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
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
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
            static const uint32_t kMinCp[5] = {0, 0, 0x80, 0x800, 0x10000};
            if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu) || cp < kMinCp[len]) {
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

void ExtractionReport::finalize_pending() {
    for (auto& e : entries) {
        if (e.status.empty()) {
            e.status = "unprocessed";
            if (e.reason.empty()) e.reason = "extraction aborted";
        }
    }
}

std::string to_json(const ExtractionReport& report) {
    std::string out = "{\"schema_version\":";
    out += std::to_string(report.schema_version);
    out += ",\"archive\":\"" + json_escape(report.archive) + "\"";
    out += ",\"exit_code\":" + std::to_string(report.exit_code);
    out += ",\"entries\":[";
    bool first = true;
    for (const auto& e : report.entries) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"" + json_escape(e.name) + "\"";
        out += ",\"status\":\"" + json_escape(e.status) + "\"";
        if (e.reason.empty()) {
            out += ",\"reason\":null";
        } else {
            out += ",\"reason\":\"" + json_escape(e.reason) + "\"";
        }
        out += ",\"security_flags\":[";
        bool first_flag = true;
        for (const auto& f : e.security_flags) {
            if (!first_flag) out += ",";
            first_flag = false;
            out += "\"" + json_escape(f) + "\"";
        }
        out += "]}";
    }
    out += "]";
    out += std::string(",\"aborted\":") + (report.aborted ? "true" : "false");
    if (report.abort_reason.empty()) {
        out += ",\"abort_reason\":null";
    } else {
        out += ",\"abort_reason\":\"" + json_escape(report.abort_reason) + "\"";
    }
    out += "}";
    return out;
}

} // namespace openrar::archive
