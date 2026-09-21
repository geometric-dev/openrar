#include "recovery_writer.hpp"
#include <iostream>
#include "recovery_record.hpp"
#include "../archive/archive_reader.hpp"
#include "../archive/archive_entry.hpp"
#include "../archive/volume.hpp"
#include "../format/header_writer.hpp"
#include "../format/header_reader.hpp"
#include "../format/headers.hpp"
#include "../core/vint.hpp"
#include "../core/thread_pool.hpp"
#include "../io/file_stream.hpp"
#include "../crypto/crc64.hpp"
#include "../crypto/crc32.hpp"
#include "rs16.hpp"

#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <filesystem>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace openrar::recovery {

// Unique temp name for repair/RR splicing. Same policy as the mutator's
// mutation_temp_path (L8): a fixed, predictable temp name opened with
// CreateAlways lets a local attacker pre-plant a symlink/hardlink at the
// well-known path and have the open follow it and truncate an arbitrary
// user-writable file. A pid+timestamp+counter name is unguessable, and
// CreateNew refuses anything already there.
std::filesystem::path recovery_temp_path(const std::filesystem::path& arc_path, const char* tag) {
    static std::atomic<core::uint32> counter{0};
#ifdef _WIN32
    const core::uint64 pid = static_cast<core::uint64>(GetCurrentProcessId());
#else
    const core::uint64 pid = static_cast<core::uint64>(getpid());
#endif
    const core::uint64 millis =
        static_cast<core::uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
    std::filesystem::path p = arc_path;
    p += std::string(".") + tag + "." + std::to_string(pid) + "-" + std::to_string(millis) + "-" +
         std::to_string(counter.fetch_add(1));
    return p;
}


// ─────────────────────────────────────────────────────────────────────────────
// RAR 5.0 inline recovery-record layout.
//
// A recovery record is stored as a RAR 5.0 service header named "RR" placed
// between the last file entry and the End-Of-Archive block. The service
// header's `SubData` extra field carries a single vint = recovery percent
// (informational; used by archive listers for display). The service
// header's data area contains `NR` back-to-back parity "shards".
//
// Each shard has the following layout (in bytes from the shard's start):
//
//     +0x00..+0x03   "{RB}" magic  (0x7B 0x52 0x42 0x7D)
//     +0x04..+0x0B   CRC-64/XZ over bytes +0x0C..+shard_size (u64 LE)
//     +0x0C..+0x0F   total_size == shard_size                (u32 LE)
//     +0x10..+0x13   header_size == D*8 + 0x48               (u32 LE)
//     +0x14          version_a = 0x01                        (u8)
//     +0x15          version_b = 0x01                        (u8)
//     +0x16..+0x1D   chunk_position = 0 for inline RR        (u64 LE)
//     +0x1E..+0x21   encoder state "chunk_data_extent"       (u32 LE)
//     +0x22..+0x29   protected_archive_size                  (u64 LE)
//     +0x2A..+0x31   group_count = parity bytes per shard    (u64 LE)
//     +0x32..+0x39   shard_size (repeated as u64)            (u64 LE)
//     +0x3A..+0x3B   D  (data-shard count per group)         (u16 LE)
//     +0x3C..+0x3D   NR (parity-shard count in this record)  (u16 LE)
//     +0x3E..+0x3F   shard_index (0..NR-1)                   (u16 LE)
//     +0x40..+0x40+D*8-1   D * u64 LE   encoder-internal state
//     +0x40+D*8..+0x47+D*8 u64 LE     encoder-internal final state
//     +header_size..+shard_size-1   parity payload (group_count bytes)
//
// The three encoder-internal fields (`chunk_data_extent`, the per-shard state
// array, and `final_state`) are not needed to decode the record; decoders do not
// parse them. This writer emits them as zero so the RR
// remains bit-for-bit reproducible from `(rec_pct, archive_size, parity)`.
//
// Sizing formula, matching standard RAR5 encoder output and format spec §4.6.2:
//
//   pct         = clamp(rec_pct, 0, 1000)
//   D           = archive_size >= 200 KiB  ? 200
//                                          : max(1, ceil(archive_size/1024))
//   NR_raw      = 2*pct*D / 200 = pct*D / 100
//   NR          = min(NR_raw, 10*D)
//   if NR == 0 and archive_size < 200 KiB: NR = 1
//   group_count = ceil(archive_size / D)      then rounded up to even
//   scale       = max(1, ceil(group_count / 65536))
//   header_size = (D*8 + 0x48) * scale
//   shard_size  = header_size + group_count
//
// Reed-Solomon parity is computed over the protected byte range (bytes
// 0 .. archive_size), split into D contiguous chunks of group_count bytes
// each (last chunk zero-padded). For each parity shard j = 0..NR-1:
//   parity[j][k] = XOR_i ( MX[j][i] * data[i][k] )   in GF(2^16)
// with the Cauchy matrix MX[j][i] = 1 / ((j+D) XOR i). Bytes within a
// group_count buffer are consumed as consecutive uint16 little-endian words
// (group_count is always even).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr core::byte SHARD_MAGIC[4] = {'{', 'R', 'B', '}'};
constexpr size_t SHARD_MAGIC_SIZE = 4;
constexpr size_t SHARD_CRC64_OFF = 0x04;
constexpr size_t SHARD_TOTAL_OFF = 0x0C;
constexpr size_t SHARD_HEADER_OFF = 0x10;
constexpr size_t SHARD_VER_A = 0x14;
constexpr size_t SHARD_VER_B = 0x15;
constexpr size_t SHARD_CHUNK_POS = 0x16;
constexpr size_t SHARD_CHUNK_EXT = 0x1E;
constexpr size_t SHARD_PROT_SIZE = 0x22;
constexpr size_t SHARD_GROUP_CNT = 0x2A;
constexpr size_t SHARD_SIZE_U64 = 0x32;
constexpr size_t SHARD_D_FIELD = 0x3A;
constexpr size_t SHARD_NR_FIELD = 0x3C;
constexpr size_t SHARD_INDEX = 0x3E;
constexpr size_t SHARD_STATE_ARR = 0x40;

struct RecoveryGeometry {
    core::uint32 pct;
    core::uint32 D;
    core::uint32 NR;
    core::uint64 group_count;
    core::uint32 scale;
    core::uint64 header_size;
    core::uint64 shard_size;
    core::uint64 archive_size;
};

RecoveryGeometry compute_geometry(core::uint64 archive_size, core::uint32 rec_pct) {
    RecoveryGeometry g{};
    g.archive_size = archive_size;
    g.pct = rec_pct > 1000 ? 1000 : rec_pct;

    if (archive_size >= 200ULL * 1024) {
        g.D = 200;
    } else {
        core::uint64 k = (archive_size + 1023) / 1024;
        if (k == 0) k = 1;
        if (k > 200) k = 200;
        g.D = static_cast<core::uint32>(k);
    }
    core::uint64 nr_raw = 2ULL * g.pct * g.D / 200; // = pct * D / 100
    core::uint32 cap = 10u * g.D;
    g.NR = static_cast<core::uint32>(nr_raw > cap ? cap : nr_raw);
    if (g.NR == 0 && archive_size < 200ULL * 1024) g.NR = 1;

    core::uint64 gc = (archive_size + g.D - 1) / g.D;
    gc += (gc & 1);
    g.group_count = gc;

    core::uint64 scale = (gc + 0xFFFF) / 0x10000;
    if (scale < 1) scale = 1;
    g.scale = static_cast<core::uint32>(scale);
    g.header_size = (static_cast<core::uint64>(g.D) * 8 + 0x48) * g.scale;
    g.shard_size = g.header_size + g.group_count;
    return g;
}

bool copy_stream_region(io::FileStream& src, io::FileStream& dest, core::uint64 src_offset,
                        core::uint64 size) {
    src.seek(static_cast<core::int64>(src_offset), io::SeekOrigin::Begin);
    core::byte buf[65536];
    core::uint64 remaining = size;
    while (remaining > 0) {
        size_t take =
            static_cast<size_t>(std::min(remaining, static_cast<core::uint64>(sizeof(buf))));
        if (src.read(buf, take) != take) return false;
        if (dest.write(buf, take) != take) return false;
        remaining -= take;
    }
    return true;
}

