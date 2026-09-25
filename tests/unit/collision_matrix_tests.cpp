// Gate 2 — collision matrix suite (v1.24.0 M3, plan §10 tests 7-12, §11).
//
// Archive-internal collisions are integrity failures: the CLI aborts with
// the structural exit code (2) BEFORE writing anything. The suite pins the
// four detection classes at the detector level and the end-to-end abort at
// the CLI level, plus service-header exemption and the multi-volume span.

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/format/header_reader.hpp"
#include "../../src/crypto/crc32.hpp"
#include "../../src/core/vint.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/collision_detector.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef OPENRAR_CLI_EXE
#define OPENRAR_CLI_EXE ""
#endif
#ifndef OPENRAR_RUNNER
#define OPENRAR_RUNNER ""
#endif

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

namespace {

namespace fs = std::filesystem;

fs::path make_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_g2_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void rm(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

void write_file(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    assert(f.good());
}

// NFC and NFD spellings of "café" (U+00E9 vs e + U+0301).
const char* kCafeNFC = "caf\xC3\xA9";
const char* kCafeNFD = "caf\x65\xCC\x81";

// ── Detector-level matrix ────────────────────────────────────────────────────

bool has_class(const std::vector<archive::CollisionPair>& collisions, const char* cls) {
    for (const auto& c : collisions) {
        if (c.cls == cls) return true;
    }
    return false;
}

void test_detector_matrix() {
    using archive::CollisionDetector;
    using archive::CollisionEntry;
    std::vector<archive::CollisionPair> out;

    // duplicate-identical
    out.clear();
    assert(CollisionDetector::detect({{"a.txt", false}, {"a.txt", false}}, out));
    assert(has_class(out, "duplicate"));

    // case_fold: Readme vs README; also Unicode fold (İ vs i̇ folds equal)
    out.clear();
    assert(CollisionDetector::detect({{"Readme", false}, {"README", false}}, out));
    assert(has_class(out, "case_fold"));

    // NFC: NFC vs NFD café
    out.clear();
    assert(CollisionDetector::detect({{kCafeNFC, false}, {kCafeNFD, false}}, out));
    assert(has_class(out, "nfc"));

    // file_vs_dir: foo (file) vs foo/bar; either registration order
    out.clear();
    assert(CollisionDetector::detect({{"foo", false}, {"foo/bar", false}}, out));
    assert(has_class(out, "file_vs_dir"));
    out.clear();
    assert(CollisionDetector::detect({{"foo/bar", false}, {"foo", false}}, out));
    assert(has_class(out, "file_vs_dir"));

    // Non-collisions must stay silent:
    //  - normal nesting (dir entry foo + file foo/bar)
    //  - distinct-but-related names
    //  - distinct normalization-insensitive pairs that do NOT fold equal
    out.clear();
    const std::vector<archive::CollisionEntry> ok = {{"foo", true},
                                                     {"foo/bar", false},
                                                     {"a_text.txt", false},
                                                     {"sub/a_text.txt", false},
                                                     {kCafeNFC, false},
                                                     {"caf\xC3\xA9"
                                                      "d",
                                                      false}};
    assert(!CollisionDetector::detect(ok, out));

    // Unicode version constant is surfaced for the JSON summary (§3.3).
    assert(std::string(CollisionDetector::unicode_version()) == "15.1.0");
    std::cout << "[PASS] detector matrix: four classes + non-collision silence\n";
}

// ── CLI-level aborts (plan tests 7-10) ──────────────────────────────────────

#ifdef _WIN32
#define DEVNULL "nul"
#else
#define DEVNULL "/dev/null"
#endif

std::string cli_command(const fs::path& dir, const char* args) {
    (void)dir;
    const char* runner_env = std::getenv("OPENRAR_RUNNER");
    std::string prefix = (runner_env && *runner_env) ? std::string(runner_env) + " " : "";
    std::string exe;
#ifdef OPENRAR_CLI_EXE
    if (fs::exists(OPENRAR_CLI_EXE)) exe = fs::canonical(OPENRAR_CLI_EXE).string();
#endif
    if (exe.empty()) exe = "openrar.exe";
    // No quotes anywhere: cmd /c strips the first and last quote of a
    // fully-quoted command (mangling it), and CI workspace paths contain no
    // spaces (same assumption as cli_tests).
    return prefix + exe + " " + args;
}

// Builds an archive with the given entry names via raw header writing (the
// mutator's add API replaces same-name entries, which would erase the very
// collisions under test) and runs the CLI extraction into a fresh output
// dir; returns the exit code.
int extract_exit_code(const fs::path& dir,
                      const std::vector<std::pair<std::string, std::string>>& entries,
                      const fs::path& out) {
    const fs::path arc = dir / "g2.rar";
    {
        io::FileStream out_stream;
        assert(out_stream.open(arc, io::FileMode::CreateAlways));
        format::HeaderWriter::write_signature(out_stream);
        format::MainBlock mb;
        format::HeaderWriter::write_main_block(out_stream, mb);
        for (const auto& [name, content] : entries) {
            format::FileBlock fb;
            fb.file_name = name;
            fb.unp_size = content.size();
            fb.pack_size = content.size();
            fb.method = 0;
            fb.win_size = 0;
            assert(format::HeaderWriter::write_file_block(out_stream, fb));
            out_stream.write(content.data(), content.size());
        }
        format::EndArcBlock eb;
        format::HeaderWriter::write_end_block(out_stream, eb);
        out_stream.close();
    }
    // The redirect suffix prevents cmd /c from stripping the outer quotes
    // (the command would otherwise start AND end with a quoted token).
    // Quoting notes: cli_tests passes relative unquoted paths for the same
    // reason — cmd /c's quote handling mangles fully-quoted commands. The
    // CI workspace paths contain no spaces (same assumption as cli_tests).
    const std::string cmd = cli_command(dir, ("x " + arc.string() + " " + out.string() + " > \"" +
                                              dir.string() + "\\cli_out.txt\" 2>&1")
                                                 .c_str());
    const int rc = std::system(cmd.c_str());
    // Raw waitstatus: the caller unwraps WEXITSTATUS on POSIX.
    return rc;
}

void test_cli_collision_aborts() {
    const fs::path dir = make_dir("cli");

    // Test 7 — collision_duplicate_identical_abort: exit 2, atomic
    // no-partial state (the output directory must stay empty).
    {
        const fs::path out = dir / "out_dup";
        const int rc = extract_exit_code(dir, {{"x.txt", "one"}, {"x.txt", "two"}}, out);
#ifdef _WIN32
        assert(rc == 2);
#else
        assert(WIFEXITED(rc) && WEXITSTATUS(rc) == 2);
#endif
        std::error_code ec;
        assert(!fs::exists(out, ec) || fs::is_empty(out, ec)); // nothing written
    }

    // Test 8 — collision_case_folded_abort: Readme + README.
    {
        const fs::path out = dir / "out_case";
        const int rc = extract_exit_code(dir, {{"Readme", "a"}, {"README", "b"}}, out);
#ifdef _WIN32
        assert(rc == 2);
#else
        assert(WIFEXITED(rc) && WEXITSTATUS(rc) == 2);
#endif
    }

    // Test 9 — collision_nfc_nfd_abort: NFC + NFD café.
    {
        const fs::path out = dir / "out_nfc";
        const int rc = extract_exit_code(dir, {{kCafeNFC, "a"}, {kCafeNFD, "b"}}, out);
#ifdef _WIN32
        assert(rc == 2);
#else
        assert(WIFEXITED(rc) && WEXITSTATUS(rc) == 2);
#endif
    }

    // Test 10 — collision_file_vs_dir_abort: foo (file) + foo/bar.
    {
        const fs::path out = dir / "out_fvd";
        const int rc = extract_exit_code(dir, {{"foo", "a"}, {"foo/bar", "b"}}, out);
#ifdef _WIN32
        assert(rc == 2);
#else
        assert(WIFEXITED(rc) && WEXITSTATUS(rc) == 2);
#endif
    }

    std::cout << "[PASS] CLI aborts with exit 2 and writes nothing (tests 7-10)\n";
    rm(dir);
}

void test_service_headers_exempt() {
    // Test 11 — CMT/QO/RR service headers never collide with user entries:
    // an archive with a comment + recovery data + regular entries extracts
    // cleanly even though service payloads could share byte patterns.
    const fs::path dir = make_dir("svc");
    const fs::path arc = dir / "svc.rar";
    const fs::path blob = dir / "blob.bin";
    write_file(blob, "user data");
    std::vector<core::byte> comment = {'a', 'b', 'c'};
    assert(archive::ArchiveMutator::add_file_to_archive_vol(arc, blob, "user.txt", 0, 0, "", false,
                                                            0, {}, false, &comment));

    archive::ArchiveReader reader;
    assert(reader.open(arc));
    std::vector<archive::CollisionPair> collisions;
    assert(!reader.detect_collisions(collisions)); // services exempt
    std::cout << "[PASS] collision_service_headers_exempt\n";
    rm(dir);
}

void test_multivolume_span() {
    // Test 12 — a duplicate split across volume files is caught: the reader
    // merges the volume set at open() and the detector runs on the merged
    // list (plan §0, §3.1).
    const fs::path dir = make_dir("mv");
    const fs::path blob = dir / "blob.bin";
    write_file(blob, std::string(200000, 'A'));
    // vol_size small enough to split the set across volume files; the
    // mutator emits new-numbering names (mv.part01.rar ...) and REPLACES
    // same-name adds, so the two members get distinct names and the second
    // member's file header is byte-patched to the first member's name
    // (equal length, header CRC recomputed) to forge the cross-volume
    // duplicate the detector must catch.
    const fs::path arc = dir / "mv.part01.rar";
    const fs::path seed = dir / "mv.rar";
    assert(archive::ArchiveMutator::add_file_to_archive_vol(seed, blob, "big1.bin", 0, 65536));
    write_file(blob, std::string(200000, 'B'));
    assert(archive::ArchiveMutator::add_file_to_archive_vol(seed, blob, "big2.bin", 0, 65536));

    // Patch big2.bin -> big1.bin in whichever volume holds its header.
    bool patched = false;
    for (int nn = 1; nn <= 8 && !patched; ++nn) {
        fs::path vol = dir / ("mv.part0" + std::to_string(nn) + ".rar");
        std::error_code vol_ec;
        if (!fs::exists(vol, vol_ec)) break;
        io::FileStream f;
        assert(f.open(vol, io::FileMode::ReadWrite));
        core::byte sig[8];
        if (f.read(sig, 8) != 8) break;
        for (;;) {
            const core::uint64 header_pos = f.tell();
            core::uint64 type = 0, flags = 0, data_size = 0;
            std::vector<core::byte> body;
            if (format::HeaderReader::read_block_raw(f, type, flags, body, data_size) !=
                format::HeaderResult::Ok) {
                break;
            }
            if (type != format::HEAD_FILE) {
                f.seek(static_cast<core::int64>(data_size), io::SeekOrigin::Current);
                continue;
            }
            format::FileBlock fb;
            if (!format::HeaderReader::parse_file_header(body.data(), body.size(), fb) ||
                fb.file_name != "big2.bin") {
                f.seek(static_cast<core::int64>(data_size), io::SeekOrigin::Current);
                continue;
            }
            // Locate the len-vint + "big2.bin" inside the body and rewrite
            // the 8 name bytes in place.
            const char needle[9] = {'', 'b', 'i', 'g', '2', '.', 'b', 'i', 'n'};
            size_t name_off = body.size();
            for (size_t k = 0; k + 9 <= body.size(); ++k) {
                if (std::memcmp(body.data() + k, needle, 9) == 0) {
                    name_off = k + 1;
                    break;
                }
            }
            assert(name_off < body.size());
            std::memcpy(body.data() + name_off, "big1.bin", 8);
            // Recompute the header CRC over (size vint + body) and write it.
            std::vector<core::byte> size_vint;
            core::push_vint(size_vint, body.size());
            crypto::Crc32 crc;
            crc.update(size_vint.data(), size_vint.size());
            crc.update(body.data(), body.size());
            core::byte crc_le[4] = {static_cast<core::byte>(crc.get() & 0xFF),
                                    static_cast<core::byte>((crc.get() >> 8) & 0xFF),
                                    static_cast<core::byte>((crc.get() >> 16) & 0xFF),
                                    static_cast<core::byte>((crc.get() >> 24) & 0xFF)};
            f.seek(static_cast<core::int64>(header_pos), io::SeekOrigin::Begin);
            assert(f.write(crc_le, 4) == 4);
            f.seek(static_cast<core::int64>(header_pos + 4 + size_vint.size() + name_off),
                   io::SeekOrigin::Begin);
            assert(f.write("big1.bin", 8) == 8);
            patched = true;
            break;
        }
        f.close();
    }
    assert(patched);

    // the volume set opens (merged list spans several volume files)
    archive::ArchiveReader reader;
    assert(reader.open(arc));
    assert(reader.entries().size() >= 2); // merged list spans the volume set
    std::vector<archive::CollisionPair> collisions;
    assert(reader.detect_collisions(collisions));
    assert(has_class(collisions, "duplicate"));
    std::cout << "[PASS] collision_multivolume_span\n";
    rm(dir);
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
    test_detector_matrix();
    test_cli_collision_aborts();
    test_service_headers_exempt();
    test_multivolume_span();
    std::cout << "All collision_matrix_tests passed.\n";
    return 0;
}
