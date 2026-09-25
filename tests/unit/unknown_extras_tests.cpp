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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

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

    rm(dir);
    return 0;
}