// Data-loss-safe replacement: the original archive must survive an atomic
// replace failure. Retry transient MOVEFILE failures, then as a fallback move
// the original aside and rename the new file in, restoring the original if
// that rename also fails. Never delete the tmp on failure: it holds the
// rewritten archive, so a later manual recovery is possible.
bool atomic_replace(const std::filesystem::path& tmp_path, const std::filesystem::path& arc_path) {
#ifdef _WIN32
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (MoveFileExW(tmp_path.wstring().c_str(), arc_path.wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        Sleep(50);
    }
    // Fallback: park the original, rename tmp into place, restore on failure.
    std::filesystem::path orig_parked = arc_path;
    orig_parked += ".replace_bak";
    std::error_code ec;
    std::filesystem::remove(orig_parked, ec);
    std::filesystem::rename(arc_path, orig_parked, ec);
    if (ec) {
        return false;
    }
    std::filesystem::rename(tmp_path, arc_path, ec);
    if (!ec) {
        std::filesystem::remove(orig_parked, ec);
        return true;
    }
    std::filesystem::rename(orig_parked, arc_path, ec);
    return false;
#else
    // std::filesystem::rename(a, b, ec) returns void — the result is ec.
    // (The old `if (!rename(a, b, ec))` shape does not compile under GCC 13.)
    std::error_code ec;
    std::filesystem::rename(tmp_path, arc_path, ec);
    // POSIX rename is atomic and replaces the target; a failure here
    // (cross-device) leaves the original intact and the tmp readable.
    return ec == std::error_code{};
#endif
}

// Exception-safe cleanup guard for the single rr_tmp file created by
// add_recovery_record. Mirrors the TempFileCleanupGuard in archive_mutator.cpp;
// duplicated here to avoid a cross-library dependency.
struct TempFileCleanupGuard {
    io::FileStream* stream{nullptr};
    std::filesystem::path path;
    bool committed{false};

    void commit() noexcept { committed = true; }

    ~TempFileCleanupGuard() {
        if (committed) return;
        if (stream && stream->is_open()) stream->close();
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

// Locate the "RR" service header inside a file. Returns file offsets and the
// raw data-area bytes; caller parses the internal shard structure.
struct RrLocation {
    bool found{false};
    core::uint64 header_offset{0}; // start of service block in the file
    core::uint64 header_size{0};   // header bytes (block header, no data area)
    core::uint64 data_offset{0};   // start of parity data area (== header_offset+header_size)
    core::uint64 data_size{0};     // size of parity data area
    core::uint32 rec_pct{0};       // decoded from SubData (0 if absent)
    std::vector<core::byte> raw_data;
};

RrLocation find_rr(io::FileStream& stream, core::uint64 sfx_offset) {
    RrLocation info;
    core::uint64 file_size = stream.size();
    core::uint64 start = sfx_offset + 8;
    if (start >= file_size) return info;

    auto try_read_rr = [&](core::uint64 head_start) -> bool {
        if (head_start >= file_size) return false;
        stream.seek(static_cast<core::int64>(head_start), io::SeekOrigin::Begin);
        core::uint64 type = 0, flags = 0, data_sz = 0;
        std::vector<core::byte> body;
        if (format::HeaderReader::read_block_raw(stream, type, flags, body, data_sz) !=
            format::HeaderResult::Ok)
            return false;
        core::uint64 head_end = stream.tell();
        if (type == format::HEAD_SERVICE) {
            format::FileBlock fb;
            if (format::HeaderReader::parse_file_header(body.data(), body.size(), fb) &&
                fb.is_service && fb.service_type == "RR") {
                info.found = true;
                info.header_offset = head_start;
                info.header_size = head_end - head_start;
                info.data_offset = head_end;
                info.data_size = data_sz;
                if (!fb.sub_data.empty()) {
                    core::uint64 pct = 0;
                    size_t used = 0;
                    if (core::read_vint(fb.sub_data.data(), fb.sub_data.size(), pct, used)) {
                        info.rec_pct = static_cast<core::uint32>(pct);
                    }
                }
                if (data_sz > 0) {
                    core::uint64 avail = std::min<core::uint64>(data_sz, file_size - head_end);
                    info.raw_data.resize(static_cast<size_t>(avail));
                    if (avail > 0) {
                        stream.seek(static_cast<core::int64>(head_end), io::SeekOrigin::Begin);
                        stream.read(info.raw_data.data(), info.raw_data.size());
                    }
                }
                return true;
            }
        }
        return false;
    };

    stream.seek(static_cast<core::int64>(start), io::SeekOrigin::Begin);

    while (stream.tell() < file_size) {
        core::uint64 head_start = stream.tell();
        core::uint64 type = 0, flags = 0, data_sz = 0;
        std::vector<core::byte> body;
        if (format::HeaderReader::read_block_raw(stream, type, flags, body, data_sz) !=
            format::HeaderResult::Ok)
            break;
        core::uint64 head_end = stream.tell();

        if (type == format::HEAD_SERVICE) {
            format::FileBlock fb;
            if (format::HeaderReader::parse_file_header(body.data(), body.size(), fb) &&
                fb.is_service && fb.service_type == "RR") {
                info.found = true;
                info.header_offset = head_start;
                info.header_size = head_end - head_start;
                info.data_offset = head_end;
                info.data_size = data_sz;
                if (!fb.sub_data.empty()) {
                    core::uint64 pct = 0;
                    size_t used = 0;
                    if (core::read_vint(fb.sub_data.data(), fb.sub_data.size(), pct, used)) {
                        info.rec_pct = static_cast<core::uint32>(pct);
                    }
                }
                if (data_sz > 0) {
                    // Only read what's actually available on disk; a truncated
                    // RR is caller's problem to detect.
                    core::uint64 avail = std::min<core::uint64>(data_sz, file_size - head_end);
                    info.raw_data.resize(static_cast<size_t>(avail));
                    if (avail > 0) {
                        stream.seek(static_cast<core::int64>(head_end), io::SeekOrigin::Begin);
                        stream.read(info.raw_data.data(), info.raw_data.size());
                    }
                }
                return info;
            }
        }
        if (type == format::HEAD_ENDARC) break;
        if (data_sz > 0) {
            // head_end + data_sz computed in uint64 can wrap below head_end
            // and seek backwards; the exact-equality guard below then allows
            // a crafted block cycle to rescan forever (report M3). Reject
            // wraps and past-EOF skips outright.
            core::uint64 next = head_end + data_sz;
            if (next < head_end || next > file_size) break;
            if (!stream.seek(static_cast<core::int64>(next), io::SeekOrigin::Begin)) break;
        }
        if (stream.tell() <= head_start) break;
    }

    // Fallback 1: Check locator in MAIN header
    if (!info.found && start < file_size) {
        stream.seek(static_cast<core::int64>(start), io::SeekOrigin::Begin);
        core::uint64 mtype = 0, mflags = 0, mdata_sz = 0;
        std::vector<core::byte> mbody;
        if (format::HeaderReader::read_block_raw(stream, mtype, mflags, mbody, mdata_sz) ==
                format::HeaderResult::Ok &&
            mtype == format::HEAD_MAIN) {
            format::MainBlock mb;
            if (format::HeaderReader::parse_main_header(mbody.data(), mbody.size(), mb) &&
                mb.has_locator && mb.locator_rr_offset > 0) {
                core::uint64 cand = start + static_cast<core::uint64>(mb.locator_rr_offset);
                if (try_read_rr(cand)) return info;
            }
        }
    }

    // Fallback 2: Search backwards for SHARD_MAGIC ("RAR_RR\0\0")
    if (!info.found && file_size > SHARD_MAGIC_SIZE) {
        const size_t CHUNK = 65536;
        std::vector<core::byte> buf(CHUNK + SHARD_MAGIC_SIZE);
        core::int64 cur_pos = static_cast<core::int64>(file_size);
        while (cur_pos > static_cast<core::int64>(start) && !info.found) {
            size_t to_read = static_cast<size_t>(std::min<core::int64>(CHUNK, cur_pos - start));
            cur_pos -= to_read;
            stream.seek(cur_pos, io::SeekOrigin::Begin);
            size_t read_bytes = stream.read(buf.data(), to_read + SHARD_MAGIC_SIZE - 1);
            if (read_bytes < SHARD_MAGIC_SIZE) break;
            for (size_t i = 0; i + SHARD_MAGIC_SIZE <= read_bytes; ++i) {
                if (std::memcmp(buf.data() + i, SHARD_MAGIC, SHARD_MAGIC_SIZE) == 0) {
                    core::uint64 magic_pos = static_cast<core::uint64>(cur_pos) + i;
                    core::uint64 scan_back = std::min<core::uint64>(magic_pos, 128);
                    for (core::uint64 off = 1; off <= scan_back; ++off) {
                        if (try_read_rr(magic_pos - off)) return info;
                    }
                }
            }
        }
    }

    return info;
}

// Assemble one parity shard: fixed prefix, structured header (all encoder-
// internal fields zeroed), then group_count parity bytes. Compute and stamp
// the CRC-64/XZ over bytes from +0x0C to end.
std::vector<core::byte> build_shard(core::uint32 shard_index, const RecoveryGeometry& g,
                                    const core::byte* parity, size_t parity_len) {
    std::vector<core::byte> shard(static_cast<size_t>(g.shard_size), 0);
    std::memcpy(shard.data(), SHARD_MAGIC, SHARD_MAGIC_SIZE);
    // KNOWN CAPABILITY CLIFF (report INFO 5): for shard sizes >= 2^32 the
    // u32 total_size field below silently truncates. The reader then rejects
    // the record via `shard_size_u != total_size`, so nothing is corrupted —
    // the RR feature simply stops being available (~858 GB archive ceiling).
    core::write_le32(shard.data() + SHARD_TOTAL_OFF, static_cast<core::uint32>(g.shard_size));
    core::write_le32(shard.data() + SHARD_HEADER_OFF, static_cast<core::uint32>(g.header_size));
    shard[SHARD_VER_A] = 0x01;
    shard[SHARD_VER_B] = 0x01;
    // chunk_position: 0 for inline RR (single 64 KiB chunk covering the whole
    // parity). chunk_data_extent: encoder-internal, safe as zero.
    core::write_le64(shard.data() + SHARD_CHUNK_POS, 0);
    core::write_le32(shard.data() + SHARD_CHUNK_EXT, 0);
    core::write_le64(shard.data() + SHARD_PROT_SIZE, g.archive_size);
    core::write_le64(shard.data() + SHARD_GROUP_CNT, g.group_count);
    core::write_le64(shard.data() + SHARD_SIZE_U64, g.shard_size);
    core::write_le16(shard.data() + SHARD_D_FIELD, static_cast<core::uint16>(g.D));
    core::write_le16(shard.data() + SHARD_NR_FIELD, static_cast<core::uint16>(g.NR));
    core::write_le16(shard.data() + SHARD_INDEX, static_cast<core::uint16>(shard_index));
    // data_shard_state[D] and final_state are zero by construction.

    // Parity payload.
    if (parity_len > g.group_count) parity_len = static_cast<size_t>(g.group_count);
    std::memcpy(shard.data() + g.header_size, parity, parity_len);

    // CRC-64/XZ covers bytes from +0x0C to end of shard.
    core::uint64 crc =
        crypto::Crc64Xz::compute(shard.data() + SHARD_TOTAL_OFF, shard.size() - SHARD_TOTAL_OFF);
    core::write_le64(shard.data() + SHARD_CRC64_OFF, crc);
    return shard;
}

// Write the "RR" service header + parity data area. The data area is a
// caller-provided vector of concatenated shard bytes.
bool write_rr_service_block(io::FileStream& dest, const std::vector<core::byte>& data_area,
                            core::uint32 rec_pct) {
    format::FileBlock fb;
    fb.is_service = true;
    fb.service_type = "RR";
    fb.file_name = "RR";
    fb.file_flags = 0;
    fb.unp_size = data_area.size();
    fb.pack_size = static_cast<core::int64>(data_area.size());
    fb.attributes = 0;
    fb.has_crc32 = false;
    fb.method = 0;
    fb.win_size = 0;
    fb.host_os = 0;

    // SubData: single vint carrying recovery percent (spec §4.6).
    core::push_vint(fb.sub_data, rec_pct);

    if (!format::HeaderWriter::write_file_block(dest, fb)) return false;
    if (!data_area.empty()) {
        if (dest.write(data_area.data(), data_area.size()) != data_area.size()) return false;
    }
    return true;
}

// Full-file header sweep: return true iff every block header parses cleanly
// and the archive terminates on an End block.
bool headers_verify(const std::filesystem::path& arc_path, core::uint64 sfx_off) {
    io::FileStream stream;
    if (!stream.open(arc_path, io::FileMode::ReadOnly)) return false;
    core::uint64 file_size = stream.size();
    core::uint64 pos = sfx_off + 8;
    if (pos > file_size) return false;

    stream.seek(static_cast<core::int64>(sfx_off), io::SeekOrigin::Begin);
    core::byte sig[8];
    if (stream.read(sig, 8) != 8) return false;
    if (std::memcmp(sig, format::rar5_signature(), 8) != 0) return false;

    bool saw_end = false;
    while (stream.tell() < file_size) {
        core::uint64 head_start = stream.tell();
        core::uint64 type = 0, flags = 0, data_sz = 0;
        std::vector<core::byte> body;
        if (format::HeaderReader::read_block_raw(stream, type, flags, body, data_sz) !=
            format::HeaderResult::Ok)
            return false;
        if (type == format::HEAD_ENDARC) {
            saw_end = true;
            break;
        }
        if (data_sz > 0) {
            core::uint64 cur = stream.tell(); // <= file_size by the loop condition
            // data_sz > file_size - cur also rejects the uint64 wrap in
            // cur + data_sz that used to allow backward seeks and an
            // unbounded block rescan (report M3).
            if (data_sz > file_size - cur) return false;
            if (!stream.seek(static_cast<core::int64>(cur + data_sz), io::SeekOrigin::Begin))
                return false;
        }
        if (stream.tell() <= head_start) return false;
    }
    return saw_end;
}

// Return true if any sibling of arc_path carries a .rev extension whose name
// also belongs to the same volume-set stem — the external recovery-volume
// case (spec §4.7). The stem prefix keeps unrelated .rev files elsewhere in
// the directory from hijacking an inline-RR repair.
bool check_has_rev_files(const std::filesystem::path& arc_path) {
    std::filesystem::path dir = arc_path.parent_path();
    if (dir.empty()) dir = std::filesystem::current_path();
    std::wstring base = arc_path.stem().wstring();
    // New-numbering sets carry the stem inside the .partNN name.
    size_t pdot = base.rfind(L".part");
    if (pdot != std::wstring::npos) base = base.substr(0, pdot);
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (e.path().extension() != ".rev") continue;
        std::wstring name = e.path().stem().wstring();
        size_t rd = name.rfind(L".part");
        if (rd != std::wstring::npos) name = name.substr(0, rd);
        // The stem must match exactly or be followed by a separator dot: an
        // unanchored prefix match let "out.rev" hijack "out2.rar" (and
        // "backup-old.rar" match "backup.rar") and route repair into an
        // unrelated rev set (report L3).
        if (name == base || (name.size() > base.size() && name.compare(0, base.size(), base) == 0 &&
                             name[base.size()] == L'.'))
            return true;
    }
    return false;
}

// Return true if any file matching arc_path's stem has a .rev or .rNN suffix,
// signalling the multi-volume .rev recovery case we do not yet handle.
bool has_rev_siblings(const std::filesystem::path& arc_path) {
    std::filesystem::path dir = arc_path.parent_path();
    if (dir.empty()) dir = std::filesystem::current_path();
    std::wstring base_stem = arc_path.stem().wstring();
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return false;
        std::wstring name = e.path().filename().wstring();
        if (name.rfind(base_stem, 0) == 0) {
            std::wstring suffix = name.substr(base_stem.size());
            if (suffix == L".rev") return true;
            if (suffix.size() == 4 && suffix[0] == L'.' && suffix[1] == L'r' &&
                std::isdigit(static_cast<unsigned char>(suffix[2])) &&
                std::isdigit(static_cast<unsigned char>(suffix[3])))
                return true;
        }
    }
    return false;
}

// Reassemble the archive around a still-intact RR block, replacing the
// protected prefix with the caller's repaired bytes.
bool splice_repair(const std::filesystem::path& arc_path,
                   const std::vector<core::byte>& repaired_prefix, core::uint64 rr_header_offset,
                   core::uint64 rr_len) {
    io::FileStream in;
    if (!in.open(arc_path, io::FileMode::ReadOnly)) return false;
    core::uint64 orig_size = in.size();
    if (rr_header_offset + rr_len > orig_size) {
        in.close();
        return false;
    }
    if (repaired_prefix.size() != rr_header_offset) {
        in.close();
        return false;
    }

    std::filesystem::path tmp_path = recovery_temp_path(arc_path, "rep_tmp");
    io::FileStream out;
    if (!out.open(tmp_path, io::FileMode::CreateNew)) {
        in.close();
        return false;
    }

    // 1. Repaired protected prefix.
    if (!repaired_prefix.empty()) {
        if (out.write(repaired_prefix.data(), repaired_prefix.size()) != repaired_prefix.size()) {
            in.close();
            out.close();
            std::filesystem::remove(tmp_path);
            return false;
        }
    }
    // 2. Verbatim RR block copied from the original file.
    if (!copy_stream_region(in, out, rr_header_offset, rr_len)) {
        in.close();
        out.close();
        std::filesystem::remove(tmp_path);
        return false;
    }
    // 3. Whatever follows the RR block in the original (End Of Archive block).
    core::uint64 tail_off = rr_header_offset + rr_len;
    core::uint64 tail_len = orig_size - tail_off;
    if (tail_len > 0) {
        if (!copy_stream_region(in, out, tail_off, tail_len)) {
            in.close();
            out.close();
            std::filesystem::remove(tmp_path);
            return false;
        }
    }
    in.close();
    out.close();
    return atomic_replace(tmp_path, arc_path);
}

// Reassemble the archive around a newly-computed RR block, writing the
// protected prefix, the new RR service block, and the EndArc terminator.
bool splice_repair_with_rr(const std::filesystem::path& arc_path,
                           const std::vector<core::byte>& repaired_prefix,
                           const std::vector<core::byte>& new_rr_data_area, core::uint32 rec_pct) {
    std::filesystem::path tmp_path = recovery_temp_path(arc_path, "rep_tmp");
    io::FileStream out;
    if (!out.open(tmp_path, io::FileMode::CreateNew)) return false;

    if (!repaired_prefix.empty()) {
        if (out.write(repaired_prefix.data(), repaired_prefix.size()) != repaired_prefix.size()) {
            out.close();
            std::filesystem::remove(tmp_path);
            return false;
        }
    }

    if (!write_rr_service_block(out, new_rr_data_area, rec_pct)) {
        out.close();
        std::filesystem::remove(tmp_path);
        return false;
    }

    format::EndArcBlock eb;
    eb.end_flags = 0;
    if (!format::HeaderWriter::write_end_block(out, eb)) {
        out.close();
        std::filesystem::remove(tmp_path);
        return false;
    }

    out.close();
    return atomic_replace(tmp_path, arc_path);
}

} // namespace

