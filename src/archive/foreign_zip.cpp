#include "foreign_zip.hpp"

#include "../compress/inflate.hpp"
#include "../crypto/crc32.hpp"
#include "../io/file_stream.hpp"
#include "../io/path_util.hpp"

#include <cstring>
#include <vector>

namespace openrar::archive::foreign {

namespace {

constexpr core::uint32 kLfhSig = 0x04034b50;    // PK\x03\x04
constexpr core::uint32 kCdhSig = 0x02014b50;    // PK\x01\x02
constexpr core::uint32 kEocdSig = 0x06054b50;   // PK\x05\x06
constexpr core::uint32 kEocd64Sig = 0x06064b50; // PK\x06\x06
constexpr core::uint32 kEocd64LocSig = 0x07064b50;

constexpr size_t kCdhFixed = 46;
constexpr size_t kLfhFixed = 30;
constexpr size_t kEocdFixed = 22;
constexpr size_t kEocd64Fixed = 56;
constexpr size_t kEocd64LocFixed = 20;

// Bounded EOCD backward-scan window (prepended SFX stubs tolerated; the
// shipped MAX_SFX_SIZE bound is the precedent).
constexpr core::uint64 kEocdScanWindow = 4 * 1024 * 1024;

core::uint16 rd16(const core::byte* p) {
    return static_cast<core::uint16>(p[0] | (p[1] << 8));
}
core::uint32 rd32(const core::byte* p) {
    return static_cast<core::uint32>(p[0]) | (static_cast<core::uint32>(p[1]) << 8) |
           (static_cast<core::uint32>(p[2]) << 16) | (static_cast<core::uint32>(p[3]) << 24);
}
core::uint64 rd64(const core::byte* p) {
    return static_cast<core::uint64>(rd32(p)) | (static_cast<core::uint64>(rd32(p + 4)) << 32);
}
core::uint32 dos_datetime_to_epoch(core::uint16 d, core::uint16 t) {
    if (d == 0 && t == 0) return 0;
    const int year = 1980 + ((d >> 9) & 0x7F);
    const int month = (d >> 5) & 0x0F;
    const int day = d & 0x1F;
    const int hour = (t >> 11) & 0x1F;
    const int minute = (t >> 5) & 0x3F;
    const int second = (t & 0x1F) * 2; // 2-second granularity
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
    // Days since epoch via the civil-from-days algorithm (proleptic
    // Gregorian; local-time interpretation is the documented divergence).
    const int y = month <= 2 ? year - 1 : year;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int mp = (month + 9) % 12;
    const int doy = (153 * mp + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    return static_cast<core::uint32>(days * 86400 + hour * 3600 + minute * 60 + second);
}

} // namespace

// ---------------------------------------------------------------------------
// ZipReader
// ---------------------------------------------------------------------------

ForeignStatus ZipReader::open(const std::filesystem::path& path, const ExtractionLimits& limits,
                              ReaderHooks hooks, std::string& detail) {
    path_ = path;
    if (!file_.open(path, io::FileMode::ReadOnly)) {
        detail = "cannot open " + path.string();
        return ForeignStatus::IoError;
    }
    const ForeignStatus st = locate_eocd(limits, detail);
    if (st != ForeignStatus::Ok) return st;
    const ForeignStatus wst = walk_central_directory(limits, hooks, detail);
    if (wst != ForeignStatus::Ok) return wst;
    // The §2.1 RELEASE GATE: the full pre-flight completes BEFORE any
    // staging file exists (nothing else has been created at this point).
    return preflight_cd_vs_lfh(detail);
}

ForeignStatus ZipReader::locate_eocd(const ExtractionLimits& limits, std::string& detail) {
    (void)limits;
    const core::uint64 fsize = file_.size();
    if (fsize < kEocdFixed) {
        detail = "file too small for an EOCD";
        return ForeignStatus::Unparseable;
    }
    const core::uint64 window = std::min<core::uint64>(fsize, kEocdScanWindow + kEocdFixed);
    std::vector<core::byte> buf(static_cast<size_t>(window));
    if (!file_.seek(static_cast<int64_t>(fsize - window), io::SeekOrigin::Begin) ||
        file_.read(buf.data(), buf.size()) != buf.size()) {
        detail = "cannot read EOCD scan window";
        return ForeignStatus::IoError;
    }
    // EOCD is the LAST occurrence of the signature that field-validates.
    for (int64_t i = static_cast<int64_t>(buf.size()) - kEocdFixed; i >= 0; --i) {
        if (rd32(buf.data() + i) != kEocdSig) continue;
        const core::uint16 comment_len = rd16(buf.data() + i + 20);
        if (static_cast<core::uint64>(i) + kEocdFixed + comment_len != buf.size()) continue;
        const core::byte* e = buf.data() + i;
        cd_offset_ = rd32(e + 16);
        cd_size_ = rd32(e + 16 - 4); // offset 12: size of CD
        total_entries_ = rd16(e + 10);
        // ZIP64: 0xFFFFFFFF / 0xFFFF placeholders → EOCD64 locator.
        if (cd_offset_ == 0xFFFFFFFFu || cd_size_ == 0xFFFFFFFFu || total_entries_ == 0xFFFF ||
            rd32(e + 8) == 0xFFFFFFFFu) {
            // Locator is 20 bytes directly before the EOCD.
            if (static_cast<core::uint64>(i) < kEocd64LocFixed) {
                detail = "EOCD64 locator out of range";
                return ForeignStatus::Unparseable;
            }
            const core::byte* loc = buf.data() + i - kEocd64LocFixed;
            if (rd32(loc) != kEocd64LocSig) {
                detail = "ZIP64 fields set but locator missing";
                return ForeignStatus::Unparseable;
            }
            const core::uint64 eocd64_off = rd64(loc + 8);
            if (eocd64_off + kEocd64Fixed > fsize) {
                detail = "EOCD64 out of range";
                return ForeignStatus::Unparseable;
            }
            std::vector<core::byte> b64(static_cast<size_t>(kEocd64Fixed));
            if (!file_.seek(static_cast<int64_t>(eocd64_off), io::SeekOrigin::Begin) ||
                file_.read(b64.data(), b64.size()) != b64.size() ||
                rd32(b64.data()) != kEocd64Sig) {
                detail = "cannot read EOCD64";
                return ForeignStatus::IoError;
            }
            total_entries_ = rd64(b64.data() + 32);
            cd_size_ = rd64(b64.data() + 40);
            cd_offset_ = rd64(b64.data() + 48);
        }
        // Field validation (a located signature must be consistent).
        if (cd_offset_ > fsize || cd_size_ > fsize || cd_offset_ + cd_size_ > fsize) {
            detail = "EOCD central-directory extents out of range";
            return ForeignStatus::Unparseable;
        }
        // Archive comment (EOCD tail) — sanitized downstream (§7.1).
        if (comment_len > 0) {
            archive_comment_.assign(reinterpret_cast<const char*>(e + kEocdFixed), comment_len);
        }
        return ForeignStatus::Ok;
    }
    detail = "EOCD not found";
    return ForeignStatus::Unparseable;
}

bool ZipReader::parse_zip64_extra(const core::byte* p, size_t n, bool need_sizes, bool need_offset,
                                  core::uint64& unp, core::uint64& pack,
                                  core::uint64& offset) const {
    // Header id 0x0001; fields appear in order for each 0xFFFFFFFF field in
    // the CDH: uncompressed size, compressed size, LFH offset, disk start.
    while (n >= 4) {
        const core::uint16 id = rd16(p);
        const core::uint16 sz = rd16(p + 2);
        if (static_cast<size_t>(sz) + 4 > n) return false;
        if (id == 0x0001) {
            const core::byte* q = p + 4;
            size_t left = sz;
            if (need_sizes && left >= 16) {
                unp = rd64(q);
                pack = rd64(q + 8);
                q += 16;
                left -= 16;
                need_sizes = false;
            }
            if (need_offset && left >= 8) {
                offset = rd64(q);
                q += 8;
                left -= 8;
                need_offset = false;
            }
            (void)q;
            return true;
        }
        p += 4 + sz;
        n -= 4 + sz;
    }
    return false;
}

ForeignStatus ZipReader::walk_central_directory(const ExtractionLimits& limits, ReaderHooks hooks,
                                                std::string& detail) {
    if (!file_.seek(static_cast<int64_t>(cd_offset_), io::SeekOrigin::Begin)) {
        detail = "cannot seek to central directory";
        return ForeignStatus::IoError;
    }
    core::uint64 header_bytes = 0;
    for (core::uint64 idx = 0; idx < total_entries_; ++idx) {
        if (hooks.cancelled()) return ForeignStatus::Aborted;
        core::byte hdr[kCdhFixed];
        if (file_.read(hdr, kCdhFixed) != kCdhFixed) {
            detail = "central directory truncated";
            return ForeignStatus::Truncated;
        }
        header_bytes += kCdhFixed;
        if (limits.hdr_bytes_limited() && header_bytes > limits.max_header_bytes) {
            detail = "header-bytes cap exceeded";
            return ForeignStatus::LimitExceeded;
        }
        if (rd32(hdr) != kCdhSig) {
            detail = "bad central-directory signature";
            return ForeignStatus::Unparseable;
        }
        Cdh e;
        e.gp_flags = rd16(hdr + 8);
        e.method = rd16(hdr + 10);
        const core::uint16 dos_time = rd16(hdr + 12);
        const core::uint16 dos_date = rd16(hdr + 14);
        e.crc = rd32(hdr + 16);
        e.packed_size = rd32(hdr + 20);
        e.unpacked_size = rd32(hdr + 24);
        const core::uint16 name_len = rd16(hdr + 28);
        const core::uint16 extra_len = rd16(hdr + 30);
        const core::uint16 comment_len = rd16(hdr + 32);
        const core::uint32 ext_attrs = rd32(hdr + 38);
        e.lfh_offset = rd32(hdr + 42);

        std::vector<core::byte> rest(static_cast<size_t>(name_len) + extra_len + comment_len);
        if (file_.read(rest.data(), rest.size()) != rest.size()) {
            detail = "central directory truncated";
            return ForeignStatus::Truncated;
        }
        header_bytes += rest.size();
        if (limits.hdr_bytes_limited() && header_bytes > limits.max_header_bytes) {
            detail = "header-bytes cap exceeded";
            return ForeignStatus::LimitExceeded;
        }

        const core::uint64 unp32 = e.unpacked_size;
        const core::uint64 pack32 = e.packed_size;
        const core::uint64 off32 = e.lfh_offset;
        if ((unp32 == 0xFFFFFFFFu || pack32 == 0xFFFFFFFFu || off32 == 0xFFFFFFFFu) &&
            !parse_zip64_extra(rest.data() + name_len, extra_len, unp32 == 0xFFFFFFFFu,
                               off32 == 0xFFFFFFFFu, e.unpacked_size, e.packed_size,
                               e.lfh_offset)) {
            detail = "ZIP64 fields missing for placeholder values";
            return ForeignStatus::Unparseable;
        }
        e.meta.unpacked_size = e.unpacked_size;

        // Name: GP bit 11 = UTF-8, else CP437 (spec 11 §2.4).
        const std::string raw(reinterpret_cast<const char*>(rest.data()), name_len);
        e.meta.name_raw = raw;
        const ForeignNameEncoding enc =
            (e.gp_flags & 0x0800) ? ForeignNameEncoding::Utf8 : ForeignNameEncoding::Cp437;
        e.meta.name = translate_foreign_name(raw, enc, e.meta.name_escaped);

        // Attributes: DOS byte → file_attr; unix mode honored only with
        // real type bits (§4.3).
        e.meta.dos_attrs = ext_attrs & 0xFF;
        const core::uint32 unix_mode = (ext_attrs >> 16) & 0xFFFF;
        if ((unix_mode & 0170000) == 0170000 || (unix_mode & 0170000) == 0040000) {
            e.meta.has_posix_mode = true;
            e.meta.posix_mode = unix_mode;
        }

        // Timestamps: UT extra (0x5455) → NTFS extra (0x000a, timestamps
        // ONLY — stream sub-blocks are never restored, §4.3) → DOS time.
        // NTFS FILETIME = 100 ns since 1601-01-01.
        int64_t mtime_sec = -1;
        int32_t mtime_nsec = 0;
        {
            const core::byte* p = rest.data() + name_len;
            size_t n = extra_len;
            while (n >= 4) {
                const core::uint16 id = rd16(p);
                const core::uint16 sz = rd16(p + 2);
                if (static_cast<size_t>(sz) + 4 > n) break;
                if (id == 0x5455 && sz >= 5) {
                    const core::uint8 flags = p[4];
                    const core::byte* t = p + 5;
                    if ((flags & 0x01) && sz >= 5 + 4) {
                        mtime_sec = static_cast<int64_t>(rd32(t));
                        mtime_nsec = 0;
                    }
                } else if (id == 0x000A && sz >= 24) {
                    // Reserved(4) then tag(2)/size(2) pairs; tag 1 = times.
                    const core::byte* q = p + 4;
                    const core::uint16 tag = rd16(q);
                    const core::uint16 tsz = rd16(q + 2);
                    if (tag == 1 && tsz >= 24) {
                        const core::uint64 ft = rd64(q + 8); // mtime is first
                        const int64_t epoch_100ns = static_cast<int64_t>(ft) - 116444736000000000LL;
                        mtime_sec = epoch_100ns / 10000000;
                        mtime_nsec = static_cast<int32_t>((epoch_100ns % 10000000) * 100);
                    }
                }
                p += 4 + sz;
                n -= 4 + sz;
            }
        }
        if (mtime_sec < 0) {
            mtime_sec = static_cast<int64_t>(dos_datetime_to_epoch(dos_date, dos_time));
            mtime_nsec = 0;
        }
        const MtimeBounds bounds;
        if (mtime_out_of_bounds(mtime_sec, bounds)) {
            mtime_sec = clamp_mtime(mtime_sec, bounds);
            e.meta.timestamp_clamped = true;
        }
        e.meta.mtime_sec = mtime_sec;
        e.meta.mtime_nsec = mtime_nsec;

        // Entry shape.
        if (e.meta.dos_attrs & 0x10 ||
            (e.meta.has_posix_mode && (e.meta.posix_mode & 0170000) == 0040000) ||
            (!raw.empty() && raw.back() == '/')) {
            e.meta.type = ForeignType::Dir;
        } else {
            e.meta.type = ForeignType::File;
        }
        e.meta.crc32 = e.crc;
        e.meta.has_crc32 = true;
        e.meta.method_name =
            e.method == 0 ? "store"
                          : (e.method == 8 ? "deflate" : "method-" + std::to_string(e.method));
        if (e.gp_flags & 0x0001) {
            e.meta.encrypted = true;
            e.meta.method_name = "encrypted";
        }

        // Per-entry comment has no RAR5 representation — skipped with
        // report at the cv layer (spec 11 §2.4).
        if (comment_len > 0)
            e.meta.comment.assign(reinterpret_cast<const char*>(rest.data() + name_len + extra_len),
                                  comment_len);

        if (limits.hdr_count_limited() && entries_.size() >= limits.max_header_count) {
            detail = "entry-count cap exceeded";
            return ForeignStatus::LimitExceeded;
        }
        entries_.push_back(std::move(e));
        hooks.emit(idx + 1, total_entries_);
    }
    return ForeignStatus::Ok;
}

ForeignStatus ZipReader::preflight_cd_vs_lfh(std::string& detail) {
    // The D6 comparison table (spec 11 §2.3): strict name/method/offset;
    // crc+sizes tolerated ONLY under GP bit 3 or ZIP64 placeholders; GP
    // flag subset {0,3,11} compared. ANY other mismatch is an integrity
    // failure — abort before any output exists.
    for (Cdh& e : entries_) {
        if (e.lfh_offset > file_.size() || e.lfh_offset + kLfhFixed > file_.size()) {
            detail = "local header out of range: " + e.meta.name_raw;
            return ForeignStatus::StructuralMismatch;
        }
        core::byte hdr[kLfhFixed];
        if (!file_.seek(static_cast<int64_t>(e.lfh_offset), io::SeekOrigin::Begin) ||
            file_.read(hdr, kLfhFixed) != kLfhFixed) {
            detail = "cannot read local header: " + e.meta.name_raw;
            return ForeignStatus::IoError;
        }
        if (rd32(hdr) != kLfhSig) {
            detail = "local header signature mismatch: " + e.meta.name_raw;
            return ForeignStatus::StructuralMismatch;
        }
        const core::uint16 lfh_flags = rd16(hdr + 6);
        const core::uint16 lfh_method = rd16(hdr + 8);
        const core::uint32 lfh_crc = rd32(hdr + 14);
        core::uint64 lfh_pack = rd32(hdr + 18);
        core::uint64 lfh_unp = rd32(hdr + 22);
        const core::uint16 lfh_name_len = rd16(hdr + 26);
        const core::uint16 lfh_extra_len = rd16(hdr + 28);

        if (lfh_name_len != e.meta.name_raw.size()) {
            detail = "local header name length mismatch: " + e.meta.name_raw;
            return ForeignStatus::StructuralMismatch;
        }
        std::vector<core::byte> lfh_name(lfh_name_len);
        if (file_.read(lfh_name.data(), lfh_name.size()) != lfh_name.size()) {
            detail = "cannot read local header name: " + e.meta.name_raw;
            return ForeignStatus::IoError;
        }
        if (std::memcmp(lfh_name.data(), e.meta.name_raw.data(), lfh_name_len) != 0) {
            detail = "local header name mismatch: " + e.meta.name_raw;
            return ForeignStatus::StructuralMismatch;
        }
        if (lfh_method != e.method) {
            detail = "local header method mismatch: " + e.meta.name_raw;
            return ForeignStatus::StructuralMismatch;
        }
        // Compared flag subset: bit 0 (encrypted), bit 3 (descriptor),
        // bit 11 (UTF-8). Other bits tolerated.
        if ((lfh_flags & 0x0801) != (e.gp_flags & 0x0801) ||
            ((lfh_flags & 0x0800) != 0) != ((e.gp_flags & 0x0800) != 0)) {
            detail = "local header flag mismatch: " + e.meta.name_raw;
            return ForeignStatus::StructuralMismatch;
        }
        // Sizes/crc: skip under bit 3 (descriptor placeholders). ZIP64
        // entries carry 0xFFFFFFFF placeholders in the LFH; resolve from
        // the LFH's own ZIP64 extra when present.
        if (!(lfh_flags & 0x0008)) {
            bool lfh_unp_placeholder = lfh_unp == 0xFFFFFFFFu;
            bool lfh_pack_placeholder = lfh_pack == 0xFFFFFFFFu;
            if (lfh_unp_placeholder || lfh_pack_placeholder) {
                // Read the LFH extra area for its ZIP64 field.
                std::vector<core::byte> lfh_extra(lfh_extra_len);
                if (lfh_extra_len > 0 &&
                    file_.read(lfh_extra.data(), lfh_extra.size()) != lfh_extra.size()) {
                    detail = "cannot read local header extra: " + e.meta.name_raw;
                    return ForeignStatus::IoError;
                }
                core::uint64 z_unp = lfh_unp, z_pack = lfh_pack, z_off = 0;
                parse_zip64_extra(lfh_extra.data(), lfh_extra.size(), lfh_unp_placeholder, false,
                                  z_unp, z_pack, z_off);
                lfh_unp = z_unp;
                lfh_pack = z_pack;
            }
            if (!lfh_unp_placeholder && lfh_unp != e.unpacked_size) {
                detail = "local header size mismatch: " + e.meta.name_raw;
                return ForeignStatus::StructuralMismatch;
            }
            if (!lfh_pack_placeholder && lfh_pack != e.packed_size) {
                detail = "local header packed-size mismatch: " + e.meta.name_raw;
                return ForeignStatus::StructuralMismatch;
            }
            if (lfh_crc != e.crc) {
                detail = "local header crc mismatch: " + e.meta.name_raw;
                return ForeignStatus::StructuralMismatch;
            }
        }
        // Data starts after the LFH (name + extra are variable — recompute
        // from the fields we just validated).
        e.data_offset = e.lfh_offset + kLfhFixed + lfh_name_len + lfh_extra_len;
        if (e.data_offset + e.packed_size > file_.size()) {
            detail = "member data out of range: " + e.meta.name_raw;
            return ForeignStatus::StructuralMismatch;
        }
    }
    return ForeignStatus::Ok;
}

ForeignStatus ZipReader::decode(size_t index, const SinkFn& sink, const ExtractionLimits& limits,
                                LimitState& state, ReaderHooks hooks) {
    if (index >= entries_.size()) return ForeignStatus::Unparseable;
    const Cdh& e = entries_[index];
    if (e.meta.type == ForeignType::Dir) return ForeignStatus::Ok;

    if (e.gp_flags & 0x0001) return ForeignStatus::EncryptedRefused;
    if (e.method != 0 && e.method != 8) return ForeignStatus::UnsupportedMethod;

    if (!file_.seek(static_cast<int64_t>(e.data_offset), io::SeekOrigin::Begin)) {
        return ForeignStatus::IoError;
    }

    crypto::Crc32 crc;
    ForeignStatus result = ForeignStatus::Ok;
    core::uint64 member_out = 0;

    auto debit = [&](core::uint64 got) -> bool {
        member_out += got;
        if (limits.member_limited() && member_out > limits.max_member_output_bytes) return false;
        return debit_total_bytes(got, limits, state);
    };

    if (e.method == 0) {
        std::vector<core::byte> buf(65536);
        core::uint64 left = e.packed_size;
        while (left > 0) {
            if (hooks.cancelled()) return ForeignStatus::Aborted;
            const size_t take = static_cast<size_t>(std::min<core::uint64>(left, buf.size()));
            if (file_.read(buf.data(), take) != take) return ForeignStatus::Truncated;
            if (!debit(take)) return ForeignStatus::LimitExceeded;
            crc.update(buf.data(), take);
            if (sink && !sink(buf.data(), take)) return ForeignStatus::Aborted;
            hooks.emit(member_out, e.unpacked_size);
            left -= take;
        }
    } else {
        // DEFLATE: the packed extent is the input; the in-flight cap rides
        // the member/total debit via the sink wrapper (plan D5: declared
        // sizes are verified, never trusted as bounds).
        compress::Inflate inf;
        auto input = [&](core::byte* buf, size_t max) -> size_t {
            const core::uint64 remaining =
                e.packed_size - (static_cast<core::uint64>(file_.tell()) - e.data_offset);
            const size_t take = static_cast<size_t>(std::min<core::uint64>(remaining, max));
            if (take == 0) return 0;
            const size_t got = file_.read(buf, take);
            return got;
        };
        auto capped_sink = [&](const core::byte* data, size_t size) -> bool {
            if (!debit(size)) return false;
            crc.update(data, size);
            if (hooks.cancelled()) return false;
            if (sink && !sink(data, size)) return false;
            hooks.emit(member_out, e.unpacked_size);
            return true;
        };
        const compress::InflateError err = inf.decode(input, capped_sink);
        if (err != compress::InflateError::Ok) {
            if (err == compress::InflateError::OutputCapExceeded)
                return ForeignStatus::LimitExceeded;
            if (err == compress::InflateError::SinkFailed) {
                return limits.member_limited() && member_out > limits.max_member_output_bytes
                           ? ForeignStatus::LimitExceeded
                           : ForeignStatus::Aborted;
            }
            result = ForeignStatus::Truncated; // decode errors are stream corruption
        }
    }

    if (result == ForeignStatus::Ok && e.meta.has_crc32 && crc.get() != e.crc) {
        return ForeignStatus::CrcMismatch;
    }
    // Declared-size verification (D5: verify actual against declared). For
    // store members the payload IS the declared extent; for DEFLATE the
    // CRC32 over the actual bytes is the format's authority (a CRC-matching
    // payload is right even when a producer lied about the size).
    if (result == ForeignStatus::Ok && member_out != e.unpacked_size && e.method == 0) {
        return ForeignStatus::Truncated;
    }
    return result;
}

} // namespace openrar::archive::foreign
