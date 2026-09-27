#include "foreign_tar.hpp"

#include "../io/file_stream.hpp"

#include <algorithm>
#include <cstring>

namespace openrar::archive::foreign {

namespace {

constexpr size_t kBlock = 512;

// Numeric field: octal ASCII (spaces/NUL padded) or GNU base-256 (high bit
// of the first byte set).
bool parse_tar_num(const core::byte* p, size_t n, core::uint64& out) {
    if (n == 0) return false;
    if (p[0] & 0x80) {
        // GNU base-256, big-endian.
        core::uint64 v = p[0] & 0x7F;
        for (size_t i = 1; i < n; ++i) {
            if (v > (~core::uint64(0) >> 8)) return false; // overflow guard
            v = (v << 8) | p[i];
        }
        out = v;
        return true;
    }
    core::uint64 v = 0;
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = static_cast<char>(p[i]);
        if (c == ' ' || c == '\0') {
            if (any) break;
            continue;
        }
        if (c < '0' || c > '7') return false;
        any = true;
        if (v > (core::uint64(1) << 61)) return false; // overflow guard
        v = v * 8 + static_cast<core::uint64>(c - '0');
    }
    out = v;
    return any;
}

// pax records: "len key=value\n" — bounded by the record area itself.
bool parse_pax_records(const core::byte* p, size_t n,
                       std::vector<std::pair<std::string, std::string>>& out) {
    size_t pos = 0;
    while (pos < n) {
        // Zero padding after the last record ends the record area (the
        // record area is NUL-padded to the block size).
        if (p[pos] < static_cast<core::byte>('0') || p[pos] > static_cast<core::byte>('9')) break;
        const size_t start = pos;
        while (pos < n && p[pos] != ' ') ++pos;
        if (pos >= n) return false;
        core::uint64 total = 0;
        for (size_t i = start; i < pos; ++i) {
            if (p[i] < '0' || p[i] > '9') return false;
            if (total > (core::uint64(1) << 40)) return false;
            total = total * 10 + (p[i] - '0');
        }
        ++pos; // space
        if (total == 0 || total > n - start) return false;
        const size_t rec_end = start + static_cast<size_t>(total);
        // key=value\n
        const size_t eq =
            pos + static_cast<size_t>(
                      std::find(p + pos, p + rec_end, static_cast<core::byte>('=')) - (p + pos));
        if (eq >= rec_end || rec_end == 0 || p[rec_end - 1] != '\n') return false;
        out.emplace_back(std::string(reinterpret_cast<const char*>(p) + pos, eq - pos),
                         std::string(reinterpret_cast<const char*>(p) + eq + 1, rec_end - eq - 2));
        pos = rec_end;
    }
    return true;
}

// pax mtime: seconds (float accepted). Truncates to ns.
bool parse_pax_time(const std::string& v, int64_t& sec, int32_t& nsec) {
    sec = 0;
    nsec = 0;
    size_t i = 0;
    bool neg = false;
    if (!v.empty() && (v[0] == '-' || v[0] == '+')) {
        neg = v[0] == '-';
        i = 1;
    }
    core::uint64 ipart = 0;
    bool any = false;
    for (; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i) {
        if (ipart > (core::uint64(1) << 55)) return false;
        ipart = ipart * 10 + static_cast<core::uint64>(v[i] - '0');
        any = true;
    }
    core::uint64 frac = 0;
    uint32_t frac_div = 1;
    if (i < v.size() && v[i] == '.') {
        ++i;
        for (; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i) {
            if (frac_div < 1000000000u) {
                frac = frac * 10 + static_cast<core::uint64>(v[i] - '0');
                frac_div *= 10;
            }
        }
    }
    if (!any) return false;
    sec = static_cast<int64_t>(ipart);
    if (neg) sec = -sec;
    // frac_div is a power of ten <= 1e9 by construction; scale to ns.
    nsec = static_cast<int32_t>(frac * (1000000000u / frac_div));
    return true;
}

} // namespace