bool RecoveryWriter::has_rev_files(const std::filesystem::path& arc_path) {
    return check_has_rev_files(arc_path);
}

// —————————————————————————————————————————————————————————————————————————————
// Public API
// —————————————————————————————————————————————————————————————————————————————

RecoveryParams RecoveryWriter::calculate_params(core::uint64 protected_size, core::uint32 percent) {
    // Legacy sector-based sizing kept for RecoveryManager unit tests. Live RR
    // writing uses compute_geometry() above.
    if (percent == 0) percent = 3;
    if (percent > 100) percent = 100;
    return RecoveryManager::calculate_params(protected_size, percent);
}

std::vector<core::byte> RecoveryWriter::generate_parity(const core::byte* protected_data,
                                                        const RecoveryParams& params) {
    return RecoveryManager::generate_parity(protected_data, params);
}

bool RecoveryWriter::write_rr_header(io::FileStream& dest, const std::vector<core::byte>& parity) {
    // Legacy overload: emit an RR service block with the caller's opaque data
    // area verbatim. Kept for source compatibility; not used by the current
    // add_recovery_record path.
    return write_rr_service_block(dest, parity, 0);
}

bool RecoveryWriter::write_rev_volumes(const std::filesystem::path& arc_path,
                                       core::uint32 count_or_percent, bool is_percent,
                                       unsigned threads) {
    // ———— .rev recovery volumes (INTEGRITY_WRITE_SIDE.md §4.7) ————————————————
    // Every data volume is one RS16 data shard; each .rev file carries one
    // parity shard. Layout per .rev file:
    //
    // QO-locator safety: this function treats each data volume as an opaque
    // stream of bytes for RS16 encoding. It never reads a MainBlock or emits
    // a locator. The QO/RR locator size-mismatch bug fixed in
    // add_recovery_record does NOT apply here.
    //   +0  "Rar!\x1aRev"   +8  HeaderCRC32 = CRC32(HeaderSize_le32 || body)
    //   +12 HeaderSize LE32 (body length)   +16 body   +16+N parity payload
    //   body: u8 version=1, u16 DataCount, u16 RecCount, u16 RecNum (=ND+j),
    //         u32 RevCRC (CRC32 of the payload), then per data volume
    //         u64 FileSize + u32 CRC32.
    namespace fs = std::filesystem;
    constexpr core::byte REV_SIGN[8] = {'R', 'a', 'r', '!', 0x1a, 'R', 'e', 'v'};
    auto push16 = [](std::vector<core::byte>& v, core::uint32 x) {
        v.push_back(static_cast<core::byte>(x & 0xFF));
        v.push_back(static_cast<core::byte>((x >> 8) & 0xFF));
    };
    auto push32 = [](std::vector<core::byte>& v, core::uint32 x) {
        for (int b = 0; b < 4; ++b) v.push_back(static_cast<core::byte>((x >> (8 * b)) & 0xFF));
    };
    auto push64 = [](std::vector<core::byte>& v, core::uint64 x) {
        for (int b = 0; b < 8; ++b) v.push_back(static_cast<core::byte>((x >> (8 * b)) & 0xFF));
    };

    // Enumerate the volume chain in both numbering styles and keep the longer
    // walk. The caller may pass the user-facing name (out.rar) while only the
    // numbered files (out.partNN.rar) exist on disk, so the exists() check
    // runs on each style's first-volume normalization.
    constexpr size_t MAX_CHAIN = 65535;
    std::vector<fs::path> chain;
    bool chain_new_numbering = false;
    for (int mode = 0; mode < 2; ++mode) {
        bool new_numbering = (mode == 0);
        fs::path first = archive::volume::vol_name_to_first_name(arc_path, !new_numbering);
        if (!fs::exists(first)) {
            if (mode == 1 && chain.empty() && fs::exists(arc_path))
                first = arc_path;
            else
                continue;
        }
        std::vector<fs::path> c;
        fs::path cur = first;
        while (fs::exists(cur) && c.size() < MAX_CHAIN) {
            c.push_back(cur);
            fs::path nxt = archive::volume::next_volume_name(cur, !new_numbering);
            if (nxt == cur) break;
            cur = nxt;
        }
        if (c.size() > chain.size()) {
            chain = std::move(c);
            chain_new_numbering = new_numbering;
        }
    }
    const core::uint32 nd = static_cast<core::uint32>(chain.size());
    if (nd < 2) return false;

    // Parity count from percentage or absolute volume count
    core::uint64 nr = 0;
    if (is_percent) {
        core::uint64 pct =
            count_or_percent < 1 ? 1 : (count_or_percent > 1000 ? 1000 : count_or_percent);
        nr = (static_cast<core::uint64>(nd) * pct + 99) / 100;
    } else {
        nr = count_or_percent < 1 ? 1 : count_or_percent;
    }
    if (nr == 0) nr = 1;
    if (nd + nr > MAX_CHAIN) nr = MAX_CHAIN - nd;
    if (nr == 0) return false;

    // Measure + CRC every data volume (streaming, 64 KiB steps).
    std::vector<core::uint64> vol_size(nd, 0);
    std::vector<core::uint32> vol_crc(nd, 0);
    for (core::uint32 i = 0; i < nd; ++i) {
        io::FileStream f;
        if (!f.open(chain[i], io::FileMode::ReadOnly)) return false;
        vol_size[i] = f.size();
        crypto::Crc32 crc;
        core::byte buf[65536];
        for (;;) {
            size_t got = f.read(buf, sizeof(buf));
            if (got > 0) crc.update(buf, got);
            if (got < sizeof(buf)) break;
        }
        vol_crc[i] = crc.get();
    }
    core::uint64 max_vol = 0;
    for (core::uint32 i = 0; i < nd; ++i) max_vol = std::max(max_vol, vol_size[i]);
    // RS16 consumes 16-bit words; the shard is processed zero-padded to an
    // even byte count.
    core::uint64 shard = (max_vol + 1) & ~core::uint64(1);
    if (shard == 0) return false;

    constexpr size_t REV_HEADER_MIN_BODY = 11; // version + DC + RC + RecNum + RevCRC
    const size_t body_size = REV_HEADER_MIN_BODY + 12u * nd;
    if (body_size > 0x100000u) return false; // HeaderSize cap per spec

    // Parity workspace cap: nr × 1 MiB buffers are allocated up front and
    // persist for the whole fold. Beyond this bound the request is hostile
    // (an absurd -rv percentage on a huge chain) — fail honestly instead of
    // throwing bad_alloc out of the writer (v1.21.1 fix).
    if (static_cast<core::uint64>(nr) * (1ull << 20) > (1ull << 30)) return false;

    ReedSolomon16 rs;
    if (!rs.init(nd, static_cast<core::uint32>(nr))) return false;

    // Stream the volumes through the RS encoder, one even-sized chunk at a
    // time, appending parity to each .rev file while its CRC accumulates.
    const size_t CHUNK = 1u << 20; // 1 MiB, even
    std::vector<std::vector<core::byte>> parity(static_cast<size_t>(nr),
                                                std::vector<core::byte>(CHUNK));
    std::vector<core::byte> data(CHUNK);
    std::vector<crypto::Crc32> rev_crc(static_cast<size_t>(nr));

    struct RevOut {
        io::FileStream file;
        std::filesystem::path tmp_path;
        std::filesystem::path final_path;
        bool published{false}; // this run renamed the final into place
    };
    std::vector<RevOut> outs(static_cast<size_t>(nr));

    struct RevCleanupGuard {
        std::vector<RevOut>* outs{nullptr};
        bool disarmed{false};
        ~RevCleanupGuard() {
            if (!disarmed && outs) {
                for (auto& o : *outs) {
                    o.file.close();
                    std::error_code ec;
                    if (!o.tmp_path.empty()) std::filesystem::remove(o.tmp_path, ec);
                    // Only remove final_path when THIS run published it: an
                    // incomplete set must not leave fresh partial shards, but
                    // a pre-existing .rev from an earlier successful run is
                    // the only recovery data the user may have (v1.21.1 fix).
                    if (o.published && !o.final_path.empty()) {
                        std::filesystem::remove(o.final_path, ec);
                    }
                }
            }
        }
    } rev_cleanup{&outs};

    for (core::uint32 j = 0; j < nr; ++j) {
        // .rev files restart the numbering: out.part01.rar -> out.part01.rev
        std::filesystem::path base = chain[0];
        std::string name = base.filename().string();
        std::filesystem::path dir = base.parent_path();
        std::string rev_name;
        bool legacy_style = true;
        if (chain_new_numbering) {
            size_t dot = name.rfind(".part");
            size_t d = (dot == std::string::npos) ? std::string::npos : name.find('.', dot + 5);
            size_t width = (dot == std::string::npos || d == std::string::npos) ? 0 : d - (dot + 5);
            if (width >= 1 && width <= 8) {
                legacy_style = false;
                core::uint32 num = j + 1;
                std::string digits = std::to_string(num);
                while (digits.size() < width) digits.insert(digits.begin(), '0');
                rev_name = name.substr(0, dot + 5) + digits + ".rev";
            }
            // else: unexpected name shape - fall through to legacy naming
            // rather than padding toward a wrapped npos width until OOM
            // (report L6).
        }
        if (legacy_style) {
            // Legacy: out.rar / out.rNN -> out.rNN.rev (2-digit counter)
            size_t dot = name.rfind('.');
            std::string stem = (dot == std::string::npos) ? name : name.substr(0, dot);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02u", static_cast<core::uint32>(j));
            rev_name = stem + ".r" + buf + ".rev";
        }
        outs[j].final_path = dir.empty() ? std::filesystem::path(rev_name) : dir / rev_name;
        // Unique temp name (L8 policy, see recovery_temp_path): the old fixed
        // ".rev_tmp" name plus CreateAlways followed a pre-planted symlink.
        outs[j].tmp_path = recovery_temp_path(outs[j].final_path, "rev_tmp");
        if (!outs[j].file.open(outs[j].tmp_path, io::FileMode::CreateNew)) {
            return false;
        }
        // Reserve the header area up front; the payload rounds append behind
        // it and the real header (with the payload CRCs) is patched in below.
        std::vector<core::byte> placeholder(16 + body_size, 0);
        if (outs[j].file.write(placeholder.data(), placeholder.size()) != placeholder.size()) {
            return false;
        }
    }

    core::uint64 processed = 0;
    bool ok = true;

    // Same fold-parallelism as add_recovery_record: parity[j] buffers are
    // disjoint and update_ecc only reads shared state.
    std::unique_ptr<core::ThreadPool> pool;
    if (threads > 1 && nr > 1) {
        pool = std::make_unique<core::ThreadPool>(
            std::min<unsigned>(threads, static_cast<unsigned>(nr)));
    }

    while (processed < shard && ok) {
        size_t chunk = static_cast<size_t>(std::min<core::uint64>(CHUNK, shard - processed));
        for (core::uint32 j = 0; j < nr; ++j) {
            std::memset(parity[j].data(), 0, chunk);
        }
        for (core::uint32 i = 0; i < nd && ok; ++i) {
            // Clamp against this volume's size: the last (typically smaller)
            // volume runs out before processed reaches shard. Without the
            // clamp vol_size[i] - processed underflows and take stays a full
            // chunk, making the short read below abort the whole operation.
            core::uint64 remaining = processed < vol_size[i] ? vol_size[i] - processed : 0;
            size_t take = static_cast<size_t>(std::min<core::uint64>(chunk, remaining));
            io::FileStream f;
            if (!f.open(chain[i], io::FileMode::ReadOnly)) {
                ok = false;
                break;
            }
            f.seek(static_cast<core::int64>(processed), io::SeekOrigin::Begin);
            if (take > 0 && f.read(data.data(), take) != take) {
                ok = false;
                break;
            }
            if (take < chunk) std::memset(data.data() + take, 0, chunk - take);
            f.close();
            if (pool) {
                core::parallel_for(*pool, 0, nr, [&](size_t j) {
                    rs.update_ecc(i, static_cast<core::uint32>(j), data.data(), parity[j].data(),
                                  chunk);
                });
            } else {
                for (core::uint32 j = 0; j < nr; ++j) {
                    rs.update_ecc(i, j, data.data(), parity[j].data(), chunk);
                }
            }
        }
        if (!ok) break;
        for (core::uint32 j = 0; j < nr; ++j) {
            if (outs[j].file.write(parity[j].data(), chunk) != chunk) {
                ok = false;
                break;
            }
            rev_crc[j].update(parity[j].data(), chunk);
        }
        processed += chunk;
    }

    if (ok) {
        // Header goes in front of the payload; write it now that the payload
        // CRCs are known, then flip the temp files into place.
        for (core::uint32 j = 0; j < nr && ok; ++j) {
            std::vector<core::byte> body;
            body.reserve(body_size);
            body.push_back(1); // Version
            push16(body, nd);
            push16(body, static_cast<core::uint32>(nr));
            push16(body, nd + j);
            push32(body, rev_crc[j].get());
            for (core::uint32 i = 0; i < nd; ++i) {
                push64(body, vol_size[i]);
                push32(body, vol_crc[i]);
            }

            // HeaderCRC32 = CRC32 over the 4-byte HeaderSize field + body.
            crypto::Crc32 hc;
            core::byte hs[4];
            for (int b = 0; b < 4; ++b)
                hs[b] = static_cast<core::byte>((body_size >> (8 * b)) & 0xFF);
            hc.update(hs, 4);
            hc.update(body.data(), body.size());

            std::vector<core::byte> hdr;
            hdr.reserve(16 + body.size());
            hdr.insert(hdr.end(), REV_SIGN, REV_SIGN + 8);
            push32(hdr, hc.get());
            hdr.insert(hdr.end(), hs, hs + 4);
            hdr.insert(hdr.end(), body.begin(), body.end());

            if (!outs[j].file.seek(0, io::SeekOrigin::Begin) ||
                outs[j].file.write(hdr.data(), hdr.size()) != hdr.size()) {
                ok = false;
                break;
            }
            outs[j].file.close();
            // Atomic publish: the replace is a single rename against the
            // existing target, so a pre-existing .rev is never destroyed if
            // the rename fails (v1.21.1 fix; no remove-then-rename window).
            if (!atomic_replace(outs[j].tmp_path, outs[j].final_path)) {
                ok = false;
                break;
            }
            outs[j].published = true;
        }
    }

    if (!ok) {
        return false;
    }
    rev_cleanup.disarmed = true;
    return true;
}

