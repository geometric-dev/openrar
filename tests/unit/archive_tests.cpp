#include "../../src/core/types.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/format/headers.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/format/header_reader.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/extraction_limits.hpp"
#include "../../src/archive/rar_errors.hpp"

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace openrar;
using namespace openrar::archive;
using namespace openrar::format;

void test_move_command() {
    std::cout << "Starting test_move_command...\n" << std::flush;
    std::filesystem::path test_arc = "build/test_move.rar";
    std::filesystem::path src_file = "build/move_source.txt";
    std::filesystem::remove(test_arc);
    std::filesystem::remove(src_file);

    // Create source file
    {
        std::ofstream ofs(src_file);
        ofs << "HELLO MOVE COMMAND 12345";
    }
    assert(std::filesystem::exists(src_file));

    // Move file to archive
    bool ok = ArchiveMutator::move_file_to_archive(test_arc, src_file, "moved_item.txt");
    std::cout << "move_file_to_archive returned " << ok << "\n" << std::flush;
    assert(ok);

    // Verify source file deleted from disk
    std::cout << "Checking exists(src_file): " << std::filesystem::exists(src_file) << "\n"
              << std::flush;
    assert(!std::filesystem::exists(src_file));

    // Verify archive contains moved item
    ArchiveReader reader;
    bool opened = reader.open(test_arc);
    std::cout << "reader.open returned " << opened << ", entries: " << reader.entries().size()
              << "\n"
              << std::flush;
    assert(opened);
    assert(reader.entries().size() == 1);
    std::cout << "entry name: " << reader.entries()[0].header.file_name << "\n" << std::flush;
    assert(reader.entries()[0].header.file_name == "moved_item.txt");
    bool tested = reader.test_entry(reader.entries()[0]);
    std::cout << "test_entry returned " << tested << "\n" << std::flush;
    assert(tested);

    // Extract item and check content
    std::filesystem::path ext_file = "build/extracted_moved.txt";
    bool ext_ok = reader.extract_store_entry(reader.entries()[0], ext_file);
    std::cout << "extract_store_entry returned " << ext_ok << "\n" << std::flush;
    assert(ext_ok);
    reader.close();

    std::ifstream ifs(ext_file);
    std::string content;
    std::getline(ifs, content);
    std::cout << "extracted content: [" << content << "] len=" << content.size()
              << " expected_len=" << std::strlen("HELLO MOVE COMMAND 12345") << "\n"
              << std::flush;
    for (size_t i = 0; i < content.size(); ++i) {
        std::cout << (int)(unsigned char)content[i] << " ";
    }
    std::cout << "\n" << std::flush;
    ifs.close();
    std::error_code ec;
    std::filesystem::remove(test_arc, ec);
    std::filesystem::remove(ext_file, ec);
    std::cout << "[PASS] Archive Move Command ('m')\n" << std::flush;
}

void test_delete_and_wildcard() {
    std::filesystem::path test_arc = "build/test_delete.rar";
    std::filesystem::remove(test_arc);

    // Populate archive with 3 files
    {
        std::filesystem::path f1 = "build/f1.txt";
        std::filesystem::path f2 = "build/f2.txt";
        std::filesystem::path f3 = "build/data.bin";

        { std::ofstream(f1) << "Text 1"; }
        { std::ofstream(f2) << "Text 2"; }
        { std::ofstream(f3) << "Binary Data"; }

        ArchiveMutator::move_file_to_archive(test_arc, f1, "f1.txt");
        ArchiveMutator::move_file_to_archive(test_arc, f2, "f2.txt");
        ArchiveMutator::move_file_to_archive(test_arc, f3, "data.bin");
    }

    // Verify 3 entries initially
    {
        ArchiveReader reader;
        bool ok = reader.open(test_arc);
        std::cout << "test_delete_and_wildcard reader.open returned " << ok
                  << ", entries: " << reader.entries().size() << "\n"
                  << std::flush;
        assert(ok);
        assert(reader.entries().size() == 3);
    }

    // Delete *.txt
    bool ok = ArchiveMutator::delete_entries(test_arc, {"*.txt"});
    assert(ok);

    // Verify only data.bin remains
    {
        ArchiveReader reader;
        assert(reader.open(test_arc));
        assert(reader.entries().size() == 1);
        assert(reader.entries()[0].header.file_name == "data.bin");
        assert(reader.test_entry(reader.entries()[0]));
    }

    std::filesystem::remove(test_arc);
    std::cout << "[PASS] Archive Wildcard Delete Command ('d')\n";
}

void test_lock_command() {
    std::filesystem::path test_arc = "build/test_lock.rar";
    std::filesystem::remove(test_arc);

    // Create archive
    std::filesystem::path f = "build/lock_item.txt";
    { std::ofstream(f) << "LOCK CONTENT"; }
    ArchiveMutator::move_file_to_archive(test_arc, f, "lock_item.txt");

    // Lock archive
    assert(ArchiveMutator::lock_archive(test_arc));

    // Verify locked
    {
        ArchiveReader reader;
        assert(reader.open(test_arc));
        assert(reader.is_locked());
    }

    // Attempt delete on locked archive -> MUST FAIL
    bool del_result = ArchiveMutator::delete_entries(test_arc, {"lock_item.txt"});
    assert(!del_result);

    std::filesystem::remove(test_arc);
    std::cout << "[PASS] Archive Lock Command ('k') and Mutation Rejection\n";
}

