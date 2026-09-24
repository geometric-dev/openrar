// v1.23.0 SFX directive-engine unit tests (M1): the directive grammar and
// SfxConfig caps (docs/sfx-v1.23-implementation-plan.md §4) and the
// ArchiveReader::read_archive_comment accessor (§3). Sandbox e2e suites
// (gate 2) build on these in M6.

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/core/types.hpp"
#include "../../src/sfx/sfx_config.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;
using namespace openrar::archive;
namespace fs = std::filesystem;
namespace fs = std::filesystem;

static std::vector<core::byte> bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

// ── Parser: grammar, multiplicity, caps ─────────────────────────────────────

static void test_parse_empty_and_prose() {
    sfx::SfxConfig cfg = sfx::parse_sfx_config({});
    assert(!cfg.has_directives());
    assert(!cfg.invalid_utf8);

    // Prose without '=' is ignored silently (dialog bodies live in Text=).
    std::string prose = "This installer will install things.\nPlease wait.\n";
    cfg = sfx::parse_sfx_config({prose.begin(), prose.end()});
    assert(!cfg.has_directives());
    assert(cfg.ignored_unknown_keys == 0);
    std::cout << "[PASS] sfx parse: empty comment and prose ignored\n";
}

static void test_parse_full_feature() {
    // CRLF endings, BOM, every key, accumulation order and last-wins.
    std::string cmt = "\xef\xbb\xbf" // UTF-8 BOM
                      "Title=My Installer\r\n"
                      "Path=tools\\\r\n"
                      "Text=Welcome.\r\n"
                      "Text=Second line.\r\n"
                      "License=Line one.\r\n"
                      "License=Line two.\r\n"
                      "Presetup=prep.exe\r\n"
                      "Setup=setup.exe /silent\r\n"
                      "Setup=msiexec /i pkg.msi\r\n"
                      "Delete=*.tmp\r\n"
                      "Delete=old\\*.log\r\n"
                      "Shortcut=a.bin,DESKTOP,Tool A,The tool,a.bin,0\r\n"
                      "Silent=2\r\n"
                      "Overwrite=2\r\n"
                      "TempMode=x\r\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config({cmt.begin(), cmt.end()});
    assert(!cfg.invalid_utf8);
    assert(cfg.title == "My Installer");
    assert(cfg.path == "tools\\");
    assert(cfg.text_lines.size() == 2 && cfg.text_lines[1] == "Second line.");
    assert(cfg.license_lines.size() == 2);
    assert(cfg.presetup.size() == 1 && cfg.presetup[0] == "prep.exe");
    assert(cfg.setup.size() == 2 && cfg.setup[0] == "setup.exe /silent");
    assert(cfg.delete_patterns.size() == 2 && cfg.delete_patterns[1] == "old\\*.log");
    assert(cfg.shortcuts.size() == 1 && cfg.shortcuts[0].name == "Tool A" &&
           cfg.shortcuts[0].folder == "DESKTOP" && cfg.shortcuts[0].icon_index == "0");
    assert(cfg.silent == sfx::SilentMode::Headless);
    assert(cfg.overwrite == sfx::OverwriteDirective::SkipExisting);
    assert(cfg.tempmode);
    assert(cfg.has_directives());
    std::cout << "[PASS] sfx parse: full feature set, BOM/CRLF, accumulation, last-wins\n";
}

static void test_parse_key_normalization_and_case() {
    std::string cmt = "SETUP=a.exe\nSetup=b.exe\nsetup=c.exe\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config({cmt.begin(), cmt.end()});
    assert(cfg.setup.size() == 3 && cfg.setup[2] == "c.exe");

    // Leading spaces on the key are tolerated; the value after the FIRST
    // '=' is verbatim (spaces kept).
    cfg = sfx::parse_sfx_config(bytes("  Path = kept "));
    assert(cfg.path == " kept ");
    std::cout << "[PASS] sfx parse: key case-folding and split-at-first-equals\n";
}

static void test_parse_unknown_keys_and_prose_count() {
    std::string cmt = "Foo=1\nBarBaz=2\nSetup=ok.exe\nNotAKey\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config({cmt.begin(), cmt.end()});
    assert(cfg.setup.size() == 1);
    assert(cfg.ignored_unknown_keys == 2); // Foo, BarBaz; "NotAKey" is prose
    std::cout << "[PASS] sfx parse: unknown keys counted, prose silent\n";
}

