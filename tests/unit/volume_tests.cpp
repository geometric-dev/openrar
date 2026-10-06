#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/volume.hpp"
#include "../../src/compress/compress_plan.hpp"
#include "../../src/core/types.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/io/file_stream.hpp"

#include <cassert>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace openrar;
using openrar::archive::volume::first_volume_name;
using openrar::archive::volume::next_volume_name;

// Every produced filename must stay within the legacy volume alphabet:
// lowercase letters, digits, and '.'. Anything else means the carry logic
// produced a byte a real filesystem path shouldn't have (e.g. incrementing
// past 'z').
static bool all_bytes_valid(const std::string& name) {
    for (unsigned char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
        if (!ok) return false;
    }
    return true;
}

void test_old_style_numbering_sequence() {
    std::filesystem::path cur = "archive.rar";

    // .rar -> .r00
    cur = next_volume_name(cur, /*old_numbering=*/true);
    assert(cur.filename().string() == "archive.r00");
    assert(all_bytes_valid(cur.filename().string()));

    // .r00 -> .r01 -> ... -> .r99
    for (int i = 1; i <= 99; ++i) {
        cur = next_volume_name(cur, true);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "archive.r%02d", i);
        assert(cur.filename().string() == buf);
        assert(all_bytes_valid(cur.filename().string()));
    }

    // cur is now archive.r99 -> next should be archive.s00
    cur = next_volume_name(cur, true);
    assert(cur.filename().string() == "archive.s00");
    assert(all_bytes_valid(cur.filename().string()));
    std::cout << "[PASS] old-style numbering .rar -> .r00 .. .r99 -> .s00\n";
}

void test_old_style_numbering_full_alphabet_and_refusal() {
    // Walk the entire letter range s..z, each through 00..99, and confirm
    // every produced name is well-formed. Then confirm .z99 refuses to
    // advance further instead of producing a non-alphabetic byte.
    std::filesystem::path cur = "archive.s00";
    for (char letter = 's'; letter <= 'z'; ++letter) {
        for (int i = 0; i <= 99; ++i) {
            char expect[16];
            std::snprintf(expect, sizeof(expect), "archive.%c%02d", letter, i);
            assert(cur.filename().string() == expect);
            assert(all_bytes_valid(cur.filename().string()));
            if (letter == 'z' && i == 99) break; // last valid name; stop here
            cur = next_volume_name(cur, true);
        }
        if (letter == 'z') break;
    }
    assert(cur.filename().string() == "archive.z99");

    // Advancing past .z99 must refuse (return the same path unchanged),
    // never emit a byte outside [a-z0-9.].
    std::filesystem::path refused = next_volume_name(cur, true);
    assert(refused == cur);
    assert(all_bytes_valid(refused.filename().string()));
    std::cout << "[PASS] old-style numbering .s00 .. .z99, refuses past .z99\n";
}

// Regression 5.1: first_volume_name used to zero EVERY digit in the stem,
// turning e.g. backup2024.part03.rar into backup0000.part01.rar. It must
// rewrite only the last digit run.
void test_first_volume_name() {
    assert(first_volume_name("backup2024.part03.rar", false).filename().string() ==
           "backup2024.part01.rar");
    assert(first_volume_name("data.part07.rar", false).filename().string() == "data.part01.rar");
    assert(first_volume_name("report99.part5.rar", false).filename().string() ==
           "report99.part1.rar");
    assert(first_volume_name("dir/backup2024.part12.rar", false).filename().string() ==
           "backup2024.part01.rar");
    std::cout << "[PASS] first_volume_name preserves stems with digits\n";
}

