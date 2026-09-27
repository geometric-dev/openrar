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

void put16(std::vector<core::byte>& b, core::uint16 v) {
    b.push_back(static_cast<core::byte>(v & 0xFF));
    b.push_back(static_cast<core::byte>(v >> 8));
}
void put32(std::vector<core::byte>& b, core::uint32 v) {
    put16(b, static_cast<core::uint16>(v & 0xFFFF));
    put16(b, static_cast<core::uint16>(v >> 16));
}
void put64(std::vector<core::byte>& b, core::uint64 v) {
    put32(b, static_cast<core::uint32>(v & 0xFFFFFFFFu));
    put32(b, static_cast<core::uint32>(v >> 32));
}
void put_str(std::vector<core::byte>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
}

struct ZipMember {
    std::string name;
    core::uint16 method = 0;
    core::uint16 flags = 0;         // GP flags (bit 3 descriptor, bit 11 utf-8, bit 0 encrypted)
    std::vector<core::byte> packed; // packed payload bytes
    core::uint32 crc = 0;           // CRC32 of the UNPACKED payload
    core::uint64 unp_size = 0;
    // Mismatch injections (default: none — consistent archive):
    bool lfh_name_mismatch = false;
    bool lfh_method_mismatch = false;
    bool lfh_size_mismatch = false;
    bool lfh_crc_mismatch = false;
    bool cd_offset_mismatch = false; // CDH points at the wrong LFH offset
    bool zip64 = false;              // 0xFFFFFFFF placeholders + ZIP64 extra
    std::string entry_comment;
    core::uint32 dos_attrs = 0;
    core::uint32 unix_mode = 0; // external attrs high 16 bits
    bool is_dir = false;
};

// Builds a complete ZIP (LFHs + data + CD + EOCD). With no injections the
// archive is fully consistent.
std::vector<core::byte> build_zip(const std::vector<ZipMember>& members,
                                  const std::string& archive_comment = {},
                                  size_t prepend_garbage = 0) {
    std::vector<core::byte> out;
    if (prepend_garbage) out.assign(prepend_garbage, 0x41);

    struct Done {
        core::uint64 lfh_off;
        core::uint64 data_off;
        const ZipMember* m;
    };
    std::vector<Done> done;

    for (const ZipMember& m : members) {
        const core::uint32 unp32 = m.zip64 ? 0xFFFFFFFFu : static_cast<core::uint32>(m.unp_size);
        const core::uint32 pack32 =
            m.zip64 ? 0xFFFFFFFFu : static_cast<core::uint32>(m.packed.size());
        const std::string lfh_name =
            m.lfh_name_mismatch ? std::string("Z") + m.name.substr(1) : m.name;
        const core::uint16 name_len = static_cast<core::uint16>(lfh_name.size());
        std::vector<core::byte> lfh;
        put32(lfh, 0x04034b50);
        put16(lfh, 20); // version needed
        put16(lfh, m.flags);
        put16(lfh, m.method);
        put16(lfh, 0x6000); // DOS time 12:00:00
        put16(lfh, 0x5821); // DOS date 2024-01-01
        put32(lfh, m.lfh_crc_mismatch ? m.crc ^ 0xFFFF : m.crc);
        put32(lfh, pack32);
        put32(lfh, m.lfh_size_mismatch ? static_cast<core::uint32>(m.unp_size + 7) : unp32);
        put16(lfh, name_len);
        put16(lfh, m.zip64 ? 20 : 0); // LFH extra len (ZIP64 size fields ride here)
        put_str(lfh, lfh_name);
        if (m.zip64) {
            put16(lfh, 0x0001);
            put16(lfh, 16);
            put64(lfh, m.unp_size);
            put64(lfh, m.packed.size());
        }
        done.push_back({out.size(), out.size() + lfh.size(), &m});
        out.insert(out.end(), lfh.begin(), lfh.end());
        out.insert(out.end(), m.packed.begin(), m.packed.end());
    }

    const core::uint64 cd_offset = out.size();
    for (const Done& d : done) {
        const ZipMember& m = *d.m;
        std::vector<core::byte> cdh;
        put32(cdh, 0x02014b50);
        put16(cdh, 0x031E); // made by: unix, 3.0
        put16(cdh, 20);
        put16(cdh, m.flags);
        put16(cdh, m.lfh_method_mismatch ? static_cast<core::uint16>(m.method ^ 0xFF) : m.method);
        put16(cdh, 0x6000);
        put16(cdh, 0x5821);
        put32(cdh, m.crc);
        put32(cdh, m.zip64 ? 0xFFFFFFFFu : static_cast<core::uint32>(m.packed.size()));
        put32(cdh, m.zip64 ? 0xFFFFFFFFu : static_cast<core::uint32>(m.unp_size));
        put16(cdh, static_cast<core::uint16>(m.name.size()));
        const bool zip64_extra = m.zip64;
        put16(cdh, zip64_extra ? 20 : 0); // extra len
        put16(cdh, static_cast<core::uint16>(m.entry_comment.size()));
        put16(cdh, 0); // disk start
        put16(cdh, 0); // internal attrs
        put32(cdh, m.dos_attrs | (m.unix_mode << 16));
        // CDH LFH offset: data offset (the LFH header start); injected
        // mismatch points at the next member's area instead.
        const core::uint64 off = d.lfh_off;
        put32(cdh, m.cd_offset_mismatch ? static_cast<core::uint32>(off + 3)
                                        : static_cast<core::uint32>(off));
        put_str(cdh, m.name);
        if (zip64_extra) {
            put16(cdh, 0x0001);
            put16(cdh, 16);
            put64(cdh, m.unp_size);
            put64(cdh, m.packed.size());
        }
        put_str(cdh, m.entry_comment);
        out.insert(out.end(), cdh.begin(), cdh.end());
    }
    const core::uint64 cd_size = out.size() - cd_offset;

    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<core::uint16>(members.size()));
    put16(out, static_cast<core::uint16>(members.size()));
    put32(out, static_cast<core::uint32>(cd_size));
    put32(out, static_cast<core::uint32>(cd_offset));
    put16(out, static_cast<core::uint16>(archive_comment.size()));
    put_str(out, archive_comment);
    return out;
}