static void test_parse_caps() {
    // Value cap: a 5000-byte value is ignored, others survive.
    std::string big(5000, 'x');
    std::string cmt = "Text=" + big + "\nSetup=ok.exe\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config({cmt.begin(), cmt.end()});
    assert(cfg.text_lines.empty() && cfg.ignored_over_limit_lines == 1);
    assert(cfg.setup.size() == 1);

    // Line cap: 65 recognized directives => truncated, the rest ignored.
    std::string many;
    for (int i = 0; i < 65; ++i) many += "Delete=p" + std::to_string(i) + "\n";
    many += "Setup=late.exe\n";
    cfg = sfx::parse_sfx_config({many.begin(), many.end()});
    assert(cfg.truncated && cfg.delete_patterns.size() == 64);
    assert(cfg.setup.empty());

    // Shortcut field caps: >6 fields or an oversized field is a malformed line.
    // (Key present — without "Shortcut=" the line is prose and ignored.)
    std::string seven = "Shortcut=a,DESKTOP,n,d,i,0,extra\n";
    cfg = sfx::parse_sfx_config({seven.begin(), seven.end()});
    assert(cfg.shortcuts.empty() && cfg.ignored_over_limit_lines == 1);
    std::string bigfield(1025, 'y');
    std::string oversized = "Shortcut=t,f,n,d," + bigfield + ",0\n";
    cfg = sfx::parse_sfx_config({oversized.begin(), oversized.end()});
    assert(cfg.shortcuts.empty() && cfg.ignored_over_limit_lines == 1);
    std::cout << "[PASS] sfx parse: line/value/field caps degrade, never abort\n";
}

static void test_parse_silent_overwrite_values() {
    auto one = [](const char* v) {
        std::string cmt = std::string("Silent=") + v;
        return sfx::parse_sfx_config({cmt.begin(), cmt.end()}).silent;
    };
    assert(one("1") == sfx::SilentMode::HideStart);
    assert(one("2") == sfx::SilentMode::Headless);
    assert(one("3") == sfx::SilentMode::Off); // unknown value: off

    auto ow = [](const char* v) {
        std::string cmt = std::string("Overwrite=") + v;
        return sfx::parse_sfx_config({cmt.begin(), cmt.end()}).overwrite;
    };
    assert(ow("1") == sfx::OverwriteDirective::OverwriteAll);
    assert(ow("2") == sfx::OverwriteDirective::SkipExisting);
    assert(ow("9") == sfx::OverwriteDirective::Ask);
    std::cout << "[PASS] sfx parse: Silent/Overwrite value mapping\n";
}

static void test_parse_invalid_utf8_disables() {
    // Overlong '/' — invalid UTF-8 anywhere in the comment disables ALL
    // directives (plan §4): the comment is attacker-controlled and a
    // partially-parsed hostile comment must not run.
    std::string cmt = "Setup=good.exe\nText=\xc0\xaf\n";
    sfx::SfxConfig cfg = sfx::parse_sfx_config({cmt.begin(), cmt.end()});
    assert(cfg.invalid_utf8);
    assert(!cfg.has_directives());
    std::cout << "[PASS] sfx parse: invalid UTF-8 disables all directives\n";
}

// ── Reader: read_archive_comment ────────────────────────────────────────────

