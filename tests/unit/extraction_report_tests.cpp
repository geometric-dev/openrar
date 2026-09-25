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
    assert(out.open(arc, io::FileMode::CreateAlways));
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
        assert(format::HeaderWriter::write_file_block(out, fb));
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
    assert(json.find("\"schema_version\":1") != std::string::npos);
    assert(json.find("\"status\":\"pending_placeholder\"") == std::string::npos);
    // the backslash and quote in the archive name are JSON-escaped
    assert(json.find("a\\\\\\\"rar") != std::string::npos);
    assert(json.find("\"reason\":null") != std::string::npos ||
           json.find("\"reason\":\"") != std::string::npos);
    // finalize: pending -> unprocessed
    r.finalize_pending();
    assert(r.entries.back().status == "unprocessed");
    assert(r.entries.back().reason == "extraction aborted");
    // control characters are \u-escaped (the "\x01" "b" split stops the
    // hex escape from swallowing the 'b')
    archive::ExtractionReport r2;
    r2.entries.push_back({std::string("a\x01"
                                      "b"),
                          "extracted",
                          "",
                          {}});
    assert(archive::to_json(r2).find("\\u0001") != std::string::npos);
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
    assert(rc == 0);
#else
    assert(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
#endif
    const std::string stdout_txt = read_text(dir / "stdout.txt");
    const std::string stderr_txt = read_text(dir / "stderr.txt");
    // stdout is exactly one JSON object
    assert(stdout_txt.find("{\"schema_version\":1") == 0);
    assert(stdout_txt.find("\"exit_code\":0") != std::string::npos);
    assert(stdout_txt.find("\"name\":\"fine.txt\"") != std::string::npos);
    assert(stdout_txt.find("\"status\":\"extracted\"") != std::string::npos);
    assert(stdout_txt.find("Extracting") == std::string::npos);
    // stderr carries the human output
    assert(stderr_txt.find("Extracting from") != std::string::npos);
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
    assert(rc == 0);
#else
    assert(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
#endif
    const std::string json = read_text(dir / "stdout.txt");
    // Win32 strips trailing dots/spaces at open time: the sanitizer trimmed
    // them, so the on-disk name is "report" and the JSON says "report".
    assert(json.find("\"name\":\"report\"") != std::string::npos);
    assert(json.find("report. . .") == std::string::npos);
    std::error_code ec;
    assert(fs::exists(out / "report", ec));
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
    assert(rc == 0);
#else
    assert(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
#endif
    const std::string json = read_text(dir / "stdout.txt");
    assert(json.find("\"name\":\"bad%FF%FE.txt\"") != std::string::npos);
    assert(json.find("name_escaped") != std::string::npos);
    // the escaped name IS the filename on disk
    std::error_code ec;
    bool found = false;
    for (const auto& e : fs::directory_iterator(out)) {
        if (e.path().filename().string() == "bad%FF%FE.txt") found = true;
    }
    assert(found);
    // reversibility: decode the escapes back to the original bytes
    assert(io::is_valid_utf8("plain.txt"));
    assert(!io::is_valid_utf8(raw_name));
    assert(io::percent_encode_invalid_utf8("ok.txt") == "ok.txt");
    std::cout << "[PASS] percent_encoded_undecodable: %XX name on disk + flag\n";
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
    test_report_model();
    test_json_stdout_purity();
    test_displayed_equals_extracted();
    test_percent_encoded_undecodable();
    std::cout << "All extraction_report_tests passed.\n";
    return 0;
}