ForeignStatus TarReader::open(const std::filesystem::path& path, const ExtractionLimits& limits,
                              ReaderHooks hooks, std::string& detail) {
    path_ = path;
    if (!file_.open(path, io::FileMode::ReadOnly)) {
        detail = "cannot open " + path.string();
        return ForeignStatus::IoError;
    }
    if (!file_.seek(0, io::SeekOrigin::End)) return ForeignStatus::IoError;
    const core::uint64 fsize = file_.tell();

    std::vector<core::byte> hdr(kBlock);
    std::string pending_long_name;                                // GNU 'L'
    std::string pending_long_link;                                // GNU 'K'
    std::vector<std::pair<std::string, std::string>> pending_pax; // 'x'
    core::uint64 offset = 0;
    core::uint64 zero_streak = 0;

    while (offset + kBlock <= fsize) {
        if (hooks.cancelled()) return ForeignStatus::Aborted;
        if (!file_.seek(static_cast<int64_t>(offset), io::SeekOrigin::Begin) ||
            file_.read(hdr.data(), kBlock) != kBlock) {
            detail = "cannot read tar header";
            return ForeignStatus::IoError;
        }
        // End-of-archive: zero blocks.
        bool all_zero = true;
        for (size_t i = 0; i < kBlock; ++i) {
            if (hdr[i] != core::byte(0)) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            zero_streak += kBlock;
            offset += kBlock;
            if (zero_streak >= 2 * kBlock) break; // documented end
            continue;
        }
        zero_streak = 0;

        // Checksum validation (also the dispatch-time identity test).
        core::uint32 sum = 0;
        for (size_t i = 0; i < kBlock; ++i) {
            sum += (i >= 148 && i < 156) ? 0x20u : static_cast<core::uint32>(hdr[i]);
        }
        core::uint64 stored = 0;
        if (!parse_tar_num(hdr.data() + 148, 8, stored)) {
            detail = "bad tar header checksum field";
            return ForeignStatus::Unparseable;
        }
        if (static_cast<core::uint64>(sum) != stored) {
            detail = "tar header checksum mismatch";
            return ForeignStatus::Unparseable;
        }

        const core::uint64 size = [&] {
            core::uint64 v = 0;
            parse_tar_num(hdr.data() + 124, 12, v);
            return v;
        }();
        const core::uint64 mtime = [&] {
            core::uint64 v = 0;
            parse_tar_num(hdr.data() + 136, 12, v);
            return v;
        }();
        const core::uint64 uid = [&] {
            core::uint64 v = 0;
            parse_tar_num(hdr.data() + 108, 8, v);
            return v;
        }();
        const core::uint64 gid = [&] {
            core::uint64 v = 0;
            parse_tar_num(hdr.data() + 116, 8, v);
            return v;
        }();
        const char typeflag = static_cast<char>(hdr[156]);
        const std::string name(reinterpret_cast<const char*>(hdr.data()), 100);
        const std::string linkname(reinterpret_cast<const char*>(hdr.data()) + 157, 100);
        const std::string uname(reinterpret_cast<const char*>(hdr.data()) + 265, 32);
        const std::string gname(reinterpret_cast<const char*>(hdr.data()) + 297, 32);
        // ustar prefix (offset 345, 155 bytes) — magic "ustar\0" at 257.
        const bool ustar = std::memcmp(hdr.data() + 257, "ustar", 5) == 0;
        std::string prefix;
        if (ustar && hdr[345] != core::byte(0)) {
            prefix.assign(reinterpret_cast<const char*>(hdr.data()) + 345, 155);
        }

        // Size vs remaining file (declared extents are verified, D5).
        if (offset + kBlock + size > fsize) {
            detail = "tar member extends past end of file";
            return ForeignStatus::Truncated;
        }

        const core::uint64 data_offset = offset + kBlock;
        const core::uint64 padded = (size + kBlock - 1) / kBlock * kBlock;

        // Meta headers: consume and continue.
        if (typeflag == 'L' || typeflag == 'K') {
            // GNU long name/linkname: the payload IS the next name/linkpath.
            std::vector<core::byte> buf(static_cast<size_t>(size));
            if (!file_.seek(static_cast<int64_t>(data_offset), io::SeekOrigin::Begin) ||
                file_.read(buf.data(), buf.size()) != buf.size()) {
                detail = "cannot read GNU long record";
                return ForeignStatus::IoError;
            }
            std::string s(reinterpret_cast<const char*>(buf.data()), buf.size());
            const size_t nul = s.find('\0');
            if (nul != std::string::npos) s.resize(nul);
            if (typeflag == 'L')
                pending_long_name = s;
            else
                pending_long_link = s;
            offset = data_offset + padded;
            continue;
        }
        if (typeflag == 'x' || typeflag == 'g') {
            std::vector<core::byte> buf(static_cast<size_t>(size));
            if (!file_.seek(static_cast<int64_t>(data_offset), io::SeekOrigin::Begin) ||
                file_.read(buf.data(), buf.size()) != buf.size()) {
                detail = "cannot read pax record area";
                return ForeignStatus::IoError;
            }
            std::vector<std::pair<std::string, std::string>> recs;
            if (!parse_pax_records(buf.data(), buf.size(), recs)) {
                detail = "malformed pax records";
                return ForeignStatus::Unparseable;
            }
            if (typeflag == 'x') pending_pax = std::move(recs);
            offset = data_offset + padded;
            continue;
        }

        // Real member: build the entry model.
        Member m;
        ForeignEntry& e = m.meta;
        std::string raw_name = name;
        {
            const size_t nul = raw_name.find('\0');
            if (nul != std::string::npos) raw_name.resize(nul);
        }
        std::string raw_link = linkname;
        {
            const size_t nul = raw_link.find('\0');
            if (nul != std::string::npos) raw_link.resize(nul);
        }
        std::string raw_uname = uname;
        {
            const size_t nul = raw_uname.find('\0');
            if (nul != std::string::npos) raw_uname.resize(nul);
        }
        std::string raw_gname = gname;
        {
            const size_t nul = raw_gname.find('\0');
            if (nul != std::string::npos) raw_gname.resize(nul);
        }
        if (!prefix.empty()) {
            const size_t nul = prefix.find('\0');
            if (nul != std::string::npos) prefix.resize(nul);
            raw_name = prefix + "/" + raw_name;
        }
        std::string path_override;
        std::string link_override;
        core::uint64 pax_uid = 0, pax_gid = 0;
        bool have_pax_uid = false, have_pax_gid = false;
        bool sparse = false;

        for (const auto& kv : pending_pax) {
            if (kv.first == "path")
                path_override = kv.second;
            else if (kv.first == "linkpath")
                link_override = kv.second;
            else if (kv.first == "size") {
                // pax size overrides the header field (big members); plain
                // decimal, extent re-validated against the file.
                core::uint64 v = 0;
                bool ok = !kv.second.empty();
                for (const char c : kv.second) {
                    if (c < '0' || c > '9') {
                        ok = false;
                        break;
                    }
                    if (v > (core::uint64(1) << 54)) {
                        ok = false;
                        break;
                    }
                    v = v * 10 + static_cast<core::uint64>(c - '0');
                }
                if (ok) {
                    m.size = v;
                    if (data_offset + v > fsize) {
                        detail = "tar member extends past end of file";
                        return ForeignStatus::Truncated;
                    }
                }
            } else if (kv.first == "uid") {
                pax_uid = 0;
                for (const char c : kv.second) {
                    if (c < '0' || c > '9') break;
                    pax_uid = pax_uid * 10 + static_cast<core::uint64>(c - '0');
                }
                have_pax_uid = true;
            } else if (kv.first == "gid") {
                pax_gid = 0;
                for (const char c : kv.second) {
                    if (c < '0' || c > '9') break;
                    pax_gid = pax_gid * 10 + static_cast<core::uint64>(c - '0');
                }
                have_pax_gid = true;
            } else if (kv.first == "uname")
                raw_uname = kv.second;
            else if (kv.first == "gname")
                raw_gname = kv.second;
            else if (kv.first == "mtime") {
                parse_pax_time(kv.second, m.meta.mtime_sec, m.meta.mtime_nsec);
            } else if (kv.first.rfind("GNU.sparse.", 0) == 0) {
                sparse = true;
            }
            // hdrcharset: BINARY handling rides the Raw decode below.
        }

        m.size = m.size ? m.size : size;
        m.data_offset = data_offset;

        // Name resolution order: pax path > GNU 'L' > ustar prefix+name.
        std::string effective = !path_override.empty()
                                    ? path_override
                                    : (!pending_long_name.empty() ? pending_long_name : raw_name);
        std::string effective_link =
            !link_override.empty() ? link_override
                                   : (!pending_long_link.empty() ? pending_long_link : raw_link);

        // hdrcharset=BINARY → raw bytes (percent-encode branch); default
        // treats names as UTF-8 with the lossless escape.
        bool binary_names = false;
        for (const auto& kv : pending_pax) {
            if (kv.first == "hdrcharset" && kv.second == "BINARY") binary_names = true;
        }
        e.name_raw = effective;
        e.name = translate_foreign_name(
            effective, binary_names ? ForeignNameEncoding::Raw : ForeignNameEncoding::Utf8,
            e.name_escaped);
        e.link_target = translate_foreign_name(
            effective_link, binary_names ? ForeignNameEncoding::Raw : ForeignNameEncoding::Utf8,
            e.name_escaped);

        switch (typeflag) {
        case '0':
        case '\0':
        case '7': // contiguous treated as regular (documented)
            e.type = ForeignType::File;
            break;
        case '5':
            e.type = ForeignType::Dir;
            break;
        case '1':
            e.type = ForeignType::Hardlink;
            break;
        case '2':
            e.type = ForeignType::Symlink;
            break;
        default:
            e.type = ForeignType::Other;
            break;
        }

        // Mode/owner: honored only when real (§4.3).
        const core::uint64 mode = [&] {
            core::uint64 v = 0;
            parse_tar_num(hdr.data() + 100, 8, v);
            return v;
        }();
        if ((mode & 0170000) == 0170000) {
            e.has_posix_mode = true;
            e.posix_mode = static_cast<core::uint32>(mode);
        } else if (typeflag == '0' || typeflag == '5' || typeflag == '\0') {
            // Plain octal mode field (no type bits) — keep the permission
            // bits only when they look like a mode (<= 07777).
            if (mode <= 07777) {
                e.has_posix_mode = true;
                e.posix_mode =
                    static_cast<core::uint32>(mode) | (typeflag == '5' ? 0040000u : 0100000u);
            }
        }
        e.owner_name = raw_uname;
        e.group_name = raw_gname;
        e.has_owner_ids = true;
        e.owner_uid = have_pax_uid ? pax_uid : uid;
        e.owner_gid = have_pax_gid ? pax_gid : gid;

        // Timestamps: header mtime (seconds) unless pax overrode it.
        if (e.mtime_sec == 0 && e.mtime_nsec == 0) {
            e.mtime_sec = static_cast<int64_t>(mtime);
        }
        const MtimeBounds bounds;
        if (mtime_out_of_bounds(e.mtime_sec, bounds)) {
            e.mtime_sec = clamp_mtime(e.mtime_sec, bounds);
            e.timestamp_clamped = true;
        }

        pending_long_name.clear();
        pending_long_link.clear();
        pending_pax.clear();

        if (sparse) e.method_name = "sparse";
        m.meta.unpacked_size = m.size;
        entries_.push_back(std::move(m));

        if (limits.hdr_count_limited() && entries_.size() >= limits.max_header_count) {
            detail = "entry-count cap exceeded";
            return ForeignStatus::LimitExceeded;
        }
        offset = data_offset + padded;
    }

    // EOF before the two zero blocks is accepted (streamed tapes).
    (void)zero_streak;
    return ForeignStatus::Ok;
}

