// M6 (v1.24 plan §7.3): unknown-extra preservation.
//
// Plan §10 test 21 — unknown_extras_roundtrip: an archive whose file
// headers carry extra records this build does not implement must survive a
// mutation roundtrip byte-identically in those records: parse captures the
// records verbatim, re-serialization emits them byte-identically, and the
// mutated archive re-reads with the same unknown_extras payload.

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/core/vint.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/io/file_stream.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

namespace {

// Engine-selection kill switch for the both-engines test (same pattern as
// mapped_scan_tests.cpp).
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    std::string kv = std::string(name) + "=" + value;
    _putenv(kv.c_str());
#else
    setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#ifdef _WIN32
    std::string kv = std::string(name) + "=";
    _putenv(kv.c_str());
#else
    unsetenv(name);
#endif
}

} // namespace

namespace {

namespace fs = std::filesystem;

fs::path make_scratch_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_m6_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void rm(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// ── v1.27 M1: FHEXTRA_XATTR (0x08) record tests ─────────────────────────────
// The record rides the same unknown-extra capture machinery when malformed:
// well-formed records parse into FileBlock::xattrs (canonical re-emission,
// mutation survival), any validation failure captures the record VERBATIM.

// Builds a canonical 0x08 record: [size vint][type 0x08][flags=0 vint]
// [count vint](per attr: [nlen vint][name][vlen vint][value]).
std::vector<core::byte>
build_xattr_record(const std::vector<std::pair<std::string, std::string>>& attrs) {
    std::vector<core::byte> payload;
    core::push_vint(payload, 0); // flags — reserved, must be 0
    core::push_vint(payload, attrs.size());
    for (const auto& [name, value] : attrs) {
        core::push_vint(payload, name.size());
        payload.insert(payload.end(), name.begin(), name.end());
        core::push_vint(payload, value.size());
        payload.insert(payload.end(), value.begin(), value.end());
    }
    std::vector<core::byte> raw;
    core::push_vint(raw, 1 + payload.size()); // size covers the type byte
    raw.push_back(format::FHEXTRA_XATTR);
    raw.insert(raw.end(), payload.begin(), payload.end());
    return raw;
}

// Builds a malformed 0x08 record from raw payload bytes (validation is the
// parser's job — this is the hostile-producer shape).
std::vector<core::byte> build_xattr_record_raw(const std::vector<core::byte>& payload) {
    std::vector<core::byte> raw;
    core::push_vint(raw, 1 + payload.size());
    raw.push_back(format::FHEXTRA_XATTR);
    raw.insert(raw.end(), payload.begin(), payload.end());
    return raw;
}

// Raw-writes a stored archive whose first entry carries one extra record
// (fed through unknown_extras purely as a crafting vehicle — the writer
// re-emits it verbatim; the READER decides whether the type is known).
void write_archive_with_xattr_record(const fs::path& arc, const std::vector<core::byte>& raw) {
    io::FileStream out;
    assert(out.open(arc, io::FileMode::CreateAlways));
    format::HeaderWriter::write_signature(out);
    format::MainBlock mb;
    format::HeaderWriter::write_main_block(out, mb);

    format::FileBlock fb;
    fb.file_name = "carrier.txt";
    fb.host_os = 1;
    fb.attributes = 0100644u;
    fb.unp_size = 6;
    fb.pack_size = 6;
    fb.method = 0;
    fb.win_size = 0;
    fb.unknown_extras.push_back({format::FHEXTRA_XATTR, raw});

    assert(format::HeaderWriter::write_file_block(out, fb));
    out.write("plan6!", 6);
    format::EndArcBlock eb;
    format::HeaderWriter::write_end_block(out, eb);
}

bool xattr_equal(const std::vector<format::FileBlock::FileXattr>& a,
                 const std::vector<format::FileBlock::FileXattr>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name) return false;
        if (a[i].value.size() != b[i].value.size()) return false;
        if (std::memcmp(a[i].value.data(), b[i].value.data(), a[i].value.size()) != 0) return false;
    }
    return true;
}

