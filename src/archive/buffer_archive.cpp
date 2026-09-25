#include "buffer_archive.hpp"
#include "collision_detector.hpp"
#include "rar_errors.hpp"

#include <ctime>
#include "../compress/compressor50.hpp"
#include "../compress/decompressor50.hpp"
#include "../core/vint.hpp"
#include "../crypto/crc32.hpp"
#include "../crypto/blake2sp.hpp"
#include "../format/header_reader.hpp"
#include "../format/header_writer.hpp"
#include "../format/headers.hpp"
#include "../io/file_stream.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace openrar::archive {

namespace {

using openrar::core::is_valid_utf8;

// ── Path segment scanner ─────────────────────────────────────────────────────
// Returns true if any segment equals "..". We don't reject '.' alone.
bool has_dotdot_segment(const std::string& path) {
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        size_t len = end - start;
        if (len == 2 && path[start] == '.' && path[start + 1] == '.') return true;
        if (end == path.size()) break;
        start = end + 1;
    }
    return false;
}

// ── DOS time → UNIX seconds (RAR5 format) ─────────────────────────────────
// On-disk format (when FHFL_UTIME is set) is a 32-bit unsigned value that is
// *either* UNIX seconds OR DOS time depending on context. Per the spec, the
// in-memory mtime exposed in the JS layer is UNIX seconds:
//   DOS time = (year-1980)<<25 | month<<21 | day<<16 | hour<<11 | min<<5 | sec/2
//   0 if mtime unset. Round-trip is lossy at 2-second granularity.

// Inverse of dos_time_to_unix (UTC-based, matching the timegm/_mkgmtime
// read path). Loses sub-2-second precision; years clamp to the DOS range
// [1980, 2107]. 0 maps to 0 (no FHFL_UTIME field).
uint32_t unix_to_dos_time(uint64_t unix_sec) {
    if (unix_sec == 0) return 0;
    std::time_t t = static_cast<std::time_t>(unix_sec);
    std::tm tm_buf{};
#if defined(_WIN32)
    if (gmtime_s(&tm_buf, &t) != 0) return 0;
#else
    if (gmtime_r(&t, &tm_buf) == nullptr) return 0;
#endif
    long year = static_cast<long>(tm_buf.tm_year) + 1900;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;
    unsigned y = static_cast<unsigned>(year - 1980);
    unsigned mo = static_cast<unsigned>(tm_buf.tm_mon) + 1;
    unsigned d = static_cast<unsigned>(tm_buf.tm_mday);
    unsigned h = static_cast<unsigned>(tm_buf.tm_hour);
    unsigned mi = static_cast<unsigned>(tm_buf.tm_min);
    unsigned s = static_cast<unsigned>(tm_buf.tm_sec) / 2;
    return (y << 25) | (mo << 21) | (d << 16) | (h << 11) | (mi << 5) | s;
}


// ── Window size from log2 ∈ {1, 2, 3, 4} (doubling from 128 KiB) ────────────
size_t window_size_from_log2(unsigned window_log2) {
    switch (window_log2) {
    case 1:
        return 128ULL * 1024;
    case 2:
        return 256ULL * 1024;
    case 3:
        return 512ULL * 1024;
    case 4:
        return 1ULL * 1024 * 1024;
    default:
        return 0; // invalid
    }
}

// ── Locate RAR5 signature within buffer (with SFX scan up to 4 MiB) ─────────
// Returns the byte offset of the 8-byte signature, or size_t(-1) if not found.
size_t find_rar5_signature(const uint8_t* data, size_t size) {
    if (size < 8) return static_cast<size_t>(-1);
    if (std::memcmp(data, format::rar5_signature(), 8) == 0) return 0;

    constexpr size_t SFX_SCAN = 4ULL * 1024 * 1024;
    size_t limit = std::min(size, SFX_SCAN);
    for (size_t i = 1; i + 8 <= limit; ++i) {
        if (std::memcmp(data + i, format::rar5_signature(), 8) == 0) {
            // Verify that a header follows: read first block at i+8.
            size_t off = i + 8;
            core::uint64 type = 0, flags = 0, data_size = 0;
            std::vector<core::byte> body;
            if (format::HeaderReader::read_block_raw_mem(data, size, off, type, flags, body,
                                                         data_size) == format::HeaderResult::Ok) {
                if (type == format::HEAD_MAIN || type == format::HEAD_CRYPT) {
                    return i;
                }
            }
        }
    }
    return static_cast<size_t>(-1);
}