ForeignStatus TarReader::decode(size_t index, const SinkFn& sink, const ExtractionLimits& limits,
                                LimitState& state, ReaderHooks hooks) {
    if (index >= entries_.size()) return ForeignStatus::Unparseable;
    const Member& m = entries_[index];

    if (m.meta.type == ForeignType::Other) return ForeignStatus::UnsupportedMethod;
    if (m.meta.method_name == "sparse") return ForeignStatus::SparseRefused;
    if (m.meta.type != ForeignType::File) return ForeignStatus::Ok; // dir/link: no payload

    if (!file_.seek(static_cast<int64_t>(m.data_offset), io::SeekOrigin::Begin))
        return ForeignStatus::IoError;

    std::vector<core::byte> buf(65536);
    core::uint64 left = m.size;
    core::uint64 member_out = 0;
    while (left > 0) {
        if (hooks.cancelled()) return ForeignStatus::Aborted;
        const size_t take = static_cast<size_t>(std::min<core::uint64>(left, buf.size()));
        if (file_.read(buf.data(), take) != take) return ForeignStatus::Truncated;
        member_out += take;
        if (limits.member_limited() && member_out > limits.max_member_output_bytes)
            return ForeignStatus::LimitExceeded;
        if (!debit_total_bytes(take, limits, state)) return ForeignStatus::LimitExceeded;
        if (sink && !sink(buf.data(), take)) return ForeignStatus::Aborted;
        hooks.emit(member_out, m.size);
        left -= take;
    }
    return ForeignStatus::Ok;
}

} // namespace openrar::archive::foreign