// New-style (.partNN.rar) next-volume naming. Pins the digit-run increment:
// same width preserved (part09 -> part10), width grows on carry
// (part99 -> part100), only the LAST digit run is touched (stem digits in
// backup2024 survive), and a name without digits becomes the first part.
void test_new_style_numbering() {
    auto next = [](const char* name) {
        return next_volume_name(name, /*old_numbering=*/false).filename().string();
    };
    assert(next("archive.part01.rar") == "archive.part02.rar");
    assert(next("archive.part09.rar") == "archive.part10.rar");  // width kept
    assert(next("archive.part99.rar") == "archive.part100.rar"); // width grows
    assert(next("archive.part00.rar") == "archive.part01.rar");
    assert(next("report99.part5.rar") == "report99.part6.rar");       // 1-digit run
    assert(next("backup2024.part12.rar") == "backup2024.part13.rar"); // stem untouched
    assert(next("dir/archive.part07.rar") == "archive.part08.rar");   // filename part
    assert(next("archive.rar") == "archive.part01.rar");              // no digits -> first part
    assert(next("archive") == "archive.part01.rar");
    std::cout << "[PASS] new-style numbering .partNN increments the last digit run\n";
}

// Regression (report L7): scan_archive never checked whether the derived
// legacy next-volume name equals the current volume name. At the .z99 wrap
// ceiling next_volume_name returns the same name (see Q14 in the review
// report), so a .z99 archive carrying a SPLITAFTER entry used to re-enter
// its own name in the chain and be re-scanned up to MAX_VOLUME_CHAIN times,
// appending one duplicate entry per pass. The reader must stop the chain
// instead: open() succeeds and yields exactly the one real entry.
void test_scan_terminates_at_z99_ceiling() {
    namespace fs = std::filesystem;
    fs::path dir = "build/test_vol_z99";
    fs::path arc = dir / "legacy.z99";
    std::error_code fs_ec;
    fs::create_directories(dir, fs_ec);
    fs::remove(arc, fs_ec);
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(openrar::format::HeaderWriter::write_signature(out));

        openrar::format::MainBlock mb;
        assert(openrar::format::HeaderWriter::write_main_block(out, mb));

        openrar::format::FileBlock fb;
        fb.file_name = "split.bin";
        fb.unp_size = 4;
        fb.pack_size = 4;
        fb.attributes = 0x20;
        fb.method = 0; // store
        fb.win_size = 0;
        // SPLITAFTER makes scan_archive look for a next volume even though
        // none exists: this volume sits at the .z99 ceiling.
        assert(openrar::format::HeaderWriter::write_file_block(out, fb,
                                                               openrar::format::HFL_SPLITAFTER));
        const core::byte payload[4] = {'a', 'b', 'c', 'd'};
        assert(out.write(payload, 4) == 4);

        openrar::format::EndArcBlock eb;
        eb.end_flags = 0x0001; // EARCF_NEXTVOL: do not end the scan here
        assert(openrar::format::HeaderWriter::write_end_block(out, eb));
    }

    openrar::archive::ArchiveReader reader;
    assert(reader.open(arc));
    assert(reader.entries().size() == 1);
    assert(reader.entries()[0].header.file_name == "split.bin");

    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
    std::cout << "[PASS] scan stops at the .z99 ceiling (no self-referential chain)\n";
}

void test_streaming_multivolume_creation() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "test_vol_stream";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // Create a 250 KiB test file that will span multiple small volumes (e.g. 32 KiB each)
    std::vector<uint8_t> payload(250 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 7 + 13) ^ (i >> 8));
    }
    fs::path src = dir / "stream_src.bin";
    {
        io::FileStream s;
        assert(s.open(src, io::FileMode::CreateAlways));
        assert(s.write(payload.data(), payload.size()) == payload.size());
    }

    // 1. Test compressed multi-volume creation (method 3, 32 KiB volume size, 128 KiB dictionary)
    fs::path arc_base = dir / "multi.rar";
    bool ok = archive::ArchiveMutator::add_file_to_archive_vol(arc_base, src, "stream_src.bin", 3,
                                                               32 * 1024, "", false, 128 * 1024);
    assert(ok);

    fs::path part1 = dir / "multi.part01.rar";
    assert(fs::exists(part1));

    // Open via ArchiveReader and verify extraction across volumes
    {
        archive::ArchiveReader reader;
        assert(reader.open(part1));
        assert(reader.is_volume());
        assert(reader.entries().size() == 1);
        assert(reader.test_entry(reader.entries()[0]));

        fs::path dest = dir / "extracted_comp.bin";
        assert(reader.extract_entry(reader.entries()[0], dest));
        assert(fs::file_size(dest) == payload.size());

        std::vector<uint8_t> readback(payload.size());
        io::FileStream in;
        assert(in.open(dest, io::FileMode::ReadOnly));
        assert(in.read(readback.data(), readback.size()) == readback.size());
        assert(readback == payload);
    }

    // 2. Test store mode (method 0) multi-volume creation with zero RAM buffering
    fs::path arc_store = dir / "store_multi.rar";
    ok = archive::ArchiveMutator::add_file_to_archive_vol(arc_store, src, "store_src.bin", 0,
                                                          32 * 1024, "");
    assert(ok);

    fs::path store_part1 = dir / "store_multi.part01.rar";
    assert(fs::exists(store_part1));

    {
        archive::ArchiveReader reader;
        assert(reader.open(store_part1));
        assert(reader.is_volume());
        assert(reader.entries().size() == 1);
        assert(reader.test_entry(reader.entries()[0]));

        fs::path dest = dir / "extracted_store.bin";
        assert(reader.extract_entry(reader.entries()[0], dest));
        assert(fs::file_size(dest) == payload.size());

        std::vector<uint8_t> readback(payload.size());
        io::FileStream in;
        assert(in.open(dest, io::FileMode::ReadOnly));
        assert(in.read(readback.data(), readback.size()) == readback.size());
        assert(readback == payload);
    }

    fs::remove_all(dir, ec);
    std::cout << "[PASS] streaming multivolume creation (compressed and store) roundtrip\n";
}

