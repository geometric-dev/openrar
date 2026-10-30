// Foreign-format reader tests — v1.29 M2 (ZIP). Named negative tests per
// plan §5: zip_cd_lf_mismatch_{name,method,size,offset},
// zip_bit3_descriptor_tolerance, zip_zip64_roundtrip,
// zip_cp437_and_utf8_flag_names, zip_traversal_names_normalized,
// zip_encrypted_refused_per_entry, zip_unsupported_method_refused,
// zip_bomb_inflight_cap, zip_comment_migrated_and_sanitized,
// zip_entry_comment_skipped_reported, zip_eocd_bounds, dispatch precedence.
// ZIP fixtures are crafted in-test (the builder below), not checked in.
#include "../../src/archive/foreign_reader.hpp"
#include "../../src/archive/foreign_zip.hpp"
#include "../../src/archive/foreign_tar.hpp"
#include "../../src/archive/foreign_gzip.hpp"
#include "../../src/compress/inflate.hpp"
#include "../../src/crypto/crc32.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "test_support.hpp"
#include <doctest.h>
#include "../support/test_runner.hpp"

using namespace openrar;
using namespace openrar::archive;
using namespace openrar::archive::foreign;

#include "deflate_kats.inc"

namespace {

#include "foreign_fixtures.inc"
} // namespace

static void test_dispatch_precedence() {
    std::cout << "[+] test_dispatch_precedence" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    // ZIP by LFH signature, even when named .tar.
    {
        const auto p = dir / "sig.tar";
        std::ofstream f(p, std::ios::binary);
        const auto buf = build_zip({});
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
        f.close();
        REQUIRE(detect_foreign_format(p) == SourceFormat::Zip);
    }
    // EOCD-only (empty) archive is still ZIP.
    {
        const auto p = dir / "empty.zip";
        std::ofstream f(p, std::ios::binary);
        const auto buf = build_zip({});
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
        f.close();
        REQUIRE(detect_foreign_format(p) == SourceFormat::Zip);
    }
    // RAR5 / legacy RAR / garbage.
    {
        const auto p = dir / "rar5.rar";
        std::ofstream f(p, std::ios::binary);
        f.write("Rar!\x1a\x07\x01\x00", 8);
        f.close();
        REQUIRE(detect_foreign_format(p) == SourceFormat::Rar5);
    }
    {
        const auto p = dir / "rar2.rar";
        std::ofstream f(p, std::ios::binary);
        f.write("Rar!\x1a\x07\x00", 7);
        f.close();
        REQUIRE(detect_foreign_format(p) == SourceFormat::LegacyRar);
    }
    {
        const auto p = dir / "junk.bin";
        std::ofstream f(p, std::ios::binary);
        f.write("not an archive at all, just text", 32);
        f.close();
        REQUIRE(detect_foreign_format(p) == SourceFormat::None);
    }
    std::cout << "    - signatures outrank names; RAR5/legacy/none pinned" << std::endl;
}