// L8: mutation temp files must no longer use the fixed, predictable names
// ".mut_tmp"/".lck_tmp"/".app_tmp" opened with CreateAlways — a local attacker
// could pre-plant a file/symlink at those names and have it truncated. Plant
// sentinels at the legacy names and assert every mutation path still succeeds
// WITHOUT touching the planted files (old code truncated them via CreateAlways;
// new code uses ".<tag>.<pid>-<millis>-<n>" + FileMode::CreateNew).
void test_mutation_temp_names_not_clobbered() {
    std::filesystem::path test_arc = "build/test_tmpname.rar";
    std::filesystem::remove(test_arc);

    // Build a small archive.
    std::filesystem::path f1 = "build/tmpname_a.txt";
    std::filesystem::path f2 = "build/tmpname_b.txt";
    std::filesystem::path f3 = "build/tmpname_c.txt";
    { std::ofstream(f1) << "AAAA"; }
    { std::ofstream(f2) << "BBBB"; }
    { std::ofstream(f3) << "CCCC"; }
    assert(ArchiveMutator::move_file_to_archive(test_arc, f1, "a.txt"));
    assert(ArchiveMutator::move_file_to_archive(test_arc, f2, "b.txt"));

    // Plant sentinels at the legacy fixed temp names.
    const std::string sentinel = "DO NOT TRUNCATE";
    std::filesystem::path legacy[3] = {
        test_arc.string() + ".mut_tmp",
        test_arc.string() + ".lck_tmp",
        test_arc.string() + ".app_tmp",
    };
    for (const auto& p : legacy) {
        std::ofstream(p) << sentinel;
    }

    // Exercise all three mutation paths (delete 'd', add 'a', lock 'k').
    assert(ArchiveMutator::delete_entries(test_arc, {"a.txt"}));
    assert(ArchiveMutator::add_file_to_archive(test_arc, f3, "c.txt", /*method=*/0));
    assert(ArchiveMutator::lock_archive(test_arc));

    // The planted files must still exist with their original content.
    for (const auto& p : legacy) {
        assert(std::filesystem::exists(p));
        std::ifstream ifs(p);
        std::string content;
        std::getline(ifs, content);
        assert(content == sentinel);
    }

    // The archive itself must be valid with the mutations applied.
    {
        ArchiveReader reader;
        assert(reader.open(test_arc));
        assert(reader.is_locked());
        assert(reader.entries().size() == 2); // b.txt + c.txt
        assert(reader.test_entry(reader.entries()[0]));
    }

    std::error_code ec;
    std::filesystem::remove(test_arc, ec);
    for (const auto& p : legacy) std::filesystem::remove(p, ec);
    std::filesystem::remove(f1, ec);
    std::filesystem::remove(f2, ec);
    std::filesystem::remove(f3, ec);
    std::cout << "[PASS] Mutation temp names unique, legacy temp names untouched (L8)\n";
}

void test_sfx_preservation() {
    std::filesystem::path test_sfx = "build/test_sfx.exe";
    std::filesystem::remove(test_sfx);

    // Create archive with 2048-byte mock SFX preamble
    {
        io::FileStream out;
        assert(out.open(test_sfx, io::FileMode::CreateAlways));

        core::byte sfx_mock[2048];
        for (int i = 0; i < 2048; ++i) sfx_mock[i] = static_cast<core::byte>(i & 0xFF);
        out.write(sfx_mock, 2048);

        format::HeaderWriter::write_signature(out);
        format::MainBlock mb;
        format::HeaderWriter::write_main_block(out, mb);

        format::FileBlock fb;
        fb.file_name = "sfx_item.txt";
        fb.unp_size = 4;
        fb.pack_size = 4;
        fb.method = 0;
        format::HeaderWriter::write_file_block(out, fb);
        out.write("TEST", 4);

        format::EndArcBlock eb;
        format::HeaderWriter::write_end_block(out, eb);
    }

    // Verify SFX detected
    {
        ArchiveReader reader;
        assert(reader.open(test_sfx));
        assert(reader.sfx_offset() == 2048);
        assert(reader.entries().size() == 1);
    }

    std::filesystem::remove(test_sfx);
    std::cout << "[PASS] SFX Module Detection and Preservation\n";
}

// ── v1.22.0 security sweep: PBKDF2 lg2_count ceilings pinned at both header
// paths ──────────────────────────────────────────────────────────────────────
// Every KDF derivation site fails closed on lg2_count > 24 (WinRAR's
// ceiling): encrypted headers at HeaderCryptReader::init /
// HeaderCryptWriter::init_existing, per-file FHEXTRA_CRYPT at the entry-KDF
// gate in ArchiveReader. A hostile 25..255 must be refused WITHOUT
// derivation (1U << 31+ would be UB, and even 1 << 25 is a ~5s DoS per
// header); 24 must stay accepted so the boundary cannot creep down.
void test_kdf_cap_pinned() {
    std::cout << "[+] test_kdf_cap_pinned" << std::endl;

    // Path 2 (encrypted headers, HEAD_CRYPT): reader gate.
    {
        format::CryptBlock crypt{};
        crypt.salt.fill(core::byte(0xAB));
        const std::string pw = "kdf-cap-probe";
        for (core::uint8 lg2 : {25, 255}) {
            crypt.lg2_count = lg2;
            format::HeaderCryptReader rd;
            assert(!rd.init(pw, crypt) && "lg2_count > 24 must be refused without derivation");
        }
        crypt.lg2_count = 24; // boundary: accepted (one real derivation, ~seconds)
        format::HeaderCryptReader rd;
        assert(rd.init(pw, crypt));
    }
    // Path 2: writer gate mirror.
    {
        format::CryptBlock crypt{};
        crypt.salt.fill(core::byte(0xCD));
        const std::string pw = "kdf-cap-probe";
        format::HeaderCryptWriter wr;
        crypt.lg2_count = 25;
        assert(!wr.init_existing(pw, crypt));
        crypt.lg2_count = 24;
        assert(wr.init_existing(pw, crypt));
    }

    // Path 1 (per-file FHEXTRA_CRYPT): hand-built hostile header with a
    // VALID header CRC (written through HeaderWriter), so the tamper cannot
    // be caught by the CRC gate and the entry-KDF lg2_count ceiling itself
    // is what must refuse the entry — fail-closed, no derivation, no hang.
    const std::filesystem::path arc = "build/kdf_cap_hostile.rar";
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb;
        fb.file_name = "hostile.bin";
        fb.unp_size = 16;
        fb.pack_size = 16;
        fb.method = 0; // store: the data area is plaintext, only the header lies
        fb.is_encrypted = true;
        fb.crypt_version = 0;
        fb.lg2_count = 25; // hostile: one past the ceiling
        fb.salt.fill(core::byte(0x5A));
        fb.init_v.fill(core::byte(0xA5));
        assert(HeaderWriter::write_file_block(out, fb, 0));
        const core::byte payload[16] = {};
        assert(out.write(payload, sizeof(payload)) == sizeof(payload));
        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }
    {
        ArchiveReader rd;
        assert(rd.open(arc, "correct"));
        std::vector<core::byte> out;
        const int rc =
            rd.extract_entry_to_memory(0, out, 16ULL * 1024 * 1024, {}, nullptr, nullptr);
        assert(rc == RAR_ERR_UNSUPPORTED_FEATURE &&
               "lg2_count = 25 in FHEXTRA_CRYPT must fail closed");
        // And the ceiling is not just an error path: 24 stays legal, so the
        // gate cannot silently creep down to 23 (the writer side pins that
        // boundary above; here only the refusal is asserted).
    }

    std::filesystem::remove(arc);
    std::cout << "[PASS] PBKDF2 lg2_count ceilings pinned (25..255 refused, 24 accepted, "
                 "both header paths)\n";
}