// ── v1.37 window-default agreement (question-log Entry 24) ──────────────────
// The multivolume add path used to default the compressor window to a flat
// 2 MiB regardless of method, and treated -md 1..15 as exact bytes. These
// rows pin that both prepare paths resolve the SAME window per method, that
// the shared table is the method-tuned one, and that a redundancy period
// larger than 2 MiB packs through the volume path at parity with the
// non-volume path.

namespace {

std::filesystem::path make_dir(const char* name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Deterministic incompressible-ish byte stream (64-bit LCG, high bits): no
// short-distance structure for the match finder to exploit, so any size
// difference between two packs of it comes from cross-block matches.
void write_lcg_file(const std::filesystem::path& path, size_t size, core::uint64 seed) {
    io::FileStream s;
    assert(s.open(path, io::FileMode::CreateAlways));
    core::uint64 x = seed * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
    std::vector<core::byte> buf(1 << 16);
    size_t left = size;
    while (left > 0) {
        size_t take = std::min(left, buf.size());
        for (size_t i = 0; i < take; ++i) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            buf[i] = static_cast<core::byte>(x >> 33);
        }
        assert(s.write(buf.data(), take) == take);
        left -= take;
    }
}

// Compressible text-like stream: LCG high bits over a 64-symbol alphabet.
// Compression must engage (no store fallback) so the header records the
// resolved window instead of 0.
void write_textlike_file(const std::filesystem::path& path, size_t size, core::uint64 seed) {
    io::FileStream s;
    assert(s.open(path, io::FileMode::CreateAlways));
    core::uint64 x = seed * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
    std::vector<core::byte> buf(1 << 16);
    size_t left = size;
    while (left > 0) {
        size_t take = std::min(left, buf.size());
        for (size_t i = 0; i < take; ++i) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            buf[i] = static_cast<core::byte>(0x20 + ((x >> 33) & 0x3F));
        }
        assert(s.write(buf.data(), take) == take);
        left -= take;
    }
}

core::uint64 volume_set_bytes(const std::filesystem::path& dir, const std::string& stem) {
    core::uint64 total = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::string n = e.path().filename().string();
        if (n.rfind(stem + ".part", 0) == 0)
            total += static_cast<core::uint64>(std::filesystem::file_size(e.path()));
    }
    return total;
}

struct MemberHeader {
    core::uint64 win_size;
    core::uint32 unp_ver;
    core::uint32 method;
    bool is_solid;
    core::int64 pack_size;
    core::uint64 unp_size;
};

MemberHeader member_header(const std::filesystem::path& arc, size_t index) {
    archive::ArchiveReader r;
    assert(r.open(arc));
    assert(r.entries().size() > index);
    const auto& h = r.entries()[index].header;
    MemberHeader m{h.win_size, h.unp_ver, h.method, h.is_solid, h.pack_size, h.unp_size};
    r.close();
    return m;
}

