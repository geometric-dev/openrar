#include "foreign_gzip.hpp"

#include "../compress/inflate.hpp"
#include "../crypto/crc32.hpp"
#include "../io/file_stream.hpp"

#include <algorithm>
#include <cstring>

namespace openrar::archive::foreign {

namespace {

bool read_exact(io::FileStream& f, core::uint64 offset, void* dst, size_t n) {
    if (!f.seek(static_cast<int64_t>(offset), io::SeekOrigin::Begin)) return false;
    return f.read(dst, n) == n;
}

// Parses one member header at `offset`. On success returns the data start
// and consumes FEXTRA/FCOMMENT (bounded skips), FNAME and FHCRC.
bool parse_member_header(io::FileStream& f, core::uint64 offset, core::uint64 fsize,
                         std::string& fname_out, bool& fcomment_present, core::uint64& data_start) {
    if (offset + 10 > fsize) return false;
    core::byte h[10];
    if (!read_exact(f, offset, h, 10)) return false;
    if (h[0] != 0x1F || h[1] != 0x8B || h[2] != 8) return false;
    if (h[3] & 0xE0) return false; // reserved FLG bits set (spec 11 §5)
    const core::uint8 flg = h[3];
    core::uint64 pos = offset + 10;
    if (flg & 0x04) { // FEXTRA: bounded skip
        if (pos + 2 > fsize) return false;
        core::byte xl[2];
        if (!read_exact(f, pos, xl, 2)) return false;
        const core::uint16 xlen = static_cast<core::uint16>(xl[0] | (xl[1] << 8));
        pos += 2 + xlen;
        if (pos > fsize) return false;
    }
    if (flg & 0x08) { // FNAME: zero-terminated
        std::string name;
        for (;;) {
            if (pos >= fsize) return false;
            core::byte c = 0;
            if (!read_exact(f, pos, &c, 1)) return false;
            ++pos;
            if (c == 0) break;
            name.push_back(static_cast<char>(c));
        }
        fname_out = name;
    }
    if (flg & 0x10) { // FCOMMENT: present → skipped with report downstream
        fcomment_present = true;
        for (;;) {
            if (pos >= fsize) return false;
            core::byte c = 0;
            if (!read_exact(f, pos, &c, 1)) return false;
            ++pos;
            if (c == 0) break;
        }
    }
    if (flg & 0x02) { // FHCRC: CRC16 over the header bytes so far
        if (pos + 2 > fsize) return false;
        core::byte hc[2];
        if (!read_exact(f, pos, hc, 2)) return false;
        crypto::Crc32 crc;
        std::vector<core::byte> hdr(static_cast<size_t>(pos - offset));
        if (!read_exact(f, offset, hdr.data(), hdr.size())) return false;
        crc.update(hdr.data(), hdr.size());
        const core::uint16 stored = static_cast<core::uint16>(hc[0] | (hc[1] << 8));
        if (crc.get() != stored) return false;
        pos += 2;
    }
    data_start = pos;
    return true;
}

} // namespace

const ForeignEntry& GzipReader::entry(size_t index) const {
    (void)index;
    return entry_;
}

ForeignStatus GzipReader::open(const std::filesystem::path& path, const ExtractionLimits& limits,
                               ReaderHooks hooks, std::string& detail) {
    (void)limits;
    (void)hooks;
    path_ = path;
    if (!file_.open(path, io::FileMode::ReadOnly)) {
        detail = "cannot open " + path.string();
        return ForeignStatus::IoError;
    }
    if (!file_.seek(0, io::SeekOrigin::End)) return ForeignStatus::IoError;
    const core::uint64 fsize = file_.tell();

    // Cheap structural verification of the FIRST member; the multi-member
    // walk happens during decode (DEFLATE self-terminates, so member ends
    // are exact — a byte-scan here would be both slow and wrong).
    std::string fname;
    bool fcomment = false;
    core::uint64 data_start = 0;
    if (!parse_member_header(file_, 0, fsize, fname, fcomment, data_start)) {
        detail = "malformed gzip member header";
        return ForeignStatus::Unparseable;
    }
    entry_.comment = fcomment ? "gzip header comment present" : "";

    // Entry name: FNAME of the first member, else the source stem
    // convention (strip .gz / .tgz→.tar — spec 11 §5).
    std::string raw = fname;
    bool escaped = false;
    if (raw.empty()) {
        std::string stem = path.filename().string();
        if (stem.size() > 3 && stem.rfind(".gz") == stem.size() - 3)
            stem.resize(stem.size() - 3);
        else if (stem.size() > 4 && stem.rfind(".tgz") == stem.size() - 4)
            stem = stem.substr(0, stem.size() - 4) + ".tar";
        raw = stem;
    }
    entry_.name_raw = raw;
    entry_.name = translate_foreign_name(raw, ForeignNameEncoding::Utf8, escaped);
    entry_.name_escaped = escaped;
    entry_.type = ForeignType::File;
    entry_.method_name = "deflate";
    entry_.unpacked_size = 0; // unknown until decode (ISIZE at member end)
    opened_ = true;
    return ForeignStatus::Ok;
}

ForeignStatus GzipReader::decode(size_t index, const SinkFn& sink, const ExtractionLimits& limits,
                                 LimitState& state, ReaderHooks hooks) {
    (void)index; // exactly one logical entry
    if (!opened_) return ForeignStatus::NotOpened;
    if (!file_.seek(0, io::SeekOrigin::End)) return ForeignStatus::IoError;
    const core::uint64 fsize = file_.tell();

    std::vector<core::byte> trailer(8);
    core::uint64 logical_out = 0;
    core::uint64 member_offset = 0;
    size_t member_count = 0;

    for (;;) {
        if (hooks.cancelled()) return ForeignStatus::Aborted;
        std::string fname;
        bool fcomment = false;
        core::uint64 data_start = 0;
        if (!parse_member_header(file_, member_offset, fsize, fname, fcomment, data_start))
            return ForeignStatus::Unparseable;
        ++member_count;
        if (limits.hdr_count_limited() && member_count >= limits.max_header_count) {
            return ForeignStatus::LimitExceeded;
        }

        crypto::Crc32 crc;
        compress::Inflate inf;
        // The stream position is shared: Inflate pulls from the current
        // position until its BFINAL block completes.
        auto input = [&](core::byte* buf, size_t max) -> size_t {
            return file_.read(buf, max);
        };
        core::uint64 member_out = 0;
        auto capped_sink = [&](const core::byte* data, size_t size) -> bool {
            member_out += size;
            if (limits.member_limited() && member_out > limits.max_member_output_bytes)
                return false;
            if (limits.total_limited() && state.total_out + size > limits.max_total_output_bytes)
                return false;
            crc.update(data, size);
            if (hooks.cancelled()) return false;
            if (sink && !sink(data, size)) return false;
            hooks.emit(logical_out + member_out, 0); // total unknown per member
            return true;
        };
        if (!file_.seek(static_cast<int64_t>(data_start), io::SeekOrigin::Begin))
            return ForeignStatus::IoError;
        const compress::InflateError err = inf.decode(input, capped_sink);
        if (err == compress::InflateError::OutputCapExceeded ||
            err == compress::InflateError::SinkFailed)
            return ForeignStatus::LimitExceeded;
        if (err != compress::InflateError::Ok) return ForeignStatus::Truncated;

        // Trailer: CRC32 + ISIZE (mod 2^32) immediately after the stream.
        const core::uint64 trailer_at = data_start + (inf.bits_consumed() + 7) / 8;
        if (trailer_at + 8 > fsize || !read_exact(file_, trailer_at, trailer.data(), 8))
            return ForeignStatus::Truncated;
        const core::uint32 stored_crc = static_cast<core::uint32>(trailer[0]) |
                                        (static_cast<core::uint32>(trailer[1]) << 8) |
                                        (static_cast<core::uint32>(trailer[2]) << 16) |
                                        (static_cast<core::uint32>(trailer[3]) << 24);
        const core::uint32 stored_isize = static_cast<core::uint32>(trailer[4]) |
                                          (static_cast<core::uint32>(trailer[5]) << 8) |
                                          (static_cast<core::uint32>(trailer[6]) << 16) |
                                          (static_cast<core::uint32>(trailer[7]) << 24);
        if (crc.get() != stored_crc) return ForeignStatus::CrcMismatch;
        if (static_cast<core::uint32>(member_out & 0xFFFFFFFFu) != stored_isize)
            return ForeignStatus::CrcMismatch;
        state.total_out += member_out;
        logical_out += member_out;

        // Next member: a fresh header at the trailer position; anything
        // else (shorter than a header) is the documented EOF tolerance.
        member_offset = trailer_at + 8;
        if (member_offset + 10 > fsize) break;
        core::byte probe[2];
        if (!read_exact(file_, member_offset, probe, 2)) return ForeignStatus::IoError;
        if (probe[0] != 0x1F || probe[1] != 0x8B) break;
    }

    entry_.unpacked_size = logical_out;
    return ForeignStatus::Ok;
}

} // namespace openrar::archive::foreign