// R5: bad_password_ must not be sticky across calls on the same reader
// instance. A caller retrying extract_entry (or test_entry) with the
// correct password after an earlier wrong-password attempt must see
// has_bad_password() == false and a successful extraction.
void test_bad_password_not_sticky() {
    std::filesystem::path test_arc = "build/test_bad_password_sticky.rar";
    std::filesystem::path src_file = "build/bp_source.txt";
    std::filesystem::remove(test_arc);
    std::filesystem::remove(src_file);

    const std::string CONTENT = "sticky bad password regression content";
    {
        std::ofstream ofs(src_file);
        ofs << CONTENT;
    }
    assert(std::filesystem::exists(src_file));

    bool ok = ArchiveMutator::add_file_to_archive(test_arc, src_file, "bp.txt", /*method=*/0,
                                                  /*sfx_stub_path=*/{}, /*vol_size=*/0,
                                                  /*password=*/"correct");
    assert(ok);

    ArchiveReader reader;
    bool opened = reader.open(test_arc);
    assert(opened);
    assert(reader.entries().size() == 1);
    const auto& entry = reader.entries()[0];

    std::filesystem::path ext_file = "build/bp_extracted.txt";
    std::filesystem::remove(ext_file);

    // First call: wrong password must fail and set has_bad_password().
    bool wrong_ok = reader.extract_entry(entry, ext_file, "wrong");
    assert(!wrong_ok);
    assert(reader.has_bad_password());

    // Second call on the SAME reader instance: correct password must
    // succeed, and has_bad_password() must be cleared, not stuck from the
    // previous failed attempt.
    bool right_ok = reader.extract_entry(entry, ext_file, "correct");
    assert(right_ok);
    assert(!reader.has_bad_password());

    std::ifstream ifs(ext_file);
    std::string content;
    std::getline(ifs, content);
    assert(content == CONTENT);
    ifs.close();

    reader.close();
    std::error_code ec;
    std::filesystem::remove(test_arc, ec);
    std::filesystem::remove(src_file, ec);
    std::filesystem::remove(ext_file, ec);
    std::cout << "[PASS] bad_password_ resets across retries on the same reader (R5)\n";
}

// Regression (report H2): a file header whose declared data_size exceeds the
// bytes remaining in the volume (truncated archive, or a crafted header) used
// to be admitted into the entry's extents unclamped. read_packed_data then
// sized its allocation from the attacker-controlled value (bad_alloc ->
// std::terminate; no catch anywhere on the extract paths). The scan must
// clamp the extent to the available bytes and stop, and reads of the
// truncated entry must fail gracefully (CRC error), never allocate by
// declared size.
void test_truncated_data_area_clamped() {
    std::error_code ec;
    std::filesystem::path test_arc = "build/test_trunc_data.rar";
    std::filesystem::path src_file = "build/trunc_source.bin";
    std::filesystem::remove(test_arc, ec);
    std::filesystem::remove(src_file, ec);

    std::string payload(100, 'x');
    payload[0] = 'H';
    payload[99] = 'Z';
    {
        std::ofstream ofs(src_file, std::ios::binary);
        ofs << payload;
    }

    assert(ArchiveMutator::move_file_to_archive(test_arc, src_file, "trunc_item.bin"));

    core::uint64 data_offset = 0;
    core::uint64 full_size = 0;
    {
        ArchiveReader probe;
        assert(probe.open(test_arc));
        assert(probe.entries().size() == 1);
        data_offset = probe.entries()[0].data_offset;
        full_size = probe.entries()[0].data_size;
        std::cout << "probe: name=" << probe.entries()[0].header.file_name
                  << " data_offset=" << data_offset << " data_size=" << full_size
                  << " unp_size=" << probe.entries()[0].header.unp_size << "\n"
                  << std::flush;
        assert(full_size > 0 && full_size < 100); // stored compressed
        assert(probe.entries()[0].header.unp_size == 100);
    }

    // Truncate mid-data: only part of the declared data bytes remain (the
    // 100-byte payload compresses to 16, so cut within the real data area).
    const core::uint64 KEEP_SIZE = full_size / 2;
    {
        io::FileStream f;
        assert(f.open(test_arc, io::FileMode::ReadOnly));
        std::vector<core::byte> head(static_cast<size_t>(data_offset + KEEP_SIZE));
        assert(f.read(head.data(), head.size()) == head.size());
        f.close();
        io::FileStream g;
        assert(g.open(test_arc, io::FileMode::CreateAlways));
        assert(g.write(head.data(), head.size()) == head.size());
    }

    ArchiveReader reader;
    bool opened = reader.open(test_arc);
    std::cout << "open(truncated)=" << opened
              << " entries=" << (opened ? reader.entries().size() : (size_t)0) << "\n"
              << std::flush;
    assert(opened);
    assert(reader.entries().size() == 1);
    const ArchiveEntry& e = reader.entries()[0];
    std::cout << "clamped data_size=" << e.data_size << " (declared 100, kept " << KEEP_SIZE
              << ")\n"
              << std::flush;
    assert(e.data_size == KEEP_SIZE); // extent clamped to available bytes
    assert(!e.extents.empty());
    assert(e.extents[0].size == KEEP_SIZE); // extent itself carries the clamp
    assert(e.header.unp_size == 100);       // header truth preserved

    // Reads of the truncated entry fail cleanly at the CRC (short data), and
    // read_packed_data yields exactly the available bytes — never an
    // allocation sized by the declared (or any untrusted) length.
    assert(!reader.test_entry(e));

    std::vector<core::byte> packed;
    assert(reader.read_packed_data(e, packed));
    assert(packed.size() == KEEP_SIZE);

    reader.close();
    std::filesystem::remove(test_arc, ec);
    std::filesystem::remove(src_file, ec);
    std::filesystem::remove("build/extracted_trunc.bin", ec);
    std::cout << "[PASS] Truncated data area clamped at scan; graceful read failure\n"
              << std::flush;
}

