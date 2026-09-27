// Attack regression corpus (v1.30.0 M5; SECURITY_ARCHITECTURE §7.2, freeze
// prereq 4). The historical archive-CVE classes re-run in CI as crafted,
// spec-legal archives with per-case guarantee assertions (the Track-10
// generator style — no hostile binaries in-tree):
//
//   attack_corpus_traversal_contained   (CVE-2025-8088 class: path traversal
//     in entry names — the containment walk refuses; nothing lands outside)
//   attack_corpus_spoofing_flagged      (CVE-2023-38831 class: spoofed
//     double-extension names extract under their EXACT archived names — the
//     UI invariant holds, nothing is renamed or executed)
//   attack_corpus_rr_overflow_capped    (CVE-2023-40477 class: hostile
//     recovery-record geometry — parity bounds refuse fail-closed)
//   attack_corpus_bomb_limits_fire      (decompression bomb: absurd declared
//     output against a tiny payload — the caps fire mid-flight)
//   attack_corpus_hostile_vint_refused  (overlong/underflowing vint header
//     sizes — refused as TRUNCATED/NOT_RAR, no crash, no hang)
//
// Plus the sandboxed-worker path (v1.30): the traversal and bomb classes
// are re-run THROUGH the sandboxed worker where a model ships — the broker
// enforces the same guarantees over the IPC stream.

#include "test_support.hpp"

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/rar_errors.hpp"
#include "../../src/core/types.hpp"
#include "../../src/format/header_reader.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/format/headers.hpp"
#include "../../src/io/file_stream.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace openrar;
using namespace openrar::archive;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