bool RecoveryWriter::patch_locator_recovery_offset(const std::filesystem::path& /*tmp_path*/,
                                                   core::uint64 /*main_start*/,
                                                   core::uint64 /*rr_offset*/) {
    // Handled inline by add_recovery_record() via a targeted rewrite of the
    // main header at the known main_start offset.
    return true;
}

bool RecoveryWriter::add_recovery_record(const std::filesystem::path& arc_path,
                                         core::uint32 percent, unsigned threads) {
    if (!std::filesystem::exists(arc_path)) return false;

    archive::ArchiveReader reader;
    if (!reader.open(arc_path)) return false;
    if (reader.is_locked()) {
        reader.close();
        return false;
    }

    core::uint64 sfx_off = reader.sfx_offset();

    // Materialise a fresh tmp archive containing SFX + signature + updated
    // main header + all non-RR file/service entries. The current file offset
    // after that write IS the `archive_size` recorded inside each parity
    // shard's structured header.
    std::filesystem::path tmp_path = recovery_temp_path(arc_path, "rr_tmp");
    io::FileStream out;
    if (!out.open(tmp_path, io::FileMode::CreateNew)) {
        reader.close();
        return false;
    }
    // Exception-safe guard: any throw (bad_alloc from parity vectors, RS16
    // init, etc.) will close + remove the tmp file before propagating.
    TempFileCleanupGuard tmp_guard{&out, tmp_path};

    io::FileStream src_for_copy;
    if (!src_for_copy.open(arc_path, io::FileMode::ReadOnly)) {
        reader.close();
        return false; // tmp_guard destructor will clean up out + tmp_path
    }

    if (sfx_off > 0) {
        if (!copy_stream_region(src_for_copy, out, 0, sfx_off)) {
            src_for_copy.close();
            reader.close();
            return false;
        }
    }
    if (!format::HeaderWriter::write_signature(out)) {
        src_for_copy.close();
        reader.close();
        return false;
    }
    core::uint64 main_start = sfx_off + 8;

    format::MainBlock mb = reader.main_block();
    mb.arc_flags |= format::MHFL_PROTECT;
    // Strip QO locator: adding RR is a mutation; per the RAR5 spec, mutations
    // always strip the QuickOpen service block and its locator. Preserving the
    // QO offset here would be wrong anyway: the combined QO+RR locator extra
    // record is 10 bytes larger than the QO-only record, shifting all
    // subsequent entries in the rewritten archive and making the stored offset
    // point to the wrong location, which WinRAR detects as a corrupt header.
    // See headers.hpp MainBlock::locator_qo_offset for sentinel semantics.
    mb.locator_qo_offset = -1;
    mb.has_locator = true;
    mb.locator_rr_offset = 0; // patched below to the actual RR offset once known
    if (!format::HeaderWriter::write_main_block(out, mb)) {
        src_for_copy.close();
        reader.close();
        return false;
    }

    for (const auto& entry : reader.entries()) {
        // Drop any pre-existing RR service block; we're replacing it.
        // Also drop the QO service block: mutations always strip QuickOpen per
        // spec (the regenerated locator would point to the wrong location in
        // the rewritten archive anyway, as the main header grows by 10 bytes
        // when going from a QO-only to a combined QO+RR locator extra record).
        if (entry.header.is_service && entry.header.service_type == "RR") continue;
        if (entry.header.is_service && entry.header.service_type == "QO") continue;
        if (!copy_stream_region(src_for_copy, out, entry.header_offset,
                                entry.header_size + entry.data_size)) {
            src_for_copy.close();
            reader.close();
            return false;
        }
    }
    src_for_copy.close();
    reader.close();

    core::uint64 rr_header_offset = out.tell();
    core::uint64 archive_size = rr_header_offset;

    // Stamp the real locator NOW, before the parity fold: the fold covers the
    // whole protected prefix including this main header, so the bytes on disk
    // when the parity is computed must be the final ones. Patching after the
    // append (the old order) changed the CRC32 and locator body bytes under
    // already-computed parity, making every reconstruction of the last data
    // shard corrupt exactly those words (found by the padded-prefix repair
    // regression; M2 follow-up). Fixed-width locator vints keep the header
    // size stable, so rr_header_offset stays valid.
    mb.locator_rr_offset = static_cast<core::int64>(rr_header_offset - main_start);
    if (!out.seek(static_cast<core::int64>(main_start), io::SeekOrigin::Begin)) {
        return false;
    }
    if (!format::HeaderWriter::write_main_block(out, mb)) {
        return false;
    }

    RecoveryGeometry g = compute_geometry(archive_size, percent);
    if (g.NR == 0 || g.D == 0 || g.group_count == 0) {
        return false;
    }

    // Reopen the tmp file for read+write so we can stream the protected
    // prefix one shard at a time into the RS encoder without buffering the
    // whole archive. Peak transient memory drops from
    //   O(archive_size + D * group_count + NR * group_count)
    // to
    //   O((1 + NR) * group_count)
    // For a 1 GiB archive with 10 % RR (D = 200, NR ≈ 20) that's ~5 MiB per
    // shard × 21 buffers ≈ 105 MiB, down from ~2.1 GiB.
    out.flush();
    out.close();
    if (!out.open(tmp_path, io::FileMode::ReadWrite)) {
        return false;
    }

    ReedSolomon16 rs;
    if (!rs.init(g.D, g.NR)) {
        return false;
    }

    std::vector<std::vector<core::byte>> parity(
        g.NR, std::vector<core::byte>(static_cast<size_t>(g.group_count), 0));

    // Byte-column tiling: with (NR * group_count) mostly-exceeding LLC, the
    // per-shard sweep of all NR parity buffers re-streams the whole parity
    // set from DRAM for every data shard. Processing ~256 KiB column stripes
    // for all (i, j) pairs keeps the active parity window (NR * stripe_len)
    // cache-resident, cutting parity memory traffic to ~2x the parity size.
    // Stripe length stays even so update_ecc's read_le16 blocks stay aligned.
    constexpr size_t STRIPE = 256u * 1024u;
    std::vector<core::byte> scratch(STRIPE);

    // The inner fold loop parallelizes cleanly: every parity shard j has its
    // own buffer (parity[j]) and update_ecc only reads the shared matrix and
    // scratch stripe. Output bytes are identical to the serial run because
    // each fold is an independent GF(2^16) XOR accumulation.
    std::unique_ptr<core::ThreadPool> pool;
    if (threads > 1 && g.NR > 1) {
        pool = std::make_unique<core::ThreadPool>(std::min<unsigned>(threads, g.NR));
    }

    for (core::uint64 base = 0; base < g.group_count; base += STRIPE) {
        size_t stripe_len =
            static_cast<size_t>(std::min<core::uint64>(STRIPE, g.group_count - base));
        for (core::uint32 i = 0; i < g.D; ++i) {
            core::uint64 shard_off = static_cast<core::uint64>(i) * g.group_count;
            core::uint64 avail = 0;
            if (shard_off < archive_size) {
                avail = std::min<core::uint64>(g.group_count, archive_size - shard_off);
            }
            // Zero-fill scratch, then overlay whatever's actually there. Data
            // shards past archive_size (only the last one in practice) are pure
            // padding, which matches how compute_geometry rounds group_count up
            // to even and covers up to D*group_count total bytes.
            std::memset(scratch.data(), 0, stripe_len);
            if (base < avail) {
                core::uint64 take = std::min<core::uint64>(stripe_len, avail - base);
                if (!out.seek(static_cast<core::int64>(shard_off + base), io::SeekOrigin::Begin)) {
                    return false;
                }
                if (out.read(scratch.data(), static_cast<size_t>(take)) != take) {
                    return false;
                }
            }
            if (pool) {
                core::parallel_for(*pool, 0, g.NR, [&](size_t j) {
                    rs.update_ecc(i, static_cast<core::uint32>(j), scratch.data(),
                                  parity[j].data() + base, stripe_len);
                });
            } else {
                for (core::uint32 j = 0; j < g.NR; ++j) {
                    rs.update_ecc(i, j, scratch.data(), parity[j].data() + base, stripe_len);
                }
            }
        }
    }

    // Position at end of archive prefix for appending RR service block.
    if (!out.seek(static_cast<core::int64>(archive_size), io::SeekOrigin::Begin)) {
        return false;
    }

    // Serialise the shards back-to-back.
    auto data_area_size =
        calculate_parity_buffer_size(g.NR, static_cast<core::uint32>(g.shard_size));
    if (!data_area_size) {
        return false;
    }
    std::vector<core::byte> data_area;
    data_area.reserve(static_cast<size_t>(*data_area_size));
    for (core::uint32 j = 0; j < g.NR; ++j) {
        auto s = build_shard(j, g, parity[j].data(), static_cast<size_t>(g.group_count));
        data_area.insert(data_area.end(), s.begin(), s.end());
    }

    if (!write_rr_service_block(out, data_area, g.pct)) {
        return false;
    }

    // End-of-archive block.
    format::EndArcBlock eb;
    eb.end_flags = 0;
    if (!format::HeaderWriter::write_end_block(out, eb)) {
        return false;
    }

    out.close();
    tmp_guard.commit();
    return atomic_replace(tmp_path, arc_path);
}