// ── Build the body of a HEAD_FILE block (mirror HeaderWriter internals) ──────
// Returns the wrapped block bytes (CRC + size_vint + body) ready to append.
std::vector<core::byte> build_file_block_bytes(const format::FileBlock& fb, bool include_data) {
    std::vector<core::byte> body;
    core::push_vint(body, fb.is_service ? format::HEAD_SERVICE : format::HEAD_FILE);

    core::uint64 head_flags = 0;
    if (include_data && fb.pack_size >= 0) head_flags |= format::HFL_DATA;
    core::push_vint(body, head_flags);

    if (head_flags & format::HFL_DATA) {
        core::push_vint(body, static_cast<core::uint64>(fb.pack_size));
    }

    core::uint64 eff_file_flags = fb.file_flags;
    if (fb.has_crc32) eff_file_flags |= format::FHFL_CRC32;
    if (fb.utime_unix != 0) eff_file_flags |= format::FHFL_UTIME;
    core::push_vint(body, eff_file_flags);

    core::push_vint(body, fb.unp_size);
    core::push_vint(body, fb.attributes);

    if (eff_file_flags & format::FHFL_UTIME) {
        core::byte buf[4];
        // On-disk mtime is DOS packed time; the reader decodes it back to
        // UNIX seconds. (Writing the raw unix value here would be decoded
        // as garbage DOS fields.)
        core::write_le32(buf, unix_to_dos_time(fb.utime_unix));
        body.insert(body.end(), buf, buf + 4);
    }
    if (eff_file_flags & format::FHFL_CRC32) {
        core::byte buf[4];
        core::write_le32(buf, fb.data_crc32);
        body.insert(body.end(), buf, buf + 4);
    }

    // Compression info: bits 0..5 = unp_ver, bit 6 = solid, bits 7..9 = method,
    // bits 10..14 = dict base power.
    core::uint32 comp_info =
        (fb.unp_ver & 0x3F) | (fb.is_solid ? 0x40 : 0) | ((fb.method & 7) << 7);
    if (fb.win_size >= 0x20000 && fb.method != 0) {
        core::uint64 pow2 = 0x20000;
        core::uint32 dict_bits = 0;
        while (2 * pow2 <= fb.win_size && dict_bits < 15u) {
            pow2 *= 2;
            dict_bits++;
        }
        comp_info |= (dict_bits << 10);
    }
    core::push_vint(body, comp_info);

    core::push_vint(body, fb.host_os);
    core::push_vint(body, fb.file_name.size());
    body.insert(body.end(), reinterpret_cast<const core::byte*>(fb.file_name.data()),
                reinterpret_cast<const core::byte*>(fb.file_name.data()) + fb.file_name.size());

    return format::HeaderWriter::wrap_block(body);
}

std::vector<core::byte> build_main_block_bytes() {
    std::vector<core::byte> body;
    core::push_vint(body, format::HEAD_MAIN);
    core::push_vint(body, 0); // common flags: no extra, no data
    core::push_vint(body, 0); // arc_flags: no volume, no solid, no protect, no lock
    return format::HeaderWriter::wrap_block(body);
}

std::vector<core::byte> build_end_block_bytes() {
    std::vector<core::byte> body;
    core::push_vint(body, format::HEAD_ENDARC);
    core::push_vint(body, 0); // common flags
    core::push_vint(body, 0); // end_flags
    return format::HeaderWriter::wrap_block(body);
}

// ── Append a vector to out ───────────────────────────────────────────────────
void append(std::vector<uint8_t>& out, const std::vector<core::byte>& src) {
    out.insert(out.end(), src.begin(), src.end());
}

void append(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
    out.insert(out.end(), p, p + n);
}

// ── progress emit helper ─────────────────────────────────────────────────────
struct ProgressTracker {
    void (*cb)(uint64_t, uint64_t, void*);
    void* user;
    uint64_t done{0};
    uint64_t total{0};

    void set(uint64_t d, uint64_t t) {
        if (d > t) d = t;
        if (d < done) d = done; // enforce monotonic
        done = d;
        total = t;
        if (cb) cb(done, total, user);
    }
};

// ── Shared header-walk state machine ─────────────────────────────────────────
// The per-block classification is shared by BufferArchive::list (memory
// source) and list_file_stream (stream source) so the two listing paths can
// never drift apart. Boundary layers map the internal status to public error
// codes; they deliberately differ on CryptHeader (see list_file_stream).
enum class WalkStatus {
    Ok,               // reached ENDARC or EOF cleanly
    Aborted,          // cancel callback returned non-zero
    Truncated,        // malformed / short header or data area
    CryptHeader,      // HEAD_CRYPT reached and no password was supplied
    EncryptedFile,    // file/service header with the encrypted-data flag
    SolidHeader,      // file/service header with the solid flag
    BadPassword,      // supplied password fails to decrypt the headers
    CryptUnsupported, // HEAD_CRYPT with an unknown crypto version
};

// Hook plumbing for the walk. Core callback convention: progress(done, total,
// user) cumulative and monotonic; cancel(user) polled between header blocks,
// non-zero aborts. Never invoked under a lock (the walk holds none).
struct WalkHooks {
    progress_cb on_progress{nullptr};
    void* progress_user{nullptr};
    cancel_cb on_cancel{nullptr};
    void* cancel_user{nullptr};

    bool cancelled() const { return on_cancel && on_cancel(cancel_user) != 0; }
    void emit(uint64_t done, uint64_t total) const {
        if (on_progress) on_progress(done, total, progress_user);
    }
};

