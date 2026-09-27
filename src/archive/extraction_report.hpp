#ifndef OPENRAR_ARCHIVE_EXTRACTION_REPORT_HPP
#define OPENRAR_ARCHIVE_EXTRACTION_REPORT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace openrar::archive {

// ── Machine-readable extraction summary (v1.24.0 plan §5) ───────────────────
//
// --json-summary[=path]: with the flag and no path, ALL human-readable output
// is routed to stderr and stdout carries only valid JSON. Consumers ignore
// unknown fields; breaking changes bump schema_version.
//
// Entry status semantics (SECURITY_ARCHITECTURE §3.1):
//   extracted   — file/dir/link written as archived
//   modified    — payload extracted plus additional metadata restored (ADS,
//                 ACL, owner, mode)
//   skipped     — not attempted (exists without consent, user declined,
//                 unsafe name, link extraction disabled, ...)
//   failed      — attempted and failed (checksum, IO, bad password, ...)
//   unprocessed — the run aborted before this entry was attempted
//
// security_flags: machine-readable tags (traversal_attempt, name_escaped,
// timestamp_clamped, collision_case, ...). The list is open-ended; consumers
// must ignore unknown tags.

// v2 (v1.29): appends the transcode fields after `abort_reason` when the run
// is a `cv` migration (has_transcode_fields) — additive, documented order;
// v1 consumer keys are unchanged in position (plan D3).
inline constexpr int kExtractionReportSchemaVersion = 2;

struct ExtractionReportEntry {
    std::string name;   // post-sanitization name == on-disk name
    std::string status; // extracted | modified | skipped | failed | unprocessed
    std::string reason; // human-readable, null when none
    std::vector<std::string> security_flags;
};

struct ExtractionReport {
    int schema_version = kExtractionReportSchemaVersion;
    std::string archive;
    int exit_code = 0;
    std::vector<ExtractionReportEntry> entries;
    bool aborted = false;
    std::string abort_reason;

    // v2 transcode fields (cv only; absent from x/e documents).
    bool has_transcode_fields = false;
    std::string format;          // "zip" | "tar" | "gzip"
    bool verified = false;       // roundtrip verify outcome
    bool source_deleted = false; // -df outcome

    // Marks every entry still pending as unprocessed (abort semantics, plan
    // §5.4: entries processed before an abort keep their real status).
    void finalize_pending();
};

// JSON escaping (RFC 8259): quotes, backslash and control characters.
std::string json_escape(const std::string& s);

// Serializes the report as a single-line JSON object (stable field order:
// schema_version, archive, exit_code, entries, aborted, abort_reason).
std::string to_json(const ExtractionReport& report);

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_EXTRACTION_REPORT_HPP