static void test_zip_roundtrip_store_and_deflate() {
    std::cout << "[+] test_zip_roundtrip_store_and_deflate" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    core::uint32 crc_txt = 0;
    const auto txt = stored_payload("Hello, migration!\n", crc_txt);
    core::uint32 crc_bin = 0;
    const auto bin = stored_payload(std::string(70000, '\0'), crc_bin); // > 64 KiB store

    std::vector<ZipMember> members;
    ZipMember a;
    a.name = "hello.txt";
    a.packed = txt;
    a.crc = crc_txt;
    a.unp_size = txt.size();
    members.push_back(a);

    ZipMember b;
    b.name = "blob.bin";
    b.packed = bin;
    b.crc = crc_bin;
    b.unp_size = bin.size();
    members.push_back(b);

    // DEFLATE member: a zlib-produced raw stream from the KAT fixture.
    ZipMember d;
    d.name = "kat_rle.bin";
    d.method = 8;
    d.packed = kat_rle_stream;
    d.crc = kat_rle_plain_crc;
    d.unp_size = kat_rle_plain_len;
    members.push_back(d);

    const auto p = dir / "mixed.zip";
    std::ofstream f(p, std::ios::binary);
    const auto buf = build_zip(members);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();

    ZipReader r;
    size_t count = 0;
    REQUIRE(open_zip(p, r, &count) == ForeignStatus::Ok);
    REQUIRE(count == 3);

    // Stored payload decodes byte-exact; mtime = DOS 2024-01-01 12:00:00.
    ExtractionLimits limits;
    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* data, size_t size) {
        out.insert(out.end(), data, data + size);
        return true;
    };
    REQUIRE(r.entry(0).type == ForeignType::File);
    REQUIRE(r.entry(0).mtime_sec == 1704110400);
    REQUIRE(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out == txt);
    REQUIRE(r.decode(1, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out.size() == txt.size() + bin.size());

    out.clear();
    REQUIRE(r.decode(2, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out.size() == kat_rle_plain_len);
    crypto::Crc32 crc;
    crc.update(out.data(), out.size());
    REQUIRE(crc.get() == kat_rle_plain_crc);
    std::cout << "    - store + >64KiB store + deflate members decode; DOS time mapped"
              << std::endl;
}

static void test_zip_cd_lf_mismatch() {
    std::cout << "[+] test_zip_cd_lf_mismatch" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    core::uint32 crc = 0;
    const auto data = stored_payload("payload", crc);
    const auto write_and_probe = [&](const std::vector<ZipMember>& members, ForeignStatus expect) {
        const auto buf = build_zip(members);
        static int n = 0;
        const auto p = dir / ("mismatch_" + std::to_string(n++) + ".zip");
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
        f.close();
        ZipReader r;
        std::string detail;
        ExtractionLimits limits;
        const ForeignStatus st = r.open(p, limits, ReaderHooks{}, detail);
        REQUIRE((st == expect && detail.c_str()));
    };

    ZipMember m;
    m.name = "a.txt";
    m.packed = data;
    m.crc = crc;
    m.unp_size = data.size();

    auto name_bad = m;
    name_bad.lfh_name_mismatch = true;
    write_and_probe({name_bad}, ForeignStatus::StructuralMismatch);

    auto method_bad = m;
    method_bad.method = 8;
    method_bad.lfh_method_mismatch = true;
    method_bad.packed = kat_text_stream;
    method_bad.crc = kat_text_plain_crc;
    method_bad.unp_size = kat_text_plain_len;
    write_and_probe({method_bad}, ForeignStatus::StructuralMismatch);

    auto size_bad = m;
    size_bad.lfh_size_mismatch = true;
    write_and_probe({size_bad}, ForeignStatus::StructuralMismatch);

    auto crc_bad = m;
    crc_bad.lfh_crc_mismatch = true;
    write_and_probe({crc_bad}, ForeignStatus::StructuralMismatch);

    auto off_bad = m;
    off_bad.cd_offset_mismatch = true;
    write_and_probe({off_bad}, ForeignStatus::StructuralMismatch);

    std::cout << "    - name/method/size/crc/offset mismatches all abort "
                 "pre-output (spec 11 §2.3)"
              << std::endl;
}

static void test_zip_bit3_descriptor_tolerance() {
    std::cout << "[+] test_zip_bit3_descriptor_tolerance" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    core::uint32 crc = 0;
    const auto data = stored_payload("descriptor data", crc);
    ZipMember m;
    m.name = "desc.txt";
    m.flags = 0x0008; // data descriptor: LFH crc/sizes are placeholders
    m.packed = data;
    m.crc = crc;
    m.unp_size = data.size();
    const auto buf = build_zip({m});
    const auto p = dir / "descriptor.zip";
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();
    ZipReader r;
    REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
    ExtractionLimits limits;
    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* d, size_t n) {
        out.insert(out.end(), d, d + n);
        return true;
    };
    REQUIRE(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out == data);
    std::cout << "    - descriptor entries pass the pre-flight; data decodes" << std::endl;
}

static void test_zip_zip64_roundtrip() {
    std::cout << "[+] test_zip_zip64_roundtrip" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    core::uint32 crc = 0;
    const auto data = stored_payload("zip64 member", crc);
    ZipMember m;
    m.name = "z64.bin";
    m.zip64 = true;
    m.packed = data;
    m.crc = crc;
    m.unp_size = data.size();
    const auto buf = build_zip({m});
    const auto p = dir / "zip64.zip";
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();
    ZipReader r;
    REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
    REQUIRE(r.entry(0).unpacked_size == data.size());
    ExtractionLimits limits;
    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* d, size_t n) {
        out.insert(out.end(), d, d + n);
        return true;
    };
    REQUIRE(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out == data);
    std::cout << "    - ZIP64 placeholders resolved from extras; pre-flight compares them"
              << std::endl;
}

static void test_zip_names() {
    std::cout << "[+] test_zip_names" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    // UTF-8 flag with an invalid sequence → percent-encoded losslessly.
    core::uint32 crc = 0;
    const auto data = stored_payload("x", crc);
    ZipMember bad;
    bad.name = std::string("bad\xff_name.txt");
    bad.flags = 0x0800; // UTF-8
    bad.packed = data;
    bad.crc = crc;
    bad.unp_size = data.size();

    // cp437 name (0x82 = é in CP437), flag clear.
    ZipMember cp;
    cp.name = std::string("caf\x82.txt");
    cp.packed = data;
    cp.crc = crc;
    cp.unp_size = data.size();

    const auto buf = build_zip({bad, cp});
    const auto p = dir / "names.zip";
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();
    ZipReader r;
    REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
    REQUIRE(r.entry(0).name == "bad%FF_name.txt");
    REQUIRE(r.entry(0).name_escaped);
    REQUIRE(r.entry(1).name == "caf\xc3\xa9.txt");
    REQUIRE(!r.entry(1).name_escaped);
    std::cout << "    - invalid UTF-8 percent-encoded; CP437 decoded losslessly" << std::endl;
}

