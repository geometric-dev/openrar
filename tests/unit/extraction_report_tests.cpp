// Extraction report + encoding invariant tests (v1.24.0 M4, plan §4/§5).
//
// Named tests from the plan that land here:
//   json_stdout_purity           (plan §10 test 15)
//   displayed_equals_extracted   (plan §10 test 16)
//   percent_encoded_undecodable  (plan §10 test 17)

#include "../../src/archive/extraction_report.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/io/path_util.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <doctest.h>
#include "../support/test_runner.hpp"

#ifndef OPENRAR_CLI_EXE
#define OPENRAR_CLI_EXE ""
#endif

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

namespace {

namespace fs = std::filesystem;

fs::path make_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("openrar_m4_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void rm(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string cli_path() {
    // Cross-arch runs (CI's QEMU leg) cannot exec the target binary directly
    // from the host kernel; OPENRAR_RUNNER prefixes every invocation (same
    // contract as cli_tests).
    const char* runner = std::getenv("OPENRAR_RUNNER");
    const std::string prefix = (runner && *runner) ? std::string(runner) + " " : "";
#ifdef OPENRAR_CLI_EXE
    if (fs::exists(OPENRAR_CLI_EXE)) return prefix + fs::canonical(OPENRAR_CLI_EXE).string();
#endif
    return prefix + "openrar.exe";
}

// Raw-writes a stored archive with the given entry names (bypasses the
// mutator's same-name replace so arbitrary colliding/odd names land).
void write_raw_archive(const fs::path& arc,
                       const std::vector<std::pair<std::string, std::string>>& entries) {
    io::FileStream out;
    REQUIRE(out.open(arc, io::FileMode::CreateAlways));
    format::HeaderWriter::write_signature(out);
    format::MainBlock mb;
    format::HeaderWriter::write_main_block(out, mb);
    for (const auto& [name, content] : entries) {
        format::FileBlock fb;
        fb.file_name = name;
        fb.unp_size = content.size();
        fb.pack_size = content.size();
        fb.method = 0;
        fb.win_size = 0;
        REQUIRE(format::HeaderWriter::write_file_block(out, fb));
        out.write(content.data(), content.size());
    }
    format::EndArcBlock eb;
    format::HeaderWriter::write_end_block(out, eb);
}

// ── Report model ─────────────────────────────────────────────────────────────

void test_report_model() {
    using archive::ExtractionReport;
    ExtractionReport r;
    r.archive = "a\\\"rar\".rar"; // needs escaping
    r.entries.push_back({"ok.txt", "extracted", "", {}});
    r.entries.push_back({"skip.txt", "skipped", "user declined overwrite", {}});
    r.entries.push_back({"bad.txt", "failed", "extraction failed", {"name_escaped"}});
    r.entries.push_back({"pending.txt", "", "", {}});
    r.aborted = true;
    r.abort_reason = "user break";
    const std::string json = archive::to_json(r);
    // v2 (v1.29 D3): schema bumped for the additive transcode fields.
    REQUIRE(json.find("\"schema_version\":2") != std::string::npos);
    // v1 documents (x/e): the transcode fields are absent.
    REQUIRE(json.find("\"format\":") == std::string::npos);
    REQUIRE(json.find("\"status\":\"pending_placeholder\"") == std::string::npos);
    // the backslash and quote in the archive name are JSON-escaped
    REQUIRE(json.find("a\\\\\\\"rar") != std::string::npos);
    REQUIRE((json.find("\"reason\":null") != std::string::npos ||
             json.find("\"reason\":\"") != std::string::npos));
    // finalize: pending -> unprocessed
    r.finalize_pending();
    REQUIRE(r.entries.back().status == "unprocessed");
    REQUIRE(r.entries.back().reason == "extraction aborted");
    // control characters are \u-escaped (the "\x01" "b" split stops the
    // hex escape from swallowing the 'b')
    archive::ExtractionReport r2;
    r2.entries.push_back({std::string("a\x01"
                                      "b"),
                          "extracted",
                          "",
                          {}});
    REQUIRE(archive::to_json(r2).find("\\u0001") != std::string::npos);
    std::cout << "[PASS] extraction report model + JSON escaping\n";
}

// ── CLI purity + invariants ──────────────────────────────────────────────────

void test_json_stdout_purity() {
    // Plan §10 test 15: with --json-summary (no path), stdout carries ONLY
    // valid JSON; every human-readable byte lands on stderr.
    const fs::path dir = make_dir("purity");
    const fs::path arc = dir / "g2.rar";
    write_raw_archive(arc, {{"fine.txt", "content"}, {"skip. . ", "trimmed name"}});
    const fs::path out = dir / "out";
    // path joins (not backslash string concatenation): the backslash form is
    // a literal-backslash filename on POSIX and breaks the redirects there.
    const std::string stdout_redir = (dir / "stdout.txt").string();
    const std::string stderr_redir = (dir / "stderr.txt").string();
    const std::string cmd = cli_path() + " x " + arc.string() + " " + out.string() +
                            " --json-summary > \"" + stdout_redir + "\" 2> \"" + stderr_redir +
                            "\"";
    const int rc = std::system(cmd.c_str());
#ifdef _WIN32
    REQUIRE(rc == 0);
#else
    REQUIRE((WIFEXITED(rc) && WEXITSTATUS(rc) == 0));
#endif
    const std::string stdout_txt = read_text(dir / "stdout.txt");
    const std::string stderr_txt = read_text(dir / "stderr.txt");
    // stdout is exactly one JSON object
    REQUIRE(stdout_txt.find("{\"schema_version\":2") == 0);
    REQUIRE(stdout_txt.find("\"exit_code\":0") != std::string::npos);
    REQUIRE(stdout_txt.find("\"name\":\"fine.txt\"") != std::string::npos);
    REQUIRE(stdout_txt.find("\"status\":\"extracted\"") != std::string::npos);
    REQUIRE(stdout_txt.find("Extracting") == std::string::npos);
    // stderr carries the human output
    REQUIRE(stderr_txt.find("Extracting from") != std::string::npos);
    std::cout << "[PASS] json_stdout_purity: stdout is JSON-only, humans on stderr\n";
    rm(dir);
}

void test_displayed_equals_extracted() {
    // Plan §10 test 16: trailing-dot/space trimming happens before BOTH the
    // UI and path evaluation — the JSON entry name equals the on-disk name.
    const fs::path dir = make_dir("disp");
    const fs::path arc = dir / "g2.rar";
    write_raw_archive(arc, {{"report. . .", "content"}});
    const fs::path out = dir / "out";
    const std::string cmd = cli_path() + " x " + arc.string() + " " + out.string() +
                            " --json-summary > \"" + (dir / "stdout.txt").string() + "\" 2> \"" +
                            (dir / "stderr.txt").string() + "\"";
    const int rc = std::system(cmd.c_str());
#ifdef _WIN32
    REQUIRE(rc == 0);
#else
    REQUIRE((WIFEXITED(rc) && WEXITSTATUS(rc) == 0));
#endif
    const std::string json = read_text(dir / "stdout.txt");
    // Win32 strips trailing dots/spaces at open time: the sanitizer trimmed
    // them, so the on-disk name is "report" and the JSON says "report".
    REQUIRE(json.find("\"name\":\"report\"") != std::string::npos);
    REQUIRE(json.find("report. . .") == std::string::npos);
    std::error_code ec;
    REQUIRE(fs::exists(out / "report", ec));
    std::cout << "[PASS] displayed_equals_extracted: trimmed name is the only name\n";
    rm(dir);
}

void test_percent_encoded_undecodable() {
    // Plan §10 test 17: an invalid-UTF-8 name extracts under the lossless
    // %XX escape; the escaped name IS the file name and carries the
    // name_escaped flag in the JSON summary.
    const fs::path dir = make_dir("pct");
    const fs::path arc = dir / "g2.rar";
    // name "bad\xFF\xFE.txt" — two invalid bytes
    const std::string raw_name = std::string("bad\xFF\xFE.txt", 9);
    write_raw_archive(arc, {{raw_name, "content"}});
    const fs::path out = dir / "out";
    const std::string cmd = cli_path() + " x " + arc.string() + " " + out.string() +
                            " --json-summary > \"" + (dir / "stdout.txt").string() + "\" 2> \"" +
                            (dir / "stderr.txt").string() + "\"";
    const int rc = std::system(cmd.c_str());
#ifdef _WIN32
    REQUIRE(rc == 0);
#else
    REQUIRE((WIFEXITED(rc) && WEXITSTATUS(rc) == 0));
#endif
    const std::string json = read_text(dir / "stdout.txt");
    REQUIRE(json.find("\"name\":\"bad%FF%FE.txt\"") != std::string::npos);
    REQUIRE(json.find("name_escaped") != std::string::npos);
    // the escaped name IS the filename on disk
    std::error_code ec;
    bool found = false;
    for (const auto& e : fs::directory_iterator(out)) {
        if (e.path().filename().string() == "bad%FF%FE.txt") found = true;
    }
    REQUIRE(found);
    // reversibility: decode the escapes back to the original bytes
    REQUIRE(io::is_valid_utf8("plain.txt"));
    REQUIRE(!io::is_valid_utf8(raw_name));
    REQUIRE(io::percent_encode_invalid_utf8("ok.txt") == "ok.txt");
    std::cout << "[PASS] percent_encoded_undecodable: %XX name on disk + flag\n";
    rm(dir);
}

// Regression: percent_encode_invalid_utf8 over-read the buffer (and copied the
// out-of-bounds bytes into the produced name) when a string ENDED in a
// truncated multi-byte sequence. A lone "\xF0" set len=4 but left one byte, so
// the escape loop indexed s[1..3] past size(). The bug was reachable from any
// archive entry name via sanitize_archive_path.
void test_percent_encode_truncated() {
    using io::percent_encode_invalid_utf8;
    // 4-byte lead, nothing after it.
    REQUIRE(percent_encode_invalid_utf8("\xF0") == "%F0");
    // 3-byte lead, nothing after it.
    REQUIRE(percent_encode_invalid_utf8("\xE2") == "%E2");
    // 3-byte lead with one continuation, still short of three.
    REQUIRE(percent_encode_invalid_utf8("\xE2\x82") == "%E2%82");
    // 4-byte lead with two continuations, short of four.
    REQUIRE(percent_encode_invalid_utf8("\xF0\x9F\x98") == "%F0%9F%98");
    // Truncated sequence not at the very start.
    REQUIRE(percent_encode_invalid_utf8("a\xF0") == "a%F0");
    REQUIRE(percent_encode_invalid_utf8("dir/\xE2") == "dir/%E2");
    // Well-formed sequences are still passed through untouched.
    REQUIRE(percent_encode_invalid_utf8("\xC3\xA9") == "\xC3\xA9");         // é (2-byte)
    REQUIRE(percent_encode_invalid_utf8("\xE2\x82\xAC") == "\xE2\x82\xAC"); // € (3-byte)
    REQUIRE(percent_encode_invalid_utf8("\xF0\x9F\x98\x80") ==
            "\xF0\x9F\x98\x80"); // emoji (4-byte)
    // A truncated lead still *claims* `len` bytes, matching the escaping
    // semantics used for a fully-present-but-invalid sequence, so the claimed
    // run is escaped as far as the buffer allows and no further. The fix is
    // that the read stops at end-of-string; it does not change how many bytes
    // a broken lead claims.
    REQUIRE(percent_encode_invalid_utf8("\xF0\xC3\xA9") == "%F0%C3%A9");
    std::cout << "[PASS] percent_encode_truncated: no OOB read on truncated lead\n";
}

} // namespace

static const openrar::test::Case k_cases[] = {
    {"test_report_model", test_report_model, false},
    {"test_json_stdout_purity", test_json_stdout_purity, false},
    {"test_displayed_equals_extracted", test_displayed_equals_extracted, false},
    {"test_percent_encoded_undecodable", test_percent_encoded_undecodable, false},
    {"test_percent_encode_truncated", test_percent_encode_truncated, false}};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    int rc =
        openrar::test::openrar_test_main(k_cases, sizeof(k_cases) / sizeof(k_cases[0]), argc, argv);
    if (rc == 0 && !openrar::test::last_run_was_filtered())
        std::cout << "All extraction_report_tests passed.\n";
    return rc;
}
