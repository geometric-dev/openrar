// v1.23.0 SFX directive-engine unit tests (M1): the directive grammar and
// SfxConfig caps (docs/sfx-v1.23-implementation-plan.md §4) and the
// ArchiveReader::read_archive_comment accessor (§3). Sandbox e2e suites
// (gate 2) build on these in M6.

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/core/types.hpp"
#include "../../src/sfx/sfx_config.hpp"
#include "../../src/sfx/sfx_consent.hpp"
#include "../../src/sfx/prompt_console.hpp"
#include "../../src/sfx/process_exec.hpp"
#include "../../src/sfx/sfx_pipeline.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
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


// ── M2: consent engine (the single decision function) ───────────────────────

namespace {
class FakeBackend final : public sfx::IPromptBackend {
public:
    explicit FakeBackend(bool interactive, std::vector<sfx::ConsentAnswer> script,
                         sfx::ConsentAnswer default_answer = sfx::ConsentAnswer::Deny)
        : interactive_(interactive), script_(std::move(script)), default_(default_answer) {}
    bool interactive() const override { return interactive_; }
    sfx::ConsentAnswer ask(const sfx::ConsentRequest& req) override {
        asks_.push_back(req);
        if (cursor_ >= script_.size()) return default_; // exhausted: scripted default
        return script_[cursor_++];
    }
    size_t asks() const { return asks_.size(); }

private:
    bool interactive_;
    std::vector<sfx::ConsentAnswer> script_;
    std::vector<sfx::ConsentRequest> asks_;
    sfx::ConsentAnswer default_;
    size_t cursor_{0};
};

sfx::ConsentRequest make_req(sfx::DirectiveType type) {
    sfx::ConsentRequest r;
    r.type = type;
    r.verb = "run a probe";
    r.command = "probe.exe";
    r.resolved_path = "C:/dest/probe.exe";
    return r;
}
} // namespace

void test_consent_engine_basic_and_batch() {
    std::cout << "[+] test_consent_engine_basic_and_batch" << std::endl;

    {
        FakeBackend be(true, {sfx::ConsentAnswer::Run, sfx::ConsentAnswer::Deny});
        sfx::SfxConsentEngine eng(be);
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Run);
        assert(eng.ask(make_req(sfx::DirectiveType::Delete)) == sfx::ConsentDecision::Deny);
        assert(eng.prompts_used() == 2 && !eng.aborted());
    }

    // RunAll batch: same type runs without prompting; other types still prompt.
    {
        FakeBackend be(true, {sfx::ConsentAnswer::RunAll});
        sfx::SfxConsentEngine eng(be);
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Run);
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Run);
        assert(eng.prompts_used() == 1);
        assert(eng.ask(make_req(sfx::DirectiveType::Delete)) == sfx::ConsentDecision::Deny);
        assert(eng.prompts_used() == 2);
    }

    // DenyAll batch mirrors it.
    {
        FakeBackend be(true, {sfx::ConsentAnswer::DenyAll});
        sfx::SfxConsentEngine eng(be);
        assert(eng.ask(make_req(sfx::DirectiveType::Presetup)) == sfx::ConsentDecision::Deny);
        assert(eng.ask(make_req(sfx::DirectiveType::Presetup)) == sfx::ConsentDecision::Deny);
        assert(eng.prompts_used() == 1);
    }

    // Abort latches: every later ask denied, aborted() true, no new prompts.
    {
        FakeBackend be(true, {sfx::ConsentAnswer::Abort});
        sfx::SfxConsentEngine eng(be);
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Deny);
        assert(eng.aborted());
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Deny);
        assert(eng.prompts_used() == 1);
    }
    std::cout << "[PASS] consent engine: run/deny, per-type batches, abort latch" << std::endl;
}

void test_consent_engine_cap_and_noninteractive() {
    std::cout << "[+] test_consent_engine_cap_and_noninteractive" << std::endl;

    // Prompt cap: the 9th ask (cap 8) is denied without prompting and latches
    // aborted (plan §6 hard cap -> abort the run).
    {
        FakeBackend be(true, {}, sfx::ConsentAnswer::Run); // always willing
        sfx::SfxConsentEngine eng(be, /*prompt_cap=*/8);
        for (int i = 0; i < 8; ++i) {
            assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Run);
        }
        assert(eng.prompts_used() == 8);
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Deny);
        assert(eng.aborted());
        assert(eng.prompts_used() == 8);
    }

    // Cap latches the run even for previously batch-approved types.
    {
        FakeBackend be(true, {sfx::ConsentAnswer::RunAll});
        sfx::SfxConsentEngine eng(be, /*prompt_cap=*/1);
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Run);
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Run);
        assert(eng.ask(make_req(sfx::DirectiveType::Delete)) == sfx::ConsentDecision::Deny);
        assert(eng.aborted());
    }

    // Non-interactive backend: every ask denied WITHOUT calling the backend.
    {
        FakeBackend be(false, {});
        sfx::SfxConsentEngine eng(be);
        assert(eng.ask(make_req(sfx::DirectiveType::Setup)) == sfx::ConsentDecision::Deny);
        assert(eng.prompts_used() == 0 && !eng.aborted() && be.asks() == 0);
    }

    // Console backend interactivity (isatty(stdin)) is covered by the
    // sandbox e2e suite (plan §13 test 18: piped stdin => deny) — a unit
    // test cannot assume its own stdin is or is not a TTY.
    std::cout << "[PASS] consent engine: prompt cap aborts, non-interactive denies" << std::endl;
}