// —————————————————————————————————————————————————————————————————————————————
// External .rev recovery (spec §4.7): reconstruct missing/corrupt data
//    volumes from the parity shards. Every data volume is one RS16 shard,
//    missing slots are substituted by valid .rev files, and the
//    reconstruction runs in even-sized rounds.
bool RecoveryWriter::repair_rev_volumes(const std::filesystem::path& arc_path) {
    namespace fs = std::filesystem;
    fs::path dir = arc_path.parent_path();
    if (dir.empty()) dir = fs::current_path();

    // Collect .rev siblings and parse their headers. The first valid header
    // carries the authoritative (DataCount, RecCount) + per-volume table.
    struct RevInfo {
        fs::path path;
        core::uint32 rec_num{0};     // absolute slot: DataCount + index
        core::uint32 payload_crc{0}; // expected CRC of the payload region
        core::uint64 payload_off{0}; // payload start in the file
        bool valid{false};
    };
    std::vector<RevInfo> revs;
    core::uint32 nd = 0, nr = 0;
    std::vector<core::uint64> table_size;
    std::vector<core::uint32> table_crc;

    // Stem-anchor the candidate set: repair must only adopt recovery volumes
    // belonging to THIS archive. An unanchored scan let a sibling set's .rev
    // files supply the authoritative table, which then failed every real
    // volume's CRC and renamed the valid set to .bad (v1.21.1 fix; same
    // anchor rule as check_has_rev_files, report L3).
    std::wstring arc_stem = arc_path.stem().wstring();
    size_t pdot = arc_stem.rfind(L".part");
    if (pdot != std::wstring::npos) arc_stem = arc_stem.substr(0, pdot);
    auto rev_belongs = [&](const fs::path& p) -> bool {
        std::wstring name = p.stem().wstring();
        size_t rd = name.rfind(L".part");
        if (rd != std::wstring::npos) {
            name = name.substr(0, rd);
        } else {
            // Legacy parity names carry a .rNN-style slot suffix.
            size_t d = name.rfind(L'.');
            if (d != std::wstring::npos && name.size() - d == 4 &&
                std::isalpha(static_cast<unsigned char>(name[d + 1])) &&
                std::isdigit(static_cast<unsigned char>(name[d + 2])) &&
                std::isdigit(static_cast<unsigned char>(name[d + 3]))) {
                name = name.substr(0, d);
            }
        }
        return name == arc_stem ||
               (name.size() > arc_stem.size() &&
                name.compare(0, arc_stem.size(), arc_stem) == 0 && name[arc_stem.size()] == L'.');
    };

    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (e.path().extension() != ".rev") continue;
        if (!rev_belongs(e.path())) continue;
        io::FileStream f;
        if (!f.open(e.path(), io::FileMode::ReadOnly)) continue;
        core::byte head[16];
        if (f.read(head, 16) != 16) continue;
        static const core::byte SIGN[8] = {'R', 'a', 'r', '!', 0x1a, 'R', 'e', 'v'};
        if (std::memcmp(head, SIGN, 8) != 0) continue;
        core::uint32 stored_crc = core::read_le32(head + 8);
        core::uint32 hsize = core::read_le32(head + 12);
        // Minimum 11: version(1) + dc(2) + rc(2) + rn(2) + payload_crc(4).
        // The old `> 5` bound let a crafted 6..10-byte body past the guard
        // and read past the heap block below (report M1).
        if (hsize < 11 || hsize > 0x100000u) continue;
        std::vector<core::byte> body(hsize);
        if (f.read(body.data(), hsize) != hsize) continue;
        crypto::Crc32 hc;
        hc.update(head + 12, 4);
        hc.update(body.data(), body.size());
        if (hc.get() != stored_crc) continue;
        if (body[0] != 1) continue; // Version
        core::uint32 dc = core::read_le16(body.data() + 1);
        core::uint32 rc = core::read_le16(body.data() + 3);
        core::uint32 rn = core::read_le16(body.data() + 5);
        // A .rev file always carries a parity slot: RecNum ∈ [dc, dc+rc).
        // Values below dc claim a *data* slot; admitting them let a crafted
        // .rev overwrite the data-slot validity flags in the mapping below
        // and route a NULL FileStream into the fold loop (report M1).
        if (dc == 0 || rc == 0 || rn < dc || rn >= dc + rc || dc + rc > 65535u) continue;
        if (!revs.empty() && (dc != nd || rc != nr)) continue; // set mismatch
        if (revs.empty()) {
            nd = dc;
            nr = rc;
            table_size.resize(nd, 0);
            table_crc.resize(nd, 0);
            size_t off = 11;
            if (off + 12u * nd > body.size()) continue;
            for (core::uint32 i = 0; i < nd; ++i) {
                table_size[i] = core::read_le64(body.data() + off);
                table_crc[i] = core::read_le32(body.data() + off + 8);
                off += 12;
            }
        }
        RevInfo ri;
        ri.path = e.path();
        ri.rec_num = rn;
        ri.payload_crc = core::read_le32(body.data() + 7);
        ri.payload_off = 16 + hsize;
        // Validate the payload CRC before trusting this parity shard.
        f.seek(static_cast<core::int64>(ri.payload_off), io::SeekOrigin::Begin);
        crypto::Crc32 pc;
        core::byte buf[65536];
        for (;;) {
            size_t got = f.read(buf, sizeof(buf));
            if (got > 0) pc.update(buf, got);
            if (got < sizeof(buf)) break;
        }
        ri.valid = pc.get() == ri.payload_crc;
        revs.push_back(ri);
    }
    if (revs.empty() || nd == 0) return false;

    // Map data volumes to their slots by the number in the file name.
    // New numbering: base.partNN.rar -> slot NN-1. Legacy: base.rar -> 0,
    // base.rNN -> NN+1 (mirrors volume::next_volume_name's sequence).
    struct DataVol {
        fs::path path;
        bool present{false};
        bool valid{false};
        bool corrupt{false};
    };
    std::vector<DataVol> vols(nd);
    std::wstring sample_name;
    core::int64 sample_slot = -1;
    auto slot_for_name = [&](const std::wstring& name, bool& matched) -> core::int64 {
        matched = false;
        std::wstring base = arc_path.filename().wstring();
        // Strip the number + extension from the arc name to get the stem.
        std::wstring stem;
        size_t dot = base.rfind(L".part");
        if (dot != std::wstring::npos && base.find(L'.', dot + 5) != std::wstring::npos) {
            stem = base.substr(0, dot);
        } else {
            size_t d = base.rfind(L'.');
            stem = (d == std::wstring::npos) ? base : base.substr(0, d);
        }
        if (name.compare(0, stem.size(), stem) != 0) return -1;
        std::wstring tail = name.substr(stem.size()); // ".partNN.rar" / ".rar" / ".rNN"
        if (tail.rfind(L".part", 0) == 0) {
            size_t d2 = tail.find(L'.', 5);
            if (d2 == std::wstring::npos || d2 == 5) return -1;
            core::uint64 num = 0;
            for (size_t i = 5; i < d2; ++i) {
                if (!std::isdigit(static_cast<unsigned char>(tail[i]))) return -1;
                num = num * 10 + (tail[i] - L'0');
            }
            if (num == 0) return -1;
            matched = true;
            return static_cast<core::int64>(num) - 1;
        }
        if (tail == L".rar") {
            matched = true;
            return 0;
        }
        if (tail.size() == 4 && tail[0] == L'.' &&
            std::isalpha(static_cast<unsigned char>(tail[1])) &&
            std::isdigit(static_cast<unsigned char>(tail[2])) &&
            std::isdigit(static_cast<unsigned char>(tail[3]))) {
            core::uint32 num = (tail[2] - L'0') * 10 + (tail[3] - L'0');
            matched = true;
            return static_cast<core::int64>(num) + 1;
        }
        return -1;
    };
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::wstring name = e.path().filename().wstring();
        // Data-volume extensions: .rar, SFX volumes (.exe, any letter case),
        // and the legacy sequence .rNN/.sNN/.tNN (letter + two digits); skip
        // .rev files themselves. (Legacy admission fixed in v1.21.1 — the
        // scan previously ignored .rNN volumes entirely, so repair refused
        // undamaged legacy sets and could overwrite them from parity without
        // the .bad preservation.)
        std::string uext = e.path().extension().string();
        for (char& c : uext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (uext == ".rev") continue;
        bool legacy_vol = uext.size() == 4 && uext[0] == '.' &&
                          std::isalpha(static_cast<unsigned char>(uext[1])) &&
                          std::isdigit(static_cast<unsigned char>(uext[2])) &&
                          std::isdigit(static_cast<unsigned char>(uext[3]));
        if (uext != ".rar" && uext != ".exe" && !legacy_vol) continue;
        bool matched = false;
        core::int64 slot = slot_for_name(name, matched);
        if (!matched || slot < 0 || slot >= static_cast<core::int64>(nd)) continue;
        vols[static_cast<size_t>(slot)].path = e.path();
        vols[static_cast<size_t>(slot)].present = true;
        if (sample_slot < 0) {
            sample_name = name;
            sample_slot = slot;
        }
    }

    // Synthesize file names for missing slots from a found sibling's naming
    // pattern, so reconstructed volumes land under the canonical name.
    if (sample_slot >= 0) {
        auto name_for_slot = [&](core::uint32 slot) -> std::wstring {
            std::wstring s = sample_name;
            size_t dot = s.rfind(L".part");
            if (dot != std::wstring::npos) {
                size_t d = s.find(L'.', dot + 5);
                if (d != std::wstring::npos) {
                    size_t width = d - (dot + 5);
                    std::wstring digits = std::to_wstring(static_cast<core::uint64>(slot) + 1);
                    while (digits.size() < width) digits.insert(digits.begin(), L'0');
                    return s.substr(0, dot + 5) + digits + s.substr(d);
                }
            }
            size_t d = s.rfind(L'.');
            std::wstring stem = (d == std::wstring::npos) ? s : s.substr(0, d);
            std::wstring ext = (d == std::wstring::npos) ? L"" : s.substr(d);
            // Slot 0 uses the sibling's own extension so an SFX stem
            // (sample_name ends .exe) reconstructs as stem.exe, not .rar.
            if (slot == 0) return ext.empty() ? (stem + L".rar") : (stem + ext);
            wchar_t b[8];
            std::swprintf(b, sizeof(b) / sizeof(b[0]), L"%02u", slot - 1);
            wchar_t letter = (ext.size() >= 2 && ((ext[1] >= L'a' && ext[1] <= L'z') ||
                                                  (ext[1] >= L'A' && ext[1] <= L'Z')))
                                 ? ext[1]
                                 : L'r';
            return stem + L"." + letter + b;
        };
        for (core::uint32 i = 0; i < nd; ++i) {
            if (!vols[i].present) {
                vols[i].path = dir / name_for_slot(i);
            }
        }
    }

    // Validate present data volumes against the .rev header table.
    for (core::uint32 i = 0; i < nd; ++i) {
        if (!vols[i].present) continue;
        io::FileStream f;
        if (!f.open(vols[i].path, io::FileMode::ReadOnly)) {
            vols[i].present = false;
            continue;
        }
        crypto::Crc32 crc;
        core::byte buf[65536];
        for (;;) {
            size_t got = f.read(buf, sizeof(buf));
            if (got > 0) crc.update(buf, got);
            if (got < sizeof(buf)) break;
        }
        vols[i].valid = crc.get() == table_crc[i];
        if (!vols[i].valid) vols[i].corrupt = true;
    }

    core::uint32 missing = 0;
    for (core::uint32 i = 0; i < nd; ++i) {
        if (!vols[i].valid) missing++;
    }
    if (missing == 0) return true; // nothing to reconstruct

    // Foreign-table belt: if present volumes exist and EVERY one of them
    // fails its CRC, the adopted table almost certainly belongs to a
    // different set (or the .rev files are hostile). Refuse rather than
    // renaming valid volumes to .bad. A single matching volume proves the
    // table belongs to this set.
    {
        core::uint32 present_valid = 0, present_corrupt = 0;
        for (core::uint32 i = 0; i < nd; ++i) {
            if (vols[i].valid) present_valid++;
            else if (vols[i].corrupt) present_corrupt++;
        }
        if (present_corrupt > 0 && present_valid == 0) return false;
    }

    core::uint32 valid_revs = 0;
    for (auto& r : revs)
        if (r.valid) valid_revs++;
    if (missing > valid_revs) return false;

    // Resource cap (report M3): reconstruction cost is ~missing²·nd GF ops in
    // RS16::init plus `missing` MiB of scratch, and the .rev writer's nd is
    // the volume-chain length (up to 65535), so nd itself cannot be capped.
    // The inline-RR writer never needs more than 200 erasures (compute_geometry
    // caps D at 200); anything larger is hostile geometry and is refused
    // before the decoder allocates.
    if (missing > 200) return false;

    // Corrupt volumes get renamed out of the way before rewriting so their
    // original bytes are preserved under a .bad name.
    for (core::uint32 i = 0; i < nd; ++i) {
        if (!vols[i].corrupt) continue;
        fs::path bad = vols[i].path;
        bad += ".bad";
        std::error_code ec2;
        fs::remove(bad, ec2);
        fs::rename(vols[i].path, bad, ec2);
        if (ec2) return false;
    }

    // ReedSolomon16 decoder over the combined slot space.
    std::vector<core::byte> valid(nd + nr, 0);
    for (core::uint32 i = 0; i < nd; ++i) valid[i] = vols[i].valid ? 1 : 0;
    for (auto& r : revs)
        if (r.valid) valid[r.rec_num] = 1;
    ReedSolomon16 rs;
    try {
        if (!rs.init(nd, nr, valid.data())) return false;
    } catch (const std::bad_alloc&) {
        // Hostile geometry must degrade to a repair failure, not escape as
        // an uncaught exception and terminate the process (report M3).
        return false;
    }

    core::uint64 max_vol = 0;
    for (core::uint32 i = 0; i < nd; ++i) max_vol = std::max(max_vol, table_size[i]);
    core::uint64 shard = (max_vol + 1) & ~core::uint64(1);

    // Round-robin source per data slot: the volume itself when valid, else
    // the next valid .rev (ascending slot order, matching the decoder
    // matrix construction in rs16).
    std::vector<io::FileStream*> src(nd, nullptr);
    std::vector<io::FileStream*> owned;
    size_t j = nd;
    for (core::uint32 i = 0; i < nd; ++i) {
        if (valid[i]) {
            io::FileStream* f = new io::FileStream();
            if (!f->open(vols[i].path, io::FileMode::ReadOnly)) {
                delete f;
                continue;
            }
            src[i] = f;
            owned.push_back(f);
        } else {
            while (j < nd + nr && !valid[j]) j++;
            if (j >= nd + nr) break;
            RevInfo* ri = nullptr;
            for (auto& r : revs)
                if (r.rec_num == j && r.valid) ri = &r;
            if (!ri) break;
            io::FileStream* f = new io::FileStream();
            if (!f->open(ri->path, io::FileMode::ReadOnly)) {
                delete f;
                j++;
                continue;
            }
            f->seek(static_cast<core::int64>(ri->payload_off), io::SeekOrigin::Begin);
            src[i] = f;
            owned.push_back(f);
            j++;
        }
    }
    // Every data slot needs a source stream: valid slots read their own
    // volume, erased slots read a .rev parity file. An open failure above
    // leaves a NULL that would crash the fold loop below, so bail out
    // unconditionally instead of keying on the validity flags (report M1 —
    // a validity flag can only mark a stream available, never conjure one).
    for (core::uint32 i = 0; i < nd; ++i) {
        if (src[i] == nullptr) {
            for (auto* f : owned) {
                f->close();
                delete f;
            }
            return false;
        }
    }

    // Output streams for the missing volumes.
    std::vector<io::FileStream*> out(nd, nullptr);
    for (core::uint32 i = 0; i < nd; ++i) {
        if (valid[i]) continue;
        io::FileStream* f = new io::FileStream();
        if (!f->open(vols[i].path, io::FileMode::CreateAlways)) {
            delete f;
            // Release every handle this loop opened (plus the sources);
            // otherwise the freshly truncated volumes stay locked open on
            // Windows until process exit (v1.21.1 fix).
            for (core::uint32 k = 0; k < nd; ++k) {
                if (out[k]) {
                    out[k]->close();
                    delete out[k];
                    out[k] = nullptr;
                }
            }
            for (auto* g : owned) {
                g->close();
                delete g;
            }
            return false;
        }
        out[i] = f;
    }

    const size_t CHUNK = 1u << 20; // 1 MiB, even
    std::vector<core::byte> srcbuf(CHUNK);
    std::vector<std::vector<core::byte>> recon(missing, std::vector<core::byte>(CHUNK));
    core::uint64 processed = 0;
    bool ok = true;
    while (processed < shard && ok) {
        size_t chunk = static_cast<size_t>(std::min<core::uint64>(CHUNK, shard - processed));
        for (core::uint32 e = 0; e < missing; ++e) {
            std::memset(recon[e].data(), 0, chunk);
        }
        for (core::uint32 i = 0; i < nd; ++i) {
            size_t got = src[i]->read(srcbuf.data(), chunk);
            if (got < chunk) std::memset(srcbuf.data() + got, 0, chunk - got);
            for (core::uint32 e = 0; e < missing; ++e) {
                rs.update_ecc(i, e, srcbuf.data(), recon[e].data(), chunk);
            }
        }
        core::uint32 e = 0;
        for (core::uint32 i = 0; i < nd && ok; ++i) {
            if (valid[i]) continue;
            // Clamp: a smaller (last) volume's size sits below shard, so the
            // naive subtraction underflows once processed passes it and a
            // full oversized chunk gets written — the verify-CRC phase below
            // then fails, leaving oversized .bad volumes behind.
            core::uint64 remain = processed < table_size[i] ? table_size[i] - processed : 0;
            size_t take = static_cast<size_t>(std::min<core::uint64>(chunk, remain));
            if (take > 0 && out[i]->write(recon[e].data(), take) != take) ok = false;
            e++;
        }
        processed += chunk;
    }

    for (auto* f : owned) {
        f->close();
        delete f;
    }
    for (core::uint32 i = 0; i < nd; ++i) {
        if (out[i]) {
            out[i]->close();
            delete out[i];
        }
    }
    if (!ok) return false;

    // Verify the reconstructed volumes against the table CRCs.
    for (core::uint32 i = 0; i < nd; ++i) {
        if (valid[i]) continue;
        io::FileStream f;
        if (!f.open(vols[i].path, io::FileMode::ReadOnly)) return false;
        crypto::Crc32 crc;
        core::byte buf[65536];
        for (;;) {
            size_t got = f.read(buf, sizeof(buf));
            if (got > 0) crc.update(buf, got);
            if (got < sizeof(buf)) break;
        }
        if (crc.get() != table_crc[i]) return false;
    }
    return true;
}