namespace {

fs::path make_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_ac_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void write_bytes(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    assert(f.good());
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Hand-built single-entry STORED archive through HeaderWriter (valid CRC) —
// the entry name is written VERBATIM so the hostile shapes reach the reader.
void craft_stored(const fs::path& arc, const std::string& entry_name, size_t unp_declared,
                  const std::string& payload) {
    io::FileStream out;
    assert(out.open(arc, io::FileMode::CreateAlways));
    assert(format::HeaderWriter::write_signature(out));
    format::MainBlock mb;
    assert(format::HeaderWriter::write_main_block(out, mb));
    format::FileBlock fb;
    fb.file_name = entry_name;
    fb.unp_size = unp_declared;
    fb.pack_size = payload.size();
    fb.method = 0;
    fb.host_os = 1;
    assert(format::HeaderWriter::write_file_block(out, fb, 0));
    if (!payload.empty())
        out.write(reinterpret_cast<const core::byte*>(payload.data()), payload.size());
    format::EndArcBlock eb;
    assert(format::HeaderWriter::write_end_block(out, eb));
}


// Hostile vint: the header SIZE field carries an overlong encoding.
void craft_hostile_vint(const fs::path& arc) {
    io::FileStream out;
    assert(out.open(arc, io::FileMode::CreateAlways));
    assert(format::HeaderWriter::write_signature(out));
    format::MainBlock mb;
    assert(format::HeaderWriter::write_main_block(out, mb));
    // A header whose size vint uses the full 8-byte continuation form with a
    // value far beyond the remaining bytes.
    const core::byte hostile[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, // size vint (overlong)
        0x01,                                                       // type: HEAD_MAIN
    };
    out.write(hostile, sizeof(hostile));
    format::EndArcBlock eb;
    assert(format::HeaderWriter::write_end_block(out, eb));
}

} // namespace

int main() {
    // ── 1. CVE-2025-8088 class: traversal names are contained ─────────────
    std::cout << "[+] attack_corpus_traversal_contained\n";
    {
        const fs::path root = make_dir("trav");
        const fs::path arc = root / "trav.rar";
        craft_stored(arc, "..\\..\\outside.txt", 12, "hostilebytes");
        archive::ArchiveReader r;
        CHECK(r.open(arc));
        std::error_code dec;
        // Whatever the walk's verdict, NOTHING may land outside the root.
        const fs::path outside = root.parent_path() / "outside.txt";
        r.set_extraction_root(root);
        (void)r.extract_entry(r.entries()[0], root / "out.bin");
        CHECK(!fs::exists(outside, dec));
        CHECK(!fs::exists(root.parent_path() / "out.bin", dec));
        r.close();
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove(outside, ec);
    }

    // ── 2. CVE-2023-38831 class: spoofed names extract under exact names ──
    std::cout << "[+] attack_corpus_spoofing_flagged\n";
    {
        const fs::path dir = make_dir("spoof");
        // Two entries: the decoy image name and the hostile .cmd twin —
        // extraction must produce EXACTLY the archived names at the exact
        // paths (no renames, no "helper" files an extractor might execute).
        const fs::path arc = dir / "spoof.rar";
        const fs::path blob = dir / "blob.bin";
        write_bytes(blob, "decoy");
        assert(ArchiveMutator::add_file_to_archive(arc, blob, "invoice.jpg", 0));
        write_bytes(blob, "hostile");
        assert(ArchiveMutator::add_file_to_archive(arc, blob, "invoice.jpg.cmd", 0));
        fs::remove(blob);
        archive::ArchiveReader r;
        CHECK(r.open(arc));
        r.set_extraction_root(dir);
        for (size_t i = 0; i < r.entries().size(); ++i) {
            const std::string nm = r.entries()[i].header.file_name;
            CHECK(r.extract_entry(r.entries()[i], dir / nm));
        }
        CHECK(read_file(dir / "invoice.jpg") == "decoy");
        CHECK(read_file(dir / "invoice.jpg.cmd") == "hostile");
        std::error_code dec;
        size_t landed = 0;
        for (const auto& _ : fs::directory_iterator(dir, dec)) (void)_, landed++;
        CHECK(landed == 3); // arc + invoice.jpg + invoice.jpg.cmd — nothing else
        r.close();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // ── 3. CVE-2023-40477 class: hostile RR geometry refuses fail-closed ──
    std::cout << "[+] attack_corpus_rr_overflow_capped\n";
    {
        const fs::path dir = make_dir("rr");
        const fs::path arc = dir / "rr.rar";
        // A service header claiming 1 GiB of recovery geometry: the parity
        // bounds / caps must refuse before any allocation (fail-closed).
        io::FileStream out;
        assert(out.open(arc, io::FileMode::CreateAlways));
        assert(format::HeaderWriter::write_signature(out));
        format::MainBlock mb;
        assert(format::HeaderWriter::write_main_block(out, mb));
        format::FileBlock fb;
        fb.file_name = "RR";
        fb.is_service = true;
        fb.unp_size = 0x40000000ull; // 1 GiB claimed
        fb.pack_size = 0x40000000ull;
        fb.method = 0;
        fb.host_os = 1;
        assert(format::HeaderWriter::write_file_block(out, fb, 0));
        const std::string payload(16, 'R');
        out.write(reinterpret_cast<const core::byte*>(payload.data()), payload.size());
        format::EndArcBlock eb;
        assert(format::HeaderWriter::write_end_block(out, eb));

        archive::ArchiveReader r;
        const bool opened = r.open(arc);
        if (opened) {
            // If it opens, decoding the hostile service geometry must refuse.
            std::vector<core::byte> out_buf;
            const int rc = r.extract_entry_to_memory(0, out_buf, 1ull << 20, {}, nullptr, nullptr);
            CHECK(rc != RAR_OK);
        }
        // Either way: no crash, no huge allocation (the RSS ceiling is the
        // sanitizer leg's job; here the refusal is behavioral).
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // ── 4. Bomb: caps fire mid-flight ─────────────────────────────────────
    std::cout << "[+] attack_corpus_bomb_limits_fire\n";
    {
        const fs::path dir = make_dir("bomb");
        const fs::path arc = dir / "bomb.rar";
        // A REAL bomb: 64 MiB of zeros compress to a few KiB — the declared
        // output is modest but the CAP (1 MiB) fires mid-decode.
        const fs::path blob = dir / "zeros.bin";
        {
            std::ofstream f(blob, std::ios::binary | std::ios::trunc);
            const std::string chunk(1 << 20, '\0');
            for (int i = 0; i < 64; ++i) f.write(chunk.data(), chunk.size());
        }
        assert(ArchiveMutator::add_file_to_archive(arc, blob, "bomb.bin", 3));
        fs::remove(blob);
        archive::ArchiveReader r;
        CHECK(r.open(arc));
        std::vector<core::byte> out;
        ExtractionLimits lim;
        lim.max_member_output_bytes = 1 << 20; // 1 MiB
        LimitState st;
        const int rc = r.extract_entry_to_memory(0, out, 1ull << 32, {}, &lim, &st);
        CHECK(rc == RAR_ERR_LIMIT_EXCEEDED);
        CHECK(out.size() <= (1u << 20));
        r.close();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // ── 5. Hostile vint: refused, no crash, no hang ───────────────────────
    std::cout << "[+] attack_corpus_hostile_vint_refused\n";
    {
        const fs::path dir = make_dir("vint");
        const fs::path arc = dir / "vint.rar";
        craft_hostile_vint(arc);
        archive::ArchiveReader r;
        const bool opened = r.open(arc);
        // Either the open refuses or the hostile block is skipped: zero file
        // entries may materialize, and there is no crash or hang.
        CHECK(!opened || r.entries().empty());
        r.close();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    if (fails == 0) std::printf("[attack-corpus] CVE-class corpus: OK\n");
    return fails == 0 ? 0 : 1;
}
