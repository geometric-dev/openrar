#include "foreign_reader.hpp"

#include "../io/file_stream.hpp"
#include "../io/path_util.hpp"

#include <algorithm>
#include <cstring>

namespace openrar::archive::foreign {

// ---------------------------------------------------------------------------
// Dispatch (plan D8 — ONE function, documented precedence)
// ---------------------------------------------------------------------------

namespace {

constexpr core::uint32 kZipLfhSig = 0x04034b50;  // PK\x03\x04
constexpr core::uint32 kZipEocdSig = 0x06054b50; // PK\x05\x06

core::uint32 le32(const core::byte* p) {
    return static_cast<core::uint32>(p[0]) | (static_cast<core::uint32>(p[1]) << 8) |
           (static_cast<core::uint32>(p[2]) << 16) | (static_cast<core::uint32>(p[3]) << 24);
}

bool read_exact(io::FileStream& f, core::uint64 offset, void* dst, size_t n) {
    if (!f.seek(static_cast<int64_t>(offset), io::SeekOrigin::Begin)) return false;
    return f.read(dst, n) == n;
}

// TAR probe: the 512-byte header checksum per spec 11 §4.1 (tried LAST —
// TAR has no magic; v7 archives lack even the ustar field).
bool probe_tar(io::FileStream& f) {
    core::byte hdr[512];
    if (f.size() < 512 || !read_exact(f, 0, hdr, 512)) return false;
    core::uint32 sum = 0;
    for (size_t i = 0; i < 512; ++i) {
        sum += (i >= 148 && i < 156) ? 0x20 : static_cast<core::uint32>(hdr[i]);
    }
    // chksum field: octal ASCII, possibly space/NUL padded.
    core::uint32 stored = 0;
    bool any_digit = false;
    for (size_t i = 148; i < 156; ++i) {
        const char c = static_cast<char>(hdr[i]);
        if (c == ' ' || c == '\0') {
            if (any_digit) break;
            continue;
        }
        if (c < '0' || c > '7') return false;
        any_digit = true;
        stored = stored * 8 + static_cast<core::uint32>(c - '0');
    }
    return any_digit && stored == sum;
}

} // namespace

SourceFormat detect_foreign_format(const std::filesystem::path& path) {
    io::FileStream f;
    if (!f.open(path, io::FileMode::ReadOnly)) return SourceFormat::None;
    const core::uint64 fsize = f.size();
    core::byte b8[8] = {0};
    const size_t got = static_cast<size_t>(std::min<core::uint64>(fsize, 8));
    if (got >= 4 && !read_exact(f, 0, b8, got)) return SourceFormat::None;

    // 1. RAR5 signature — not a cv source (usage refusal upstream).
    if (got >= 8 && std::memcmp(b8, "Rar!\x1a\x07\x01\x00", 8) == 0) return SourceFormat::Rar5;
    // 2. Legacy RAR — OUT OF SCOPE (explicit refusal, never "not an archive").
    if (got >= 7 && std::memcmp(b8, "Rar!\x1a\x07\x00", 7) == 0) return SourceFormat::LegacyRar;
    // 3. ZIP: local header signature, or an EOCD-only (empty) archive.
    if (got >= 4 && le32(b8) == kZipLfhSig) return SourceFormat::Zip;
    if (fsize >= 22) {
        core::byte tail[22];
        if (read_exact(f, fsize - 22, tail, 22) && le32(tail) == kZipEocdSig && tail[20] == 0 &&
            tail[21] == 0) {
            return SourceFormat::Zip;
        }
    }
    // 4. GZIP.
    if (got >= 2 && b8[0] == 0x1F && b8[1] == 0x8B) return SourceFormat::Gzip;
    // 5. TAR last (checksum validation; v7-tolerant).
    if (probe_tar(f)) return SourceFormat::Tar;
    // 6. Unrecognized.
    return SourceFormat::None;
}

// ---------------------------------------------------------------------------
// Status strings
// ---------------------------------------------------------------------------

const char* foreign_status_string(ForeignStatus s) {
    switch (s) {
    case ForeignStatus::Ok:
        return "ok";
    case ForeignStatus::Unparseable:
        return "unparseable archive";
    case ForeignStatus::StructuralMismatch:
        return "structural integrity failure";
    case ForeignStatus::Truncated:
        return "truncated member";
    case ForeignStatus::CrcMismatch:
        return "payload checksum mismatch";
    case ForeignStatus::LimitExceeded:
        return "resource limit exceeded";
    case ForeignStatus::Aborted:
        return "aborted";
    case ForeignStatus::IoError:
        return "io error";
    case ForeignStatus::UnsupportedMethod:
        return "unsupported compression method";
    case ForeignStatus::EncryptedRefused:
        return "encrypted entries refused";
    case ForeignStatus::SparseRefused:
        return "sparse member refused";
    case ForeignStatus::NameEmpty:
        return "name sanitizes to empty";
    case ForeignStatus::NotOpened:
        return "archive not opened";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Name composition (plan D14 — the ONE pipeline; spec 11 §6)
// ---------------------------------------------------------------------------

namespace {
// UTF-8 well-formedness (RFC 3629): sequence length at s[i], 0 = invalid.
size_t utf8_sequence_len(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    size_t len = 0;
    core::uint32 cp = 0;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (len == 2 && cp < 0x80) return 0;        // overlong
    if (len == 3 && cp < 0x800) return 0;       // overlong
    if (len == 4 && cp < 0x10000) return 0;     // overlong
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0; // surrogate
    if (cp > 0x10FFFF) return 0;
    return len;
}

void append_percent_encoded_byte(std::string& out, unsigned char b) {
    static const char* kHex = "0123456789ABCDEF";
    out.push_back('%');
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xF]);
}
} // namespace

std::string cp437_to_utf8(const std::string& raw) {
    // CP437 → Unicode (total map; in-tree table — CONTRIBUTING ground rule 2).
    static const core::uint16 kMap[128] = {
        0xC7,   0xFC,   0xE9,   0xE2,   0xE4,   0xE0,   0xE5,   0xE7,   0xEA,   0xEB,   0xE8,
        0xEF,   0xEE,   0xEC,   0xC4,   0xC5,   0xC9,   0xE6,   0xC6,   0xF4,   0xF6,   0xF2,
        0xFB,   0xF9,   0xFF,   0xD6,   0xDC,   0xA2,   0xA3,   0xA5,   0x20A7, 0x192,  0xE1,
        0xED,   0xF3,   0xFA,   0xF1,   0xD1,   0xAA,   0xBA,   0xBF,   0x2310, 0xAC,   0xBD,
        0xBC,   0xA1,   0xAB,   0xBB,   0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562,
        0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534,
        0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560,
        0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
        0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x3B1,  0xDF,   0x393,
        0x3C0,  0x3A3,  0x3C3,  0xB5,   0x3C4,  0x3A6,  0x398,  0x3A9,  0x3B4,  0x221E, 0x3C6,
        0x3B5,  0x2229, 0x2261, 0xB1,   0x2265, 0x2264, 0x2320, 0x2321, 0xF7,   0x2248, 0xB0,
        0x2219, 0xB7,   0x221A, 0x207F, 0xB2,   0x25A0, 0xA0};
    std::string out;
    out.reserve(raw.size());
    char buf[4];
    for (unsigned char c : raw) {
        const core::uint32 cp = c < 0x80 ? c : kMap[c - 0x80];
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            buf[0] = static_cast<char>(0xC0 | (cp >> 6));
            buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
            out.append(buf, 2);
        } else {
            buf[0] = static_cast<char>(0xE0 | (cp >> 12));
            buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
            out.append(buf, 3);
        }
    }
    return out;
}