static void test_zip_traversal_names() {
    std::cout << "[+] test_zip_traversal_names" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    core::uint32 crc = 0;
    const auto data = stored_payload("x", crc);

    std::vector<ZipMember> members;
    for (const char* n : {"../evil.txt", "/abs/path.txt", "C:\\windows\\x.txt", "a\\..\\b.txt"}) {
        ZipMember m;
        m.name = n;
        m.flags = 0x0800; // decode as UTF-8 for determinism
        m.packed = data;
        m.crc = crc;
        m.unp_size = data.size();
        members.push_back(m);
    }
    const auto buf = build_zip(members);
    const auto p = dir / "traversal.zip";
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();
    ZipReader r;
    REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
    REQUIRE(r.entry(0).name == "evil.txt");      // .. popped
    REQUIRE(r.entry(1).name == "abs/path.txt");  // leading slash stripped
    REQUIRE(r.entry(2).name == "windows/x.txt"); // drive stripped, \ → /
    REQUIRE(r.entry(3).name == "b.txt");         // backslashes normalized, .. popped
    // No traversal shape survives into the emitted-name space.
    for (size_t i = 0; i < r.entry_count(); ++i) {
        REQUIRE(r.entry(i).name.find("..") == std::string::npos);
        REQUIRE((r.entry(i).name.empty() || r.entry(i).name.front() != '/'));
    }
    std::cout << "    - traversal shapes normalized at transcode time; no weapons "
                 "forwarded"
              << std::endl;
}

static void test_zip_encrypted_and_methods() {
    std::cout << "[+] test_zip_encrypted_and_methods" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    core::uint32 crc = 0;
    const auto data = stored_payload("secret", crc);

    ZipMember enc;
    enc.name = "enc.txt";
    enc.flags = 0x0001; // encrypted
    enc.packed = data;
    enc.crc = crc;
    enc.unp_size = data.size();

    ZipMember bzip2;
    bzip2.name = "b.txt";
    bzip2.method = 12; // bzip2
    bzip2.packed = data;
    bzip2.crc = crc;
    bzip2.unp_size = data.size();

    const auto buf = build_zip({enc, bzip2});
    const auto p = dir / "refused.zip";
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();
    ZipReader r;
    REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
    REQUIRE(r.entry(0).encrypted);
    ExtractionLimits limits;
    LimitState state;
    REQUIRE(r.decode(0, {}, limits, state, ReaderHooks{}) == ForeignStatus::EncryptedRefused);
    REQUIRE(r.decode(1, {}, limits, state, ReaderHooks{}) == ForeignStatus::UnsupportedMethod);
    std::cout << "    - ZipCrypto refused by policy; bzip2 refused method-named" << std::endl;
}

static void test_zip_bomb_cap() {
    std::cout << "[+] test_zip_bomb_cap" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    // Declared unpacked size is a LIE (10 bytes) but the deflate stream
    // inflates to 100005. The in-flight cap (not the declaration) bounds it.
    ZipMember bomb;
    bomb.name = "bomb.bin";
    bomb.method = 8;
    bomb.flags = 0x0800;
    bomb.packed = kat_rle_stream;
    bomb.crc = kat_rle_plain_crc;
    bomb.unp_size = 10; // lying header
    const auto buf = build_zip({bomb});
    const auto p = dir / "bomb.zip";
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    f.close();
    ZipReader r;
    REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
    ExtractionLimits limits;
    limits.max_member_output_bytes = 1000;
    LimitState state;
    const ForeignStatus st = r.decode(0, {}, limits, state, ReaderHooks{});
    REQUIRE((st == ForeignStatus::LimitExceeded && "declared size must NOT bound output"));
    REQUIRE(state.total_out <= 1000 + 65536 + 1); // capped within a chunk
    std::cout << "    - lying header + real bomb: in-flight cap fired (D5)" << std::endl;
}

static void test_zip_comments_and_dirs() {
    std::cout << "[+] test_zip_comments_and_dirs" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    core::uint32 crc = 0;
    const auto data = stored_payload("x", crc);

    ZipMember f;
    f.name = "dir/file.txt";
    f.flags = 0x0800;
    f.packed = data;
    f.crc = crc;
    f.unp_size = data.size();
    f.entry_comment = "no RAR5 home for this";

    ZipMember d;
    d.name = "dir/";
    d.flags = 0x0800;
    d.is_dir = true;
    d.dos_attrs = 0x10;
    d.unix_mode = 0040755;

    const auto buf = build_zip({f, d}, "archive comment here");
    const auto p = dir / "meta.zip";
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    out.close();
    ZipReader r;
    REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
    REQUIRE(r.entry(0).comment == "no RAR5 home for this");
    REQUIRE(r.entry(1).type == ForeignType::Dir);
    REQUIRE(r.entry(1).name == "dir");
    REQUIRE(r.entry(0).has_posix_mode == false);
    std::cout << "    - per-entry comment captured for reporting; dir record "
                 "recognized"
              << std::endl;
}

