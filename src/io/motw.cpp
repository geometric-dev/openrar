#include "motw.hpp"

#include "../crypto/rng.hpp"
#include "posix_xattr.hpp"
#include "win32_meta.hpp"

#include <cctype>
#include <ctime>
#include <cstdio>

namespace openrar::io {

// ── Platform-independent helpers ────────────────────────────────────────────

bool is_zone_stream_name(const std::string& stream_name) {
    // Normalize: lowercase, strip a trailing ":$DATA" (NTFS stream-type
    // suffix; the read side of every producer tolerates it), then compare.
    // ADS names are case-insensitive on NTFS, so a case-sensitive match
    // would be a checklist control a hostile archive defeats.
    std::string lower;
    lower.reserve(stream_name.size());
    for (char c : stream_name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    static const char kSuffix[] = ":$data"; // compared against the lowercased copy
    constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
    if (lower.size() >= kSuffixLen &&
        lower.compare(lower.size() - kSuffixLen, kSuffixLen, kSuffix) == 0) {
        lower.resize(lower.size() - kSuffixLen);
    }
    return lower == ":zone.identifier";
}

bool parse_zone_id(const std::vector<core::byte>& content, int& out_zone) {
    // The content is INI-shaped: "[ZoneTransfer]\r\nZoneId=3\r\n...". The
    // [ZoneTransfer] header is tolerated-but-not-required; the ZoneId line
    // is matched case-insensitively against bounded input only.
    if (content.empty() || content.size() > (1u << 16)) return false;
    std::string text;
    text.reserve(content.size());
    for (core::byte b : content)
        text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(b))));
    static const char kKey[] = "zoneid=";
    constexpr size_t kKeyLen = sizeof(kKey) - 1;
    size_t pos = text.find(kKey);
    if (pos == std::string::npos) return false;
    pos += kKeyLen;
    // The value runs to the next control character / separator.
    size_t end = pos;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
    if (end == pos) return false; // no digits
    long value = 0;
    for (size_t i = pos; i < end; ++i) {
        value = value * 10 + (text[i] - '0');
        if (value > 99) return false; // zone ids are 0..4 — fail fast on garbage
    }
    if (value < 0 || value > 4) return false; // outside the Windows zone range
    out_zone = static_cast<int>(value);
    return true;
}

std::vector<core::byte> generate_zone_identifier_content(int zone) {
    // Standard IAttachmentExecute shape. NOTHING else travels: no HostUrl,
    // no ReferrerUrl — content is generated locally, never copied.
    const std::string text = "[ZoneTransfer]\r\nZoneId=" + std::to_string(zone) + "\r\n";
    return std::vector<core::byte>(text.begin(), text.end());
}

std::string generate_quarantine_value(const std::string& flag4hex) {
    // "flags;timestamp;agent;UUID" shape. Flag byte comes from the archive's
    // own provenance; timestamp/UUID are generated locally. Returns empty on
    // RNG failure (the caller skips the write rather than proceeding with
    // weak entropy).
    core::byte rnd[8];
    if (!crypto::secure_random_bytes(rnd, sizeof(rnd))) return std::string();
    char body[48];
    std::snprintf(body, sizeof(body), "%s;%08x;OpenRAR;", flag4hex.c_str(),
                  static_cast<unsigned>(::time(nullptr)));
    std::string out(body);
    char hex[3];
    for (core::byte b : rnd) {
        std::snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned>(b));
        out += hex;
    }
    return out;
}

// ── Windows implementation (ADS read/write via native paths) ────────────────
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// Reads the file's own Zone.Identifier ADS, bounded at 64 KiB (the transport
// writes ~100 bytes; anything larger is treated as unparseable).
bool read_zone_ads(const std::filesystem::path& file, std::vector<core::byte>& out) {
    out.clear();
    std::wstring ads_path = file.wstring() + L":Zone.Identifier";
    HANDLE h = CreateFileW(ads_path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.resize(1u << 16);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) {
        out.clear();
        return false;
    }
    out.resize(got);
    return true;
}

} // namespace

MotwProvenance probe_archive_motw(const std::filesystem::path& archive_file) {
    MotwProvenance p;
    std::vector<core::byte> content;
    if (read_zone_ads(archive_file, content)) {
        p.marked = true;
        // Fail-safe: an ADS we cannot parse still means "came from the web".
        p.zone = parse_zone_id(content, p.zone) ? p.zone : 3;
    }
    return p;
}

bool write_file_zone_id(const std::filesystem::path& file, int zone) {
    // Never remove or downgrade an existing stronger mark.
    std::vector<core::byte> existing;
    int existing_zone = 0;
    if (read_zone_ads(file, existing) && parse_zone_id(existing, existing_zone) &&
        existing_zone >= zone) {
        return false;
    }
    const std::vector<core::byte> content = generate_zone_identifier_content(zone);
    return write_alternate_stream(file, ":Zone.Identifier", content.data(), content.size());
}

bool read_quarantine_flag(const std::filesystem::path&, std::string&) {
    return false; // macOS provenance does not exist on Windows
}

bool write_file_quarantine(const std::filesystem::path&, const std::string&) {
    return false;
}

// ── POSIX implementation (macOS quarantine via xattrs) ──────────────────────
#else

MotwProvenance probe_archive_motw(const std::filesystem::path& archive_file) {
    MotwProvenance p;
#if defined(__APPLE__)
    std::string flag;
    if (read_quarantine_flag(archive_file, flag)) {
        p.marked = true;
        p.quarantine_flag = flag;
    }
#else
    (void)archive_file;
#endif
    return p;
}

bool write_file_zone_id(const std::filesystem::path&, int) {
    return false; // Zone.Identifier is a Windows ADS; nothing to write
}

#if defined(__APPLE__)
bool read_quarantine_flag(const std::filesystem::path& file, std::string& flag_out) {
    std::vector<core::byte> raw;
    if (!get_xattr(file, "com.apple.quarantine", raw) || raw.empty()) return false;
    // Shape: "flags;timestamp;agent;UUID" — the flag is the first field.
    std::string text(raw.begin(), raw.end());
    const size_t semi = text.find(';');
    flag_out = semi == std::string::npos ? text : text.substr(0, semi);
    return !flag_out.empty();
}
#else
bool read_quarantine_flag(const std::filesystem::path&, std::string&) {
    return false;
}
#endif

bool write_file_quarantine(const std::filesystem::path& file, const std::string& flag4hex) {
#if defined(__APPLE__)
    // Never touch a file that already carries a quarantine mark.
    std::string existing;
    if (read_quarantine_flag(file, existing)) return false;
    const std::string value = generate_quarantine_value(flag4hex);
    if (value.empty()) return false;
    return set_xattr(file, "com.apple.quarantine",
                     reinterpret_cast<const core::byte*>(value.data()), value.size());
#else
    (void)file;
    (void)flag4hex;
    return false;
#endif
}

#endif

} // namespace openrar::io