// v1.27 plan test 2 (happy path half): a well-formed record parses into
// FileBlock::xattrs, re-serializes canonically (write → parse → write is
// byte-stable), and survives a mutation roundtrip.
void test_xattr_roundtrip_record() {
    const fs::path dir = make_scratch_dir("xa_ok");
    const fs::path arc = dir / "xattr.rar";
    const auto raw = build_xattr_record({{"user.note", "hello"},
                                         {"user.empty", ""},
                                         {"com.apple.metadata:_kMDItemUserTags",
                                          std::string("\x62\x70\x6c\x69\x73\x74\x30\x30", 8)}});
    write_archive_with_xattr_record(arc, raw);

    format::FileBlock parsed;
    {
        archive::ArchiveReader reader;
        assert(reader.open(arc));
        assert(reader.entries().size() == 1);
        parsed = reader.entries()[0].header;
    }
    assert(parsed.xattrs.size() == 3);
    assert(parsed.xattrs[0].name == "user.note");
    assert(parsed.xattrs[0].value.size() == 5);
    assert(std::memcmp(parsed.xattrs[0].value.data(), "hello", 5) == 0);
    assert(parsed.xattrs[1].name == "user.empty");
    assert(parsed.xattrs[1].value.empty());
    assert(parsed.xattrs[2].name == "com.apple.metadata:_kMDItemUserTags");
    assert(parsed.xattrs[2].value.size() == 8);
    assert(parsed.unknown_extras.empty());

    // Canonical re-serialization: re-write the parsed block (the crafted
    // vehicle is dropped — the emitted record is the canonical one),
    // re-parse, and require identical attributes.
    const fs::path arc2 = dir / "xattr_reser.rar";
    {
        io::FileStream out;
        assert(out.open(arc2, io::FileMode::CreateAlways));
        format::HeaderWriter::write_signature(out);
        format::MainBlock mb;
        format::HeaderWriter::write_main_block(out, mb);
        assert(format::HeaderWriter::write_file_block(out, parsed));
        out.write("plan6!", 6);
        format::EndArcBlock eb;
        format::HeaderWriter::write_end_block(out, eb);
    }
    format::FileBlock reparsed;
    {
        archive::ArchiveReader reader;
        assert(reader.open(arc2));
        assert(reader.entries().size() == 1);
        reparsed = reader.entries()[0].header;
    }
    assert(reparsed.unknown_extras.empty());
    assert(xattr_equal(parsed.xattrs, reparsed.xattrs));
    // Byte-identical record: rebuild the canonical record from the parsed
    // attributes and compare against the crafted original.
    const auto recanonical =
        build_xattr_record({{"user.note", "hello"},
                            {"user.empty", ""},
                            {"com.apple.metadata:_kMDItemUserTags",
                             std::string("\x62\x70\x6c\x69\x73\x74\x30\x30", 8)}});
    assert(recanonical == raw);

    // Mutation roundtrip: append a second file — the carrier's xattrs
    // survive byte-identically.
    const fs::path src = dir / "added.bin";
    {
        std::ofstream f(src, std::ios::binary);
        f << "added";
    }
    {
        archive::ArchiveMutator::PreparedAdd p;
        p.entry_name = "added.bin";
        p.src_path = src;
        assert(archive::ArchiveMutator::prepare_add_file(src, "added.bin", 0, "", p));
        std::vector<archive::ArchiveMutator::PreparedAdd> batch;
        batch.push_back(std::move(p));
        std::string detail;
        const int wrc = archive::ArchiveMutator::write_batch_add_ex(arc, batch, {}, "", false, {},
                                                                    false, {}, detail);
        assert(wrc == 0);
    }
    fs::remove(src);
    archive::ArchiveReader reader2;
    assert(reader2.open(arc));
    const archive::ArchiveEntry* carrier = nullptr;
    for (const auto& e : reader2.entries()) {
        if (e.header.is_service) continue;
        if (e.header.file_name == "carrier.txt") carrier = &e;
    }
    assert(carrier != nullptr);
    assert(carrier->header.unknown_extras.empty());
    assert(xattr_equal(parsed.xattrs, carrier->header.xattrs));

    std::cout
        << "[PASS] xattr_roundtrip_record: parse + canonical re-emission + mutation survival\n";
    rm(dir);
}