void extract_and_compare(const std::filesystem::path& arc, size_t index,
                         const std::filesystem::path& src) {
    archive::ArchiveReader r;
    assert(r.open(arc));
    assert(r.entries().size() > index);
    std::filesystem::path out = src.string() + ".extracted";
    assert(r.extract_entry(r.entries()[index], out));
    r.close();
    assert(std::filesystem::file_size(out) == std::filesystem::file_size(src));
    io::FileStream a, b;
    assert(a.open(out, io::FileMode::ReadOnly) && b.open(src, io::FileMode::ReadOnly));
    std::vector<core::byte> ba(1 << 16), bb(1 << 16);
    for (;;) {
        size_t ga = a.read(ba.data(), ba.size()), gb = b.read(bb.data(), bb.size());
        assert(ga == gb);
        if (ga == 0) break;
        assert(std::memcmp(ba.data(), bb.data(), ga) == 0);
    }
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// Non-volume reference pack: stage 1 prepare (is_solid skips the per-file
// pow2 clamp so the raw table value is observable in the header) + stage 2
// batch write.
std::filesystem::path pack_nonvolume(const std::filesystem::path& dir,
                                     const std::filesystem::path& src, const char* stem, int method,
                                     core::uint64 dict_size, bool is_solid) {
    archive::ArchiveMutator::PreparedAdd prepared;
    prepared.entry_name = stem;
    prepared.src_path = src;
    assert(archive::ArchiveMutator::prepare_add_file(src, stem, method, "", prepared,
                                                     archive::time_flags::MTIME, dict_size, false,
                                                     false, is_solid, /*direct_stream=*/true));
    std::vector<archive::ArchiveMutator::PreparedAdd> batch;
    batch.push_back(std::move(prepared));
    std::filesystem::path arc = dir / (std::string(stem) + ".nv.rar");
    assert(archive::ArchiveMutator::write_batch_add(arc, batch, {}, "", false, {}, false, {}, false,
                                                    false, compress::FilterConfig{}));
    return arc;
}

std::filesystem::path pack_volume(const std::filesystem::path& dir,
                                  const std::filesystem::path& src, const char* stem, int method,
                                  core::uint64 vol_size, core::uint64 dict_size, bool solid) {
    std::filesystem::path arc = dir / (std::string(stem) + ".rar");
    assert(archive::ArchiveMutator::add_file_to_archive_vol(arc, src, stem, method, vol_size, "",
                                                            solid, dict_size));
    // New-style naming: volume 1 of the chain is stem.part01.rar; the plain
    // stem.rar never exists on disk.
    return dir / (std::string(stem) + ".part01.rar");
}

} // namespace

// The shared table itself: six method-tuned defaults, the legacy 1..15 scale,
// exact bytes above 15.
void test_window_default_table() {
    using compress::default_dict_size_for_method;
    using compress::resolve_dict_window_size;
    assert(default_dict_size_for_method(0) == 0x20000ULL);   // 128 KiB
    assert(default_dict_size_for_method(1) == 0x80000ULL);   // 512 KiB
    assert(default_dict_size_for_method(2) == 0x100000ULL);  // 1 MiB
    assert(default_dict_size_for_method(3) == 0x800000ULL);  // 8 MiB
    assert(default_dict_size_for_method(4) == 0x1000000ULL); // 16 MiB
    assert(default_dict_size_for_method(5) == 0x4000000ULL); // 64 MiB
    assert(resolve_dict_window_size(0, 3) == 0x800000ULL);
    assert(resolve_dict_window_size(3, 3) == 0x80000ULL); // legacy scale, not 3 bytes
    assert(resolve_dict_window_size(15, 3) == 0x20000ULL << 14);
    assert(resolve_dict_window_size(16, 3) == 16); // exact bytes above 15
    std::cout << "[PASS] window default table (method-tuned, legacy scale, exact bytes)\n";
}

