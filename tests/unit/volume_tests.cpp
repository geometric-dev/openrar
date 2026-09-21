#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/volume.hpp"
#include "../../src/core/types.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/io/file_stream.hpp"

#include <cassert>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#ifdef _MSC_VER
#include <crtdbg.h>
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

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::cout << "Running volume naming tests...\n";
    test_old_style_numbering_sequence();
    test_old_style_numbering_full_alphabet_and_refusal();
    test_first_volume_name();
    test_new_style_numbering();
    test_scan_terminates_at_z99_ceiling();
    test_streaming_multivolume_creation();
    std::cout << "All volume naming tests PASSED!\n";
    return 0;
}