static void test_zip_posix_modes_and_symlinks() {
    std::cout << "[+] test_zip_posix_modes_and_symlinks" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    core::uint32 crc1 = 0;
    const auto data1 = stored_payload("hello world", crc1);
    ZipMember f;
    f.name = "regular.txt";
    f.flags = 0x0800;
    f.packed = data1;
    f.crc = crc1;
    f.unp_size = data1.size();
    f.unix_mode = 0100644;

    core::uint32 crc2 = 0;
    const auto data2 = stored_payload("regular.txt", crc2);
    ZipMember s;
    s.name = "symlink.txt";
    s.flags = 0x0800;
    s.packed = data2;
    s.crc = crc2;
    s.unp_size = data2.size();
    s.unix_mode = 0120777;

    const auto buf = build_zip({f, s});
    const auto p = dir / "posix_zip.zip";
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    out.close();

    ZipReader r;
    REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
    REQUIRE(r.entry_count() == 2);
    REQUIRE(r.entry(0).has_posix_mode == true);
    REQUIRE(r.entry(0).posix_mode == 0100644);
    REQUIRE(r.entry(0).type == ForeignType::File);

    REQUIRE(r.entry(1).has_posix_mode == true);
    REQUIRE(r.entry(1).posix_mode == 0120777);
    REQUIRE(r.entry(1).type == ForeignType::Symlink);
    REQUIRE(r.entry(1).link_target == "regular.txt");
    std::cout << "    - ZIP regular POSIX mode and symlink type & target preserved" << std::endl;
}