bool RecoveryWriter::repair(const std::filesystem::path& arc_path) {
    // External .rev volumes: reconstruct missing/corrupt data volumes from
    // the parity shards (spec §4.7). If .rev files exist for this archive stem,
    // reconstruct even if arc_path itself is one of the missing volumes.
    if (has_rev_files(arc_path)) return repair_rev_volumes(arc_path);

    if (!std::filesystem::exists(arc_path)) return false;

    // Old-numbering multi-volume sets without .rev files: inline RR repair
    // does not apply; refuse rather than pretending.
    if (has_rev_siblings(arc_path)) return false;

    core::uint64 sfx_off = 0;
    {
        archive::ArchiveReader reader;
        if (reader.open(arc_path)) sfx_off = reader.sfx_offset();
    }

    RrLocation rr;
    {
        io::FileStream f;
        if (!f.open(arc_path, io::FileMode::ReadOnly)) return false;
        rr = find_rr(f, sfx_off);
        f.close();
    }
    if (!rr.found) return false;

    // Interpret the RR data area. If it doesn't look like the spec layout the
    // record was written by some third party we don't yet grok; refuse
    // honestly rather than misparsing random parity bytes.
    if (rr.raw_data.size() < SHARD_STATE_ARR + 8) return false;
    if (std::memcmp(rr.raw_data.data(), SHARD_MAGIC, SHARD_MAGIC_SIZE) != 0) return false;

    core::uint32 total_size = core::read_le32(rr.raw_data.data() + SHARD_TOTAL_OFF);
    core::uint32 header_size32 = core::read_le32(rr.raw_data.data() + SHARD_HEADER_OFF);
    core::uint64 group_count = core::read_le64(rr.raw_data.data() + SHARD_GROUP_CNT);
    core::uint64 shard_size_u = core::read_le64(rr.raw_data.data() + SHARD_SIZE_U64);
    core::uint64 prot_size = core::read_le64(rr.raw_data.data() + SHARD_PROT_SIZE);
    core::uint16 D_u16 = core::read_le16(rr.raw_data.data() + SHARD_D_FIELD);
    core::uint16 NR_u16 = core::read_le16(rr.raw_data.data() + SHARD_NR_FIELD);

    if (total_size == 0 || shard_size_u != total_size) return false;
    if (D_u16 == 0 || NR_u16 == 0) return false;
    if (header_size32 == 0 || header_size32 >= total_size) return false;
    if (group_count == 0 || (group_count & 1u)) return false; // must be even
    if (static_cast<core::uint64>(total_size) !=
        static_cast<core::uint64>(header_size32) + group_count)
        return false;
    if (static_cast<core::uint64>(NR_u16) * total_size > rr.data_size) return false;
    // Writer invariant: the record's data shards cover exactly
    // ceil(prot_size/group_count)*group_count bytes, so prot_size can never
    // exceed D*group_count. Enforcing it here keeps a crafted record from
    // driving the unbounded file_bytes resize below (report M2).
    if (prot_size > static_cast<core::uint64>(D_u16) * group_count) return false;

    core::uint32 D = D_u16;
    core::uint32 NR = NR_u16;
    // Resource cap (report M3): the writer never emits D > 200 or NR > 10*D
    // (compute_geometry), and RS16::init allocates ne*nd cells with Gauss-
    // Jordan costing ~ne²·nd GF ops. Hostile records with huge D/NR must be
    // rejected before init instead of attempting a multi-GB allocation and
    // hours of decode.
    if (D > 200 || NR > 10u * 200) return false;
    core::uint64 shard_size = total_size;

    // Validate every parity shard by magic + CRC-64/XZ. A shard that fails
    // either check is treated as an erased parity unit.
    std::vector<core::byte> parity_valid(NR, 0);
    core::uint32 parity_valid_count = 0;
    if (rr.raw_data.size() < static_cast<size_t>(NR) * static_cast<size_t>(shard_size)) {
        // Fewer shards on disk than declared. Anything past the truncation
        // point is invalid; keep going with what we have.
    }
    for (core::uint32 j = 0; j < NR; ++j) {
        size_t off = static_cast<size_t>(j) * static_cast<size_t>(shard_size);
        if (off + shard_size > rr.raw_data.size()) break;
        const core::byte* s = rr.raw_data.data() + off;
        if (std::memcmp(s, SHARD_MAGIC, SHARD_MAGIC_SIZE) != 0) continue;
        core::uint64 crc_stored = core::read_le64(s + SHARD_CRC64_OFF);
        core::uint64 crc_actual = crypto::Crc64Xz::compute(
            s + SHARD_TOTAL_OFF, static_cast<size_t>(shard_size) - SHARD_TOTAL_OFF);
        if (crc_stored != crc_actual) continue;
        // shard_size / group_count must agree across shards for the record
        // to be usable.
        if (core::read_le32(s + SHARD_TOTAL_OFF) != total_size) continue;
        if (core::read_le32(s + SHARD_HEADER_OFF) != header_size32) continue;
        if (core::read_le16(s + SHARD_D_FIELD) != D_u16) continue;
        if (core::read_le16(s + SHARD_NR_FIELD) != NR_u16) continue;
        parity_valid[j] = 1;
        ++parity_valid_count;
    }
    if (parity_valid_count == 0) return false;

    // Load the protected prefix currently on disk. It may be shorter than the
    // recorded `prot_size` (tail truncation).
    core::uint64 file_size = 0;
    std::vector<core::byte> file_bytes;
    {
        io::FileStream f;
        if (!f.open(arc_path, io::FileMode::ReadOnly)) return false;
        file_size = f.size();
        file_bytes.resize(static_cast<size_t>(file_size));
        if (file_bytes.size() > 0) {
            if (f.read(file_bytes.data(), file_bytes.size()) != file_bytes.size()) return false;
        }
        f.close();
    }
    if (rr.header_offset + rr.header_size > file_bytes.size()) return false;

    // rr.data_size is an unbounded vint: find_rr's wrap guard only covers
    // non-RR blocks, so the naive header_offset + header_size + data_size sum
    // can wrap past 2^64 and sail past the bounds check. Subtraction form;
    // header_offset + header_size <= file_bytes.size() is established above.
    if (rr.data_size > file_bytes.size() - rr.header_offset - rr.header_size) return false;
    core::uint64 rr_len_on_disk = rr.header_size + rr.data_size;

    // Protected bytes = everything before the RR block, padded to prot_size.
    // Anything above `available` counts as an erased data-shard tail.
    core::uint64 available = std::min<core::uint64>(prot_size, rr.header_offset);

    // Sanity guard: rr.header_offset should equal prot_size on an intact
    // archive. If the archive has trailing garbage BEFORE the RR block, we
    // don't know how to interpret it.
    if (rr.header_offset > prot_size) return false;

    // Data-shard validity: shard i covers bytes [i*group_count, (i+1)*group_count).
    // Valid iff every byte of that range is inside `available`.
    std::vector<core::byte> data_valid(D, 0);
    core::uint32 data_valid_count = 0;
    for (core::uint32 i = 0; i < D; ++i) {
        core::uint64 shard_end = static_cast<core::uint64>(i + 1) * group_count;
        core::uint64 shard_end_capped = std::min<core::uint64>(shard_end, prot_size);
        if (shard_end_capped <= available) {
            data_valid[i] = 1;
            ++data_valid_count;
        }
    }

    core::uint32 missing_data = D - data_valid_count;
    if (missing_data > parity_valid_count) return false;

    // Pad file_bytes up to D * group_count so each shard can be read contiguously.
    auto full_data_size = calculate_parity_buffer_size(D, group_count);
    if (!full_data_size) return false;
    if (file_bytes.size() < *full_data_size) {
        file_bytes.resize(static_cast<size_t>(*full_data_size), 0);
    }
    // Bytes past rr.header_offset (such as the RR block or EndArc on disk) must be
    // zeroed because parity encoding treats data past the protected prefix as pure zero padding.
    if (file_bytes.size() > rr.header_offset) {
        std::memset(file_bytes.data() + rr.header_offset, 0,
                    file_bytes.size() - static_cast<size_t>(rr.header_offset));
    }

    // Initialize Cauchy RS encoder to compute syndromes and evaluate parity.
    ReedSolomon16 rs_enc;
    if (!rs_enc.init(D, NR)) return false;

    std::vector<std::vector<core::byte>> recomputed_parity(NR);
    for (core::uint32 j = 0; j < NR; ++j) {
        if (parity_valid[j]) {
            recomputed_parity[j].assign(static_cast<size_t>(group_count), 0);
        }
    }
    for (core::uint32 i = 0; i < D; ++i) {
        const core::byte* data_ptr =
            file_bytes.data() + static_cast<size_t>(i) * static_cast<size_t>(group_count);
        for (core::uint32 j = 0; j < NR; ++j) {
            if (!parity_valid[j]) continue;
            rs_enc.update_ecc(i, j, data_ptr, recomputed_parity[j].data(),
                              static_cast<size_t>(group_count));
        }
    }

    std::vector<std::vector<core::byte>> syndromes(NR);
    core::uint32 non_zero_syndromes = 0;
    core::int32 first_non_zero_j = -1;

    for (core::uint32 j = 0; j < NR; ++j) {
        if (!parity_valid[j]) continue;
        syndromes[j].resize(static_cast<size_t>(group_count));
        const core::byte* P_j = rr.raw_data.data() +
                                static_cast<size_t>(j) * static_cast<size_t>(shard_size) +
                                static_cast<size_t>(header_size32);
        bool all_zero = true;
        for (size_t b = 0; b < group_count; ++b) {
            core::byte diff = static_cast<core::byte>(P_j[b] ^ recomputed_parity[j][b]);
            syndromes[j][b] = diff;
            if (diff != 0) all_zero = false;
        }
        if (!all_zero) {
            ++non_zero_syndromes;
            if (first_non_zero_j < 0) first_non_zero_j = static_cast<core::int32>(j);
        }
    }

    if (missing_data == 0) {
        if (non_zero_syndromes == 0 && parity_valid_count == NR) {
            // Already completely intact
            return headers_verify(arc_path, sfx_off);
        }

        // Check if data is completely healthy
        bool all_data_ok = false;
        if (headers_verify(arc_path, sfx_off)) {
            archive::ArchiveReader reader;
            if (reader.open(arc_path)) {
                bool entries_ok = true;
                for (const auto& entry : reader.entries()) {
                    if (entry.header.is_service) continue;
                    if (!reader.test_entry(entry)) {
                        entries_ok = false;
                        break;
                    }
                }
                all_data_ok = entries_ok;
            }
        }

        if (all_data_ok) {
            // Parity-Only Corruption branch:
            // Recompute all NR parity shards from file_bytes
            std::vector<std::vector<core::byte>> all_new_parity(
                NR, std::vector<core::byte>(static_cast<size_t>(group_count), 0));
            for (core::uint32 i = 0; i < D; ++i) {
                const core::byte* data_ptr =
                    file_bytes.data() + static_cast<size_t>(i) * static_cast<size_t>(group_count);
                for (core::uint32 j = 0; j < NR; ++j) {
                    rs_enc.update_ecc(i, j, data_ptr, all_new_parity[j].data(),
                                      static_cast<size_t>(group_count));
                }
            }

            RecoveryGeometry g{};
            g.archive_size = prot_size;
            g.pct = rr.rec_pct > 0
                        ? rr.rec_pct
                        : static_cast<core::uint32>((static_cast<core::uint64>(NR) * 100) / D);
            g.D = D;
            g.NR = NR;
            g.group_count = group_count;
            g.header_size = header_size32;
            g.shard_size = shard_size;

            auto data_area_size =
                calculate_parity_buffer_size(NR, static_cast<core::uint32>(shard_size));
            if (!data_area_size) return false;
            std::vector<core::byte> data_area;
            data_area.reserve(static_cast<size_t>(*data_area_size));
            for (core::uint32 j = 0; j < NR; ++j) {
                auto s =
                    build_shard(j, g, all_new_parity[j].data(), static_cast<size_t>(group_count));
                data_area.insert(data_area.end(), s.begin(), s.end());
            }

            std::vector<core::byte> prefix(
                file_bytes.begin(), file_bytes.begin() + static_cast<size_t>(rr.header_offset));
            if (!splice_repair_with_rr(arc_path, prefix, data_area, g.pct)) return false;
            return headers_verify(arc_path, sfx_off);
        }
    }

    if (non_zero_syndromes > 0) {
        std::vector<core::uint32> matching_candidates;
        if (parity_valid_count >= 2 && first_non_zero_j >= 0 && missing_data == 0) {
            core::uint32 j0 = static_cast<core::uint32>(first_non_zero_j);
            size_t num_words = static_cast<size_t>(group_count / 2);

            for (core::uint32 e = 0; e < D; ++e) {
                if (!data_valid[e]) continue;
                core::uint32 c0 = rs_enc.gf_inv(rs_enc.gf_add(j0 + D, e));
                bool candidate_matches = true;

                for (core::uint32 j = 0; j < NR && candidate_matches; ++j) {
                    if (!parity_valid[j] || j == j0) continue;
                    core::uint32 cj = rs_enc.gf_inv(rs_enc.gf_add(j + D, e));

                    for (size_t w = 0; w < num_words; ++w) {
                        core::uint16 s0 = static_cast<core::uint16>(
                            syndromes[j0][2 * w] | (syndromes[j0][2 * w + 1] << 8));
                        core::uint16 sj = static_cast<core::uint16>(syndromes[j][2 * w] |
                                                                    (syndromes[j][2 * w + 1] << 8));
                        if (rs_enc.gf_mul(sj, c0) != rs_enc.gf_mul(s0, cj)) {
                            candidate_matches = false;
                            break;
                        }
                    }
                }

                if (candidate_matches) {
                    matching_candidates.push_back(e);
                }
            }
        }

        if (matching_candidates.size() == 1) {
            // Tier 1 Guaranteed: uniquely identified single corrupted shard
            core::uint32 bad_shard = matching_candidates[0];
            data_valid[bad_shard] = 0;
            ++missing_data;
            --data_valid_count;
        } else {
            // Tier 2: Route to block header / entry CRC scan fallback
            io::FileStream stream;
            if (stream.open(arc_path, io::FileMode::ReadOnly)) {
                core::uint64 file_sz = stream.size();
                if (sfx_off + 8 <= file_sz) {
                    stream.seek(static_cast<core::int64>(sfx_off + 8), io::SeekOrigin::Begin);
                    while (stream.tell() < file_sz) {
                        core::uint64 head_start = stream.tell();
                        core::uint64 type = 0, flags = 0, data_sz = 0;
                        std::vector<core::byte> body;
                        if (format::HeaderReader::read_block_raw(
                                stream, type, flags, body, data_sz) != format::HeaderResult::Ok) {
                            core::uint64 bad_shard = head_start / group_count;
                            if (bad_shard < D && data_valid[bad_shard]) {
                                data_valid[bad_shard] = 0;
                                ++missing_data;
                                --data_valid_count;
                            }
                            break;
                        }
                        if (type == format::HEAD_ENDARC) break;
                        if (data_sz > 0) {
                            core::uint64 cur = stream.tell();
                            if (data_sz > file_sz - cur) break;
                            if (!stream.seek(static_cast<core::int64>(cur + data_sz),
                                             io::SeekOrigin::Begin))
                                break;
                        }
                        if (stream.tell() <= head_start) break;
                    }
                }
                stream.close();
            }

            archive::ArchiveReader reader;
            if (reader.open(arc_path)) {
                for (const auto& entry : reader.entries()) {
                    if (entry.header.is_service) continue;
                    if (!reader.test_entry(entry)) {
                        core::uint64 s_start = entry.header_offset / group_count;
                        core::uint64 s_end =
                            (entry.data_offset + entry.data_size + group_count - 1) / group_count;
                        if (s_end <= s_start) s_end = s_start + 1;
                        if (s_end > D) s_end = D;

                        core::uint32 found_single_shard = D;
                        if (s_end - s_start == 1) {
                            found_single_shard = static_cast<core::uint32>(s_start);
                        } else if (first_non_zero_j >= 0 && parity_valid_count >= 1) {
                            core::uint32 j0 = static_cast<core::uint32>(first_non_zero_j);
                            size_t num_words = static_cast<size_t>(group_count / 2);
                            core::uint32 match_count = 0;
                            core::uint32 candidate_shard = D;

                            for (core::uint64 s = s_start; s < s_end; ++s) {
                                if (!data_valid[s]) continue;
                                core::uint32 inv_c0 = rs_enc.gf_inv(
                                    rs_enc.gf_add(j0 + D, static_cast<core::uint32>(s)));
                                bool consistent = true;
                                for (core::uint32 j = 0; j < NR && consistent; ++j) {
                                    if (!parity_valid[j] || j == j0) continue;
                                    core::uint32 cj = rs_enc.gf_inv(
                                        rs_enc.gf_add(j + D, static_cast<core::uint32>(s)));
                                    for (size_t w = 0; w < num_words; ++w) {
                                        core::uint16 s0 = static_cast<core::uint16>(
                                            syndromes[j0][2 * w] | (syndromes[j0][2 * w + 1] << 8));
                                        core::uint16 sj = static_cast<core::uint16>(
                                            syndromes[j][2 * w] | (syndromes[j][2 * w + 1] << 8));
                                        if (rs_enc.gf_mul(sj, inv_c0) != rs_enc.gf_mul(s0, cj)) {
                                            consistent = false;
                                            break;
                                        }
                                    }
                                }
                                if (!consistent) continue;

                                if (entry.header.method == 0) {
                                    crypto::Crc32 cand_crc;
                                    core::uint64 s_byte_start = s * group_count;
                                    core::uint64 s_byte_end = (s + 1) * group_count;
                                    for (core::uint64 off = entry.data_offset;
                                         off < entry.data_offset + entry.data_size; ++off) {
                                        core::byte b = file_bytes[off];
                                        if (off >= s_byte_start && off < s_byte_end) {
                                            size_t w =
                                                static_cast<size_t>((off - s_byte_start) / 2);
                                            core::uint16 s0 = static_cast<core::uint16>(
                                                syndromes[j0][2 * w] |
                                                (syndromes[j0][2 * w + 1] << 8));
                                            core::uint16 err_word =
                                                static_cast<core::uint16>(rs_enc.gf_mul(
                                                    rs_enc.gf_add(j0 + D,
                                                                  static_cast<core::uint32>(s)),
                                                    s0));
                                            core::byte err_byte =
                                                ((off - s_byte_start) % 2 == 0)
                                                    ? static_cast<core::byte>(err_word & 0xFF)
                                                    : static_cast<core::byte>((err_word >> 8) &
                                                                              0xFF);
                                            b ^= err_byte;
                                        }
                                        cand_crc.update(&b, 1);
                                    }
                                    if (cand_crc.get() == entry.header.data_crc32) {
                                        match_count++;
                                        candidate_shard = static_cast<core::uint32>(s);
                                    }
                                } else if (parity_valid_count >= 2) {
                                    match_count++;
                                    candidate_shard = static_cast<core::uint32>(s);
                                }
                            }
                            if (match_count == 1) {
                                found_single_shard = candidate_shard;
                            }
                        }

                        if (found_single_shard < D) {
                            if (data_valid[found_single_shard]) {
                                data_valid[found_single_shard] = 0;
                                ++missing_data;
                                --data_valid_count;
                            }
                        } else {
                            for (core::uint64 s = s_start; s < s_end; ++s) {
                                if (data_valid[s]) {
                                    data_valid[s] = 0;
                                    ++missing_data;
                                    --data_valid_count;
                                }
                            }
                        }
                    }
                }
                reader.close();
            }
        }
    }

    if (missing_data == 0 || missing_data > parity_valid_count) {
        return false;
    }

    // Combined valid_flags vector for ReedSolomon16::init(D, NR, flags).
    std::vector<core::byte> valid(D + NR, 0);
    for (core::uint32 i = 0; i < D; ++i) valid[i] = data_valid[i];
    for (core::uint32 j = 0; j < NR; ++j) valid[D + j] = parity_valid[j];

    ReedSolomon16 rs_dec;
    if (!rs_dec.init(D, NR, valid.data())) return false;

    // Map ND source rows: valid data shards contribute themselves,
    // missing data shards are substituted by the next valid parity shard.
    std::vector<const core::byte*> src(D, nullptr);
    core::uint32 r_idx = D;
    for (core::uint32 i = 0; i < D; ++i) {
        if (data_valid[i]) {
            src[i] = file_bytes.data() + static_cast<size_t>(i) * static_cast<size_t>(group_count);
        } else {
            while (r_idx < D + NR && !valid[r_idx]) ++r_idx;
            if (r_idx >= D + NR) return false;
            core::uint32 pj = r_idx - D;
            size_t poff = static_cast<size_t>(pj) * static_cast<size_t>(shard_size) +
                          static_cast<size_t>(header_size32);
            src[i] = rr.raw_data.data() + poff;
            ++r_idx;
        }
    }

    // Reconstruct the missing data shards into a compact buffer.
    auto recon_size = calculate_parity_buffer_size(missing_data, group_count);
    if (!recon_size) return false;
    std::vector<core::byte> recon(static_cast<size_t>(*recon_size), 0);
    for (core::uint32 j = 0; j < D; ++j) {
        for (core::uint32 e = 0; e < missing_data; ++e) {
            core::byte* dst =
                recon.data() + static_cast<size_t>(e) * static_cast<size_t>(group_count);
            rs_dec.update_ecc(j, e, src[j], dst, static_cast<size_t>(group_count));
        }
    }

    // Splice reconstructed shards back into file_bytes.
    core::uint32 cur = 0;
    for (core::uint32 i = 0; i < D; ++i) {
        if (data_valid[i]) continue;
        core::uint64 off = static_cast<core::uint64>(i) * group_count;
        core::uint64 end = off + group_count;
        if (end > prot_size) end = prot_size;
        if (end > off) {
            std::memcpy(file_bytes.data() + off,
                        recon.data() + static_cast<size_t>(cur) * static_cast<size_t>(group_count),
                        static_cast<size_t>(end - off));
        }
        ++cur;
    }

    // Post-repair syndrome verification: recompute parity from repaired file_bytes.
    for (core::uint32 j = 0; j < NR; ++j) {
        if (parity_valid[j]) {
            std::memset(recomputed_parity[j].data(), 0, static_cast<size_t>(group_count));
        }
    }
    for (core::uint32 i = 0; i < D; ++i) {
        const core::byte* data_ptr =
            file_bytes.data() + static_cast<size_t>(i) * static_cast<size_t>(group_count);
        for (core::uint32 j = 0; j < NR; ++j) {
            if (!parity_valid[j]) continue;
            rs_enc.update_ecc(i, j, data_ptr, recomputed_parity[j].data(),
                              static_cast<size_t>(group_count));
        }
    }

    bool post_repair_syndromes_zero = true;
    for (core::uint32 j = 0; j < NR; ++j) {
        if (!parity_valid[j]) continue;
        const core::byte* P_j = rr.raw_data.data() +
                                static_cast<size_t>(j) * static_cast<size_t>(shard_size) +
                                static_cast<size_t>(header_size32);
        for (size_t b = 0; b < group_count; ++b) {
            if ((P_j[b] ^ recomputed_parity[j][b]) != 0) {
                post_repair_syndromes_zero = false;
                break;
            }
        }
        if (!post_repair_syndromes_zero) break;
    }
    if (!post_repair_syndromes_zero) {
        return false;
    }

    file_bytes.resize(static_cast<size_t>(rr.header_offset));
    if (!splice_repair(arc_path, file_bytes, rr.header_offset, rr_len_on_disk)) return false;
    return headers_verify(arc_path, sfx_off);
}

} // namespace openrar::recovery