// ── M3: process-execution contract ──────────────────────────────────────────

static std::string g_self_exe; // set in main; self-spawn probe target

static void test_process_exec_resolution() {
    std::cout << "[+] test_process_exec_resolution" << std::endl;
    const fs::path dest = "build/sfx_exec_dest";
    fs::create_directories(dest);
    const std::string probe_name =
#ifdef _WIN32
        "probe.exe";
#else
        "probe";
#endif
    {
        std::ofstream ofs(dest / probe_name, std::ios::binary);
        ofs << "x";
    }
#ifndef _WIN32
    std::string chmod_cmd = "chmod +x \"" + (dest / probe_name).string() + "\"";
    assert(std::system(chmod_cmd.c_str()) == 0);
#endif

    // Destination-relative resolution (allow_dest=true).
    const std::string found =
        sfx::resolve_command_executable(probe_name, dest.string(), /*allow_dest=*/true);
    assert(!found.empty() && found.find(probe_name) != std::string::npos);

    // Disallowed destination + unknown name => not found.
    assert(
        sfx::resolve_command_executable(probe_name, dest.string(), /*allow_dest=*/false).empty());
    assert(sfx::resolve_command_executable("definitely_missing_sfx_probe.exe", dest.string(), true)
               .empty());

    // Absolute path passes through.
    const std::string abs = "\"" + g_self_exe + "\"";
    assert(!sfx::resolve_command_executable(abs, dest.string(), /*allow_dest=*/false).empty());
    fs::remove_all(dest);
    std::cout << "[PASS] process exec: destination/PATH/absolute resolution" << std::endl;
}

// Builds the invocation for a self-probe child. Cross-compiled legs route
// the child through the emulator (raw target ELFs cannot exec on the host
// kernel); native legs exec directly.
static void probe_invocation(const std::string& probe_args, std::string& command,
                             std::string& resolved_exe) {
#ifdef OPENRAR_SFX_EMULATOR
    command = std::string(OPENRAR_SFX_EMULATOR) + " \"" + g_self_exe + "\" " + probe_args;
    resolved_exe = command.substr(0, command.find(' ')); // the emulator binary
#else
    command = "\"" + g_self_exe + "\" " + probe_args;
    resolved_exe = sfx::resolve_command_executable(command, "", false);
#endif
}

static void test_process_exec_spawn_exit_code() {
    std::cout << "[+] test_process_exec_spawn_exit_code" << std::endl;
    std::string cmd, exe;
    probe_invocation("--sfx-probe-exit 7", cmd, exe);
    assert(!exe.empty());
    sfx::ExecResult r = sfx::spawn_contained(cmd, exe, "", {});
    assert(r.spawned && r.contained && !r.cancelled);
    assert(r.exit_code == 7);

    probe_invocation("--sfx-probe-exit 0", cmd, exe);
    r = sfx::spawn_contained(cmd, exe, "", {});
    assert(r.spawned && r.exit_code == 0);

    // AMSI: the contract is "functions and reports a verdict" — clean OR
    // flagged are both valid engine outcomes for a benign string; a fail-open
    // (failed=true) is the documented behavior when AMSI is unavailable.
    bool failed = false;
    const bool clean = sfx::amsi_scan_command(cmd, failed);
    std::cout << "  amsi verdict: " << (failed ? "fail-open" : (clean ? "clean" : "flagged"))
              << std::endl;
    std::cout << "[PASS] process exec: contained spawn + exit-code propagation" << std::endl;
}

static void test_process_exec_cancellation() {
    std::cout << "[+] test_process_exec_cancellation" << std::endl;
    std::string cmd, exe;
    probe_invocation("--sfx-probe-sleep 30", cmd, exe);
    assert(!exe.empty());
    const auto t0 = std::chrono::steady_clock::now();
    const auto start = t0;
    sfx::ExecResult r = sfx::spawn_contained(cmd, exe, "", [&start]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count() > 200;
    });
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    assert(r.spawned && r.contained && r.cancelled);
    assert(elapsed < 5.0 && "cancellation must kill the contained tree promptly");
    std::cout << "[PASS] process exec: cancellation kills the contained tree" << std::endl;
}

int main(int argc, char* argv[]) {
    // Self-probe mode: the M3 spawn tests use this executable as the child
    // (no shell, no external fixture binaries).
    if (argc >= 3 && std::strcmp(argv[1], "--sfx-probe-exit") == 0) {
        return std::atoi(argv[2]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--sfx-probe-sleep") == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[2])));
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--sfx-probe-touch") == 0) {
        std::ofstream ofs(argv[2], std::ios::binary);
        ofs << "touched";
        return 0;
    }
    if (argc > 0 && argv[0] != nullptr) g_self_exe = argv[0];
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
        test_consent_engine_basic_and_batch();
        std::cout << std::flush;
        test_consent_engine_cap_and_noninteractive();
        std::cout << std::flush;
        test_process_exec_resolution();
        std::cout << std::flush;
        test_process_exec_spawn_exit_code();
        std::cout << std::flush;
        test_process_exec_cancellation();
        std::cout << std::flush;
        std::cout << "All SFX M1+M2+M3 unit tests PASSED!\n";
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