// Regression (report H3): get_dest_root walked one real filesystem parent per
// RAW component of the entry name, including "..", and the HARDLINK/FILECOPY
// branches joined the raw redir target to that root. An entry named
// "../leak.txt" with a benign root-relative target therefore resolved its
// copy/link SOURCE above the extraction root — an arbitrary-file disclosure
// primitive. Now the root is computed from the sanitized component count and
// resolve_under_root() refuses sources that climb above the extraction root.
// Shape: out_root=build/i3_out, secret at build/i3_secret.txt (outside
// out_root, inside its parent). Entry "../i3_leak.txt" (sanitize -> leak.txt)
// with target "i3_secret.txt": old code resolved src = parent(out)/secret and
// copied it out; new code must skip. A benign FILECOPY in the same archive
// must keep working.
void test_filecopy_source_confined_to_root() {
    std::error_code ec;
    namespace fs = std::filesystem;
    fs::path out_root = "build/i3_out";
    fs::path secret = "build/i3_secret.txt";
    fs::path arc = "build/i3_attack.rar";
    fs::remove_all(out_root, ec);
    fs::remove(secret, ec);
    fs::remove(arc, ec);
    fs::create_directories(out_root);
    const std::string SECRET = "TOP SECRET OUTSIDE ROOT";
    {
        std::ofstream ofs(secret, std::ios::binary);
        ofs << SECRET;
    }
    const std::string PUBLIC = "benign public content";
    {
        std::ofstream ofs(out_root / "source_file.txt", std::ios::binary);
        ofs << PUBLIC;
    }

    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock leak;
        leak.file_name = "../i3_leak.txt";
        leak.unp_size = 0;
        leak.pack_size = -1;
        leak.method = 0;
        leak.win_size = 0;
        leak.redir_type = 5; // FILECOPY
        leak.redir_target = "i3_secret.txt";
        assert(HeaderWriter::write_file_block(out, leak));

        FileBlock ok;
        ok.file_name = "ok_link.txt";
        ok.unp_size = 0;
        ok.pack_size = -1;
        ok.method = 0;
        ok.win_size = 0;
        ok.redir_type = 5;
        ok.redir_target = "source_file.txt";
        assert(HeaderWriter::write_file_block(out, ok));

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    ArchiveReader reader;
    assert(reader.open(arc));
    assert(reader.entries().size() == 2);

    // Attack entry: extraction "succeeds" (unsafe target skipped), but the
    // secret must not appear anywhere under out_root.
    assert(reader.extract_entry(reader.entries()[0], out_root / "i3_leak.txt"));
    assert(!fs::exists(out_root / "i3_leak.txt", ec));

    // Benign FILECOPY still resolves inside the root and copies the target.
    assert(reader.extract_entry(reader.entries()[1], out_root / "ok_link.txt"));
    assert(fs::exists(out_root / "ok_link.txt", ec));
    {
        std::ifstream ifs(out_root / "ok_link.txt", std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        assert(got == PUBLIC);
    }

    reader.close();
    fs::remove_all(out_root, ec);
    fs::remove(secret, ec);
    fs::remove(arc, ec);
    std::cout << "[PASS] FILECOPY source confined to extraction root\n" << std::flush;
}

// Regression (report M9): rescan_links_to_dirs walked from the extraction
// destination to the filesystem root and DELETED any symlink/junction it
// met, replacing it with a real directory - so extracting a benign entry
// whose name merely contained ".." destroyed pre-existing user links
// anywhere up the chain. Safe links-to-dirs extraction only converts links created by
// the extractor itself. Now the reader records the links it creates and
// converts only those. Needs symlink privilege (developer mode / admin);
// skips otherwise.
void test_links_to_dirs_leaves_preexisting_links() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path link = "build/i10_link";
    fs::path target = "build/i10_target";
    fs::path out_root = "build/i10_out";
    fs::path arc = "build/i10_arc.rar";
    fs::remove_all(link, ec);
    fs::remove_all(target, ec);
    fs::remove_all(out_root, ec);
    fs::remove(arc, ec);
    fs::create_directories(target);
    fs::create_directory_symlink(fs::absolute(target), link, ec);
    if (ec) {
        std::cout << "  Skip: no symlink privilege, cannot exercise LinksToDirs scoping\n";
        return;
    }

    // Stored file whose archive name contains ".." (triggers the rescan).
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));
        FileBlock fb;
        fb.file_name = "a/../x.txt";
        fb.unp_size = 4;
        fb.pack_size = 4;
        fb.method = 0;
        fb.win_size = 0;
        fb.has_crc32 = false;
        assert(HeaderWriter::write_file_block(out, fb));
        const core::byte payload[4] = {'t', 'e', 's', 't'};
        assert(out.write(payload, 4) == 4);
        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    ArchiveReader reader;
    assert(reader.open(arc));
    assert(reader.entries().size() == 1);

    // Under B3 untrusted symlink policy, extraction into a destination inside a
    // pre-existing symlink parent must be rejected (never followed), while leaving the link intact.
    assert(!reader.extract_entry(reader.entries()[0], link / "x.txt"));

    auto st = fs::symlink_status(link, ec);
    assert(!ec);
    assert(fs::is_symlink(st));                // user's link survived
    assert(!fs::exists(target / "x.txt", ec)); // write did not go through it

    reader.close();
    fs::remove_all(link, ec);
    fs::remove_all(target, ec);
    fs::remove_all(out_root, ec);
    fs::remove(arc, ec);
    std::cout << "[PASS] LinksToDirs conversion leaves pre-existing links alone\n" << std::flush;
}