std::string translate_foreign_name(const std::string& raw, ForeignNameEncoding enc, bool& escaped) {
    escaped = false;
    // 1. Decode per source rule.
    std::string decoded;
    if (enc == ForeignNameEncoding::Cp437) {
        decoded = cp437_to_utf8(raw);
    } else if (enc == ForeignNameEncoding::Utf8) {
        decoded.reserve(raw.size());
        for (size_t i = 0; i < raw.size();) {
            const size_t len = utf8_sequence_len(raw, i);
            if (len == 0) {
                append_percent_encoded_byte(decoded, static_cast<unsigned char>(raw[i]));
                escaped = true;
                ++i;
            } else {
                decoded.append(raw, i, len);
                i += len;
            }
        }
    } else {
        decoded = raw;
    }

    // 2. '\' → '/' (spec 11 §2.4/§6 — documented lossy rule: APPNOTE defines
    // '/' as the separator; a literal backslash in a stored Unix name is
    // indistinguishable from a weaponized one).
    for (char& c : decoded) {
        if (c == '\\') c = '/';
    }

    // 3+4. Percent-encode remaining invalid sequences, then the shipped
    // sanitizer resolves '..', strips drives/leading slashes, hardens
    // components (§4.1/§4.4).
    std::string validated;
    validated.reserve(decoded.size());
    for (size_t i = 0; i < decoded.size();) {
        const size_t len = utf8_sequence_len(decoded, i);
        if (len == 0) {
            append_percent_encoded_byte(validated, static_cast<unsigned char>(decoded[i]));
            escaped = true;
            ++i;
        } else {
            validated.append(decoded, i, len);
            i += len;
        }
    }
    return io::sanitize_archive_path(validated);
}

} // namespace openrar::archive::foreign
