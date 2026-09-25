// Gate 1 — TOCTOU fault-injection suite (v1.24.0 M2, plan §10 tests 1-6,
// §11). Deterministic pre-planted adversaries at every position the
// containment walk defends: the write target leaf, an intermediate path
// component, and 8.3-alias-shaped names. The whole suite runs twice on
// POSIX — with openat2 enabled and with the walk fallback forced — so the
// fallback path is exercised everywhere (and WSL1, which lacks openat2,
// runs it by construction).
//
//   toctou_symlink_target_race    (plan §10 test 1)
//   toctou_midpath_junction       (plan §10 test 2)
//   toctou_journal_sweep_race     — landed with M1 (extraction_atomic_tests)
//   toctou_rename_leaf_symlink    (plan §10 test 4)
//   openat2_unavailable_falls_back (plan §10 test 5 — second suite pass)
//   alias_83_rejected             (plan §10 test 6)

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/io/containment.hpp"
#include "../../src/io/extraction_journal.hpp"
#include "../../src/io/win32_meta.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

namespace {

namespace fs = std::filesystem;

fs::path make_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_g1_") + name);
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

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void plant_link(const fs::path& link_path, const fs::path& target, bool dir_link) {
    std::error_code ec;
    fs::remove(link_path, ec);
#ifdef _WIN32
    // Junctions need no privilege and are the realistic Windows attack
    // surface for planted directory reparse points.
    io::RedirEntry re;
    re.type = dir_link ? io::RedirType::Junction : io::RedirType::WinSymlink;
    re.target = target.string();
    re.is_directory = dir_link;
    assert(io::create_reparse_link(link_path, re));
#else
    (void)dir_link;
    assert(::symlink(target.c_str(), link_path.c_str()) == 0);
#endif
}

bool is_symlink_like(const fs::path& p) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (!ec && fs::is_symlink(st)) return true;
#ifdef _WIN32
    // MSVC's symlink_status reports junctions (MOUNT_POINT reparse points)
    // as directories — check the reparse attribute directly.
    const DWORD attrs = GetFileAttributesW(p.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}

// Builds a stored archive containing the given (name → content) entries.
fs::path build_archive(const fs::path& dir,
                       const std::vector<std::pair<std::string, std::string>>& entries) {
    const fs::path arc = dir / "g1_fixture.rar";
    const fs::path blob = dir / "g1_blob.bin";
    bool first = true;
    for (const auto& [name, content] : entries) {
        write_file(blob, content);
        assert(archive::ArchiveMutator::add_file_to_archive(arc, blob, name, 0) || !first);
        first = false;
    }
    fs::remove(blob);
    return arc;
}

// ── The suite body (run twice: openat2 on / fallback forced) ────────────────

void suite_toctou_planted_links(const fs::path& work, bool force_fallback) {
    (void)work;
    const std::string tag = force_fallback ? "fallback" : "primary";

    // Test 1 — toctou_symlink_target_race: a symlink pre-planted AT the
    // write target (dangling, pointing outside the root) is caught: the
    // extraction still lands the correct file, the link is replaced rather
    // than followed, and nothing appears at the outside target.
    {
        const fs::path root = make_dir("t1_root");
        const fs::path outside = make_dir("t1_outside");
        const fs::path arc = build_archive(root, {{"sub/victim.txt", "contained content"}});
        fs::create_directories(root / "sub");
        plant_link(root / "sub" / "victim.txt", outside / "planted.txt", false);
        write_file(outside / "planted.txt", "do not touch");

        archive::ArchiveReader reader;
        reader.set_extraction_root(root);
        if (force_fallback) reader.force_containment_fallback_for_test();
        assert(reader.open(arc));
        assert(reader.extract_entry(reader.entries()[0], root / "sub" / "victim.txt"));

        assert(read_file(root / "sub" / "victim.txt") == "contained content");
        assert(!is_symlink_like(root / "sub" / "victim.txt"));
        assert(read_file(outside / "planted.txt") == "do not touch"); // never followed
        (void)tag;
        rm(root);
        rm(outside);
    }

    // Test 2 — toctou_midpath_junction: a junction/symlink planted at an
    // INTERMEDIATE component is rejected by the walk; the entry fails, the
    // planted link and its outside target are untouched, and no temp or
    // output appears anywhere.
    {
        const fs::path root = make_dir("t2_root");
        const fs::path outside = make_dir("t2_outside");
        const fs::path arc = build_archive(root, {{"sub/inner.txt", "should not land"}});
        fs::create_directories(outside / "real");
        plant_link(root / "sub", outside / "real", true);

        archive::ArchiveReader reader;
        reader.set_extraction_root(root);
        if (force_fallback) reader.force_containment_fallback_for_test();
        assert(reader.open(arc));
        assert(!reader.extract_entry(reader.entries()[0], root / "sub" / "inner.txt"));

        assert(is_symlink_like(root / "sub")); // the planted link survives
        bool outside_clean = true;
        for (const auto& e : fs::directory_iterator(outside / "real")) {
            (void)e;
            outside_clean = false; // nothing may be written through the link
        }
        assert(outside_clean);
        // No temps and no journals anywhere (archive fixture + the planted
        // junction are expected).
        std::error_code dec;
        for (const auto& e : fs::directory_iterator(root)) {
            const std::string name = io::u8_str(e.path().filename());
            assert(name == "g1_fixture.rar" || name == "sub");
        }
        assert(!fs::exists(root / "sub" / "inner.txt", dec));
        rm(root);
        rm(outside);
    }

    // Test 4 — toctou_rename_leaf_symlink: a symlink at the rename
    // destination pointing at a REAL outside file is replaced (not followed)
    // by the anchored commit rename; the outside file keeps its bytes.
    {
        const fs::path root = make_dir("t4_root");
        const fs::path outside = make_dir("t4_outside");
        const fs::path arc = build_archive(root, {{"leaf.txt", "replacing content"}});
        plant_link(root / "leaf.txt", outside / "real_target.txt", false);
        write_file(outside / "real_target.txt", "original outside bytes");

        archive::ArchiveReader reader;
        reader.set_extraction_root(root);
        if (force_fallback) reader.force_containment_fallback_for_test();
        assert(reader.open(arc));
        assert(reader.extract_entry(reader.entries()[0], root / "leaf.txt"));

        assert(read_file(root / "leaf.txt") == "replacing content");
        assert(!is_symlink_like(root / "leaf.txt"));
        assert(read_file(outside / "real_target.txt") == "original outside bytes");
        rm(root);
        rm(outside);
    }

    std::cout << "[PASS] toctou planted-link suite (" << tag << ")\n";
}

void suite_containment_shapes(const fs::path& work, bool force_fallback) {
    (void)work;
    // Cache-hit path: several files through the SAME directory — the first
    // walk populates the LRU cache, the rest must stay contained and land
    // exactly where the caller aimed.
    {
        const fs::path root = make_dir("cache_root");
        const fs::path arc = build_archive(root, {{"deep/nest/a.txt", "A"},
                                                  {"deep/nest/b.txt", "BB"},
                                                  {"deep/nest/c.txt", "CCC"},
                                                  {"top.txt", "T"}});
        archive::ArchiveReader reader;
        reader.set_extraction_root(root);
        if (force_fallback) reader.force_containment_fallback_for_test();
        assert(reader.open(arc));
        const auto& es = reader.entries();
        assert(reader.extract_entry(es[0], root / "deep" / "nest" / "a.txt"));
        assert(reader.extract_entry(es[1], root / "deep" / "nest" / "b.txt"));
        assert(reader.extract_entry(es[2], root / "deep" / "nest" / "c.txt"));
        assert(reader.extract_entry(es[3], root / "top.txt"));
        assert(read_file(root / "deep" / "nest" / "a.txt") == "A");
        assert(read_file(root / "deep" / "nest" / "b.txt") == "BB");
        assert(read_file(root / "deep" / "nest" / "c.txt") == "CCC");
        assert(read_file(root / "top.txt") == "T");
        rm(root);
    }

    // A plain FILE planted where a directory component must be opened is
    // rejected (ENOTDIR class), the file untouched.
    {
        const fs::path root = make_dir("file_root");
        const fs::path arc = build_archive(root, {{"block/inner.txt", "nope"}});
        write_file(root / "block", "i am a file");

        archive::ArchiveReader reader;
        reader.set_extraction_root(root);
        if (force_fallback) reader.force_containment_fallback_for_test();
        assert(reader.open(arc));
        assert(!reader.extract_entry(reader.entries()[0], root / "block" / "inner.txt"));
        assert(read_file(root / "block") == "i am a file");
        rm(root);
    }

    // Directory records materialize through the contained walk too: the
    // file's parent chain is created no-follow and lands under the root.
    {
        const fs::path root = make_dir("dir_root");
        const fs::path arc = build_archive(root, {{"made/by/walk.txt", "nested"}});
        archive::ArchiveReader reader;
        reader.set_extraction_root(root);
        if (force_fallback) reader.force_containment_fallback_for_test();
        assert(reader.open(arc));
        assert(reader.extract_entry(reader.entries()[0], root / "made" / "by" / "walk.txt"));
        assert(read_file(root / "made" / "by" / "walk.txt") == "nested");
        rm(root);
    }

    std::cout << "[PASS] containment walk shapes (" << (force_fallback ? "fallback" : "primary")
              << ")\n";
}

void test_toctou_symlink_target_race_and_friends() {
    const fs::path work = make_dir("work");
    // Primary path (openat2 on Linux 5.6+, NtCreateFile walk on Windows)…
    suite_toctou_planted_links(work, false);
    suite_containment_shapes(work, false);
    // …and the forced-fallback pass (plan test 5). On Windows the containment
    // walk IS the only path; the flag is a no-op there.
#ifdef __linux__
    suite_toctou_planted_links(work, true);
    suite_containment_shapes(work, true);
#endif
    rm(work);
    std::cout << "[PASS] openat2_unavailable_falls_back: both paths pass the suite\n";
}

void test_alias_83_rejected() {
    // Predicate level.
    assert(io::is_83_alias_component("PROGRA~1"));
    assert(io::is_83_alias_component("progra~1.txt"));
    assert(io::is_83_alias_component("LONGFI~1.TXT"));
    assert(!io::is_83_alias_component("a~b.txt"));
    assert(!io::is_83_alias_component("plain.txt"));
    assert(!io::is_83_alias_component("~filename")); // '~' not followed by a digit
    assert(io::path_has_83_component("docs/PROGRA~1/x.txt"));
    assert(!io::path_has_83_component("docs/fine~name/x.txt"));

    // Reader level: an 8.3-shaped directory component fails closed in the
    // walk — the entry is rejected, nothing is created.
    const fs::path root = make_dir("alias_root");
    const fs::path arc = build_archive(root, {{"PROGRA~1/app.txt", "aliased"}});
    archive::ArchiveReader reader;
    reader.set_extraction_root(root);
    assert(reader.open(arc));
    assert(!reader.extract_entry(reader.entries()[0], root / "PROGRA~1" / "app.txt"));
    assert(!fs::exists(root / "PROGRA~1"));
    std::cout << "[PASS] alias_83_rejected: 8.3-shaped paths fail closed\n";
    rm(root);
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
    test_toctou_symlink_target_race_and_friends();
    test_alias_83_rejected();
    std::cout << "All extraction_containment_tests passed.\n";
    return 0;
}