// Source adapters for walk_headers(): tell() is the absolute byte offset of
// the next header, limit() the end of the byte range, read_block() consumes
// one raw header at the current position, skip() jumps a data area with the
// same past-the-end guard the memory walk has always applied.
struct MemSource {
    const uint8_t* data;
    size_t size;
    size_t pos;
    uint64_t tell() const { return static_cast<uint64_t>(pos); }
    uint64_t limit() const { return static_cast<uint64_t>(size); }
    bool bad_password() const { return false; }
    format::HeaderResult read_block(core::uint64& type, core::uint64& flags,
                                    std::vector<core::byte>& body, core::uint64& data_size) {
        return format::HeaderReader::read_block_raw_mem(data, size, pos, type, flags, body,
                                                        data_size);
    }
    bool skip(core::uint64 data_size, core::uint64 head_end) {
        if (head_end > limit() || data_size > limit() - head_end) return false;
        pos = static_cast<size_t>(head_end + data_size);
        return true;
    }
};

struct StreamSource {
    io::FileStream& stream;
    core::uint64 size;
    format::HeaderCryptReader* crypt; // may be null; activated by walk_headers on HEAD_CRYPT
    core::uint64 tell() const { return stream.tell(); }
    core::uint64 limit() const { return size; }
    bool bad_password() const { return crypt != nullptr && crypt->bad_password; }
    format::HeaderResult read_block(core::uint64& type, core::uint64& flags,
                                    std::vector<core::byte>& body, core::uint64& data_size) {
        return format::HeaderReader::read_block_raw(stream, type, flags, body, data_size, crypt);
    }
    bool skip(core::uint64 data_size, core::uint64 head_end) {
        if (head_end > size || data_size > size - head_end) return false;
        if (data_size == 0) return true;
        return stream.seek(static_cast<core::int64>(head_end + data_size), io::SeekOrigin::Begin);
    }
};

// `password` (null = none) enables header decryption via `crypt` — only the
// streaming source supports it; the memory walk never receives one.
// `emit_encrypted`: false keeps the frozen early-return at the first
// encrypted-data header; true reports such entries (is_encrypted = 1) and
// continues. Solid entries stay a hard stop in both modes: the 64-byte
// entry layout cannot express solidness, so reporting them would silently
// promise extraction that cannot work.
template <typename Source>
WalkStatus walk_headers(Source& src, const WalkHooks& hooks,
                        std::vector<BufferArchiveEntry>& out_entries, bool& seen_volume,
                        bool& seen_protect, const char* password, bool emit_encrypted,
                        format::HeaderCryptReader* crypt) {
    bool saw_end = false;
    while (src.tell() < src.limit() && !saw_end) {
        if (hooks.cancelled()) return WalkStatus::Aborted;
        hooks.emit(src.tell(), src.limit());

        const uint64_t head_start = src.tell();
        core::uint64 type = 0, flags = 0, data_size = 0;
        std::vector<core::byte> body;
        if (src.read_block(type, flags, body, data_size) != format::HeaderResult::Ok) {
            return src.bad_password() ? WalkStatus::BadPassword : WalkStatus::Truncated;
        }
        const uint64_t head_end = src.tell();

        if (type == format::HEAD_MAIN) {
            format::MainBlock mb;
            if (!format::HeaderReader::parse_main_header(body.data(), body.size(), mb)) {
                return WalkStatus::Truncated;
            }
            seen_volume = (mb.arc_flags & format::MHFL_VOLUME) != 0;
            seen_protect = (mb.arc_flags & format::MHFL_PROTECT) != 0;
        } else if (type == format::HEAD_FILE || type == format::HEAD_SERVICE) {
            format::FileBlock fb;
            if (!format::HeaderReader::parse_file_header(body.data(), body.size(), fb)) {
                return WalkStatus::Truncated;
            }

            if (fb.is_solid) {
                if (!src.skip(data_size, head_end)) return WalkStatus::Truncated;
                return WalkStatus::SolidHeader;
            }
            if (fb.is_encrypted && !emit_encrypted) {
                // Frozen mode: stop here, past the encrypted payload.
                if (!src.skip(data_size, head_end)) return WalkStatus::Truncated;
                return WalkStatus::EncryptedFile;
            }

            BufferArchiveEntry e;
            e.path = fb.file_name;
            e.is_dir = (fb.file_flags & format::FHFL_DIRECTORY) != 0;
            e.size = fb.unp_size;
            e.packed_size = fb.pack_size < 0 ? 0 : static_cast<uint64_t>(fb.pack_size);
            e.method = static_cast<int>(fb.method);
            e.win_size = fb.win_size;
            e.is_encrypted = fb.is_encrypted;
            e.header_offset = head_start;
            e.data_offset = head_end;
            e.data_size = e.packed_size;
            e.has_crc32 = fb.has_crc32;
            e.crc32 = fb.has_crc32 ? fb.data_crc32 : 0;
            e.has_blake2sp = fb.has_blake2sp;
            if (fb.has_blake2sp) {
                std::memcpy(e.blake2sp.data(), fb.blake2sp.data(), 32);
            }
            e.mtime = dos_time_to_unix(fb.utime_unix);
            out_entries.push_back(std::move(e));

            if (!src.skip(data_size, head_end)) return WalkStatus::Truncated;
        } else if (type == format::HEAD_ENDARC) {
            format::EndArcBlock eb;
            if (!format::HeaderReader::parse_end_header(body.data(), body.size(), eb)) {
                return WalkStatus::Truncated;
            }
            if ((eb.end_flags & 0x0001) == 0) {
                saw_end = true;
            }
        } else if (type == format::HEAD_CRYPT) {
            if (crypt != nullptr && crypt->active) {
                // Duplicate HEAD_CRYPT: corrupt (mirrors ArchiveReader's scan).
                return WalkStatus::Truncated;
            }
            if (password == nullptr || crypt == nullptr) {
                // No password: headers after this block are unreadable.
                return WalkStatus::CryptHeader;
            }
            format::CryptBlock cb;
            if (!format::HeaderReader::parse_crypt_header(body.data(), body.size(), cb)) {
                return WalkStatus::Truncated;
            }
            // HEAD_CRYPT itself is clear; init activates decryption for
            // every following header through src.read_block.
            if (!crypt->init(password, cb)) {
                return crypt->bad_password ? WalkStatus::BadPassword : WalkStatus::CryptUnsupported;
            }
        } else {
            // Unknown block: skip its data area.
            if (!src.skip(data_size, head_end)) return WalkStatus::Truncated;
        }
    }
    return WalkStatus::Ok;
}

} // namespace

