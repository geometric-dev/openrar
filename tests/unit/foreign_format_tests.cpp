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
#include "../../src/compress/inflate.hpp"
#include "../../src/crypto/crc32.hpp"

#include <cassert>
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
    std::cout << "All Foreign Format (v1.29 M2) tests PASSED!" << std::endl;
    return 0;
}