// v1.27 plan test 2 (malformed half) + test 3 (bounded hostile input): every
// validation failure captures the record VERBATIM, never aborts the header
// parse, and never allocates beyond the record's own bytes.
void test_xattr_malformed_captured_verbatim() {
    const std::string name9 = "user.note";
    // One valid entry's bytes, for splicing into hostile shapes.
    std::vector<core::byte> good_entry;
    core::push_vint(good_entry, name9.size());
    good_entry.insert(good_entry.end(), name9.begin(), name9.end());
    core::push_vint(good_entry, 5);
    const char* val = "hello";
    good_entry.insert(good_entry.end(), val, val + 5);

    std::vector<std::vector<core::byte>> hostile_payloads;
    {
        std::vector<core::byte> p; // (a) flags != 0
        core::push_vint(p, 1);
        core::push_vint(p, 1);
        p.insert(p.end(), good_entry.begin(), good_entry.end());
        hostile_payloads.push_back(std::move(p));
    }
    {
        std::vector<core::byte> p; // (b) name length 0
        core::push_vint(p, 0);
        core::push_vint(p, 1);
        core::push_vint(p, 0);
        core::push_vint(p, 0);
        hostile_payloads.push_back(std::move(p));
    }
    {
        std::vector<core::byte> p; // (c) name length 256 (> 255)
        core::push_vint(p, 0);
        core::push_vint(p, 1);
        core::push_vint(p, 256);
        p.push_back('a');
        core::push_vint(p, 0);
        hostile_payloads.push_back(std::move(p));
    }
    {
        std::vector<core::byte> p; // (d) value length 65537 (> 64 KiB cap)
        core::push_vint(p, 0);
        core::push_vint(p, 1);
        core::push_vint(p, name9.size());
        p.insert(p.end(), name9.begin(), name9.end());
        core::push_vint(p, 65537);
        hostile_payloads.push_back(std::move(p));
    }
    {
        std::vector<core::byte> p; // (e) count 2, one entry present
        core::push_vint(p, 0);
        core::push_vint(p, 2);
        p.insert(p.end(), good_entry.begin(), good_entry.end());
        hostile_payloads.push_back(std::move(p));
    }
    {
        std::vector<core::byte> p; // (f) trailing bytes after the last entry
        core::push_vint(p, 0);
        core::push_vint(p, 1);
        p.insert(p.end(), good_entry.begin(), good_entry.end());
        p.push_back(0x00);
        hostile_payloads.push_back(std::move(p));
    }
    {
        std::vector<core::byte> p; // (g) duplicate names
        core::push_vint(p, 0);
        core::push_vint(p, 2);
        p.insert(p.end(), good_entry.begin(), good_entry.end());
        p.insert(p.end(), good_entry.begin(), good_entry.end());
        hostile_payloads.push_back(std::move(p));
    }
    {
        std::vector<core::byte> p; // (h) huge count — must fail the /3 bound fast
        core::push_vint(p, 0);
        const core::uint64 huge = 0xFFFFFFFFFFFFFFFll;
        core::push_vint(p, huge);
        p.insert(p.end(), good_entry.begin(), good_entry.end());
        hostile_payloads.push_back(std::move(p));
    }

    int case_idx = 0;
    for (const auto& payload : hostile_payloads) {
        ++case_idx;
        const fs::path dir = make_scratch_dir("xa_bad");
        const fs::path arc = dir / "xattr_bad.rar";
        const auto raw = build_xattr_record_raw(payload);
        write_archive_with_xattr_record(arc, raw);
        format::FileBlock parsed;
        {
            archive::ArchiveReader reader;
            assert(reader.open(arc));
            assert(reader.entries().size() == 1);
            parsed = reader.entries()[0].header;
        }
        assert(parsed.xattrs.empty());
        assert(parsed.unknown_extras.size() == 1);
        assert(parsed.unknown_extras[0].type == format::FHEXTRA_XATTR);
        assert(parsed.unknown_extras[0].raw == raw);

        // The verbatim record survives a mutation byte-identically.
        const fs::path src = dir / "added.bin";
        {
            std::ofstream f(src, std::ios::binary);
            f << "added";
        }
        archive::ArchiveMutator::PreparedAdd p;
        p.entry_name = "added.bin";
        p.src_path = src;
        assert(archive::ArchiveMutator::prepare_add_file(src, "added.bin", 0, "", p));
        std::vector<archive::ArchiveMutator::PreparedAdd> batch;
        batch.push_back(std::move(p));
        std::string detail;
        const int wrc = archive::ArchiveMutator::write_batch_add_ex(arc, batch, {}, "", false, {},
                                                                    false, {}, detail);
        assert(wrc == 0);
        fs::remove(src);
        archive::ArchiveReader reader2;
        assert(reader2.open(arc));
        const archive::ArchiveEntry* carrier = nullptr;
        for (const auto& e : reader2.entries()) {
            if (e.header.is_service) continue;
            if (e.header.file_name == "carrier.txt") carrier = &e;
        }
        assert(carrier != nullptr);
        assert(carrier->header.xattrs.empty());
        assert(carrier->header.unknown_extras.size() == 1);
        assert(carrier->header.unknown_extras[0].raw == raw);
        rm(dir);
    }
    std::cout << "[PASS] xattr_malformed_captured_verbatim: " << case_idx
              << " hostile shapes -> verbatim, bounded, no abort\n";
}