void test_solid_window_size() {
    std::cout << "Starting test_solid_window_size...\n" << std::flush;
    std::filesystem::path test_arc = "build/tiny_solid.rar";
    std::error_code ec;
    std::filesystem::remove(test_arc, ec);
    std::ofstream("build/t1.txt") << "file 1 data...";
    std::ofstream("build/t2.txt") << "file 2 data...";

    // Create tiny_solid.rar with 1MiB dictionary using system rar
    int res = system("\"C:\\Program Files\\WinRAR\\rar.exe\" a -s -md1m build/tiny_solid.rar "
                     "build/t1.txt build/t2.txt > nul 2>&1");
    if (res != 0) {
        std::cout << "  Skip: rar.exe not found or failed\n";
        return;
    }

    ArchiveReader reader;
    assert(reader.open(test_arc.string()));

    const std::vector<ArchiveEntry>& entries = reader.entries();
    assert(entries.size() >= 2);

    std::filesystem::path out1 = "build/t1_out.txt";
    std::filesystem::path out2 = "build/t2_out.txt";
    std::filesystem::remove(out1, ec);
    std::filesystem::remove(out2, ec);

    assert(reader.extract_entry(entries[0], out1));
    size_t win1 = reader.test_get_solid_window_size();

    assert(reader.extract_entry(entries[1], out2));
    size_t win2 = reader.test_get_solid_window_size();

    // Verify peak memory drops from 32MB to <8MB based on header
    assert(win1 > 0 && win1 <= 8 * 1024 * 1024);
    assert(win2 == win1); // window size survives across files

    std::filesystem::remove(out1, ec);
    std::filesystem::remove(out2, ec);
    std::filesystem::remove(test_arc, ec);
    std::filesystem::remove("build/t1.txt", ec);
    std::filesystem::remove("build/t2.txt", ec);

    std::cout << "[PASS] solid_window_size\n";
}