static void test_reader_comment_roundtrip_and_absent() {
    const fs::path arc = "build/sfx_cmt_roundtrip.rar";
    const fs::path src = "build/sfx_cmt_payload.txt";
    fs::create_directories("build");
    {
        std::ofstream ofs(src, std::ios::binary);
        ofs << "payload";
    }
    const std::string comment = "Setup=probe.exe\r\nPath=bin\\\r\n";
    std::vector<core::byte> cmt(comment.begin(), comment.end());
    std::vector<ArchiveMutator::PreparedAdd> files;
    ArchiveMutator::PreparedAdd p;
    p.entry_name = "payload.txt";
    p.src_path = src;
    assert(ArchiveMutator::prepare_add_file(src, "payload.txt", 0, "", p));
    files.push_back(std::move(p));
    assert(ArchiveMutator::write_batch_add(arc, files, /*sfx_stub=*/{}, /*password=*/"",
                                           /*encrypt_headers=*/false, {}, /*solid=*/false, cmt));

    ArchiveReader rd;
    assert(rd.open(arc));
    std::vector<core::byte> out;
    assert(rd.read_archive_comment(out));
    assert(std::string(out.begin(), out.end()) == comment);
    sfx::SfxConfig cfg = sfx::parse_sfx_config(out);
    assert(cfg.setup.size() == 1 && cfg.setup[0] == "probe.exe");
    assert(cfg.path == "bin\\");
    rd.close();

    // No comment => false, out empty.
    const fs::path plain = "build/sfx_cmt_absent.rar";
    std::vector<ArchiveMutator::PreparedAdd> files2;
    ArchiveMutator::PreparedAdd p2;
    p2.entry_name = "payload.txt";
    p2.src_path = src;
    assert(ArchiveMutator::prepare_add_file(src, "payload.txt", 0, "", p2));
    files2.push_back(std::move(p2));
    assert(ArchiveMutator::write_batch_add(plain, files2));
    ArchiveReader rd2;
    assert(rd2.open(plain));
    std::vector<core::byte> out2;
    assert(!rd2.read_archive_comment(out2) && out2.empty());
    rd2.close();
    fs::remove(arc);
    fs::remove(plain);
    fs::remove(src);
    std::cout << "[PASS] reader comment: roundtrip + absent\n";
}

static void test_reader_comment_size_cap() {
    // A 2 MiB comment exceeds the 1 MiB decode cap: read_archive_comment
    // returns false (directives disabled) — and extraction of the archive's
    // real entries is unaffected.
    const fs::path arc = "build/sfx_cmt_oversize.rar";
    const fs::path src = "build/sfx_cmt_big_payload.txt";
    fs::create_directories("build");
    {
        std::ofstream ofs(src, std::ios::binary);
        ofs << "payload";
    }
    std::vector<core::byte> big(2u * 1024 * 1024, core::byte(0x41));
    std::vector<ArchiveMutator::PreparedAdd> files;
    ArchiveMutator::PreparedAdd p;
    p.entry_name = "payload.txt";
    p.src_path = src;
    assert(ArchiveMutator::prepare_add_file(src, "payload.txt", 0, "", p));
    files.push_back(std::move(p));
    assert(ArchiveMutator::write_batch_add(arc, files, /*sfx_stub=*/{}, /*password=*/"",
                                           /*encrypt_headers=*/false, {}, /*solid=*/false, big));

    ArchiveReader rd;
    assert(rd.open(arc));
    std::vector<core::byte> out;
    assert(!rd.read_archive_comment(out) && out.empty());
    rd.close();
    fs::remove(arc);
    fs::remove(src);
    std::cout << "[PASS] reader comment: 1 MiB decode cap disables directives\n";
}

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    // abort() must print to stderr, never pop the modal "abort() has been
    // called" dialog — under ctest a dialog silently hangs the test forever
    // (same class of failure the assert routing above exists to prevent).
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    try {
        std::cout << "Running v1.23.0 SFX M1 unit tests...\n";
        test_parse_empty_and_prose();
        std::cout << std::flush;
        test_parse_full_feature();
        std::cout << std::flush;
        test_parse_key_normalization_and_case();
        std::cout << std::flush;
        test_parse_unknown_keys_and_prose_count();
        std::cout << std::flush;
        test_parse_caps();
        std::cout << std::flush;
        test_parse_silent_overwrite_values();
        std::cout << std::flush;
        test_parse_invalid_utf8_disables();
        std::cout << std::flush;
        test_reader_comment_roundtrip_and_absent();
        std::cout << std::flush;
        test_reader_comment_size_cap();
        std::cout << std::flush;
        std::cout << "All SFX M1 unit tests PASSED!\n";
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[sfx-tests] exception: %s\n", ex.what());
        std::fflush(stderr);
        return 2;
    } catch (...) {
        std::fprintf(stderr, "[sfx-tests] unknown exception\n");
        std::fflush(stderr);
        return 2;
    }
    return 0;
}