uint64_t dos_time_to_unix(uint32_t dos) {
    if (dos == 0) return 0;
    unsigned y = (dos >> 25) & 0x7F;
    unsigned mo = (dos >> 21) & 0x0F;
    unsigned d = (dos >> 16) & 0x1F;
    unsigned h = (dos >> 11) & 0x1F;
    unsigned mi = (dos >> 5) & 0x3F;
    unsigned s = (dos) & 0x1F;
    if (mo == 0 || mo > 12) mo = 1;
    if (d == 0 || d > 31) d = 1;
    std::tm tm_buf{};
    tm_buf.tm_year = static_cast<int>(y) + 1980 - 1900;
    tm_buf.tm_mon = static_cast<int>(mo) - 1;
    tm_buf.tm_mday = static_cast<int>(d);
    tm_buf.tm_hour = static_cast<int>(h);
    tm_buf.tm_min = static_cast<int>(mi);
    tm_buf.tm_sec = static_cast<int>(s) * 2;
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm_buf);
#else
    std::time_t t = timegm(&tm_buf);
#endif
    if (t < 0) return 0;
    return static_cast<uint64_t>(t);
}


// ─────────────────────────────────────────────────────────────────────────────
// validate_archive_path
// ─────────────────────────────────────────────────────────────────────────────
bool validate_archive_path(const std::string& path, bool is_dir, std::string& err_out) {
    if (path.empty()) {
        err_out = "path is empty";
        return false;
    }
    if (path.size() > 2048) {
        err_out = "path exceeds 2048 bytes";
        return false;
    }
    if (!is_valid_utf8(path)) {
        err_out = "path is not valid UTF-8";
        return false;
    }
    if (path[0] == '/') {
        // Also covers "//" UNC-style prefixes: any leading '/' is rejected
        // here, so no separate second-byte check is needed.
        err_out = "path has leading '/'";
        return false;
    }
    for (unsigned char c : path) {
        if (c == 0x00) {
            err_out = "path contains NUL byte";
            return false;
        }
        if (c < 0x20) {
            err_out = "path contains control character";
            return false;
        }
        if (c == '\\') {
            err_out = "path contains backslash";
            return false;
        }
    }
    if (has_dotdot_segment(path)) {
        err_out = "path contains '..' segment";
        return false;
    }
    bool ends_slash = path.back() == '/';
    if (is_dir && !ends_slash) {
        err_out = "directory entry must have trailing '/'";
        return false;
    }
    if (!is_dir && ends_slash) {
        err_out = "file entry must not have trailing '/'";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BufferArchive::list
// ─────────────────────────────────────────────────────────────────────────────
int BufferArchive::list(const uint8_t* data, size_t size,
                        std::vector<BufferArchiveEntry>& out_entries, progress_cb on_progress,
                        void* progress_user, cancel_cb on_cancel, void* cancel_user) {
    out_entries.clear();
    cached_entries_.clear();

    if (data == nullptr || size == 0) return RAR_ERR_TRUNCATED;

    size_t sig_off = find_rar5_signature(data, size);
    if (sig_off == static_cast<size_t>(-1)) {
        return RAR_ERR_NOT_RAR;
    }

    WalkHooks hooks{on_progress, progress_user, on_cancel, cancel_user};
    hooks.emit(static_cast<uint64_t>(sig_off), static_cast<uint64_t>(size));

    MemSource src{data, size, sig_off + 8};
    bool seen_volume = false;
    bool seen_protect = false;
    WalkStatus st = walk_headers(src, hooks, out_entries, seen_volume, seen_protect,
                                 /*password=*/nullptr, /*emit_encrypted=*/false, /*crypt=*/nullptr);
    switch (st) {
    case WalkStatus::Aborted:
        out_entries.clear();
        return RAR_ERR_ABORTED;
    case WalkStatus::BadPassword:
        out_entries.clear();
        return RAR_ERR_BAD_PASSWORD;
    case WalkStatus::CryptHeader:
    case WalkStatus::CryptUnsupported:
    case WalkStatus::EncryptedFile:
    case WalkStatus::SolidHeader:
        out_entries.clear();
        return RAR_ERR_UNSUPPORTED_FEATURE;
    case WalkStatus::Truncated:
        out_entries.clear();
        return RAR_ERR_TRUNCATED;
    case WalkStatus::Ok:
        break;
    }
    if (seen_volume || seen_protect) {
        out_entries.clear();
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    // Main-header presence is informational; we don't require it (permissive parse).

    hooks.emit(static_cast<uint64_t>(size), static_cast<uint64_t>(size));
    cached_entries_ = out_entries;
    cached_signature_offset_ = sig_off;
    cached_buffer_size_ = size;
    cached_buffer_ = data;
    return RAR_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// list_file_stream
// ─────────────────────────────────────────────────────────────────────────────
int list_file_stream(const std::filesystem::path& arc_path,
                     std::vector<BufferArchiveEntry>& out_entries, const char* password,
                     bool emit_encrypted_entries, progress_cb on_progress, void* progress_user,
                     cancel_cb on_cancel, void* cancel_user) {
    out_entries.clear();

    // Empty password = none (RAR5 never uses the empty password for -hp).
    const char* pw = (password != nullptr && *password != '\0') ? password : nullptr;

    io::FileStream stream;
    if (!stream.open(arc_path, io::FileMode::ReadOnly)) return RAR_ERR_IO;
    const core::uint64 file_size = stream.size();
    if (file_size == 0) return RAR_ERR_TRUNCATED;
    if (file_size < 8) return RAR_ERR_NOT_RAR;

    WalkHooks hooks{on_progress, progress_user, on_cancel, cancel_user};

    // ── Locate the RAR5 signature (SFX-aware, streaming) ─────────────────────
    // Mirrors find_rar5_signature(): offset 0 first, then a scan of the first
    // 4 MiB where each candidate must be followed by a parseable MAIN/CRYPT
    // header. Chunked with cancel/progress hooks so aborting a listing on
    // slow storage does not require a whole-file slurp first. Short reads end
    // the scan as "not found" (parity with ArchiveReader's SFX scan).
    core::uint64 sig_off = 0;
    core::byte sig_buf[8];
    if (stream.read(sig_buf, 8) != 8) return RAR_ERR_TRUNCATED;
    if (std::memcmp(sig_buf, format::rar5_signature(), 8) != 0) {
        constexpr core::uint64 SFX_SCAN = 4ULL * 1024 * 1024;
        const core::uint64 scan_limit = std::min(file_size, SFX_SCAN);
        std::vector<core::byte> scan_buf(65536);
        bool found = false;
        core::uint64 pos = 0;
        while (pos < scan_limit) {
            if (hooks.cancelled()) return RAR_ERR_ABORTED;
            hooks.emit(pos, file_size);
            stream.seek(static_cast<core::int64>(pos), io::SeekOrigin::Begin);
            size_t bytes_to_read =
                static_cast<size_t>(std::min<core::uint64>(scan_buf.size(), scan_limit - pos));
            size_t got = stream.read(scan_buf.data(), bytes_to_read);
            if (got < 8) break;
            for (size_t i = 0; i + 8 <= got; ++i) {
                if (std::memcmp(scan_buf.data() + i, format::rar5_signature(), 8) != 0) continue;
                stream.seek(static_cast<core::int64>(pos + i + 8), io::SeekOrigin::Begin);
                core::uint64 type = 0, flags = 0, data_size = 0;
                std::vector<core::byte> body;
                if (format::HeaderReader::read_block_raw(stream, type, flags, body, data_size) ==
                        format::HeaderResult::Ok &&
                    (type == format::HEAD_MAIN || type == format::HEAD_CRYPT)) {
                    sig_off = pos + i;
                    found = true;
                    break;
                }
            }
            if (found) break;
            pos += (got > 8) ? (got - 7) : 1;
        }
        if (!found) return RAR_ERR_NOT_RAR;
    }

    hooks.emit(sig_off, file_size);
    stream.seek(static_cast<core::int64>(sig_off + 8), io::SeekOrigin::Begin);

    format::HeaderCryptReader hcrypt;
    StreamSource src{stream, file_size, &hcrypt};
    bool seen_volume = false;
    bool seen_protect = false;
    WalkStatus st = walk_headers(src, hooks, out_entries, seen_volume, seen_protect, pw,
                                 emit_encrypted_entries, &hcrypt);
    switch (st) {
    case WalkStatus::Aborted:
        out_entries.clear();
        return RAR_ERR_ABORTED;
    case WalkStatus::CryptHeader:
        // Early password signal: headers are unreadable without a password,
        // so a longer walk could never succeed.
        out_entries.clear();
        return RAR_ERR_ENCRYPTED;
    case WalkStatus::BadPassword:
        out_entries.clear();
        return RAR_ERR_BAD_PASSWORD;
    case WalkStatus::CryptUnsupported:
    case WalkStatus::EncryptedFile:
    case WalkStatus::SolidHeader:
        out_entries.clear();
        return RAR_ERR_UNSUPPORTED_FEATURE;
    case WalkStatus::Truncated:
        out_entries.clear();
        return RAR_ERR_TRUNCATED;
    case WalkStatus::Ok:
        break;
    }
    if (seen_volume || seen_protect) {
        out_entries.clear();
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }

    hooks.emit(file_size, file_size);
    return RAR_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// BufferArchive::extract
// ─────────────────────────────────────────────────────────────────────────────
int BufferArchive::extract(const uint8_t* data, size_t size, size_t entry_index,
                           std::vector<uint8_t>& out, const ExtractionLimits* limits,
                           LimitState* state) {
    out.clear();

    if (data == nullptr || size == 0) return RAR_ERR_TRUNCATED;
    if (entry_index >= cached_entries_.size()) return RAR_ERR_INVALID_ARG;

    // Re-validate that the cached buffer is the same one being asked about.
    // INTENDED (pointer-identity cache guard): the cache is keyed on the
    // caller's pointer, which assumes the buffer outlives the BufferArchive
    // and is left unchanged for its lifetime — the documented WASM/DLL host
    // contract. Deliberately no ownership and no copying; do not replace
    // the pointer comparison with a content hash.
    if (data != cached_buffer_ || size != cached_buffer_size_) {
        return RAR_ERR_INVALID_ARG;
    }

    const BufferArchiveEntry& e = cached_entries_[entry_index];

    LimitState local_state;
    if (!state) state = &local_state;
    state->member_out = 0;

    if (limits && e.size > 0) {
        if (limits->member_limited() && e.size > limits->max_member_output_bytes) {
            return RAR_ERR_LIMIT_EXCEEDED;
        }
        if (limits->total_limited() && state->total_out + e.size > limits->max_total_output_bytes) {
            return RAR_ERR_LIMIT_EXCEEDED;
        }
    }

    if (e.is_dir) {
        // Directories have no payload; nothing to extract.
        return RAR_OK;
    }
    if (e.is_encrypted) {
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }
    if (e.data_size == 0) {
        // Empty file.
        return RAR_OK;
    }
    if (e.data_offset > size || e.data_size > size - e.data_offset) {
        return RAR_ERR_TRUNCATED;
    }

    const uint8_t* payload = data + e.data_offset;

    if (e.method == 0) {
        out.assign(payload, payload + e.data_size);
    } else if (e.method >= 1 && e.method <= 5) {
        // Use the window written into the file header. If 0 (which shouldn't happen
        // for valid RAR5 files but might in edge cases), default to 32 MiB.
        size_t win = e.win_size > 0 ? static_cast<size_t>(e.win_size) : 32 * 1024 * 1024;
        compress::Decompressor50 unpacker(win);
        if (!unpacker.decompress_to_vector(payload, static_cast<size_t>(e.data_size), out)) {
            if (unpacker.last_error() == compress::DecompressErrorCode::AllocationFailed) {
                return RAR_ERR_NOMEM;
            }
            if (unpacker.last_error() == compress::DecompressErrorCode::DictionaryTooLarge) {
                return RAR_ERR_LIMIT_EXCEEDED;
            }
            return RAR_ERR_TRUNCATED;
        }
        if (out.size() != e.size) {
            return RAR_ERR_TRUNCATED;
        }
    } else {
        return RAR_ERR_UNSUPPORTED_FEATURE;
    }

    if (limits) {
        if (limits->member_limited() && out.size() > limits->max_member_output_bytes) {
            return RAR_ERR_LIMIT_EXCEEDED;
        }
        if (limits->total_limited() &&
            state->total_out + out.size() > limits->max_total_output_bytes) {
            return RAR_ERR_LIMIT_EXCEEDED;
        }
        state->member_out = out.size();
        state->total_out += out.size();
    }

    if (e.has_crc32) {
        crypto::Crc32 crc;
        crc.update(out.data(), out.size());
        if (crc.get() != e.crc32) return RAR_ERR_CRC_MISMATCH;
    }
    if (e.has_blake2sp) {
        crypto::Blake2sp b2;
        b2.update(out.data(), out.size());
        std::array<uint8_t, 32> digest{};
        b2.finish(digest.data());
        if (std::memcmp(digest.data(), e.blake2sp.data(), 32) != 0) {
            return RAR_ERR_CRC_MISMATCH;
        }
    }
    return RAR_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// BufferArchive::extract_all
// ─────────────────────────────────────────────────────────────────────────────
int BufferArchive::extract_all(const uint8_t* data, size_t size,
                               std::vector<std::pair<std::string, std::vector<uint8_t>>>& out_files,
                               progress_cb on_progress, void* user, cancel_cb on_cancel,
                               void* cancel_user, const ExtractionLimits* limits,
                               LimitState* state) {
    out_files.clear();

    std::vector<BufferArchiveEntry> entries;
    int rc = list(data, size, entries);
    if (rc != RAR_OK) return rc;

    // v1.24 M3 (plan §3.4): the detector runs identically on the in-memory
    // surface — archive-internal collisions are structural failures and
    // nothing is extracted.
    {
        std::vector<CollisionEntry> names;
        names.reserve(entries.size());
        for (const auto& e : entries) names.push_back(CollisionEntry{e.path, e.is_dir});
        std::vector<CollisionPair> collisions;
        if (CollisionDetector::detect(names, collisions)) {
            // Bad-archive family (structural integrity failure, plan §3); the
            // internal error enum has no dedicated value and the ABI contract
            // is frozen, so the collision abort maps to RAR_ERR_NOT_RAR.
            return RAR_ERR_NOT_RAR;
        }
    }

    out_files.reserve(entries.size());

    LimitState local_state;
    if (!state) state = &local_state;

    uint64_t total = 0;
    for (const auto& e : entries) {
        if (e.is_dir) continue;
        total += e.size;
    }

    ProgressTracker prog;
    prog.cb = on_progress;
    prog.user = user;
    // Emit (0, total) up front so consumers can size the bar.
    prog.set(0, total);
    // The list phase itself is accounted as ~50% of the bar.
    uint64_t list_phase = total / 2;
    prog.set(list_phase, total);

    // Carry the running total instead of re-walking all previously
    // extracted payloads after every entry (O(n^2) memory-traffic).
    uint64_t cumulative = 0;
    // Aggregate output budget (report M8): per-entry decompression is capped
    // by MAX_STREAM_OUTPUT, but the buffer API holds every entry in memory at
    // once, so a hostile archive with many entries could multiply that into
#if defined(__EMSCRIPTEN__) || defined(_M_IX86) || defined(__i386__)
    constexpr uint64_t MAX_TOTAL_OUTPUT = 2ULL * 1024 * 1024 * 1024; // 2 GiB on 32-bit / WASM
#else
    constexpr uint64_t MAX_TOTAL_OUTPUT = 64ULL * 1024 * 1024 * 1024; // 64 GiB on 64-bit native
#endif

    for (const auto& e : entries) {
        // Cancel is polled once per entry — the coarsest granularity the
        // buffer API can honestly offer (each extract() runs to completion).
        if (on_cancel && on_cancel(cancel_user)) return RAR_ERR_ABORTED;
        std::vector<uint8_t> bytes;
        size_t idx = static_cast<size_t>(&e - entries.data());
        if (e.is_dir) {
            out_files.emplace_back(e.path, std::move(bytes));
            continue;
        }
        if (limits) {
            if (limits->member_limited() && e.size > limits->max_member_output_bytes)
                return RAR_ERR_LIMIT_EXCEEDED;
            if (limits->total_limited() &&
                state->total_out + e.size > limits->max_total_output_bytes)
                return RAR_ERR_LIMIT_EXCEEDED;
        }
        if (cumulative + e.size > MAX_TOTAL_OUTPUT) return RAR_ERR_NOMEM;
        rc = extract(data, size, idx, bytes, limits, state);
        if (rc == RAR_ERR_LIMIT_EXCEEDED) return RAR_ERR_LIMIT_EXCEEDED;
        if (rc != RAR_OK) {
            // Per spec §5.3: per-entry failure is captured; we still surface
            // RAR_ERR_PARTIAL_OK and include the entry with empty bytes.
            // Callers can probe bytes.empty() + path to detect failure.
            out_files.emplace_back(e.path, std::vector<uint8_t>{});
            return RAR_ERR_PARTIAL_OK;
        }
        out_files.emplace_back(e.path, std::move(bytes));
        // Carry the running total instead of re-walking all previously
        // extracted payloads after every entry (O(n^2) memory-traffic).
        cumulative += out_files.back().second.size();
        if (cumulative > MAX_TOTAL_OUTPUT) return RAR_ERR_NOMEM;
        uint64_t payload_phase = total - list_phase;
        uint64_t done =
            list_phase + (total > 0 ? (cumulative * payload_phase / total) : payload_phase);
        if (done > total) done = total;
        // Reserve final (total,total) emit for end.
        if (done == total && idx + 1 < entries.size()) {
            done = total > 0 ? total - 1 : 0;
        }
        if (done < total) prog.set(done, total);
    }

    // Final emit: exactly (total, total), once.
    prog.set(total, total);
    return RAR_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// create_archive
// ─────────────────────────────────────────────────────────────────────────────
int create_archive_impl(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
                        const std::vector<uint64_t>* mtimes, std::vector<uint8_t>& out, int method,
                        unsigned window_log2, progress_cb on_progress, void* user,
                        cancel_cb on_cancel, void* cancel_user) {
    out.clear();

    // ── Validate inputs ─────────────────────────────────────────────────────
    if (method != 0 && method != 3 && method != 5) return RAR_ERR_INVALID_ARG;
    if (window_log2 < 1 || window_log2 > 4) return RAR_ERR_INVALID_ARG;

    size_t win_size = window_size_from_log2(window_log2);

    // ── Validate paths and compute totals ───────────────────────────────────
    uint64_t total_input_bytes = 0;
    for (const auto& f : files) {
        bool is_dir = (!f.first.empty() && f.first.back() == '/');
        std::string err;
        if (!validate_archive_path(f.first, is_dir, err)) {
            return RAR_ERR_INVALID_ARG;
        }
        if (is_dir && !f.second.empty()) {
            return RAR_ERR_INVALID_ARG;
        }
        if (!is_dir) total_input_bytes += f.second.size();
    }

    // ── Reserve and emit signature ───────────────────────────────────────────
    out.reserve(static_cast<size_t>(total_input_bytes + 256));
    append(out, format::rar5_signature(), format::RAR5_SIGNATURE_SIZE);

    ProgressTracker prog;
    prog.cb = on_progress;
    prog.user = user;
    uint64_t total_est = total_input_bytes + 64 + 16 * static_cast<uint64_t>(files.size());
    if (total_est == 0) total_est = 1;
    prog.set(0, total_est);

    // ── Emit HEAD_MAIN ───────────────────────────────────────────────────────
    {
        auto blk = build_main_block_bytes();
        append(out, blk);
    }

    // mtime resolution: explicit value wins; 0 (or absent) keeps the legacy
    // "no utime field" bytes so the pair-based API stays byte-identical.
    // utime_unix is core::uint32 (DOS-era field width); narrow here so the
    // assignment sites below don't warn.
    auto mtime_for = [&](size_t i) -> core::uint32 {
        if (!mtimes || i >= mtimes->size()) return 0;
        return static_cast<core::uint32>((*mtimes)[i]);
    };

    // ── Per-entry emit ──────────────────────────────────────────────────────
    for (const auto& f : files) {
        if (on_cancel && on_cancel(cancel_user)) return RAR_ERR_ABORTED;
        const std::string& path = f.first;
        const std::vector<uint8_t>& data = f.second;
        bool is_dir = path.back() == '/';

        format::FileBlock fb;
        fb.file_name = path;
        fb.attributes = 0x20;
        fb.host_os = 1; // Unix
        fb.unp_ver = 0;
        fb.method = 0;
        fb.win_size = 0;
        fb.is_solid = false;

        if (is_dir) {
            fb.file_flags = format::FHFL_DIRECTORY;
            fb.unp_size = 0;
            fb.pack_size = 0;
            fb.utime_unix = mtime_for(static_cast<size_t>(&f - files.data()));
            fb.has_crc32 = false;
            auto header = build_file_block_bytes(fb, /*include_data=*/false);
            append(out, header);
        } else if (data.empty()) {
            // Empty file: header without HFL_DATA, no payload.
            fb.file_flags = 0;
            fb.unp_size = 0;
            fb.pack_size = 0;
            fb.utime_unix = mtime_for(static_cast<size_t>(&f - files.data()));
            fb.has_crc32 = false;
            fb.data_crc32 = 0;
            auto header = build_file_block_bytes(fb, /*include_data=*/false);
            append(out, header);
        } else {
            fb.file_flags = 0;
            fb.unp_size = data.size();
            fb.utime_unix = mtime_for(static_cast<size_t>(&f - files.data()));
            fb.has_crc32 = true;
            crypto::Crc32 crc;
            crc.update(data.data(), data.size());
            fb.data_crc32 = crc.get();

            // If compression requested, attempt; fall back to store on
            // failure or when compressed >= uncompressed.
            if (method != 0) {
                std::vector<core::byte> compressed;
                if (compress::Compressor50::compress_buffer(data.data(), data.size(), compressed,
                                                            method, win_size) &&
                    !compressed.empty() && compressed.size() < data.size()) {
                    fb.method = static_cast<core::uint32>(method);
                    fb.win_size = win_size;
                    fb.pack_size = static_cast<int64_t>(compressed.size());
                    auto header = build_file_block_bytes(fb, /*include_data=*/true);
                    append(out, header);
                    append(out, compressed.data(), compressed.size());
                } else {
                    fb.method = 0;
                    fb.pack_size = static_cast<int64_t>(data.size());
                    auto header = build_file_block_bytes(fb, /*include_data=*/true);
                    append(out, header);
                    append(out, data.data(), data.size());
                }
            } else {
                fb.method = 0;
                fb.pack_size = static_cast<int64_t>(data.size());
                auto header = build_file_block_bytes(fb, /*include_data=*/true);
                append(out, header);
                append(out, data.data(), data.size());
            }
        }

        prog.set(static_cast<uint64_t>(out.size()), total_est);
    }

    // ── Emit HEAD_ENDARC ─────────────────────────────────────────────────────
    {
        auto blk = build_end_block_bytes();
        append(out, blk);
    }

    prog.set(static_cast<uint64_t>(out.size()), total_est);
    // Final clamp to (total,total): if out exceeded our estimate, snap done to total.
    if (out.size() >= static_cast<size_t>(total_est)) {
        prog.set(total_est, total_est);
    }
    return RAR_OK;
}

// ── Public overloads of create_archive ───────────────────────────────────────
// Pair-based (legacy input shape; mtime stays 0 on disk — byte-identical to
// previous releases) and struct-based (mtime_unix, 0 ⇒ now()).
int create_archive(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
                   std::vector<uint8_t>& out, int method, unsigned window_log2,
                   progress_cb on_progress, void* user, cancel_cb on_cancel, void* cancel_user) {
    return create_archive_impl(files, nullptr, out, method, window_log2, on_progress, user,
                               on_cancel, cancel_user);
}

int create_archive(const std::vector<ArchiveFileInput>& files, std::vector<uint8_t>& out,
                   int method, unsigned window_log2, progress_cb on_progress, void* user,
                   cancel_cb on_cancel, void* cancel_user) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> adapted;
    adapted.reserve(files.size());
    std::vector<uint64_t> mtimes;
    mtimes.reserve(files.size());
    const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    for (const auto& f : files) {
        adapted.emplace_back(f.path, f.data);
        mtimes.push_back(f.mtime_unix != 0 ? f.mtime_unix : now);
    }
    return create_archive_impl(adapted, &mtimes, out, method, window_log2, on_progress, user,
                               on_cancel, cancel_user);
}

} // namespace openrar::archive