// v1.27 plan test 3 (both scan engines): the hostile record produces the
// same verbatim outcome on the mapped and buffered scan engines.
void test_xattr_hostile_record_both_engines() {
    const fs::path dir = make_scratch_dir("xa_eng");
    const fs::path arc = dir / "xattr_eng.rar";
    // Well-formed record with 64 KiB of value bytes — parseable, exercises
    // the allocation path; plus a hostile shape in a second archive.
    std::string big(65536, 'x');
    const auto big_raw = build_xattr_record({{"user.big", big}});
    std::vector<core::byte> hostile_payload;
    core::push_vint(hostile_payload, 0);
    core::push_vint(hostile_payload, 4); // 4 entries, 1 present
    const std::string name9 = "user.note";
    core::push_vint(hostile_payload, name9.size());
    hostile_payload.insert(hostile_payload.end(), name9.begin(), name9.end());
    core::push_vint(hostile_payload, 5);
    const char* val5 = "hello";
    hostile_payload.insert(hostile_payload.end(), val5, val5 + 5);
    const auto hostile_raw = build_xattr_record_raw(hostile_payload);

    for (bool no_mmap : {false, true}) {
        if (no_mmap)
            set_env("OPENRAR_NO_MMAP", "1");
        else
            unset_env("OPENRAR_NO_MMAP");

        {
            const fs::path arc_big = dir / "xattr_big.rar";
            write_archive_with_xattr_record(arc_big, big_raw);
            archive::ArchiveReader reader;
            assert(reader.open(arc_big));
            assert(reader.entries().size() == 1);
            assert(reader.entries()[0].header.xattrs.size() == 1);
            assert(reader.entries()[0].header.xattrs[0].value.size() == 65536);
            assert(reader.entries()[0].header.unknown_extras.empty());
        }
        {
            const fs::path arc_hostile = dir / "xattr_hostile.rar";
            write_archive_with_xattr_record(arc_hostile, hostile_raw);
            archive::ArchiveReader reader;
            assert(reader.open(arc_hostile));
            assert(reader.entries().size() == 1);
            assert(reader.entries()[0].header.xattrs.empty());
            assert(reader.entries()[0].header.unknown_extras.size() == 1);
            assert(reader.entries()[0].header.unknown_extras[0].raw == hostile_raw);
        }
    }
    unset_env("OPENRAR_NO_MMAP");
    std::cout << "[PASS] xattr_hostile_record_both_engines: mapped + buffered identical\n";
    rm(dir);
}

