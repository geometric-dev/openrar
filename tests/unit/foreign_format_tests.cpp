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
        assert(detect_foreign_format(p) == SourceFormat::Zip);
    }
    // EOCD-only (empty) archive is still ZIP.
    {
        const auto p = dir / "empty.zip";
        std::ofstream f(p, std::ios::binary);
        const auto buf = build_zip({});
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
        f.close();
        assert(detect_foreign_format(p) == SourceFormat::Zip);
    }
    // RAR5 / legacy RAR / garbage.
    {
        const auto p = dir / "rar5.rar";
        std::ofstream f(p, std::ios::binary);
        f.write("Rar!\x1a\x07\x01\x00", 8);
        f.close();
        assert(detect_foreign_format(p) == SourceFormat::Rar5);
    }
    {
        const auto p = dir / "rar2.rar";
        std::ofstream f(p, std::ios::binary);
        f.write("Rar!\x1a\x07\x00", 7);
        f.close();
        assert(detect_foreign_format(p) == SourceFormat::LegacyRar);
    }
    {
        const auto p = dir / "junk.bin";
        std::ofstream f(p, std::ios::binary);
        f.write("not an archive at all, just text", 32);
        f.close();
        assert(detect_foreign_format(p) == SourceFormat::None);
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
    assert(open_zip(p, r, &count) == ForeignStatus::Ok);
    assert(count == 3);

    // Stored payload decodes byte-exact; mtime = DOS 2024-01-01 12:00:00.
    ExtractionLimits limits;
    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* data, size_t size) {
        out.insert(out.end(), data, data + size);
        return true;
    };
    assert(r.entry(0).type == ForeignType::File);
    assert(r.entry(0).mtime_sec == 1704110400);
    assert(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    assert(out == txt);
    assert(r.decode(1, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    assert(out.size() == txt.size() + bin.size());

    out.clear();
    assert(r.decode(2, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    assert(out.size() == kat_rle_plain_len);
    crypto::Crc32 crc;
    crc.update(out.data(), out.size());
    assert(crc.get() == kat_rle_plain_crc);
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
        assert(st == expect && detail.c_str());
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
    assert(open_zip(p, r) == ForeignStatus::Ok);
    ExtractionLimits limits;
    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* d, size_t n) {
        out.insert(out.end(), d, d + n);
        return true;
    };
    assert(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    assert(out == data);
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
    assert(open_zip(p, r) == ForeignStatus::Ok);
    assert(r.entry(0).unpacked_size == data.size());
    ExtractionLimits limits;
    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* d, size_t n) {
        out.insert(out.end(), d, d + n);
        return true;
    };
    assert(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    assert(out == data);
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
    assert(open_zip(p, r) == ForeignStatus::Ok);
    assert(r.entry(0).name == "bad%FF_name.txt");
    assert(r.entry(0).name_escaped);
    assert(r.entry(1).name == "caf\xc3\xa9.txt");
    assert(!r.entry(1).name_escaped);
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
    assert(open_zip(p, r) == ForeignStatus::Ok);
    assert(r.entry(0).name == "evil.txt");      // .. popped
    assert(r.entry(1).name == "abs/path.txt");  // leading slash stripped
    assert(r.entry(2).name == "windows/x.txt"); // drive stripped, \ → /
    assert(r.entry(3).name == "b.txt");         // backslashes normalized, .. popped
    // No traversal shape survives into the emitted-name space.
    for (size_t i = 0; i < r.entry_count(); ++i) {
        assert(r.entry(i).name.find("..") == std::string::npos);
        assert(r.entry(i).name.empty() || r.entry(i).name.front() != '/');
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
    assert(open_zip(p, r) == ForeignStatus::Ok);
    assert(r.entry(0).encrypted);
    ExtractionLimits limits;
    LimitState state;
    assert(r.decode(0, {}, limits, state, ReaderHooks{}) == ForeignStatus::EncryptedRefused);
    assert(r.decode(1, {}, limits, state, ReaderHooks{}) == ForeignStatus::UnsupportedMethod);
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
    assert(open_zip(p, r) == ForeignStatus::Ok);
    ExtractionLimits limits;
    limits.max_member_output_bytes = 1000;
    LimitState state;
    const ForeignStatus st = r.decode(0, {}, limits, state, ReaderHooks{});
    assert(st == ForeignStatus::LimitExceeded && "declared size must NOT bound output");
    assert(state.total_out <= 1000 + 65536 + 1); // capped within a chunk
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
    assert(open_zip(p, r) == ForeignStatus::Ok);
    assert(r.entry(0).comment == "no RAR5 home for this");
    assert(r.entry(1).type == ForeignType::Dir);
    assert(r.entry(1).name == "dir");
    assert(r.entry(0).has_posix_mode == false);
    std::cout << "    - per-entry comment captured for reporting; dir record "
                 "recognized"
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
    assert(open_zip(p1, r) == ForeignStatus::Ok);

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
        assert(st == ForeignStatus::Unparseable || st == ForeignStatus::Truncated);
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
    assert(write_file(p, build_tar(members)));

    TarReader r;
    std::string detail;
    ExtractionLimits limits;
    assert(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    assert(r.entry_count() == 4);
    assert(r.entry(0).name == "empty.txt" && r.entry(0).type == ForeignType::File);
    assert(r.entry(1).unpacked_size == 100);
    assert(r.entry(2).unpacked_size == 70000);
    assert(r.entry(2).has_posix_mode && r.entry(2).posix_mode == 0100600);
    assert(r.entry(3).type == ForeignType::Dir);
    assert(r.entry(1).owner_name == "tester" && r.entry(1).owner_uid == 1000);
    assert(r.entry(1).mtime_sec == 1704110400);

    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* data, size_t size) {
        out.insert(out.end(), data, data + size);
        return true;
    };
    assert(r.decode(1, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    assert(out.size() == 100);
    out.clear();
    assert(r.decode(2, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    assert(out.size() == 70000);
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
    assert(write_file(p, build_tar(members)));

    TarReader r;
    std::string detail;
    ExtractionLimits limits;
    const ForeignStatus st = r.open(p, limits, ReaderHooks{}, detail);
    if (st != ForeignStatus::Ok)
        std::cout << "    ! open st=" << static_cast<int>(st) << " detail=" << detail << std::endl;
    assert(st == ForeignStatus::Ok);
    assert(r.entry_count() == 3);
    assert(r.entry(0).name == long_name);
    assert(r.entry(1).name == pref.name); // prefix split reconstructed
    assert(r.entry(2).name == "pax/renamed.txt");
    assert(r.entry(2).mtime_sec == 1704110400 && r.entry(2).mtime_nsec == 500000000);
    assert(r.entry(2).owner_uid == 42 && r.entry(2).owner_name == "paxuser");
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
    assert(write_file(p, build_tar(members)));

    TarReader r;
    std::string detail;
    ExtractionLimits limits;
    assert(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    assert(r.entry_count() == 4);
    LimitState state;
    assert(r.decode(0, {}, limits, state, ReaderHooks{}) == ForeignStatus::SparseRefused);
    assert(r.entry(1).type == ForeignType::Other);
    assert(r.entry(2).type == ForeignType::Hardlink);
    assert(r.entry(2).link_target == "hello.txt");
    assert(r.entry(3).type == ForeignType::Symlink);
    std::cout << "    - sparse refused per entry; specials Other; links as records" << std::endl;
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
    assert(write_file(p1, cut));
    {
        TarReader r;
        std::string detail;
        ExtractionLimits limits;
        assert(r.open(p1, limits, ReaderHooks{}, detail) == ForeignStatus::Truncated);
    }

    const auto p2 = dir / "badck.tar";
    auto bad = good;
    bad[10] ^= 0xFF; // inside the name field -> checksum no longer matches
    assert(write_file(p2, bad));
    {
        TarReader r;
        std::string detail;
        ExtractionLimits limits;
        assert(r.open(p2, limits, ReaderHooks{}, detail) == ForeignStatus::Unparseable);
    }

    // EOF without the two zero blocks is accepted (streamed tapes).
    const auto p3 = dir / "nopad.tar";
    std::vector<core::byte> nopad(good.begin(), good.end() - 1024);
    assert(write_file(p3, nopad));
    {
        TarReader r;
        std::string detail;
        ExtractionLimits limits;
        assert(r.open(p3, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
        assert(r.entry_count() == 1);
    }
    std::cout << "    - truncation refused, checksum refused, missing padding tolerated"
              << std::endl;
}

static void test_gzip_multimember_and_caps() {
    std::cout << "[+] test_gzip_multimember_and_caps" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    const auto p = dir / "multi.gz";
    assert(write_file(p, build_gzip({&kat_text_stream, &kat_rle_stream},
                                    {kat_text_plain_crc, kat_rle_plain_crc},
                                    {static_cast<core::uint32>(kat_text_plain_len),
                                     static_cast<core::uint32>(kat_rle_plain_len)},
                                    "combined.txt")));

    GzipReader r;
    std::string detail;
    ExtractionLimits limits;
    assert(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    assert(r.entry_count() == 1); // ONE logical entry
    assert(r.entry(0).name == "combined.txt");

    LimitState state;
    std::vector<core::byte> out;
    auto sink = [&](const core::byte* data, size_t size) {
        out.insert(out.end(), data, data + size);
        return true;
    };
    assert(r.decode(0, sink, limits, state, ReaderHooks{}) == ForeignStatus::Ok);
    assert(out.size() == kat_text_plain_len + kat_rle_plain_len);
    crypto::Crc32 c3;
    c3.update(out.data(), kat_text_plain_len);
    assert(c3.get() == kat_text_plain_crc);
    crypto::Crc32 c4;
    c4.update(out.data() + kat_text_plain_len, kat_rle_plain_len);
    assert(c4.get() == kat_rle_plain_crc);

    // In-flight cap (D5): the 100005-byte second member cannot hide.
    GzipReader r2;
    assert(r2.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
    ExtractionLimits capped;
    capped.max_member_output_bytes = 1000;
    LimitState s2;
    assert(r2.decode(0, {}, capped, s2, ReaderHooks{}) == ForeignStatus::LimitExceeded);

    // Corrupt trailer -> CrcMismatch.
    const auto p3 = dir / "badcrc.gz";
    assert(write_file(p3, build_gzip({&kat_text_stream}, {kat_text_plain_crc},
                                     {static_cast<core::uint32>(kat_text_plain_len)}, "", true)));
    {
        GzipReader r3;
        assert(r3.open(p3, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
        LimitState s3;
        assert(r3.decode(0, {}, limits, s3, ReaderHooks{}) == ForeignStatus::CrcMismatch);
    }
    std::cout << "    - multi-member concatenated, per-member CRC/ISIZE, cap, bad CRC" << std::endl;
}

static void test_gzip_name_and_detect() {
    std::cout << "[+] test_gzip_name_and_detect" << std::endl;
    const auto dir = openrar::test::scratch_dir("foreign_format");

    // No FNAME -> stem convention (strip .gz).
    const auto p = dir / "archive.tar.gz";
    assert(write_file(p, build_gzip({&kat_text_stream}, {kat_text_plain_crc},
                                    {static_cast<core::uint32>(kat_text_plain_len)})));
    assert(detect_foreign_format(p) == SourceFormat::Gzip);
    {
        GzipReader r;
        std::string detail;
        ExtractionLimits limits;
        assert(r.open(p, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
        assert(r.entry(0).name == "archive.tar");
    }

    // .tgz -> .tar.
    const auto p2 = dir / "bundle.tgz";
    assert(write_file(p2, build_gzip({&kat_text_stream}, {kat_text_plain_crc},
                                     {static_cast<core::uint32>(kat_text_plain_len)})));
    {
        GzipReader r;
        std::string detail;
        ExtractionLimits limits;
        assert(r.open(p2, limits, ReaderHooks{}, detail) == ForeignStatus::Ok);
        assert(r.entry(0).name == "bundle.tar");
    }
    std::cout << "    - stem conventions and gzip dispatch pinned" << std::endl;
}

int main() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    test_dispatch_precedence();
    std::cout << std::flush;
    test_zip_roundtrip_store_and_deflate();
    std::cout << std::flush;
    test_zip_cd_lf_mismatch();
    std::cout << std::flush;
    test_zip_bit3_descriptor_tolerance();
    std::cout << std::flush;
    test_zip_zip64_roundtrip();
    std::cout << std::flush;
    test_zip_names();
    std::cout << std::flush;
    test_zip_traversal_names();
    std::cout << std::flush;
    test_zip_encrypted_and_methods();
    std::cout << std::flush;
    test_zip_bomb_cap();
    std::cout << std::flush;
    test_zip_comments_and_dirs();
    std::cout << std::flush;
    test_zip_eocd_bounds();
    std::cout << std::flush;
    test_tar_ustar_roundtrip();
    std::cout << std::flush;
    test_tar_longname_and_pax();
    std::cout << std::flush;
    test_tar_sparse_refused_and_specials();
    std::cout << std::flush;
    test_tar_truncated_and_checksum();
    std::cout << std::flush;
    test_gzip_multimember_and_caps();
    std::cout << std::flush;
    test_gzip_name_and_detect();
    std::cout << std::flush;
    std::cout << "All Foreign Format (v1.29 M2/M3) tests PASSED!" << std::endl;
    return 0;
}
