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

// Upper bound on an FNAME/FCOMMENT field. The stream is also bounded by
// fsize, but a huge unterminated field would otherwise be copied into memory
// in full before that bound is reached.
constexpr size_t MAX_GZIP_HEADER_FIELD = 64 * 1024;

// Scan forward for the terminating NUL, reading in chunks.
//
// This replaced a byte-at-a-time loop. read_exact() seeks before every read,
// so the old code issued two syscalls per byte of FNAME/FCOMMENT - roughly
// 2*10^8 for a 100 MiB header field with no NUL, i.e. a trivially cheap
// denial of service against any tool that sniffs a .gz.
//
// Appends the bytes before the NUL to `out` when non-null; on success leaves
// `pos` just past the NUL.
bool read_until_nul(io::FileStream& f, core::uint64& pos, core::uint64 fsize, std::string* out) {
    constexpr size_t CHUNK = 4096;
    core::byte buf[CHUNK];
    while (pos < fsize) {
        const size_t want = static_cast<size_t>(std::min<core::uint64>(CHUNK, fsize - pos));
        if (!read_exact(f, pos, buf, want)) return false;
        for (size_t k = 0; k < want; ++k) {
            if (buf[k] == 0) {
                if (out) out->append(reinterpret_cast<const char*>(buf), k);
                pos += k + 1;
                return true;
            }
        }
        if (out) {
            out->append(reinterpret_cast<const char*>(buf), want);
            if (out->size() > MAX_GZIP_HEADER_FIELD) return false;
        }
        pos += want;
    }
    return false; // ran off the end without a NUL
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
    if (flg & 0x08) {      // FNAME: zero-terminated
        fname_out.clear(); // read_until_nul appends
        if (!read_until_nul(f, pos, fsize, &fname_out)) return false;
    }
    if (flg & 0x10) { // FCOMMENT: present → skipped with report downstream
        fcomment_present = true;
        if (!read_until_nul(f, pos, fsize, nullptr)) return false;
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
        // RFC 1952 s2.3.1: FHCRC is the low 16 bits of the CRC32 over the
        // header. crc.get() returns the full 32-bit value, so compare the
        // masked low half - testing the full value against a uint16 rejected
        // every valid `gzip -H` file (all but 1 in 65536).
        if ((crc.get() & 0xFFFFu) != stored) return false;
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
        // "exactly max" is the convention the other readers follow
        // (foreign_zip.cpp / foreign_tar.cpp / archive_reader.cpp); testing
        // >= after the increment made the effective cap max - 1.
        if (limits.hdr_count_limited() && member_count > limits.max_header_count) {
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
            if (size > core::uint64(-1) - member_out) return false; // size arithmetic
            member_out += size;
            if (limits.member_limited() && member_out > limits.max_member_output_bytes)
                return false;
            // Debit the cumulative budget PER CHUNK. This used to be
            // `state.total_out + size > ...` while state.total_out was only
            // advanced once per member (below), so a single member could emit
            // arbitrarily far past the cap - the check compared against the
            // total from BEFORE this member. debit_total_bytes advances state
            // as it goes, and guards its own overflow.
            if (!debit_total_bytes(size, limits, state)) return false;
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
        // state.total_out was already advanced per chunk by capped_sink above;
        // only the local running total remains to update here.
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
