#ifndef OPENRAR_IO_MOTW_HPP
#define OPENRAR_IO_MOTW_HPP

#include "../core/types.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace openrar::io {

// ── Mark-of-the-Web / quarantine (v1.27, SECURITY_ARCHITECTURE §4.3) ────────
// Policy: zone data is read EXCLUSIVELY from the archive file's own
// filesystem metadata (its Windows Zone.Identifier ADS, its macOS
// com.apple.quarantine xattr) — never from archive-internal streams; fresh
// mark content is GENERATED LOCALLY per extracted file. These helpers keep
// the provenance read, the local generation, and the never-strip/never-
// downgrade rule in one place (v1.27 plan §1.5/§1.6).

struct MotwProvenance {
    bool marked{false};          // the archive file carries a transport mark
    int zone{-1};                // Windows ZoneId (3 when marked but unparseable)
    std::string quarantine_flag; // macOS: flag field of the archive's own xattr
};

// Probes the archive file's own transport metadata once per extraction
// session. No path heuristics, no browser data — the file's own ADS/xattr
// is the entire provenance surface.
MotwProvenance probe_archive_motw(const std::filesystem::path& archive_file);

// True for the Windows zone stream name in any legal spelling (case-
// insensitive, with or without the trailing ":$DATA" type suffix). Used to
// keep archive-provided zone content away from disk: -os never stores it,
// extraction never restores it.
bool is_zone_stream_name(const std::string& stream_name);

// Parses ZoneId=N out of Zone.Identifier content. false when absent or
// malformed (callers fail safe to zone 3).
bool parse_zone_id(const std::vector<core::byte>& content, int& out_zone);

// The exact bytes written for a propagated mark — generated locally, no
// HostUrl/ReferrerUrl ever travels (plan test motw_hosturl_never_copied).
std::vector<core::byte> generate_zone_identifier_content(int zone);

// Writes a fresh, locally generated mark (Windows ADS). Never removes or
// downgrades: an existing ZoneId >= zone suppresses the write. Returns false
// when nothing was written (unsupported filesystem, downgrade suppression).
bool write_file_zone_id(const std::filesystem::path& file, int zone);

// macOS quarantine (provenance): reads the archive's own quarantine flag
// field ("0083;..."), and writes a freshly generated value with the same
// flag but local timestamp/agent/UUID. Never touches a file that already
// carries a quarantine mark.
bool read_quarantine_flag(const std::filesystem::path& file, std::string& flag_out);
std::string generate_quarantine_value(const std::string& flag4hex);
bool write_file_quarantine(const std::filesystem::path& file, const std::string& flag4hex);

} // namespace openrar::io

#endif // OPENRAR_IO_MOTW_HPP