// Per-method: the volume path's header win_size must equal the non-volume
// path's, and both must expose the raw method-tuned table (solid-requested
// packs skip the per-file pow2 clamp on both sides). dict_size=0 everywhere.
void test_volume_nonvolume_window_agreement() {
    std::filesystem::path dir = make_dir("test_vol_windef");
    std::filesystem::path src = dir / "src.bin";
    write_textlike_file(src, 300 * 1024, 1);
    const core::uint64 table[6] = {0x20000ULL,  0x80000ULL,   0x100000ULL,
                                   0x800000ULL, 0x1000000ULL, 0x4000000ULL};
    for (int m = 0; m <= 5; ++m) {
        // Digit-free stems: the volume-name derivation rewrites the last
        // digit run in a stem, so "m3.rar" would not stay "m3.rar".
        std::string stem = "meth" + std::string(1, static_cast<char>('a' + m));
        std::filesystem::path nv = pack_nonvolume(dir, src, stem.c_str(), m, 0, /*is_solid=*/true);
        std::filesystem::path v1 = pack_volume(dir, src, stem.c_str(), m, 64 * 1024, 0, true);
        MemberHeader hnv = member_header(nv, 0);
        MemberHeader hv = member_header(v1, 0);
        // Stored members (m0) record win 0; compressed members record the
        // raw table value (solid-requested packs skip the pow2 clamp).
        core::uint64 expect = (m == 0) ? 0 : table[m];
        assert(hnv.win_size == expect);
        assert(hv.win_size == hnv.win_size);
        if (m == 0) {
            assert(hv.method == 0 && hv.win_size == 0 && hv.unp_ver == 0);
        }
        extract_and_compare(v1, 0, src);
        extract_and_compare(nv, 0, src);
    }
    std::cout << "[PASS] volume/non-volume header win_size agree per method (m0..m5)\n";

    // Empty member: win 0, unp_ver 0 on both paths regardless of method.
    std::filesystem::path empty = dir / "empty.bin";
    {
        io::FileStream s;
        assert(s.open(empty, io::FileMode::CreateAlways));
    }
    std::filesystem::path nv = pack_nonvolume(dir, empty, "efile", 3, 0, false);
    std::filesystem::path v1 = pack_volume(dir, empty, "efile", 3, 4096, 0, false);
    MemberHeader hnv = member_header(nv, 0);
    MemberHeader hv = member_header(v1, 0);
    assert(hnv.win_size == 0 && hnv.unp_ver == 0 && hnv.method == 0);
    assert(hv.win_size == 0 && hv.unp_ver == 0 && hv.method == 0);
    std::cout << "[PASS] empty member carries win 0 / unp_ver 0 on both paths\n";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The per-file pow2 file-size clamp engages identically on both paths for
// non-solid fresh members (dict_size=0): a 3 MiB file at m3 clamps the 8 MiB
// default down to 4 MiB.
void test_volume_pow2_clamp_agreement() {
    std::filesystem::path dir = make_dir("test_vol_pow2clamp");
    std::filesystem::path src = dir / "src.bin";
    write_textlike_file(src, 3 * 1024 * 1024, 2);
    std::filesystem::path nv = pack_nonvolume(dir, src, "clmp", 3, 0, /*is_solid=*/false);
    std::filesystem::path v1 = pack_volume(dir, src, "clmp", 3, 1024 * 1024, 0, false);
    MemberHeader hnv = member_header(nv, 0);
    MemberHeader hv = member_header(v1, 0);
    assert(hnv.win_size == 4ULL * 1024 * 1024);
    assert(hv.win_size == hnv.win_size);
    std::cout << "[PASS] pow2 file-size clamp agrees (3 MiB file at m3 -> 4 MiB)\n";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// -md semantics on the volume path: the legacy 1..15 scale, exact bytes, and
// the unp_ver=1 requirement for non-pow2 windows (v0 headers cannot encode
// the fraction — the writer would floor the base power and shrink the window
// under the encoder's match horizon).
void test_volume_dict_scale_and_unpver() {
    std::filesystem::path dir = make_dir("test_vol_dictscale");
    std::filesystem::path src = dir / "src.bin";
    write_textlike_file(src, 300 * 1024, 3);

    // Legacy scale: dict_size=3 means 128 KiB<<2 = 512 KiB, never 3 bytes.
    {
        std::filesystem::path nv = pack_nonvolume(dir, src, "dlegacy", 3, 3, false);
        std::filesystem::path v1 = pack_volume(dir, src, "dlegacy", 3, 64 * 1024, 3, false);
        MemberHeader hnv = member_header(nv, 0);
        MemberHeader hv = member_header(v1, 0);
        assert(hnv.win_size == 0x80000ULL && hv.win_size == 0x80000ULL);
        extract_and_compare(v1, 0, src);
    }
    // Exact bytes (non-pow2 grid-exact): dict_size=5 MiB passes through
    // unclamped (5 MiB is not a power of two -> unp_ver must be 1).
    {
        std::filesystem::path nv = pack_nonvolume(dir, src, "dfive", 3, 5ULL * 1024 * 1024, false);
        std::filesystem::path v1 =
            pack_volume(dir, src, "dfive", 3, 64 * 1024, 5ULL * 1024 * 1024, false);
        MemberHeader hnv = member_header(nv, 0);
        MemberHeader hv = member_header(v1, 0);
        assert(hnv.win_size == 5ULL * 1024 * 1024 && hv.win_size == 5ULL * 1024 * 1024);
        assert(hnv.unp_ver == 1 && hv.unp_ver == 1);
        extract_and_compare(v1, 0, src);
    }
    // Exact bytes (non-pow2): 3 MiB snaps on the FCI grid and BOTH paths must
    // declare unp_ver=1 — a v0 header cannot carry the fraction.
    {
        std::filesystem::path nv = pack_nonvolume(dir, src, "dnp", 3, 3ULL * 1024 * 1024, false);
        std::filesystem::path v1 =
            pack_volume(dir, src, "dnp", 3, 64 * 1024, 3ULL * 1024 * 1024, false);
        MemberHeader hnv = member_header(nv, 0);
        MemberHeader hv = member_header(v1, 0);
        assert(hnv.win_size == 3ULL * 1024 * 1024 && hv.win_size == 3ULL * 1024 * 1024);
        assert(hnv.unp_ver == 1 && hv.unp_ver == 1);
        extract_and_compare(v1, 0, src);
        extract_and_compare(nv, 0, src);
    }
    std::cout << "[PASS] -md legacy scale, exact bytes, and unp_ver=1 for non-pow2 windows\n";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Regression for Entry 24: an 18 MiB corpus whose redundancy period is 3 MiB
// (three distinct 1 MiB blocks, repeated six times). With the old flat 2 MiB
// window the volume path could not reach the cross-period matches and packed
// near the stored size; with the method-tuned 8 MiB default it must land
// within a small factor of the non-volume pack.
void test_volume_period_corpus_regression() {
    std::filesystem::path dir = make_dir("test_vol_period");
    std::filesystem::path src = dir / "period.bin";
    {
        io::FileStream s;
        assert(s.open(src, io::FileMode::CreateAlways));
        const size_t block = 1024 * 1024;
        for (size_t i = 0; i < 18; ++i) {
            std::filesystem::path blk = dir / ("blk" + std::to_string(i % 3) + ".bin");
            if (!std::filesystem::exists(blk)) write_lcg_file(blk, block, 100 + (i % 3));
            std::vector<core::byte> buf(block);
            {
                io::FileStream r;
                assert(r.open(blk, io::FileMode::ReadOnly));
                assert(r.read(buf.data(), block) == block);
            }
            assert(s.write(buf.data(), block) == block);
        }
    }
    const core::uint64 src_sz = static_cast<core::uint64>(std::filesystem::file_size(src));

    std::filesystem::path nv = pack_nonvolume(dir, src, "reg", 3, 0, /*is_solid=*/false);
    std::filesystem::path v1 = pack_volume(dir, src, "reg", 3, 4 * 1024 * 1024, 0, false);
    MemberHeader hnv = member_header(nv, 0);
    MemberHeader hv = member_header(v1, 0);
    assert(hnv.win_size == 8ULL * 1024 * 1024); // m3 default, clamp floor 32 MiB > 8 MiB
    assert(hv.win_size == hnv.win_size);

    core::uint64 nv_pack = static_cast<core::uint64>(hnv.pack_size);
    core::uint64 vol_total = volume_set_bytes(dir, "reg");
    assert(nv_pack > 0 && nv_pack < src_sz); // the corpus actually compresses
    assert(vol_total * 5 <= nv_pack * 6);    // within 1.2x of the non-volume pack
    assert(vol_total < src_sz);              // pre-fix this row packed ~src_sz (stored)
    extract_and_compare(v1, 0, src);
    extract_and_compare(nv, 0, src);
    std::cout << "[PASS] 3 MiB-period corpus: volume set within 1.2x of non-volume ("
              << (vol_total / (1024 * 1024)) << " MB vs " << (nv_pack / (1024 * 1024)) << " MB)\n";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Solid-run discipline on the volume path: a member that continues a solid
// chain must not GROW the window above what the chain already carries
// (shrink-or-equal is safe under every decoder model). The head of a fresh
// run keeps its own resolution.
void test_volume_solid_window_clamp() {
    std::filesystem::path dir = make_dir("test_vol_solidwin");
    std::filesystem::path a = dir / "a.bin";
    std::filesystem::path b = dir / "b.bin";
    write_textlike_file(a, 300 * 1024, 7);
    write_textlike_file(b, 300 * 1024, 8);

    // Grow case: head m3 (non-solid, 300 KiB -> pow2 clamp 512 KiB), then a
    // solid m5 continuation whose table default is 64 MiB. The continuation
    // must pack with the chain's 512 KiB window.
    {
        std::filesystem::path arc = dir / "grow.rar";
        assert(archive::ArchiveMutator::add_file_to_archive_vol(arc, a, "a.bin", 3, 64 * 1024));
        assert(archive::ArchiveMutator::add_file_to_archive_vol(arc, b, "b.bin", 5, 64 * 1024, "",
                                                                true));
        std::filesystem::path v1 = dir / "grow.part01.rar";
        MemberHeader ha = member_header(v1, 0);
        MemberHeader hb = member_header(v1, 1);
        assert(ha.win_size == 0x80000ULL && !ha.is_solid);
        assert(hb.win_size == 0x80000ULL && hb.is_solid && hb.method == 5);
        extract_and_compare(v1, 0, a);
        extract_and_compare(v1, 1, b);
    }
    // Shrink case: head carries an explicit 16 MiB window, solid m3
    // continuation resolves 8 MiB — no clamp needed, chain decodes.
    {
        std::filesystem::path arc = dir / "shrink.rar";
        assert(archive::ArchiveMutator::add_file_to_archive_vol(arc, a, "a.bin", 3, 64 * 1024, "",
                                                                false, 16ULL * 1024 * 1024));
        assert(archive::ArchiveMutator::add_file_to_archive_vol(arc, b, "b.bin", 3, 64 * 1024, "",
                                                                true));
        std::filesystem::path v1 = dir / "shrink.part01.rar";
        MemberHeader ha = member_header(v1, 0);
        MemberHeader hb = member_header(v1, 1);
        assert(ha.win_size == 16ULL * 1024 * 1024 && !ha.is_solid);
        assert(hb.win_size == 8ULL * 1024 * 1024 && hb.is_solid);
        extract_and_compare(v1, 0, a);
        extract_and_compare(v1, 1, b);
    }
    std::cout << "[PASS] solid continuation clamps window to the chain's (grow blocked)\n";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // assert() ends in abort(), whose Debug-CRT "abort() has been called"
    // modal is a SEPARATE dialog (_CALL_REPORTFAULT) — without this the
    // process still hangs after printing the assert.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    std::cout << "Running volume naming tests...\n";
    test_old_style_numbering_sequence();
    test_old_style_numbering_full_alphabet_and_refusal();
    test_first_volume_name();
    test_new_style_numbering();
    test_scan_terminates_at_z99_ceiling();
    test_window_default_table();
    test_streaming_multivolume_creation();
    test_volume_nonvolume_window_agreement();
    test_volume_pow2_clamp_agreement();
    test_volume_dict_scale_and_unpver();
    test_volume_period_corpus_regression();
    test_volume_solid_window_clamp();
    std::cout << "All volume naming tests PASSED!\n";
    return 0;
}
