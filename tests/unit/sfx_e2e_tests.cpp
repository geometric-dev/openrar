// v1.23.0 sandbox e2e suite (blocking gate 2, plan §13): runs the REAL
// Default.SFX stub against archives whose comment carries directives, and
// asserts the consent/containment/suppression contract end to end.
//
// Mechanics: the e2e binary converts a directive archive into an SFX module
// via ArchiveMutator::convert_to_sfx (stub prepended), executes the stub with
// scripted stdin and the OPENRAR_SFX_FORCE_INTERACTIVE automation hook, and
// verifies behavior through sentinel files written by the probe directives
// (the probe is this executable itself — no external fixtures).
//
// Quoting note: std::system routes through cmd /c, which strips the first AND
// last quote of a line carrying more than two — all scratch paths below are
// asserted space-free and passed UNQUOTED (cli_tests documents this trap).

#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/core/types.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <sys/wait.h>
#endif
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

static fs::path g_self_exe;
static fs::path g_stub_exe;
static fs::path g_work;

static std::vector<core::byte> bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

static void write_file(const fs::path& p, const std::string& content) {
    std::ofstream ofs(p, std::ios::binary);
    ofs << content;
}

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void set_env(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

struct StubRun {
    int exit_code;
    std::string output;
};

// Cross-compiled legs: raw target ELFs cannot exec on the host kernel, so
// both the stub launch and the directive command carry the emulator prefix.
#ifdef OPENRAR_SFX_EMULATOR
static const std::string g_emu = OPENRAR_SFX_EMULATOR " ";
#else
static const std::string g_emu;
#endif

static std::string stub_cmd(const fs::path& sfx, const std::string& args) {
    return g_emu + sfx.string() + " " + args;
}

static std::string probe_cmd_for(const fs::path& sentinel) {
    return g_emu + "\"" + g_self_exe.string() + "\" --sfx-probe-touch \"" + sentinel.string() +
           "\"";
}

static std::string probe_exit_cmd() {
    return g_emu + "\"" + g_self_exe.string() + "\" --sfx-probe-exit 0";
}

static StubRun run_stub(const fs::path& sfx, const std::string& args,
                        const std::string& stdin_data) {
    const fs::path in_file = g_work / "stdin.txt";
    const fs::path out_file = g_work / "stdout.txt";
    write_file(in_file, stdin_data);
    set_env("OPENRAR_SFX_FORCE_INTERACTIVE", "1");
    // cmd /c strips the first AND last quote when a line carries more than
    // two: everything here must be space-free and unquoted.
    assert(sfx.string().find(' ') == std::string::npos);
    assert(args.find('"') == std::string::npos); // quotes get cmd-stripped
    assert(in_file.string().find(' ') == std::string::npos);
    assert(out_file.string().find(' ') == std::string::npos);
    std::string cmd =
        stub_cmd(sfx, args) + " < " + in_file.string() + " > " + out_file.string() + " 2>&1";
    int rc = std::system(cmd.c_str());
#ifndef _WIN32
    // POSIX std::system returns the raw waitstatus: extract the exit code.
    if (WIFEXITED(rc))
        rc = WEXITSTATUS(rc);
    else if (WIFSIGNALED(rc))
        rc = 128 + WTERMSIG(rc);
#endif
    return {rc, slurp(out_file)};
}

static fs::path make_sfx(const std::string& directives, const std::string& name) {
    const fs::path arc = g_work / (name + ".rar");
    const fs::path payload = g_work / (name + "_payload.txt");
    write_file(payload, "sfx e2e payload");
    std::vector<ArchiveMutator::PreparedAdd> files;
    ArchiveMutator::PreparedAdd p;
    p.entry_name = name + ".txt";
    p.src_path = payload;
    assert(ArchiveMutator::prepare_add_file(payload, p.entry_name, 0, "", p));
    files.push_back(std::move(p));
    assert(ArchiveMutator::write_batch_add(arc, files, /*sfx_stub=*/{}, /*password=*/"",
                                           /*encrypt_headers=*/false, {}, /*solid=*/false,
                                           bytes(directives)));
    std::string err;
    assert(ArchiveMutator::convert_to_sfx(arc, g_stub_exe, err));
    fs::path sfx = arc;
    sfx.replace_extension(".exe"); // convert_to_sfx derives the output path
    assert(fs::exists(sfx));
    fs::remove(arc);
    fs::remove(payload);
    return sfx;
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

static void test_e2e_consent_run_and_deny() {
    std::cout << "[+] test_e2e_consent_run_and_deny" << std::endl;
    const fs::path sentinel = g_work / "run_sentinel.txt";
    fs::remove(sentinel);
    const fs::path sfx = make_sfx("Setup=" + probe_cmd_for(sentinel) + "\n", "e2e_run");

    StubRun r = run_stub(sfx, "-ddest_run", "R\n");
    if (r.exit_code != 0) {
        std::fprintf(stderr, "[e2e-diag] exit=%d output:\n%s\n", r.exit_code, r.output.c_str());
        std::fflush(stderr);
    }
    assert(r.exit_code == 0);
    assert(fs::exists(sentinel));
    assert(contains(r.output, "executed"));
    fs::remove(sentinel);

    // Default focus: empty line denies (Don't Run) — sentinel absent, exit 0.
    r = run_stub(sfx, "-ddest_deny", "\n");
    assert(r.exit_code == 0);
    assert(!fs::exists(sentinel));
    assert(contains(r.output, "skipped-consent"));
    std::cout << "[PASS] e2e: consent run + Don't-Run default focus" << std::endl;
}

static void test_e2e_noexec_suppresses() {
    std::cout << "[+] test_e2e_noexec_suppresses" << std::endl;
    const fs::path sentinel = g_work / "noexec_sentinel.txt";
    fs::remove(sentinel);
    const fs::path sfx = make_sfx("Setup=" + probe_cmd_for(sentinel) + "\n", "e2e_noexec");

    StubRun r = run_stub(sfx, "-sfxnoexec -ddest_noexec", "R\n");
    assert(r.exit_code == 0);
    assert(!fs::exists(sentinel));            // suppressed even with scripted consent
    assert(contains(r.output, "suppressed")); // observable, never silent
    std::cout << "[PASS] e2e: -sfxnoexec suppresses directives and reports" << std::endl;
}

static void test_e2e_prompt_cap_aborts() {
    std::cout << "[+] test_e2e_prompt_cap_aborts" << std::endl;
    std::string cmt;
    for (int i = 0; i < 9; ++i) cmt += "Setup=" + probe_exit_cmd() + "\n";
    const fs::path sfx = make_sfx(cmt, "e2e_cap");

    std::string stdin_data;
    for (int i = 0; i < 9; ++i) stdin_data += "R\n";
    StubRun r = run_stub(sfx, "-ddest_cap", stdin_data);
    assert(r.exit_code == 2); // hostile-archive abort per §8
    std::cout << "[PASS] e2e: prompt cap aborts the run (exit 2)" << std::endl;
}

static void test_e2e_no_directives_plain_extraction() {
    std::cout << "[+] test_e2e_no_directives_plain_extraction" << std::endl;
    const fs::path sfx = make_sfx("", "e2e_plain");
    // Absolute destination: relative -d resolves against the stub's CWD,
    // which is the test binary's directory, not g_work.
    const fs::path dest = g_work / "dest_plain";
    StubRun r = run_stub(sfx, "-d" + dest.string(), "");
    assert(r.exit_code == 0);
    assert(fs::exists(dest / "e2e_plain.txt"));
    assert(!contains(r.output, "[sfx]")); // no directive machinery output
    std::cout << "[PASS] e2e: directive-free archive extracts untouched" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc >= 3 && std::strcmp(argv[1], "--sfx-probe-touch") == 0) {
        std::ofstream ofs(argv[2], std::ios::binary);
        ofs << "touched";
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--sfx-probe-exit") == 0) {
        return std::atoi(argv[2]);
    }

    // Locate the Default.SFX stub: ctest provides OPENRAR_SFX_STUB (a
    // $<TARGET_FILE> generator expression in CMakeLists); the argv[0] dir is
    // the fallback for direct runs.
    g_self_exe = fs::absolute(argv[0]);
    fs::path stub;
    const char* stub_env = getenv("OPENRAR_SFX_STUB");
    if (stub_env && *stub_env) {
        stub = stub_env;
    } else {
        stub = g_self_exe.parent_path() / "Default.SFX";
#ifdef _WIN32
        stub += ".exe";
#endif
    }
    if (!fs::exists(stub)) {
        std::cout << "[SKIP] sfx e2e: Default.SFX stub not found at " << stub.string()
                  << " (build it first)\n";
        return 0;
    }
    g_stub_exe = stub;
    g_work = g_self_exe.parent_path() / "sfx_e2e_work";
    fs::remove_all(g_work);
    fs::create_directories(g_work);

    std::cout << "Running v1.23.0 SFX sandbox e2e suite...\n";
    test_e2e_consent_run_and_deny();
    test_e2e_noexec_suppresses();
    test_e2e_prompt_cap_aborts();
    test_e2e_no_directives_plain_extraction();
    fs::remove_all(g_work);
    std::cout << "All SFX sandbox e2e tests PASSED!\n";
    return 0;
}
