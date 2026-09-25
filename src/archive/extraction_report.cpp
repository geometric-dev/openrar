#include "extraction_report.hpp"

namespace openrar::archive {

std::string json_escape(const std::string& s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
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