// C-08 regression (x64-reachable, not 32-bit specific): the NTFS extra
// (0x000A) FILETIME is attacker-controlled. Bit-63 / max values previously hit
// signed overflow; they must now clamp, pre-1970 values fall back to the DOS
// timestamp, and a sane value round-trips exactly.
static void test_zip_ntfs_filetime_hostile_values() {
    std::cout << "[+] test_zip_ntfs_filetime_hostile_values" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    core::uint32 crc = 0;
    const auto data = stored_payload("ft", crc);

    const auto probe = [&](core::uint64 ft, int n) {
        ZipMember m;
        m.name = "t.txt";
        m.flags = 0x0800;
        m.packed = data;
        m.crc = crc;
        m.unp_size = data.size();
        put16(m.cd_extra, 0x000A);
        put16(m.cd_extra, 32); // reserved(4) + tag(2) + size(2) + 3*8
        put32(m.cd_extra, 0);
        put16(m.cd_extra, 1);
        put16(m.cd_extra, 24);
        put64(m.cd_extra, ft); // mtime
        put64(m.cd_extra, 0);  // atime
        put64(m.cd_extra, 0);  // ctime
        const auto buf = build_zip({m});
        const auto p = dir / ("ntfs_ft_" + std::to_string(n) + ".zip");
        std::ofstream out(p, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
        out.close();
        ZipReader r;
        REQUIRE(open_zip(p, r) == ForeignStatus::Ok);
        return r.entry(0);
    };

    const core::uint64 kUnixEpochFt = 116444736000000000ULL;
    for (core::uint64 ft : {0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL}) {
        const auto e = probe(ft, static_cast<int>(ft & 0xF));
        REQUIRE(e.timestamp_clamped);
        REQUIRE(e.mtime_sec >= 0);
        REQUIRE(e.mtime_sec <= 32503680000LL);
    }
    {
        const auto e = probe(1, 100); // pre-1970 -> DOS fallback (2024-01-01)
        REQUIRE(!e.timestamp_clamped);
        REQUIRE(e.mtime_sec > 1600000000LL);
        REQUIRE(e.mtime_sec < 1800000000LL);
    }
    {
        const auto e = probe(kUnixEpochFt + 1577836800ULL * 10000000ULL, 101);
        REQUIRE(!e.timestamp_clamped);
        REQUIRE(e.mtime_sec == 1577836800LL);
        REQUIRE(e.mtime_nsec == 0);
    }
    std::cout << "    - bit-63 / max FILETIME clamp; pre-1970 falls back; sane value exact"
              << std::endl;
}

static void test_zip_eocd_bounds() {
    std::cout << "[+] test_zip_eocd_bounds" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    core::uint32 crc = 0;
    const auto data = stored_payload("prepended", crc);
    ZipMember m;
    m.name = "p.txt";
    m.flags = 0x0800;
    m.packed = data;
    m.crc = crc;
    m.unp_size = data.size();

    // Prepended garbage (SFX-style) is tolerated via the bounded scan.
    const auto p1 = dir / "prepended.zip";
    {
        const auto buf = build_zip({m}, {}, 512);
        std::ofstream out(p1, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
        out.close();
    }
    ZipReader r;
    REQUIRE(open_zip(p1, r) == ForeignStatus::Ok);

    // Truncated archive → unparseable/truncated, never silent success.
    const auto p2 = dir / "cut.zip";
    {
        const auto buf = build_zip({m});
        std::ofstream out(p2, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size() / 2));
        out.close();
    }
    {
        ZipReader r2;
        std::string detail;
        ExtractionLimits limits;
        const ForeignStatus st = r2.open(p2, limits, ReaderHooks{}, detail);
        REQUIRE((st == ForeignStatus::Unparseable || st == ForeignStatus::Truncated));
    }
    std::cout << "    - prepended stub tolerated; truncated archive refused" << std::endl;
}


// ---------------------------------------------------------------------------
// TAR + GZIP fixtures and tests (v1.29 M3)
// ---------------------------------------------------------------------------

namespace {} // namespace

static void test_tar_ustar_roundtrip() {
    std::cout << "[+] test_tar_ustar_roundtrip" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    std::vector<TarMember> members;
    TarMember empty;
    empty.name = "empty.txt";
    members.push_back(empty);

    TarMember txt;
    txt.name = "hello.txt";
    txt.data = std::vector<core::byte>(100, core::byte('A'));
    members.push_back(txt);

    TarMember big;
    big.name = "big.bin";
    big.data = std::vector<core::byte>(70000, core::byte('B')); // multi-block payload
    big.mode = 0600;
    members.push_back(big);

    TarMember d;
    d.name = "subdir";
    d.typeflag = '5';
    d.mode = 0755;
    members.push_back(d);

    const auto p = dir / "ustar.tar";
    REQUIRE(write_file(p, build_tar(members)));

    TarReader r;
    std::string detail;
    ExtractionLimits limits;
    REQUIRE(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    REQUIRE(r.entry_count() == 4);
    REQUIRE((r.entry(0).name == "empty.txt" && r.entry(0).type == ForeignType::File));
    REQUIRE(r.entry(1).unpacked_size == 100);
    REQUIRE(r.entry(2).unpacked_size == 70000);
    REQUIRE((r.entry(2).has_posix_mode && r.entry(2).posix_mode == 0100600));
    REQUIRE(r.entry(3).type == ForeignType::Dir);
    REQUIRE((r.entry(1).owner_name == "tester" && r.entry(1).owner_uid == 1000));
    REQUIRE(r.entry(1).mtime_sec == 1704110400);

    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* data, size_t size) {
        out.insert(out.end(), data, data + size);
        return true;
    };
    REQUIRE(r.decode(1, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out.size() == 100);
    out.clear();
    REQUIRE(r.decode(2, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out.size() == 70000);
    std::cout << "    - ustar members, modes, owners, times, multi-block payload" << std::endl;
}

static void test_tar_longname_and_pax() {
    std::cout << "[+] test_tar_longname_and_pax" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    const std::string long_name(150, 'n');
    std::vector<TarMember> members;
    TarMember ln;
    ln.name = long_name;
    ln.use_longname = true;
    ln.data = std::vector<core::byte>(10, core::byte('x'));
    members.push_back(ln);

    TarMember pref;
    pref.name = std::string(60, 'd') + "/" + std::string(60, 'f');
    pref.data = std::vector<core::byte>(5, core::byte('y'));
    members.push_back(pref);

    TarMember px;
    px.name = "orig.txt";
    px.pax = {{"path", "pax/renamed.txt"},
              {"mtime", "1704110400.5"},
              {"uid", "42"},
              {"gid", "43"},
              {"uname", "paxuser"},
              {"gname", "paxgroup"},
              {"vendor.custom.key", "ignored"}};
    px.data = std::vector<core::byte>(3, core::byte('z'));
    members.push_back(px);

    const auto p = dir / "long.tar";
    REQUIRE(write_file(p, build_tar(members)));

    TarReader r;
    std::string detail;
    ExtractionLimits limits;
    const ForeignStatus st = r.open(p, limits, ReaderHooks{}, detail);
    if (st != ForeignStatus::Ok)
        std::cout << "    ! open st=" << static_cast<int>(st) << " detail=" << detail << std::endl;
    REQUIRE(st == ForeignStatus::Ok);
    REQUIRE(r.entry_count() == 3);
    REQUIRE(r.entry(0).name == long_name);
    REQUIRE(r.entry(1).name == pref.name); // prefix split reconstructed
    REQUIRE(r.entry(2).name == "pax/renamed.txt");
    REQUIRE((r.entry(2).mtime_sec == 1704110400 && r.entry(2).mtime_nsec == 500000000));
    REQUIRE((r.entry(2).owner_uid == 42 && r.entry(2).owner_name == "paxuser"));
    std::cout << "    - GNU longname, ustar prefix split, pax subset handled" << std::endl;
}

static void test_tar_sparse_refused_and_specials() {
    std::cout << "[+] test_tar_sparse_refused_and_specials" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    std::vector<TarMember> members;
    TarMember sparse;
    sparse.name = "sparse.bin";
    sparse.pax = {{"GNU.sparse.major", "1"}, {"GNU.sparse.minor", "0"}};
    sparse.data = std::vector<core::byte>(512, core::byte(0));
    members.push_back(sparse);

    TarMember chr;
    chr.name = "null";
    chr.typeflag = '3';
    members.push_back(chr);

    TarMember lnk;
    lnk.name = "link-to-hello";
    lnk.typeflag = '1';
    lnk.linkname = "hello.txt";
    members.push_back(lnk);

    TarMember sym;
    sym.name = "sym-to-hello";
    sym.typeflag = '2';
    sym.linkname = "hello.txt";
    members.push_back(sym);

    const auto p = dir / "special.tar";
    REQUIRE(write_file(p, build_tar(members)));

    TarReader r;
    std::string detail;
    ExtractionLimits limits;
    REQUIRE(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    REQUIRE(r.entry_count() == 4);
    LimitState state;
    REQUIRE(r.decode(0, {}, limits, state, ReaderHooks{}) == ForeignStatus::SparseRefused);
    REQUIRE(r.entry(1).type == ForeignType::Other);
    REQUIRE(r.entry(2).type == ForeignType::Hardlink);
    REQUIRE(r.entry(2).link_target == "hello.txt");
    REQUIRE(r.entry(3).type == ForeignType::Symlink);
    std::cout << "    - sparse refused per entry; specials Other; links as records" << std::endl;
}

// C-06: a pax "size" record overrides the ustar size field (members > 8 GiB,
// or writers that zero the octal field). The payload advance must follow the
// effective size; advancing by the stale ustar padding parsed the member's
// data as the next header.
static void test_tar_pax_size_override_framing() {
    std::cout << "[+] test_tar_pax_size_override_framing" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    const std::vector<core::byte> payload(700, core::byte('P')); // 2 blocks padded

    std::vector<core::byte> tar;
    {
        const std::string rec_body = "size=" + std::to_string(payload.size()) + "\n";
        std::string rec = std::to_string(rec_body.size() + 3) + " " + rec_body; // "NN size=..."
        if (rec.size() != rec_body.size() + 3) rec = std::to_string(rec.size()) + " " + rec_body;
        std::vector<core::byte> recs(rec.begin(), rec.end());
        recs.resize(512, core::byte(0));
        auto ph = tar_header("PaxHeaders/big", 'x', rec.size(), 0644, 0, "", "root", "root", 0, 0);
        tar.insert(tar.end(), ph.begin(), ph.end());
        tar.insert(tar.end(), recs.begin(), recs.end());
    }
    // The real member's ustar size field is ZERO; only pax carries the size.
    auto mh = tar_header("big.bin", '0', 0, 0644, 1704110400, "", "u", "g", 1, 1);
    tar.insert(tar.end(), mh.begin(), mh.end());
    tar.insert(tar.end(), payload.begin(), payload.end());
    tar.resize(tar.size() + (512 - payload.size() % 512) % 512, core::byte(0));
    // A second member proves the reader re-synchronised after the payload.
    const std::vector<core::byte> tail(5, core::byte('T'));
    auto th = tar_header("after.txt", '0', tail.size(), 0644, 1704110400, "", "u", "g", 1, 1);
    tar.insert(tar.end(), th.begin(), th.end());
    tar.insert(tar.end(), tail.begin(), tail.end());
    tar.resize(tar.size() + 507 + 1024, core::byte(0));

    const auto p = dir / "pax_size.tar";
    REQUIRE(write_file(p, tar));
    TarReader r;
    std::string detail;
    ExtractionLimits limits;
    REQUIRE(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    REQUIRE(r.entry_count() == 2);
    REQUIRE(r.entry(0).unpacked_size == payload.size());
    REQUIRE(r.entry(1).name == "after.txt");
    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* d, size_t n) {
        out.insert(out.end(), d, d + n);
        return true;
    };
    REQUIRE(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out == payload);
    std::cout << "    - pax size override frames the next member correctly" << std::endl;
}

// C-05: the ZIP64 extra carries an 8-byte field only for each 32-bit field
// that is 0xFFFFFFFF. A CDH with ONLY the uncompressed size as placeholder
// (a > 4 GiB file that compressed below 4 GiB) has an 8-byte record; the old
// parser required 16 bytes, left the placeholder in place, and failed the
// LFH cross-check.
static void test_zip64_uncompressed_only_placeholder() {
    std::cout << "[+] test_zip64_uncompressed_only_placeholder" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");
    core::uint32 crc = 0;
    const auto data = stored_payload("hello", crc);
    const std::string name = "u64.txt";

    std::vector<core::byte> z;
    // LFH: both sizes are placeholders; the LOCAL ZIP64 record carries both
    // (APPNOTE 4.5.3).
    put32(z, 0x04034b50);
    put16(z, 45);
    put16(z, 0);
    put16(z, 0); // stored
    put16(z, 0x6000);
    put16(z, 0x5821);
    put32(z, crc);
    put32(z, 0xFFFFFFFFu);
    put32(z, 0xFFFFFFFFu);
    put16(z, static_cast<core::uint16>(name.size()));
    put16(z, 20);
    put_str(z, name);
    put16(z, 0x0001);
    put16(z, 16);
    put64(z, data.size());
    put64(z, data.size());
    z.insert(z.end(), data.begin(), data.end());
    // CDH: only the uncompressed size is a placeholder -> 8-byte record.
    const core::uint32 cd_off = static_cast<core::uint32>(z.size());
    put32(z, 0x02014b50);
    put16(z, 0x031E);
    put16(z, 45);
    put16(z, 0);
    put16(z, 0);
    put16(z, 0x6000);
    put16(z, 0x5821);
    put32(z, crc);
    put32(z, static_cast<core::uint32>(data.size())); // compressed: real value
    put32(z, 0xFFFFFFFFu);                            // uncompressed: placeholder
    put16(z, static_cast<core::uint16>(name.size()));
    put16(z, 12); // extra len: 4 header + 8 (unp only)
    put16(z, 0);
    put16(z, 0);
    put16(z, 0);
    put32(z, 0);
    put32(z, 0); // LFH offset
    put_str(z, name);
    put16(z, 0x0001);
    put16(z, 8);
    put64(z, data.size());
    const core::uint32 cd_size = static_cast<core::uint32>(z.size()) - cd_off;
    put32(z, 0x06054b50);
    put16(z, 0);
    put16(z, 0);
    put16(z, 1);
    put16(z, 1);
    put32(z, cd_size);
    put32(z, cd_off);
    put16(z, 0);

    const auto p = dir / "zip64_unp_only.zip";
    REQUIRE(write_file(p, z));
    ZipReader r;
    size_t count = 0;
    REQUIRE(open_zip(p, r, &count) == ForeignStatus::Ok);
    REQUIRE(count == 1);
    REQUIRE(r.entry(0).unpacked_size == data.size());
    std::cout << "    - unp-only ZIP64 placeholder parsed from an 8-byte record" << std::endl;
}

static void test_tar_truncated_and_checksum() {
    std::cout << "[+] test_tar_truncated_and_checksum" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    std::vector<TarMember> members;
    TarMember m;
    m.name = "a.txt";
    m.data = std::vector<core::byte>(1000, core::byte('q'));
    members.push_back(m);
    const auto good = build_tar(members);

    const auto p1 = dir / "cut.tar";
    std::vector<core::byte> cut(good.begin(), good.begin() + 512 + 400);
    REQUIRE(write_file(p1, cut));
    {
        TarReader r;
        std::string detail;
        ExtractionLimits limits;
        REQUIRE(r.open(p1, limits, ReaderHooks{}, detail) == ForeignStatus::Truncated);
    }

    const auto p2 = dir / "badck.tar";
    auto bad = good;
    bad[10] ^= 0xFF; // inside the name field -> checksum no longer matches
    REQUIRE(write_file(p2, bad));
    {
        TarReader r;
        std::string detail;
        ExtractionLimits limits;
        REQUIRE(r.open(p2, limits, ReaderHooks{}, detail) == ForeignStatus::Unparseable);
    }

    // EOF without the two zero blocks is accepted (streamed tapes).
    const auto p3 = dir / "nopad.tar";
    std::vector<core::byte> nopad(good.begin(), good.end() - 1024);
    REQUIRE(write_file(p3, nopad));
    {
        TarReader r;
        std::string detail;
        ExtractionLimits limits;
        REQUIRE(r.open(p3, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
        REQUIRE(r.entry_count() == 1);
    }
    std::cout << "    - truncation refused, checksum refused, missing padding tolerated"
              << std::endl;
}

static void test_gzip_multimember_and_caps() {
    std::cout << "[+] test_gzip_multimember_and_caps" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    const auto p = dir / "multi.gz";
    REQUIRE(write_file(p, build_gzip({&kat_text_stream, &kat_rle_stream},
                                     {kat_text_plain_crc, kat_rle_plain_crc},
                                     {static_cast<core::uint32>(kat_text_plain_len),
                                      static_cast<core::uint32>(kat_rle_plain_len)},
                                     "combined.txt")));

    GzipReader r;
    std::string detail;
    ExtractionLimits limits;
    REQUIRE(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    REQUIRE(r.entry_count() == 1); // ONE logical entry
    REQUIRE(r.entry(0).name == "combined.txt");

    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* data, size_t size) {
        out.insert(out.end(), data, data + size);
        return true;
    };
    REQUIRE(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    REQUIRE(out.size() == kat_text_plain_len + kat_rle_plain_len);
    crypto::Crc32 c3;
    c3.update(out.data(), kat_text_plain_len);
    REQUIRE(c3.get() == kat_text_plain_crc);
    crypto::Crc32 c4;
    c4.update(out.data() + kat_text_plain_len, kat_rle_plain_len);
    REQUIRE(c4.get() == kat_rle_plain_crc);

    // In-flight cap (D5): the 100005-byte second member cannot hide.
    GzipReader r2;
    REQUIRE(r2.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    ExtractionLimits capped;
    capped.max_member_output_bytes = 1000;
    LimitState s2;
    REQUIRE(r2.decode(0, {}, capped, s2, ReaderHooks{}) == ForeignStatus::LimitExceeded);

    // Corrupt trailer -> CrcMismatch.
    const auto p3 = dir / "badcrc.gz";
    REQUIRE(write_file(p3, build_gzip({&kat_text_stream}, {kat_text_plain_crc},
                                      {static_cast<core::uint32>(kat_text_plain_len)}, "", true)));
    {
        GzipReader r3;
        REQUIRE(r3.open(p3, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
        LimitState s3;
        REQUIRE(r3.decode(0, {}, limits, s3, ReaderHooks{}) == ForeignStatus::CrcMismatch);
    }
    std::cout << "    - multi-member concatenated, per-member CRC/ISIZE, cap, bad CRC" << std::endl;
}

static void test_gzip_name_and_detect() {
    std::cout << "[+] test_gzip_name_and_detect" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    // No FNAME -> stem convention (strip .gz).
    const auto p = dir / "archive.tar.gz";
    REQUIRE(write_file(p, build_gzip({&kat_text_stream}, {kat_text_plain_crc},
                                     {static_cast<core::uint32>(kat_text_plain_len)})));
    REQUIRE(detect_foreign_format(p) == SourceFormat::Gzip);
    {
        GzipReader r;
        std::string detail;
        ExtractionLimits limits;
        REQUIRE(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
        REQUIRE(r.entry(0).name == "archive.tar");
    }

    // .tgz -> .tar.
    const auto p2 = dir / "bundle.tgz";
    REQUIRE(write_file(p2, build_gzip({&kat_text_stream}, {kat_text_plain_crc},
                                      {static_cast<core::uint32>(kat_text_plain_len)})));
    {
        GzipReader r;
        std::string detail;
        ExtractionLimits limits;
        REQUIRE(r.open(p2, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
        REQUIRE(r.entry(0).name == "bundle.tar");
    }
    std::cout << "    - stem conventions and gzip dispatch pinned" << std::endl;
}

static const openrar::test::Case k_cases[] = {
    {"test_dispatch_precedence", test_dispatch_precedence, false},
    {"test_zip_roundtrip_store_and_deflate", test_zip_roundtrip_store_and_deflate, false},
    {"test_zip_cd_lf_mismatch", test_zip_cd_lf_mismatch, false},
    {"test_zip_bit3_descriptor_tolerance", test_zip_bit3_descriptor_tolerance, false},
    {"test_zip_zip64_roundtrip", test_zip_zip64_roundtrip, false},
    {"test_zip_names", test_zip_names, false},
    {"test_zip_traversal_names", test_zip_traversal_names, false},
    {"test_zip_encrypted_and_methods", test_zip_encrypted_and_methods, false},
    {"test_zip_bomb_cap", test_zip_bomb_cap, false},
    {"test_zip_comments_and_dirs", test_zip_comments_and_dirs, false},
    {"test_zip_posix_modes_and_symlinks", test_zip_posix_modes_and_symlinks, false},
    {"test_zip_eocd_bounds", test_zip_eocd_bounds, false},
    {"test_tar_ustar_roundtrip", test_tar_ustar_roundtrip, false},
    {"test_tar_longname_and_pax", test_tar_longname_and_pax, false},
    {"test_tar_sparse_refused_and_specials", test_tar_sparse_refused_and_specials, false},
    {"test_tar_truncated_and_checksum", test_tar_truncated_and_checksum, false},
    {"test_tar_pax_size_override_framing", test_tar_pax_size_override_framing, false},
    {"test_zip64_uncompressed_only_placeholder", test_zip64_uncompressed_only_placeholder, false},
    {"test_zip_ntfs_filetime_hostile_values", test_zip_ntfs_filetime_hostile_values, false},
    {"test_gzip_multimember_and_caps", test_gzip_multimember_and_caps, false},
    {"test_gzip_name_and_detect", test_gzip_name_and_detect, false}};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All Foreign Format (v1.29 M2/M3) tests PASSED!";
    return rc;
}