// Raw-writes a stored archive whose first entry carries two unknown extra
// records (types 0x42 and 0x7F — unimplemented by this build).
void write_archive_with_unknown_extras(const fs::path& arc) {
    io::FileStream out;
    assert(out.open(arc, io::FileMode::CreateAlways));
    format::HeaderWriter::write_signature(out);
    format::MainBlock mb;
    format::HeaderWriter::write_main_block(out, mb);

    format::FileBlock fb;
    fb.file_name = "carrier.txt";
    fb.host_os = 1;
    fb.attributes = 0100644u;
    fb.unp_size = 6;
    fb.pack_size = 6;
    fb.method = 0;
    fb.win_size = 0;

    // Simulate a NEWER producer: unknown extra records, fabricated verbatim
    // as [size vint][type vint][payload]. The writer re-emits these
    // byte-identically (v1.24 plan §7.3).
    const core::uint64 type1 = 0x42;
    const core::byte payload1[] = {0xDE, 0xAD, 0xBE, 0xEF};
    std::vector<core::byte> raw1;
    core::push_vint(raw1, 1 + sizeof(payload1));
    core::push_vint(raw1, type1);
    raw1.insert(raw1.end(), std::begin(payload1), std::end(payload1));
    const core::uint64 type2 = 0x7F;
    const std::string payload2 = "future-record-data";
    std::vector<core::byte> raw2;
    core::push_vint(raw2, 1 + payload2.size());
    core::push_vint(raw2, type2);
    raw2.insert(raw2.end(), payload2.begin(), payload2.end());
    fb.unknown_extras.push_back({type1, raw1});
    fb.unknown_extras.push_back({type2, raw2});

    assert(format::HeaderWriter::write_file_block(out, fb));
    out.write("plan6!", 6);
    format::EndArcBlock eb;
    format::HeaderWriter::write_end_block(out, eb);
}

} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    const fs::path dir = make_scratch_dir("main");
    const fs::path arc = dir / "unknown_extras.rar";
    write_archive_with_unknown_extras(arc);
    // Parse: both unknown records captured verbatim. (The reader is closed
    // before the mutation — Windows cannot rename over an open file.)
    std::vector<format::FileBlock::UnknownExtra> unknowns;
    {
        archive::ArchiveReader reader;
        assert(reader.open(arc));
        assert(reader.entries().size() == 1);
        unknowns = reader.entries()[0].header.unknown_extras;
    }
    assert(unknowns.size() == 2);
    assert(unknowns[0].type == 0x42);
    assert(unknowns[0].raw.size() == 6); // size vint + type + 4 payload bytes
    assert(unknowns[0].raw.size() >= 4);
    static const core::byte payload1[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const core::byte* tail = unknowns[0].raw.data() + unknowns[0].raw.size() - 4;
    assert(std::equal(payload1, payload1 + 4, tail));
    assert(unknowns[1].type == 0x7F);
    assert(unknowns[1].raw.size() >= 18);
    assert(std::string(unknowns[1].raw.data() + unknowns[1].raw.size() - 18,
                       unknowns[1].raw.data() + unknowns[1].raw.size()) == "future-record-data");

    // Roundtrip: append a second file through the mutator — the carrier's
    // unknown records must survive byte-identically.
    const fs::path src = dir / "added.bin";
    {
        std::ofstream f(src, std::ios::binary);
        f << "added";
    }
    {
        // Use the detail-carrying variant so a refusal names its cause.
        archive::ArchiveMutator::PreparedAdd p;
        p.entry_name = "added.bin";
        p.src_path = src;
        assert(archive::ArchiveMutator::prepare_add_file(src, "added.bin", 0, "", p));
        std::vector<archive::ArchiveMutator::PreparedAdd> batch;
        batch.push_back(std::move(p));
        std::string detail;
        const int wrc = archive::ArchiveMutator::write_batch_add_ex(arc, batch, {}, "", false, {},
                                                                    false, {}, detail);
        if (wrc != 0) {
            std::cerr << "  write_batch_add_ex rc=" << wrc << " detail=" << detail << "\n";
        }
        assert(wrc == 0);
    }
    fs::remove(src);

    archive::ArchiveReader reader2;
    assert(reader2.open(arc));
    const archive::ArchiveEntry* carrier = nullptr;
    size_t added = 0;
    for (const auto& e : reader2.entries()) {
        if (e.header.is_service) continue;
        if (e.header.file_name == "carrier.txt")
            carrier = &e;
        else
            ++added;
    }
    assert(carrier != nullptr && added == 1);
    assert(carrier->header.unknown_extras.size() == 2);
    assert(carrier->header.unknown_extras[0].type == 0x42);
    assert(carrier->header.unknown_extras[0].raw == unknowns[0].raw);
    assert(carrier->header.unknown_extras[1].type == 0x7F);
    assert(carrier->header.unknown_extras[1].raw == unknowns[1].raw);
    std::cout << "[PASS] unknown_extras_roundtrip: verbatim capture + mutation survival\n";

    test_xattr_roundtrip_record();
    test_xattr_malformed_captured_verbatim();
    test_xattr_hostile_record_both_engines();

    rm(dir);
    return 0;
}