void test_b9_b5_corrupt_payload_fails_cleanly() {
    // T5 + B9 regression: (a) same-length payload corruption must surface the
    // canonical RAR_ERR_CRC_MISMATCH code on the streaming verify path, not a
    // generic IO; (b) a failed extract_entry must not leave a partial
    // destination file behind (the v1.24 atomic writer abandons its temp), and
    // keep_broken=false is the default.
    namespace fs = std::filesystem;
    fs::path src = "build/crc_payload.bin";
    fs::path arc = "build/crc_payload.rar";
    fs::path corrupt = "build/crc_payload_corrupt.rar";
    fs::path dest = "build/crc_payload_out.bin";
    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove(corrupt, ec);
    fs::remove(dest, ec);
    {
        std::ofstream f(src, std::ios::binary);
        f << std::string(4096, 'A');
        f << "OPENRAR CRC-MISMATCH PAYLOAD TAIL";
    }

    // method 0 (store): the corrupted byte is payload itself, so the CRC32
    // verdict is deterministic — no LZ stream reinterpretation in between.
    assert(ArchiveMutator::add_file_to_archive(arc, src, "payload.bin", 0));

    {
        // Flip one byte in the MIDDLE of the payload region, located from the
        // intact archive's scan — the archive tail can carry an ENDARC block,
        // so the last file byte is not necessarily payload.
        ArchiveReader probe;
        assert(probe.open(arc));
        const auto& pe = probe.entries()[0];
        const core::uint64 flip_off = pe.data_offset + pe.data_size / 2;
        std::ifstream in(arc, std::ios::binary);
        std::vector<core::byte> bytes((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        in.close();
        assert(flip_off < bytes.size());
        bytes[static_cast<size_t>(flip_off)] ^= 0xFF;
        std::ofstream out(corrupt, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    {
        ArchiveReader reader;
        assert(reader.open(corrupt));
        // T5: streaming verify reports the dedicated CRC code.
        ReaderHooks hooks{};
        assert(reader.test_entry_stream(0, hooks) == RAR_ERR_CRC_MISMATCH);

        // B9: the file-path extraction fails AND removes its partial output.
        assert(!reader.extract_entry(reader.entries()[0], dest));
        assert(!fs::exists(dest, ec));
    }

    // keep_broken=true is the explicit opt-in to keep the partial file.
    {
        ArchiveReader reader;
        reader.set_keep_broken(true);
        assert(reader.open(corrupt));
        assert(!reader.extract_entry(reader.entries()[0], dest));
        assert(fs::exists(dest, ec));
        assert(fs::file_size(dest, ec) > 0);
    }

    fs::remove(src, ec);
    fs::remove(arc, ec);
    fs::remove(corrupt, ec);
    fs::remove(dest, ec);
    std::cout << "[PASS] corrupt payload: RAR_ERR_CRC_MISMATCH + no partial file (T5, B9)\n";
}

void test_b9_compressed_corrupt_fails_cleanly() {
    // Verification-asymmetry regression (sweep finding): the bool
    // extract_entry path used to write COMPRESSED payload with no CRC/BLAKE
    // verification at all — corrupt data extracted "successfully" while the
    // stored path failed. The compressed path must refuse corrupt payload
    // and leave no partial file behind, same as stored.
    namespace fs = std::filesystem;
    fs::path src = "build/crc_comp_src.bin";
    fs::path arc = "build/crc_comp.rar";
    fs::path corrupt = "build/crc_comp_corrupt.rar";
    fs::path dest = "build/crc_comp_out.bin";
    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove(corrupt, ec);
    fs::remove(dest, ec);
    {
        std::ofstream f(src, std::ios::binary);
        f << std::string(4096, 'A') << "COMPRESSED PAYLOAD TAIL";
    }

    // method 3 (default LZ): exercises decode_compressed, not the store path.
    assert(ArchiveMutator::add_file_to_archive(arc, src, "payload.bin", 3));

    {
        ArchiveReader probe;
        assert(probe.open(arc));
        const auto& pe = probe.entries()[0];
        const core::uint64 flip_off = pe.data_offset + pe.data_size / 2;
        std::ifstream in(arc, std::ios::binary);
        std::vector<core::byte> bytes((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        in.close();
        bytes[static_cast<size_t>(flip_off)] ^= 0xFF;
        std::ofstream out(corrupt, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    ArchiveReader reader;
    assert(reader.open(corrupt));
    assert(!reader.extract_entry(reader.entries()[0], dest));
    assert(!fs::exists(dest, ec)); // FileUnlinker removed the partial output

    fs::remove(src, ec);
    fs::remove(arc, ec);
    fs::remove(corrupt, ec);
    fs::remove(dest, ec);
    std::cout << "[PASS] corrupt compressed payload: extract fails, no partial file\n";
}

void test_b2_parent_is_file_fails_cleanly() {
    // B2 regression: extraction whose destination parent chain collides with
    // a regular FILE must fail with a clean false — via the error_code
    // overloads in ensure_parent_dir — never an uncaught filesystem_error
    // across the C ABI, and never writing anything.
    namespace fs = std::filesystem;
    fs::path src = "build/b2_payload.txt";
    fs::path arc = "build/b2_parent.rar";
    fs::path dest_root = "build/b2_dest";
    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove_all(dest_root, ec);
    fs::create_directories(dest_root);
    {
        std::ofstream f(src);
        f << "B2 parent-is-file payload";
    }

    assert(ArchiveMutator::add_file_to_archive(arc, src, "blocker/child.txt", 0));

    // The collision: "blocker" exists as a FILE, not a directory.
    {
        std::ofstream f(dest_root / "blocker");
        f << "occupied";
    }

    ArchiveReader reader;
    assert(reader.open(arc));
    assert(!reader.extract_entry(reader.entries()[0], dest_root / "blocker" / "child.txt"));
    assert(fs::is_regular_file(dest_root / "blocker", ec)); // blocker untouched
    assert(!fs::exists(dest_root / "blocker" / "child.txt", ec));

    fs::remove(src, ec);
    fs::remove(arc, ec);
    fs::remove_all(dest_root, ec);
    std::cout << "[PASS] parent-is-file collision fails cleanly, no throw (B2)\n";
}

void test_extraction_limits_member_cap_known_size() {
    namespace fs = std::filesystem;
    fs::path dir = "build/test_limits_known";
    fs::path arc = dir / "test.rar";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(arc, ec);

    // Create an archive with 1024 bytes of stored data
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb;
        fb.file_name = "test.bin";
        fb.unp_size = 1024;
        fb.pack_size = 1024;
        fb.attributes = 0x20;
        fb.method = 0; // store
        assert(HeaderWriter::write_file_block(out, fb, format::HFL_DATA));
        std::vector<core::byte> payload(1024, 'X');
        assert(out.write(payload.data(), payload.size()) == payload.size());

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    ArchiveReader reader;
    int status = 0;
    std::string detail;
    assert(reader.open_ex(arc, "", status, detail));
    assert(reader.entries().size() == 1);

    // Member cap = 500 bytes (< 1024): must fail with RAR_ERR_LIMIT_EXCEEDED
    ExtractionLimits limits;
    limits.max_member_output_bytes = 500;
    LimitState state;
    std::vector<core::byte> mem_out;
    int rc = reader.extract_entry_to_memory(0, mem_out, 65536, {}, &limits, &state);
    assert(rc == RAR_ERR_LIMIT_EXCEEDED);

    // Member cap = 2000 bytes (> 1024): succeeds
    limits.max_member_output_bytes = 2000;
    mem_out.clear();
    LimitState state2;
    rc = reader.extract_entry_to_memory(0, mem_out, 65536, {}, &limits, &state2);
    assert(rc == RAR_OK);
    assert(mem_out.size() == 1024);

    fs::remove_all(dir, ec);
    std::cout << "[PASS] ExtractionLimits_MemberCapFiredOnKnownSize\n";
}

void test_extraction_limits_member_cap_unknown_size() {
    namespace fs = std::filesystem;
    fs::path dir = "build/test_limits_unknown";
    fs::path arc = dir / "bomb.rar";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(arc, ec);

    // Create an archive with FHFL_UNPUNKNOWN (unp_size = 0 declared in header)
    // but with 2048 bytes of stored payload
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb;
        fb.file_name = "bomb.bin";
        fb.file_flags = format::FHFL_UNPUNKNOWN;
        fb.unp_size = 0; // unknown uncompressed size
        fb.pack_size = 2048;
        fb.attributes = 0x20;
        fb.method = 0; // store
        assert(HeaderWriter::write_file_block(out, fb, format::HFL_DATA));
        std::vector<core::byte> payload(2048, 'Z');
        assert(out.write(payload.data(), payload.size()) == payload.size());

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    ArchiveReader reader;
    int status = 0;
    std::string detail;
    assert(reader.open_ex(arc, "", status, detail));
    assert(reader.entries().size() == 1);
    assert(reader.entries()[0].header.unp_size == 0); // verified unknown size

    // Member cap = 512 bytes: must fire mid-stream in sink lambda!
    ExtractionLimits limits;
    limits.max_member_output_bytes = 512;
    LimitState state;
    std::vector<core::byte> mem_out;
    int rc = reader.extract_entry_to_memory(0, mem_out, 65536, {}, &limits, &state);
    assert(rc == RAR_ERR_LIMIT_EXCEEDED);

    fs::remove_all(dir, ec);
    std::cout << "[PASS] ExtractionLimits_MemberCapFiredOnUnknownSize\n";
}

void test_extraction_limits_total_cap_spans_entries() {
    namespace fs = std::filesystem;
    fs::path dir = "build/test_limits_total";
    fs::path arc = dir / "two_files.rar";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(arc, ec);

    // Create archive with two files, 500 bytes each
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb1;
        fb1.file_name = "f1.bin";
        fb1.unp_size = 500;
        fb1.pack_size = 500;
        fb1.method = 0;
        assert(HeaderWriter::write_file_block(out, fb1, format::HFL_DATA));
        std::vector<core::byte> payload1(500, 'A');
        assert(out.write(payload1.data(), 500) == 500);

        FileBlock fb2;
        fb2.file_name = "f2.bin";
        fb2.unp_size = 500;
        fb2.pack_size = 500;
        fb2.method = 0;
        assert(HeaderWriter::write_file_block(out, fb2, format::HFL_DATA));
        std::vector<core::byte> payload2(500, 'B');
        assert(out.write(payload2.data(), 500) == 500);

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    ArchiveReader reader;
    int status = 0;
    std::string detail;
    assert(reader.open_ex(arc, "", status, detail));
    assert(reader.entries().size() == 2);

    // Limits: member cap = 800 (allows 500), total cap = 800 (< 1000)
    ExtractionLimits limits;
    limits.max_member_output_bytes = 800;
    limits.max_total_output_bytes = 800;
    LimitState state; // shared across entries

    std::vector<core::byte> mem_out;
    int rc1 = reader.extract_entry_to_memory(0, mem_out, 65536, {}, &limits, &state);
    assert(rc1 == RAR_OK);
    assert(state.total_out == 500);

    // Second entry pushes total_out to 1000 > 800: must fail with RAR_ERR_LIMIT_EXCEEDED
    mem_out.clear();
    int rc2 = reader.extract_entry_to_memory(1, mem_out, 65536, {}, &limits, &state);
    assert(rc2 == RAR_ERR_LIMIT_EXCEEDED);

    fs::remove_all(dir, ec);
    std::cout << "[PASS] ExtractionLimits_TotalCapSpansEntries\n";
}

void test_extraction_limits_header_count_cap() {
    namespace fs = std::filesystem;
    fs::path dir = "build/test_limits_hdr_count";
    fs::path arc = dir / "many_headers.rar";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(arc, ec);

    // Write signature, main header (1), and 10 file headers
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        for (int i = 0; i < 10; ++i) {
            FileBlock fb;
            fb.file_name = "file_" + std::to_string(i) + ".txt";
            fb.unp_size = 0;
            fb.pack_size = 0;
            fb.method = 0;
            assert(HeaderWriter::write_file_block(out, fb, 0));
        }

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    // Set max_header_count = 5.
    // Reading headers (main + 10 files) exceeds cap at 5th header!
    ExtractionLimits limits;
    limits.max_header_count = 5;
    LimitState state;

    ArchiveReader reader;
    int status = 0;
    std::string detail;
    bool opened = reader.open_ex(arc, "", status, detail, {}, false, &limits, &state);
    assert(!opened);
    assert(status == RAR_ERR_LIMIT_EXCEEDED);
    assert(state.header_count == 5);

    fs::remove_all(dir, ec);
    std::cout << "[PASS] ExtractionLimits_HeaderCountCap\n";
}

void test_extraction_limits_header_count_cumulative_across_volumes() {
    namespace fs = std::filesystem;
    fs::path dir = "build/test_limits_vol_cumulative";
    fs::path vol1 = dir / "vol.part01.rar";
    fs::path vol2 = dir / "vol.part02.rar";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(vol1, ec);
    fs::remove(vol2, ec);

    // Volume 1: signature, main block (volume), 2 file headers, end arc (nextvol)
    {
        io::FileStream out;
        assert(out.open(vol1, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        mb.arc_flags = format::MHFL_VOLUME;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb1;
        fb1.file_name = "vol_f1.txt";
        fb1.unp_size = 0;
        fb1.pack_size = 0;
        fb1.method = 0;
        assert(HeaderWriter::write_file_block(out, fb1, 0));

        FileBlock fb2;
        fb2.file_name = "vol_f2.txt";
        fb2.unp_size = 0;
        fb2.pack_size = 0;
        fb2.method = 0;
        assert(HeaderWriter::write_file_block(out, fb2, 0));

        EndArcBlock eb;
        eb.end_flags = 0x0001; // next volume
        assert(HeaderWriter::write_end_block(out, eb));
    }

    // Volume 2: signature, main block (volume), 2 file headers, end arc
    {
        io::FileStream out;
        assert(out.open(vol2, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        mb.arc_flags = format::MHFL_VOLUME;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb3;
        fb3.file_name = "vol_f3.txt";
        fb3.unp_size = 0;
        fb3.pack_size = 0;
        fb3.method = 0;
        assert(HeaderWriter::write_file_block(out, fb3, 0));

        FileBlock fb4;
        fb4.file_name = "vol_f4.txt";
        fb4.unp_size = 0;
        fb4.pack_size = 0;
        fb4.method = 0;
        assert(HeaderWriter::write_file_block(out, fb4, 0));

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    // Total headers across both volumes is ~8.
    // If cap is 5, volume 1 has 4 headers (main, f1, f2, endarc).
    // Volume 2's main block makes 5 headers, and next block reaches 6 > 5.
    // It must fail across the volume boundary with RAR_ERR_LIMIT_EXCEEDED!
    ExtractionLimits limits;
    limits.max_header_count = 5;
    LimitState state;

    ArchiveReader reader;
    int status = 0;
    std::string detail;
    bool opened = reader.open_ex(vol1, "", status, detail, {}, true, &limits, &state);
    assert(!opened);
    assert(status == RAR_ERR_LIMIT_EXCEEDED);
    assert(state.header_count == 5);

    fs::remove_all(dir, ec);
    std::cout << "[PASS] ExtractionLimits_HeaderCountCumulativeAcrossVolumes\n";
}

void test_invalid_utf8_filename_rejected() {
    namespace fs = std::filesystem;
    fs::path dir = "build/test_invalid_utf8";
    fs::path arc = dir / "invalid_name.rar";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(arc, ec);

    // Create an archive with an invalid UTF-8 filename (raw 0xFF byte)
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb;
        fb.file_name = "bad_\xFF_name.txt";
        fb.unp_size = 0;
        fb.pack_size = 0;
        fb.method = 0;
        assert(HeaderWriter::write_file_block(out, fb, 0));

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    ArchiveReader reader;
    int status = 0;
    std::string detail;
    bool opened = reader.open_ex(arc, "", status, detail);
    // v1.24 (plan §4.3) policy revision: names that are not well-formed
    // UTF-8 are admitted byte-losslessly — the header no longer fails
    // parse; the extraction pipeline percent-encodes the invalid sequences
    // so the escaped name IS the on-disk name (previously such entries —
    // and with them whole archives — were unreadable). Escaping itself is
    // pinned by extraction_report_tests (percent_encoded_undecodable).
    assert(opened);
    assert(reader.entries().size() == 1);
    assert(reader.entries()[0].header.file_name == "bad_\xFF_name.txt");

    fs::remove_all(dir, ec);
    std::cout << "[PASS] test_invalid_utf8_filename_rejected\n";
}

void test_decompressor_bad_alloc_returns_nomem() {
    std::cout << "Starting test_decompressor_bad_alloc_returns_nomem...\n" << std::flush;
    namespace fs = std::filesystem;
    fs::path dir = "build/test_bad_alloc_nomem";
    fs::path arc = dir / "huge_win.rar";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(arc, ec);

    // Create an archive with a valid 64 GiB window (within ALLOC_LIMIT on 64-bit)
    // but when decompress_internal allocates window_.assign(64 GiB, 0), it triggers
    // std::bad_alloc on hosts without 64 GiB committed RAM, which must return RAR_ERR_NOMEM (-5)
    // instead of misleading RAR_ERR_TRUNCATED (-3).
    {
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(HeaderWriter::write_signature(out));
        MainBlock mb;
        assert(HeaderWriter::write_main_block(out, mb));

        FileBlock fb;
        fb.file_name = "huge_win.bin";
        fb.unp_size = 1024;
        fb.pack_size = 8;
        fb.method = 3;
        fb.win_size = 64ULL * 1024 * 1024 * 1024; // 64 GiB
        fb.unp_ver = 1;
        fb.has_crc32 = true;
        fb.data_crc32 = 0x12345678;
        assert(HeaderWriter::write_file_block(out, fb));

        core::byte payload[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
        assert(out.write(payload, sizeof(payload)) == sizeof(payload));

        EndArcBlock eb;
        assert(HeaderWriter::write_end_block(out, eb));
    }

    ArchiveReader reader;
    int status = 0;
    std::string detail;
    assert(reader.open_ex(arc, "", status, detail));
    assert(reader.entries().size() == 1);

    // Environment probe (v1.22.0 CI fix): this test observes the
    // bad_alloc -> RAR_ERR_NOMEM mapping, which only exists where the host
    // REFUSES a 64 GiB window. Lazily-committing allocators (macOS) answer
    // the allocation with lazily-backed zero pages — there is nothing to
    // observe, and insisting would assert or get the process jetsammed.
    // The probe is 64-bit-only: on ILP32 the 64 GiB count does not fit
    // size_t (compile error) and the host trivially cannot materialize it.
    bool alloc_refuses = false;
#if SIZE_MAX >= UINT64_MAX
    {
        try {
            std::vector<core::byte> probe;
            probe.assign(64ULL * 1024 * 1024 * 1024, core::byte(0));
        } catch (const std::bad_alloc&) {
            alloc_refuses = true;
        } catch (...) {
            alloc_refuses = true;
        }
    }
#endif
    ReaderHooks hooks;
#if !defined(__EMSCRIPTEN__) && !defined(__wasm__) && !defined(_M_IX86) && !defined(__i386__) &&   \
    !defined(__arm__) && !defined(_M_ARM)
    if (!alloc_refuses) {
        std::cout << "[SKIP] decompressor bad_alloc -> RAR_ERR_NOMEM"
                     " (host materializes 64 GiB windows)\n";
        fs::remove_all(dir, ec);
        return;
    }
    // On 64-bit platforms where the allocation refuses: bad_alloc -> RAR_ERR_NOMEM
    int rc = reader.test_entry_stream(0, hooks);
    assert(rc == RAR_ERR_NOMEM);
#else
    // On 32-bit platforms: 64 GiB exceeds 1 GiB ALLOC_LIMIT -> RAR_ERR_LIMIT_EXCEEDED
    int rc = reader.test_entry_stream(0, hooks);
    assert(rc == RAR_ERR_LIMIT_EXCEEDED);
#endif

    fs::remove_all(dir, ec);
    std::cout << "[PASS] decompressor bad_alloc returns RAR_ERR_NOMEM (-5)\n";
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
    std::cout << "Running Clean-Room Milestone 5 Archive Operations Verification...\n";
    test_move_command();
    test_delete_and_wildcard();
    test_lock_command();
    test_mutation_temp_names_not_clobbered();
    test_sfx_preservation();
    test_bad_password_not_sticky();
    test_kdf_cap_pinned();
    test_truncated_data_area_clamped();
    test_filecopy_source_confined_to_root();
    test_links_to_dirs_leaves_preexisting_links();
    test_b9_b5_corrupt_payload_fails_cleanly();
    test_b9_compressed_corrupt_fails_cleanly();
    test_b2_parent_is_file_fails_cleanly();
    test_solid_window_size();
    test_extraction_limits_member_cap_known_size();
    test_extraction_limits_member_cap_unknown_size();
    test_extraction_limits_total_cap_spans_entries();
    test_extraction_limits_header_count_cap();
    test_extraction_limits_header_count_cumulative_across_volumes();
    test_invalid_utf8_filename_rejected();
    test_decompressor_bad_alloc_returns_nomem();
    std::cout << "All Milestone 5 Archive Operations & Mutation Primitives PASSED!\n";
    return 0;
}