std::vector<core::byte> stored_payload(const std::string& s, core::uint32& crc_out) {
    crypto::Crc32 crc;
    crc.update(s.data(), s.size());
    crc_out = crc.get();
    return std::vector<core::byte>(s.begin(), s.end());
}

ForeignStatus open_zip(const std::filesystem::path& p, ZipReader& r, size_t* count = nullptr) {
    ExtractionLimits limits;
    r = ZipReader{};
    std::string detail;
    const ForeignStatus st = r.open(p, limits, ReaderHooks{}, detail);
    if (st == ForeignStatus::Ok && count) *count = r.entry_count();
    return st;
}

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

namespace {

struct TarMember {
    std::string name;
    std::string linkname;
    char typeflag = '0';
    std::vector<core::byte> data;
    core::uint32 mode = 0644;
    core::uint64 mtime = 1704110400;
    std::string uname = "tester";
    std::string gname = "tgroup";
    core::uint64 uid = 1000;
    core::uint64 gid = 1000;
    std::vector<std::pair<std::string, std::string>> pax; // emits 'x' first
    bool use_longname = false;                            // GNU 'L' for the name
};

void put_octal(std::vector<core::byte>& b, size_t field, core::uint64 v, size_t width) {
    char buf[32] = {0};
    for (size_t i = 0; i < width; ++i) b[field + i] = static_cast<core::byte>(' ');
    snprintf(buf, sizeof(buf), "%0*llo", static_cast<int>(width - 1),
             static_cast<unsigned long long>(v));
    for (size_t i = 0; i < width - 1 && buf[i]; ++i) b[field + i] = static_cast<core::byte>(buf[i]);
}

std::vector<core::byte> tar_header(const std::string& name, char typeflag, core::uint64 size,
                                   core::uint32 mode, core::uint64 mtime,
                                   const std::string& linkname, const std::string& uname,
                                   const std::string& gname, core::uint64 uid, core::uint64 gid,
                                   const std::string& prefix = {}) {
    std::vector<core::byte> h(512, core::byte(0));
    const auto copy_field = [&](size_t off, const std::string& s, size_t max) {
        const size_t n = s.size() < max ? s.size() : max;
        std::memcpy(h.data() + off, s.data(), n);
    };
    copy_field(0, name, 100);
    put_octal(h, 100, mode, 8);
    put_octal(h, 108, uid, 8);
    put_octal(h, 116, gid, 8);
    put_octal(h, 124, size, 12);
    put_octal(h, 136, mtime, 12);
    std::memcpy(h.data() + 148, "        ", 8); // checksum placeholder
    h[156] = static_cast<core::byte>(typeflag);
    copy_field(157, linkname, 100);
    std::memcpy(h.data() + 257, "ustar\0", 6);
    std::memcpy(h.data() + 263, "00", 2);
    copy_field(265, uname, 32);
    copy_field(297, gname, 32);
    copy_field(345, prefix, 155);
    core::uint32 sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += static_cast<core::uint32>(h[i]);
    char cs[8] = {0};
    snprintf(cs, sizeof(cs), "%06o", sum);
    std::memcpy(h.data() + 148, cs, 6);
    h[154] = core::byte(0);
    h[155] = static_cast<core::byte>(' ');
    return h;
}

std::vector<core::byte> build_tar(const std::vector<TarMember>& members, bool end_blocks = true) {
    std::vector<core::byte> out;
    for (const TarMember& m : members) {
        if (!m.pax.empty()) {
            std::vector<core::byte> recs;
            for (const auto& kv : m.pax) {
                std::string rec = kv.first + "=" + kv.second + "\n";
                size_t total = rec.size() + std::to_string(rec.size()).size() + 1;
                while (std::to_string(total).size() + 1 + rec.size() != total)
                    total = std::to_string(total).size() + 1 + rec.size();
                std::string full = std::to_string(total) + " " + rec;
                recs.insert(recs.end(), full.begin(), full.end());
            }
            const size_t pad = (512 - recs.size() % 512) % 512;
            recs.resize(recs.size() + pad, core::byte(0));
            auto h =
                tar_header("PaxHeaders/x", 'x', recs.size(), 0644, 0, "", "root", "root", 0, 0);
            out.insert(out.end(), h.begin(), h.end());
            out.insert(out.end(), recs.begin(), recs.end());
        }
        if (m.use_longname) {
            std::vector<core::byte> lname(m.name.begin(), m.name.end());
            lname.push_back(core::byte(0));
            const size_t pad = (512 - lname.size() % 512) % 512;
            lname.resize(lname.size() + pad, core::byte(0));
            auto h =
                tar_header("././@LongLink", 'L', lname.size(), 0644, 0, "", "root", "root", 0, 0);
            out.insert(out.end(), h.begin(), h.end());
            out.insert(out.end(), lname.begin(), lname.end());
        }
        std::string stored_name = m.use_longname ? std::string("longlink-target") : m.name;
        std::string prefix;
        if (!m.use_longname && m.name.size() > 100) {
            const size_t slash = m.name.rfind('/', 154);
            if (slash != std::string::npos) {
                prefix = m.name.substr(0, slash);
                stored_name = m.name.substr(slash + 1);
            }
        }
        auto h = tar_header(stored_name, m.typeflag, m.data.size(), m.mode, m.mtime, m.linkname,
                            m.uname, m.gname, m.uid, m.gid, prefix);
        out.insert(out.end(), h.begin(), h.end());
        out.insert(out.end(), m.data.begin(), m.data.end());
        const size_t pad = (512 - m.data.size() % 512) % 512;
        out.resize(out.size() + pad, core::byte(0));
    }
    if (end_blocks) out.resize(out.size() + 1024, core::byte(0));
    return out;
}

bool write_file(const std::filesystem::path& p, const std::vector<core::byte>& data) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::vector<core::byte> build_gzip(const std::vector<const std::vector<core::byte>*>& streams,
                                   const std::vector<core::uint32>& crcs,
                                   const std::vector<core::uint32>& isizes,
                                   const std::string& fname = "", bool corrupt_trailer = false) {
    std::vector<core::byte> out;
    auto put16le = [&](core::uint16 v) {
        out.push_back(static_cast<core::byte>(v & 0xFF));
        out.push_back(static_cast<core::byte>(v >> 8));
    };
    auto put32le = [&](core::uint32 v) {
        put16le(static_cast<core::uint16>(v & 0xFFFF));
        put16le(static_cast<core::uint16>(v >> 16));
    };
    std::string first_fname = fname;
    for (size_t i = 0; i < streams.size(); ++i) {
        out.push_back(0x1F);
        out.push_back(0x8B);
        out.push_back(8); // CM = deflate
        const core::uint8 flg = (i == 0 && !first_fname.empty()) ? 0x08 : 0x00;
        out.push_back(static_cast<core::byte>(flg));
        put32le(1704110400); // MTIME
        out.push_back(0);    // XFL
        out.push_back(3);    // OS = unix
        if (flg & 0x08) {
            out.insert(out.end(), first_fname.begin(), first_fname.end());
            out.push_back(0);
        }
        out.insert(out.end(), streams[i]->begin(), streams[i]->end());
        put32le(corrupt_trailer ? crcs[i] ^ 0xFFFFFFFFu : crcs[i]);
        put32le(isizes[i]);
    }
    return out;
}

} // namespace

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
