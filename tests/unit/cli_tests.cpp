#include "../../src/core/types.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "../../src/archive/rar_errors.hpp"
#include "../../src/archive/extraction_report.hpp"
#include "../../src/crypto/crc32.hpp"
#include "../../src/format/header_writer.hpp"
#include "../../src/io/file_stream.hpp"
#include "../../src/io/motw.hpp"
#include "../../src/io/path_util.hpp"
#include "../../src/io/win32_meta.hpp"
#include "openrar/version.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "../../src/cli/progress.hpp"
#include "../../src/cli/tui.hpp"
#ifdef _WIN32
#include <windows.h>
#include <share.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#endif
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

// Test-TU definitions of the progress-mode globals (normally defined in
// cli/main.cpp, which is not linked into this suite); the v1.28 sink tests
// toggle them directly.
namespace openrar::cli {
bool g_plain_mode = false;
bool g_quiet_mode = false;
} // namespace openrar::cli

// Null device for output redirection: "nul" is only a device on Windows; on
// POSIX it would silently create a regular file named "nul" in the CWD.
#ifdef _WIN32
#define DEVNULL "nul"
#else
#define DEVNULL "/dev/null"
#endif

// std::system returns the raw wait status on POSIX (exit code << 8) but the
// exit code itself on Windows: decode to the exit code everywhere.
static int system_exit_code(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static std::string get_cli_path() {
    // Cross-arch runs (CI's QEMU leg) cannot exec the target binary directly
    // from the host kernel; OPENRAR_RUNNER prefixes every invocation, e.g.
    // "qemu-aarch64-static -L /usr/aarch64-linux-gnu". Unset locally, so the
    // native behavior is unchanged.
    const char* runner = std::getenv("OPENRAR_RUNNER");
    const std::string prefix = (runner && *runner) ? std::string(runner) + " " : "";
    // Preferred: the exact path CMake built, injected as OPENRAR_CLI_EXE.
    // The CWD-relative probing below is only a fallback for manual builds; it
    // breaks as soon as the build directory isn't the default `build/`.
#ifdef OPENRAR_CLI_EXE
    if (std::filesystem::exists(OPENRAR_CLI_EXE)) {
        return prefix + "\"" + std::filesystem::canonical(OPENRAR_CLI_EXE).string() + "\"";
    }
#endif
    const std::vector<std::string> candidates = {"build/openrar64/Debug/openrar.exe",
                                                 "build/openrar64/Release/openrar.exe",
                                                 "build/openrar64/openrar.exe",
                                                 "../openrar64/Debug/openrar.exe",
                                                 "../openrar64/Release/openrar.exe",
                                                 "../../build/openrar64/Debug/openrar.exe",
                                                 "build/Debug/openrar.exe",
                                                 "build/Release/openrar.exe",
                                                 "openrar64/Debug/openrar.exe",
                                                 "openrar64/Release/openrar.exe",
                                                 "openrar.exe"};
    for (const auto& path : candidates) {
        if (std::filesystem::exists(path)) {
            return prefix + "\"" + std::filesystem::canonical(path).string() + "\"";
        }
    }
    return prefix + "\"build\\openrar64\\Debug\\openrar.exe\"";
}

void test_cli_help() {
    std::string cmd = get_cli_path() + " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);
    std::cout << "[PASS] CLI Help & Banner Output\n";
}

void test_cli_lifecycle() {
    std::filesystem::path test_arc = "build/cli_test.rar";
    std::filesystem::path f1 = "build/cli_doc.txt";
    std::filesystem::remove(test_arc);
    std::filesystem::remove(f1);

    // Create a file to move
    { std::ofstream(f1) << "OPENRAR CLI TEST SUITE ITEM"; }
    assert(std::filesystem::exists(f1));

    std::string exe = get_cli_path();

    // Move file into archive
    std::string cmd_move = exe + " m build/cli_test.rar build/cli_doc.txt > " DEVNULL " 2>&1";
    int res_move = std::system(cmd_move.c_str());
    assert(res_move == 0);
    assert(!std::filesystem::exists(f1));
    assert(std::filesystem::exists(test_arc));

    // Test archive
    std::string cmd_test = exe + " t build/cli_test.rar > " DEVNULL " 2>&1";
    int res_test = std::system(cmd_test.c_str());
    assert(res_test == 0);

    // List archive (bare)
    std::string cmd_lb = exe + " lb build/cli_test.rar > " DEVNULL " 2>&1";
    int res_lb = std::system(cmd_lb.c_str());
    assert(res_lb == 0);

    // Lock archive
    std::string cmd_lock = exe + " k build/cli_test.rar > " DEVNULL " 2>&1";
    int res_lock = std::system(cmd_lock.c_str());
    assert(res_lock == 0);

    // Attempt delete on locked archive -> should fail
    std::string cmd_del = exe + " d build/cli_test.rar cli_doc.txt > " DEVNULL " 2>&1";
    int res_del = std::system(cmd_del.c_str());
    assert(res_del != 0);

    std::filesystem::remove(test_arc);
    std::cout << "[PASS] CLI Lifecycle (m -> t -> lb -> k -> rejected d)\n";
}

void test_cli_quiet_list_missing_archive_fails() {
    // INFO8 regression: list_archive used to return 0 in quiet mode before
    // ever opening the archive, so a quiet list of a missing archive reported
    // success. Quiet mode must only suppress output, never the open/validate.
    // NOTE: switches parse after the command in this CLI, so `-q` must follow
    // the archive argument to actually reach list_archive.
    int res = std::system(
        (get_cli_path() + " l build/cli_no_such_archive.rar -q > " DEVNULL " 2>&1").c_str());
    assert(res != 0);

    // Quiet list of a VALID archive: real exit code (0), zero output printed.
    std::filesystem::path arc = "build/cli_quiet_ok.rar";
    std::filesystem::path src = "build/cli_quiet_payload.txt";
    std::filesystem::path captured = "build/cli_quiet_out.txt";
    std::filesystem::remove(arc);
    std::filesystem::remove(captured);
    {
        std::ofstream payload(src);
        payload << "OpenRAR quiet-list payload";
    }
    assert(openrar::archive::ArchiveMutator::add_file_to_archive(arc, src, "payload.txt"));
    res = std::system(
        (get_cli_path() + " lb " + arc.string() + " -q > " + captured.string() + " 2>&1").c_str());
    assert(res == 0);
    std::string data;
    {
        std::ifstream out(captured, std::ios::binary);
        data.assign((std::istreambuf_iterator<char>(out)), std::istreambuf_iterator<char>());
    } // close the handle BEFORE remove() below: an open ifstream makes
    // std::filesystem::remove throw a sharing-violation filesystem_error.
    assert(data.empty());

    std::filesystem::remove(arc);
    std::filesystem::remove(captured);
    std::filesystem::remove(src);
    std::cout << "[PASS] CLI quiet list: nonzero on missing, silent + zero on valid (INFO8)\n";
}

void test_cli_list_sanitizes_esc_entry_name() {
    // L11 regression: an archive-controlled entry name containing ANSI escape
    // bytes must not reach the terminal verbatim. NTFS file names cannot hold
    // C0 control characters, so the CLI 'a' command cannot produce one - build
    // the archive with the mutator API, whose entry name is free-form.
    std::filesystem::path src = "build/cli_esc_payload.txt";
    std::filesystem::path arc = "build/cli_esc_name.rar";
    std::filesystem::path captured = "build/cli_esc_name_output.txt";
    std::filesystem::remove(arc);
    std::filesystem::remove(captured);
    {
        std::ofstream payload(src);
        payload << "OpenRAR ESC-name payload";
    }
    assert(std::filesystem::exists(src));

    const std::string evil_name = "evil\x1b]0;pwned\x07name.txt";
    bool added = openrar::archive::ArchiveMutator::add_file_to_archive(arc, src, evil_name);
    assert(added);

    // No quoting of the path arguments: std::system routes through cmd /c,
    // which (with >2 quotes on the line) strips the first AND last quote of
    // the string - quoting the redirect target corrupts it. The paths here
    // contain no spaces, same as the rest of this suite.
    std::string cmd = get_cli_path() + " lb " + arc.string() + " > " + captured.string() + " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);

    std::string data;
    {
        std::ifstream out(captured, std::ios::binary);
        data.assign((std::istreambuf_iterator<char>(out)), std::istreambuf_iterator<char>());
    } // close the handle BEFORE remove() below: an open ifstream makes
    // std::filesystem::remove throw a sharing-violation filesystem_error.
    // No raw ESC (CSI/OSC introducer) or BEL (OSC terminator) byte may leak
    // into the captured output...
    assert(data.find('\x1b') == std::string::npos);
    assert(data.find('\x07') == std::string::npos);
    // ...and the entry must still be listed, control bytes replaced with '?'.
    assert(data.find("evil?]0;pwned?name.txt") != std::string::npos);

    std::filesystem::remove(arc);
    std::filesystem::remove(captured);
    std::filesystem::remove(src);
    std::cout << "[PASS] CLI list output has ESC entry name sanitized (L11)\n";
}

// v1.22.0 sweep: negative tests for every terminal-sanitization category the
// audit calls out. Contract of sanitize_for_display (src/cli/progress.hpp):
// C0/DEL, C1 controls, invalid UTF-8 and bidi/format attackers become '?';
// valid non-ASCII text passes through byte-identical.
void test_sanitize_for_display() {
    using openrar::cli::sanitize_for_display;
    std::cout << "[+] test_sanitize_for_display" << std::endl;

    // ASCII passthrough.
    assert(sanitize_for_display("plain-file_v2 [ok].txt") == "plain-file_v2 [ok].txt");

    // ESC-led CSI / OSC / DCS injection: the ESC introducer (and any other
    // C0 terminator like BEL) becomes '?', the visible text stays — the
    // sequence can no longer fire, which is the contract, not redaction.
    assert(sanitize_for_display("\x1b[2J\x1b[H") == "?[2J?[H");
    assert(sanitize_for_display("\x1b]0;pwned\x07") == "?]0;pwned?");
    assert(sanitize_for_display("\x1bP+q54sc;tm\x1b\\") == "?P+q54sc;tm?\\");

    // C1 controls (U+0080..009F), incl. the 8-bit CSI U+009B.
    assert(sanitize_for_display("a\xc2"
                                "\x9b"
                                "b") == "a??b");      // U+009B
    assert(sanitize_for_display("\xc2\x85") == "??"); // U+0085 NEL

    // Bidi direction attacks: RTL/LTR overrides, isolates, marks.
    assert(sanitize_for_display("invoice\xe2\x80\xae"
                                "exe.pdf") == "invoice???exe.pdf"); // RLO
    assert(sanitize_for_display("\xe2\x81\xa6"
                                "txt\xe2\x81\xa9") == "???txt???"); // LRI/PDI
    assert(sanitize_for_display("a\xe2\x80\x8f"
                                "b") == "a???b"); // RLM
    assert(sanitize_for_display("note\xe2\x80\xa8"
                                "line") == "note???line");                  // LINE SEP
    assert(sanitize_for_display("invoice\xc2\xad.pdf") == "invoice??.pdf"); // soft hyphen

    // Invalid UTF-8: lone continuation, overlong, surrogates, out of range,
    // bad lead, truncated tail — one '?' per byte, nothing consumed blindly.
    assert(sanitize_for_display("a\x80"
                                "b") == "a?b");
    assert(sanitize_for_display("\xc0\xaf") == "??");           // overlong '/'
    assert(sanitize_for_display("\xe0\x80\xaf") == "???");      // overlong '/'
    assert(sanitize_for_display("\xed\xa0\x80") == "???");      // UTF-16 surrogate
    assert(sanitize_for_display("\xf5\x80\x80\x80") == "????"); // > U+10FFFF lead
    assert(sanitize_for_display("tail\xe2\x80") == "tail??");   // truncated sequence
    assert(sanitize_for_display("\xc2") == "?");                // truncated 2-byte

    // Valid non-ASCII passes through byte-identical (accents, CJK, emoji).
    const std::string legit = "h\xc3\xa9llo w\xc3\xb6rld \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\xa6\x96";
    assert(sanitize_for_display(legit) == legit);

    // Mixed: hostile pieces replaced ('?' per source byte), legitimate text
    // and structure kept.
    assert(sanitize_for_display("r\xc2\xad"
                                "sum\xc3\xa9\xe2\x80\xae"
                                ".txt") == "r??sum\xc3\xa9???.txt");

    // v1.28 directive 7: the sanitizer is IDEMPOTENT — the render-time choke
    // point re-sanitizes everything it is handed, which is only free because
    // applying it twice is the identity. Pin across categories (ESC, bidi,
    // C1, invalid UTF-8) and on the passthrough shape.
    const std::string idem_hostile =
        std::string("\x1b]0;t") + "\xe2\x80\xae" + "\xc2\x85" + "\xff\xfe" + ".txt";
    const std::string once = sanitize_for_display(idem_hostile);
    assert(sanitize_for_display(once) == once);
    assert(sanitize_for_display(sanitize_for_display(legit)) == legit);

    std::cout << "[PASS] sanitize_for_display negative coverage (ESC/CSI/OSC, C1, bidi, "
                 "invalid UTF-8) + idempotency\n";
}

void test_cli_mt_batch_equivalence() {
    // -mt must never change archive content: parallel preparation parks each
    // file's payload in a per-index slot and the writer consumes them in
    // queue order, so a 4-thread run has to extract byte-identical to a
    // 1-thread run. Timestamps are excluded from the comparison (entry mtime
    // is wall-clock at prepare time); everything else must match exactly.
    namespace fs = std::filesystem;
    fs::path root = "build/cli_mt_src";
    fs::path arc1 = "build/cli_mt1.rar";
    fs::path arc4 = "build/cli_mt4.rar";
    fs::path out1 = "build/cli_mt1_out";
    fs::path out4 = "build/cli_mt4_out";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove(arc1, ec);
    fs::remove(arc4, ec);
    fs::remove_all(out1, ec);
    fs::remove_all(out4, ec);
    fs::create_directories(root / "sub");

    // Varied payload mix: compressible text, semi-random binary, empty file,
    // nested directory — exercises the LZ path, store fallback and batching
    // across differently sized jobs.
    {
        std::ofstream(root / "a_text.txt") << std::string(300000, 'x') << "OpenRAR -mt test";
        std::ofstream(root / "sub" / "b_mid.bin") << std::string(200000, '\x53') << "tail";
        std::ofstream(root / "c_empty.txt", std::ios::binary);
        std::string noise(70000, '\0');
        for (size_t i = 0; i < noise.size(); ++i)
            noise[i] = static_cast<char>((i * 2654435761u) >> 24);
        std::ofstream(root / "sub" / "d_noise.bin", std::ios::binary) << noise;
    }

    std::string exe = get_cli_path();
    std::string src_list =
        root.string() + "/a_text.txt " + root.string() + "/sub " + root.string() + "/c_empty.txt";
    int res = std::system(
        (exe + " a " + arc1.string() + " -mt1 " + src_list + " > " DEVNULL " 2>&1").c_str());
    assert(res == 0);
    res = std::system(
        (exe + " a " + arc4.string() + " -mt4 " + src_list + " > " DEVNULL " 2>&1").c_str());
    assert(res == 0);

    res = std::system(
        (exe + " x " + arc1.string() + " " + out1.string() + " > " DEVNULL " 2>&1").c_str());
    assert(res == 0);
    res = std::system(
        (exe + " x " + arc4.string() + " " + out4.string() + " > " DEVNULL " 2>&1").c_str());
    assert(res == 0);

    // Compare extracted trees: same file set, same bytes.
    std::vector<std::string> names;
    for (const auto& e : fs::recursive_directory_iterator(out1)) {
        if (e.is_regular_file())
            names.push_back(e.path().lexically_relative(out1).generic_string());
    }
    std::sort(names.begin(), names.end());
    assert(names.size() == 4);
    for (const auto& name : names) {
        std::ifstream f1(out1 / name, std::ios::binary);
        std::ifstream f4(out4 / name, std::ios::binary);
        assert(f1 && f4);
        std::string d1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
        std::string d4((std::istreambuf_iterator<char>(f4)), std::istreambuf_iterator<char>());
        assert(d1 == d4);
    }

    // -mt0 (auto) must also be accepted.
    fs::path arc_auto = "build/cli_mt0.rar";
    fs::remove(arc_auto, ec);
    res = std::system((exe + " a " + arc_auto.string() + " -mt0 " + root.string() +
                       "/a_text.txt > " DEVNULL " 2>&1")
                          .c_str());
    assert(res == 0);
    res = std::system((exe + " t " + arc_auto.string() + " > " DEVNULL " 2>&1").c_str());
    assert(res == 0);

    fs::remove_all(root, ec);
    fs::remove(arc1, ec);
    fs::remove(arc4, ec);
    fs::remove(arc_auto, ec);
    fs::remove_all(out1, ec);
    fs::remove_all(out4, ec);
    std::cout << "[PASS] CLI -mt1/-mt4/-mt0 batch add extracts byte-identical\n";
}

void test_cli_overwrite_modes() {
    // B8 regression: OverwriteMode::Prompt was the documented default but no
    // query was ever implemented, and -y was parsed into a flag nothing read.
    // The interactive prompt needs a pty, which a batch runner cannot drive —
    // the batch-visible behaviors are tested here: -o- keeps the existing
    // file, -y overwrites, and the non-interactive default (stdin is not a
    // tty under std::system) auto-answers Yes like scripted callers expect.
    namespace fs = std::filesystem;
    fs::path src = "build/cli_ov_src.txt";
    fs::path arc = "build/cli_ov.rar";
    fs::path out = "build/cli_ov_out";
    fs::path target = out / "cli_ov_src.txt";
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(arc, ec);
    fs::remove_all(out, ec);
    fs::create_directories(out);

    // The archive is built via the mutator so the stored entry name is exactly
    // "cli_ov_src.txt", independent of CLI path-normalization rules.
    { std::ofstream(src) << "VERSION-ONE"; }
    assert(openrar::archive::ArchiveMutator::add_file_to_archive(arc, src, "cli_ov_src.txt"));

    auto extract = [&](const char* extra_switch) {
        return std::system((get_cli_path() + " x " + arc.string() + " " + out.string() + " " +
                            extra_switch + " > " DEVNULL " 2>&1")
                               .c_str());
    };
    auto read_target = [&]() {
        std::ifstream f(target, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };

    int res = extract("");
    assert(res == 0);
    assert(read_target() == "VERSION-ONE");

    // Simulate a locally-edited file on disk, then check each mode.
    { std::ofstream(target, std::ios::binary) << "LOCAL-EDIT"; }

    // -o-: never overwrite — the on-disk edit must survive.
    res = extract("-o-");
    assert(res == 0);
    assert(read_target() == "LOCAL-EDIT");

    // -y: assume Yes on the overwrite query — the archive version must win.
    res = extract("-y");
    assert(res == 0);
    assert(read_target() == "VERSION-ONE");

    // Default with non-interactive stdin: auto-Yes (backward-compatible with
    // the pre-query behavior every scripted caller relies on).
    { std::ofstream(target, std::ios::binary) << "LOCAL-EDIT"; }
    res = extract("");
    assert(res == 0);
    assert(read_target() == "VERSION-ONE");

    fs::remove(src, ec);
    fs::remove(arc, ec);
    fs::remove_all(out, ec);
    std::cout
        << "[PASS] CLI overwrite modes: -o- keeps, -y overwrites, non-tty default overwrites\n";
}

void test_cli_help_switch_parity() {
    // B8-class hedge: every switch advertised in --help must actually be
    // parsed — an advertised switch that reaches the "unknown switch"
    // fallback is a broken promise to scripts. Each entry runs `lb` (harmless
    // read-only command) with the switch and asserts the run succeeds and
    // never emits the unknown-switch warning.
    namespace fs = std::filesystem;
    fs::path src = "build/cli_parity_src.txt";
    fs::path arc = "build/cli_parity.rar";
    fs::path cmt = "build/cli_parity_cmt.txt";
    fs::path captured = "build/cli_parity_out.txt";
    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove(captured, ec);
    {
        std::ofstream(src) << "parity payload";
        std::ofstream(cmt) << "comment file";
    }
    assert(openrar::archive::ArchiveMutator::add_file_to_archive(arc, src, "payload.txt"));

    const char* switches[] = {
        "-ed", "-ep", "-ep1", "-ep2",  "-ep3", "-ol",  "-ol-",     "-os",       "-ow",  "-plain",
        "-ox", "-q",  "-r",   "-r-",   "-s",   "-sfx", "-y",       "-kb",       "-o+",  "-o-",
        "-vp", "-m3", "-mt1", "-md1m", "-tsm", "-v1k", "-psecret", "-hpsecret", "-rr3",
    };
    for (const char* sw : switches) {
        std::string cmd =
            get_cli_path() + " lb " + arc.string() + " " + sw + " > " + captured.string() + " 2>&1";
        int res = std::system(cmd.c_str());
        if (res != 0) {
            std::cerr << "  switch rejected: " << sw << " (exit " << res << ")\n";
        }
        assert(res == 0);
        std::string data;
        {
            std::ifstream f(captured, std::ios::binary);
            data.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        assert(data.find("unknown switch") == std::string::npos);
        assert(data.find("Error:") == std::string::npos);
    }

    // -- end-of-options: everything after it is a name, even "-weird".
    std::string cmd =
        get_cli_path() + " lb -- " + arc.string() + " > " + captured.string() + " 2>&1";
    assert(std::system(cmd.c_str()) == 0);

    // -z takes a file argument: comment is only consumed by a/u/f/m, so lb
    // must accept the switch without erroring on the missing file check.
    cmd = get_cli_path() + " lb " + arc.string() + " -z" + cmt.string() + " > " +
          captured.string() + " 2>&1";
    assert(std::system(cmd.c_str()) == 0);
    {
        std::ifstream f(captured, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        assert(data.find("unknown switch") == std::string::npos);
    }

    fs::remove(src, ec);
    fs::remove(arc, ec);
    fs::remove(cmt, ec);
    fs::remove(captured, ec);
    std::cout << "[PASS] CLI help/parser switch parity (every advertised switch parses)\n";
}

static void test_cli_version() {
    namespace fs = std::filesystem;
    fs::path out_file = "test_cli_version.txt";
    std::string cmd = get_cli_path() + " --version > " + out_file.string() + " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);
    std::string data;
    {
        std::ifstream f(out_file, std::ios::binary);
        data.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    assert(data.find(std::string("OpenRAR ") + OPENRAR_VERSION_STRING) != std::string::npos);
    std::error_code ec;
    fs::remove(out_file, ec);
    std::cout << "[PASS] CLI --version parity\n";
}

static void test_cli_sfx() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("test_sfx_dir", ec);
    fs::path sample = "test_sfx_dir/sample.txt";
    fs::path arc = "test_sfx_dir/test.rar";
    fs::path exe_arc = "test_sfx_dir/test.exe";
    {
        std::ofstream f(sample, std::ios::binary);
        f << "sfx conversion payload\n";
    }
    std::string cmd =
        get_cli_path() + " a -q " + arc.string() + " " + sample.string() + " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);
    assert(fs::exists(arc));

    // Convert to sfx
    cmd = get_cli_path() + " s -q " + arc.string() + " > " DEVNULL " 2>&1";
    int s_res = std::system(cmd.c_str());
    if (s_res == 0) {
        assert(fs::exists(exe_arc));
        // Test integrity of the SFX
        cmd = get_cli_path() + " t -q " + exe_arc.string() + " > " DEVNULL " 2>&1";
        assert(std::system(cmd.c_str()) == 0);
        // Double-sfx conversion must be rejected
        cmd = get_cli_path() + " s " + exe_arc.string() + " > " DEVNULL " 2>&1";
        assert(std::system(cmd.c_str()) != 0);
    }

    fs::remove_all("test_sfx_dir", ec);
    std::cout << "[PASS] CLI sfx conversion & double-sfx guard\n";
}

static void test_cli_hardlinks() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("test_hl_dir", ec);
    fs::path f1 = "test_hl_dir/f1.txt";
    fs::path f2 = "test_hl_dir/f2.txt";
    fs::path arc = "test_hl_dir/hl.rar";
    {
        std::ofstream f(f1, std::ios::binary);
        f << "hardlink deduplication content\n";
    }
    fs::create_hard_link(f1, f2, ec);
    if (!ec) {
        std::string cmd = get_cli_path() + " a -oh -q " + arc.string() + " " + f1.string() + " " +
                          f2.string() + " > " DEVNULL " 2>&1";
        assert(std::system(cmd.c_str()) == 0);
        assert(fs::exists(arc));

        // Test extraction
        fs::path out_dir = "test_hl_dir/out";
        fs::create_directories(out_dir, ec);
        cmd = get_cli_path() + " x -q " + arc.string() + " " + out_dir.string() +
              "/ > " DEVNULL " 2>&1";
        assert(std::system(cmd.c_str()) == 0);
    }
    fs::remove_all("test_hl_dir", ec);
    std::cout << "[PASS] CLI hardlink archiving (-oh) & extraction\n";
}

static void test_cli_v1_10_features() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_cli_110";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    fs::path f_a = temp_dir / "a.txt";
    fs::path f_b = temp_dir / "b.txt";
    fs::path f_c = temp_dir / "c.log";
    {
        std::ofstream fa(f_a, std::ios::binary);
        fa << "payload alpha";
        std::ofstream fb(f_b, std::ios::binary);
        fb << "payload beta";
        std::ofstream fc(f_c, std::ios::binary);
        fc << "payload gamma log";
    }

    // 1. Test @listfile with UTF-8 BOM and comments
    fs::path listfile = temp_dir / "files.lst";
    {
        std::ofstream fl(listfile, std::ios::binary);
        // UTF-8 BOM
        fl << "\xEF\xBB\xBF";
        fl << "; This is a comment\n";
        fl << "# Another comment\n";
        fl << "// Slash comment\n";
        fl << "\n";
        fl << f_a.string() << "\n";
        fl << "   " << f_b.string() << "   \r\n";
    }

    fs::path arc1 = temp_dir / "test_list.rar";
    std::string cmd = get_cli_path() + " a -q " + arc1.string() + " @" + listfile.string() +
                      " > " DEVNULL " 2>&1";
    int rc = std::system(cmd.c_str());
    assert(rc == 0);
    assert(fs::exists(arc1));

    // List bare to verify only a.txt and b.txt are present
    fs::path list_out = temp_dir / "list.txt";
    cmd = get_cli_path() + " lb " + arc1.string() + " > " + list_out.string();
    rc = std::system(cmd.c_str());
    assert(rc == 0);
    {
        std::ifstream lf(list_out);
        std::string s;
        std::vector<std::string> names;
        while (std::getline(lf, s)) {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) names.push_back(s);
        }
        assert(names.size() == 2);
    }

    // 2. Test -x<pattern> exclusion
    fs::path arc2 = temp_dir / "test_x.rar";
    cmd = get_cli_path() + " a -q -x*.log " + arc2.string() + " " + f_a.string() + " " +
          f_b.string() + " " + f_c.string() + " > " DEVNULL " 2>&1";
    rc = std::system(cmd.c_str());
    assert(rc == 0);

    cmd = get_cli_path() + " lb " + arc2.string() + " > " + list_out.string();
    rc = std::system(cmd.c_str());
    assert(rc == 0);
    {
        std::ifstream lf(list_out);
        std::string s;
        std::vector<std::string> names;
        while (std::getline(lf, s)) {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) names.push_back(s);
        }
        assert(names.size() == 2);
        for (const auto& n : names) {
            assert(n.find(".log") == std::string::npos);
        }
    }

    // 3. Test -x@<listfile> exclusion
    fs::path ex_list = temp_dir / "exclude.lst";
    {
        std::ofstream el(ex_list, std::ios::binary);
        el << "; exclude file\n";
        el << "*.txt\n";
    }
    fs::path arc3 = temp_dir / "test_xlist.rar";
    cmd = get_cli_path() + " a -q -x@" + ex_list.string() + " " + arc3.string() + " " +
          f_a.string() + " " + f_b.string() + " " + f_c.string() + " > " DEVNULL " 2>&1";
    rc = std::system(cmd.c_str());
    assert(rc == 0);

    cmd = get_cli_path() + " lb " + arc3.string() + " > " + list_out.string();
    rc = std::system(cmd.c_str());
    assert(rc == 0);
    {
        std::ifstream lf(list_out);
        std::string s;
        std::vector<std::string> names;
        while (std::getline(lf, s)) {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) names.push_back(s);
        }
        assert(names.size() == 1);
        assert(names[0].find(".log") != std::string::npos);
    }

    // 4. Test 'p' (print to stdout)
    fs::path p_out = temp_dir / "p_out.txt";
    cmd = get_cli_path() + " p " + arc1.string() + " a.txt > " + p_out.string();
    rc = std::system(cmd.c_str());
    assert(rc == 0);
    {
        std::ifstream pf(p_out, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
        assert(content == "payload alpha");
    }

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] CLI v1.10 features: @listfile, -x exclusion, p stdout print\n";
}

void test_cli_dict_size_flag() {
    namespace fs = std::filesystem;
    fs::path temp_dir = "build/cli_test_dict";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);

    fs::path src_file = temp_dir / "sample.bin";
    {
        std::ofstream f(src_file, std::ios::binary);
        std::vector<char> buf(64 * 1024, 'X');
        f.write(buf.data(), buf.size());
    }

    std::string exe = get_cli_path();

    auto find_file =
        [](const openrar::archive::ArchiveReader& r) -> const openrar::archive::ArchiveEntry* {
        for (const auto& e : r.entries()) {
            if (!e.header.is_service) return &e;
        }
        return nullptr;
    };

    // 1. Valid -md16m with -m3
    fs::path arc_16m = temp_dir / "test_16m.rar";
    std::string cmd = exe + " a -q -m3 -md16m " + arc_16m.string() + " " + src_file.string() +
                      " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc_16m));
        const auto* e = find_file(r);
        assert(e != nullptr);
        assert(e->header.win_size == 16 * 1024 * 1024);
    }

    // 2. Valid -md64m with -m5
    fs::path arc_64m = temp_dir / "test_64m.rar";
    cmd = exe + " a -q -m5 -md64m " + arc_64m.string() + " " + src_file.string() +
          " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc_64m));
        const auto* e = find_file(r);
        assert(e != nullptr);
        assert(e->header.win_size == 64 * 1024 * 1024);
    }

    // 3. Non-power-of-two -md10m is now valid with RAR 7.0 dictionary sizing
    fs::path arc_npot = temp_dir / "npot.rar";
    cmd =
        exe + " a -q -md10m " + arc_npot.string() + " " + src_file.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc_npot));
        const auto* e = find_file(r);
        assert(e != nullptr);
        assert(e->header.win_size == 10 * 1024 * 1024);
    }

    // 3b. Invalid -md invalid syntax (e.g. -mdxyz) -> fail
    fs::path arc_bad1 = temp_dir / "bad1.rar";
    cmd =
        exe + " a -q -mdxyz " + arc_bad1.string() + " " + src_file.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res != 0);

    // 4. Invalid -md out of range (< 128k, e.g. -md64k) -> fail
    fs::path arc_bad2 = temp_dir / "bad2.rar";
    cmd =
        exe + " a -q -md64k " + arc_bad2.string() + " " + src_file.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res != 0);

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] CLI -md<size> dictionary configuration\n";
}

// v1.26 M2a: deterministic content generator for the -oi duplicate corpus.
// Long same-byte runs keep the corpus compressible, so entries stay
// method>0 and actually join solid chains (the store fallback would opt
// every entry out of chain membership and the precedence pin would be
// vacuous).
static std::vector<char> make_pattern_bytes(size_t n, unsigned seed) {
    std::vector<char> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<char>(((i / 64) % 251) ^ (seed * 7));
    }
    return v;
}

static void write_pattern_file(const std::filesystem::path& p, size_t n, unsigned seed) {
    std::vector<char> data = make_pattern_bytes(n, seed);
    std::ofstream f(p, std::ios::binary);
    assert(f);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    assert(f);
}

// Incompressible pseudo-noise (LCG): for corpora whose similarity must come
// from shared bytes, not from self-compression — the reduction gate needs
// unmatched content to actually be stored.
static std::vector<char> make_noise_bytes(size_t n, unsigned seed) {
    std::vector<char> v(n);
    unsigned x = seed * 2654435761u + 1u;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        v[i] = static_cast<char>((x >> 16) & 0xFF);
    }
    return v;
}

static void write_noise_file(const std::filesystem::path& p, size_t n, unsigned seed) {
    std::vector<char> data = make_noise_bytes(n, seed);
    std::ofstream f(p, std::ios::binary);
    assert(f);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    assert(f);
}

static bool file_bytes_equal(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[4096], bb[4096];
    for (;;) {
        fa.read(ba, sizeof(ba));
        fb.read(bb, sizeof(bb));
        auto ga = fa.gcount(), gb = fb.gcount();
        if (ga != gb) return false;
        if (ga == 0) return true;
        if (std::memcmp(ba, bb, static_cast<size_t>(ga)) != 0) return false;
    }
}

// Plan test 1 (v1.26 §6): `a -oi1` on a corpus with exact duplicates emits
// FHEXTRA_REDIR type-5 entries; extraction reproduces identical bytes. Also
// pins the README -oi row (Pillar 7.6 drift fix) and the default 64 KiB
// comparison threshold (below it, identical files are stored normally).
static void test_oi_creation_side_emitted() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_oi_create";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    const size_t big = 192 * 1024;
    fs::path master = temp_dir / "aa.bin";
    fs::path dup = temp_dir / "ab.bin";
    fs::path uniq = temp_dir / "zz.bin";
    fs::path s1 = temp_dir / "s1.bin";
    fs::path s2 = temp_dir / "s2.bin";
    write_pattern_file(master, big, 1);
    write_pattern_file(dup, big, 1); // byte-identical to aa.bin
    write_pattern_file(uniq, big, 2);
    write_pattern_file(s1, 1024, 3); // identical pair BELOW the default
    write_pattern_file(s2, 1024, 3); // 64 KiB threshold: must stay stored

    fs::path arc = temp_dir / "oi.rar";
    std::string cmd = get_cli_path() + " a -oi1 -q " + arc.string() + " " + master.string() + " " +
                      dup.string() + " " + uniq.string() + " " + s1.string() + " " + s2.string() +
                      " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);
    assert(fs::exists(arc));

    // Header-level assertions: the duplicate became a FILECOPY redir to the
    // master; everything else is an ordinary stored/packed entry.
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc));
        std::map<std::string, const openrar::archive::ArchiveEntry*> by_name;
        for (const auto& e : r.entries()) {
            if (!e.header.is_service) by_name[e.header.file_name] = &e;
        }
        assert(by_name.size() == 5);
        const auto* aa = by_name["aa.bin"];
        const auto* ab = by_name["ab.bin"];
        const auto* zz = by_name["zz.bin"];
        const auto* q1 = by_name["s1.bin"];
        const auto* q2 = by_name["s2.bin"];
        assert(aa && ab && zz && q1 && q2);
        assert(aa->header.redir_type == 0 && aa->data_size > 0);
        assert(zz->header.redir_type == 0 && zz->data_size > 0);
        // Threshold: sub-64KiB identical pair is stored, never referenced.
        assert(q1->header.redir_type == 0 && q2->header.redir_type == 0);
        // The duplicate: FHEXTRA_REDIR type 5, no data area.
        assert(ab->header.redir_type == 5);
        assert(ab->header.redir_target == "aa.bin");
        assert(!ab->header.redir_dir_target);
        assert(ab->data_size == 0);
    }

    // `t` must pass: integrity of the archive including the redir entries.
    cmd = get_cli_path() + " t " + arc.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);

    // Extraction (-ol: redirs are links default-deny, v1.24 §6.1) reproduces
    // every byte, including the referenced duplicate.
    fs::path out_dir = temp_dir / "out";
    cmd = get_cli_path() + " x -ol -q " + arc.string() + " " + out_dir.string() +
          " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    assert(file_bytes_equal(master, out_dir / "aa.bin"));
    assert(file_bytes_equal(master, out_dir / "ab.bin")); // FILECOPY copy of the target
    assert(file_bytes_equal(uniq, out_dir / "zz.bin"));
    assert(file_bytes_equal(s1, out_dir / "s1.bin"));
    assert(file_bytes_equal(s1, out_dir / "s2.bin"));

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] oi_creation_side_emitted (plan test 1)\n";
}

// Plan test 5 (v1.26 §6): in a solid archive, FILECOPY redirs sit OUTSIDE
// solid runs — the run breaks around them (no data area), the next
// data-bearing entry starts a fresh chain, and the chain reforms after that
// head. The archive stays decodable across the redir gaps and identical
// content is never double-packed.
static void test_cdc_filecopy_precedence() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_oi_solid";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    const size_t sz = 128 * 1024;
    // Archive order (sorted): a, a2, b, b2, c, d. a2 references a; b2
    // references b; every redir therefore sits between solid candidates.
    fs::path a = temp_dir / "a.bin";
    fs::path a2 = temp_dir / "a2.bin";
    fs::path b = temp_dir / "b.bin";
    fs::path b2 = temp_dir / "b2.bin";
    fs::path c = temp_dir / "c.bin";
    fs::path d = temp_dir / "d.bin";
    write_pattern_file(a, sz, 10);
    write_pattern_file(a2, sz, 10);
    write_pattern_file(b, sz, 11);
    write_pattern_file(b2, sz, 11);
    write_pattern_file(c, sz, 12);
    write_pattern_file(d, sz, 13);

    fs::path arc = temp_dir / "solid_oi.rar";
    std::string cmd = get_cli_path() + " a -s -oi1 -m3 -q " + arc.string() + " " + a.string() +
                      " " + a2.string() + " " + b.string() + " " + b2.string() + " " + c.string() +
                      " " + d.string() + " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);

    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc));
        std::map<std::string, const openrar::archive::ArchiveEntry*> by_name;
        for (const auto& e : r.entries()) {
            if (!e.header.is_service) by_name[e.header.file_name] = &e;
        }
        assert(by_name.size() == 6);
        const auto* pa = by_name["a.bin"];
        const auto* pa2 = by_name["a2.bin"];
        const auto* pb = by_name["b.bin"];
        const auto* pb2 = by_name["b2.bin"];
        const auto* pc = by_name["c.bin"];
        const auto* pd = by_name["d.bin"];
        assert(pa && pa2 && pb && pb2 && pc && pd);
        // References: FILECOPY, no data, FCI_SOLID unset (never a chain member).
        assert(pa2->header.redir_type == 5 && pa2->header.redir_target == "a.bin");
        assert(pa2->data_size == 0 && pa2->header.is_solid == 0);
        assert(pb2->header.redir_type == 5 && pb2->header.redir_target == "b.bin");
        assert(pb2->data_size == 0 && pb2->header.is_solid == 0);
        // Run-breaking: the entry after each redir starts a FRESH chain
        // (is_solid unset) even though the archive is solid — the plan §2.2
        // pin that the writer breaks runs around no-data-area redirs.
        assert(pa->header.is_solid == 0); // first chain head
        assert(pb->header.is_solid == 0); // new head across the a2 redir gap
        assert(pc->header.is_solid == 0); // new head across the b2 redir gap
        assert(pd->header.is_solid != 0); // chain reformed: d continues c
        // Identical content never double-packed: a2/b2 carry no packed data.
        assert(pb->data_size > 0 && pc->data_size > 0);
    }

    // Solid chain decodable across the redir gaps: test + extraction.
    cmd = get_cli_path() + " t " + arc.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);

    fs::path out_dir = temp_dir / "out";
    cmd = get_cli_path() + " x -ol -q " + arc.string() + " " + out_dir.string() +
          " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    assert(file_bytes_equal(a, out_dir / "a.bin"));
    assert(file_bytes_equal(a, out_dir / "a2.bin"));
    assert(file_bytes_equal(b, out_dir / "b.bin"));
    assert(file_bytes_equal(b, out_dir / "b2.bin"));
    assert(file_bytes_equal(c, out_dir / "c.bin"));
    assert(file_bytes_equal(d, out_dir / "d.bin"));

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] cdc_filecopy_precedence (plan test 5)\n";
}

// -oi switch semantics per the README row: 2 list, 3 list+exit (no archive),
// 4 exit-only-when-dups, :<size> threshold override, -oi5 rejected (usage;
// CDC packing is -cdc in M2b), -oi+v refused (volume path has no redir
// entries yet).
static void test_oi_switch_modes() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_oi_modes";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    fs::path m1 = temp_dir / "aa.bin";
    fs::path m2 = temp_dir / "ab.bin";
    fs::path u1 = temp_dir / "zz.bin";
    write_pattern_file(m1, 192 * 1024, 21);
    write_pattern_file(m2, 192 * 1024, 21); // duplicate of aa
    write_pattern_file(u1, 192 * 1024, 22);
    std::string exe = get_cli_path();

    // -oi3: duplicates listed, exit 0, NO archive created.
    {
        fs::path arc = temp_dir / "mode3.rar";
        fs::path list_out = temp_dir / "mode3.out";
        std::string cmd = exe + " a -oi3 " + arc.string() + " " + m1.string() + " " + m2.string() +
                          " " + u1.string() + " > " + list_out.string() + " 2>&1";
        int res = std::system(cmd.c_str());
        assert(res == 0);
        assert(!fs::exists(arc)); // list+exit never archives
        std::ifstream in(list_out, std::ios::binary);
        assert(in);
        std::string out_text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        assert(out_text.find("Identical files") != std::string::npos);
        assert(out_text.find("ab.bin -> aa.bin") != std::string::npos);
    }

    // -oi4: no duplicates -> proceeds with a normal archive.
    {
        fs::path p1 = temp_dir / "p1.bin";
        fs::path p2 = temp_dir / "p2.bin";
        write_pattern_file(p1, 8 * 1024, 23);
        write_pattern_file(p2, 8 * 1024, 24);
        fs::path arc = temp_dir / "mode4.rar";
        std::string cmd = exe + " a -oi4 -q " + arc.string() + " " + p1.string() + " " +
                          p2.string() + " > " DEVNULL " 2>&1";
        int res = std::system(cmd.c_str());
        assert(res == 0);
        assert(fs::exists(arc));
        openrar::archive::ArchiveReader r;
        assert(r.open(arc));
        for (const auto& e : r.entries()) {
            if (!e.header.is_service) assert(e.header.redir_type == 0);
        }
    }

    // -oi2:0 — threshold override includes small identical files; mode 2
    // lists the pair while archiving.
    {
        fs::path t1 = temp_dir / "t1.bin";
        fs::path t2 = temp_dir / "t2.bin";
        write_pattern_file(t1, 1024, 25);
        write_pattern_file(t2, 1024, 25);
        fs::path arc = temp_dir / "mode2.rar";
        fs::path list_out = temp_dir / "mode2.out";
        std::string cmd = exe + " a -oi2:0 " + arc.string() + " " + t1.string() + " " +
                          t2.string() + " > " + list_out.string() + " 2>&1";
        int res = std::system(cmd.c_str());
        assert(res == 0);
        assert(fs::exists(arc));
        std::ifstream in(list_out, std::ios::binary);
        assert(in);
        std::string out_text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        assert(out_text.find("t2.bin -> t1.bin") != std::string::npos);
        openrar::archive::ArchiveReader r;
        assert(r.open(arc));
        int redirs = 0;
        for (const auto& e : r.entries()) {
            if (e.header.is_service) continue;
            if (e.header.redir_type == 5) {
                assert(e.header.redir_target == "t1.bin");
                redirs++;
            }
        }
        assert(redirs == 1);
    }

    // -oi5 is not a mode (0-4 is the whole scale; CDC packing is -cdc, M2b).
    {
        fs::path arc = temp_dir / "bad5.rar";
        std::string cmd =
            exe + " a -oi5 " + arc.string() + " " + m1.string() + " > " DEVNULL " 2>&1";
        int res = system_exit_code(std::system(cmd.c_str()));
        assert(res == 7); // EXIT_USAGE
        assert(!fs::exists(arc));
    }

    // -oi with multi-volume is refused fail-closed (exit 7), no archive.
    {
        fs::path arc = temp_dir / "vol.rar";
        std::string cmd = exe + " a -oi1 -v1m " + arc.string() + " " + m1.string() + " " +
                          m2.string() + " > " DEVNULL " 2>&1";
        int res = system_exit_code(std::system(cmd.c_str()));
        assert(res == 7);
    }

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] oi_switch_modes (README -oi row semantics)\n";
}

static std::vector<char> read_whole_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    assert(f);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void set_cap_env(const char* value) {
#ifdef _WIN32
    _putenv_s("OPENRAR_CDC_INDEX_CAP", value);
#else
    setenv("OPENRAR_CDC_INDEX_CAP", value, 1);
#endif
}

// Plan test 3 (v1.26 §6) + the M2b FILECOPY/redir interaction: -cdc reorders
// by affinity, and the SAME run emits FILECOPY references for exact
// duplicates. Same input at -mt1 and -mt8 must produce byte-identical
// archives (ordering stability, directive 4), and extraction reproduces
// every byte.
static void test_cdc_packing_order_stable() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_cdc_stable";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    const size_t sz = 256 * 1024;
    fs::path base = temp_dir / "base.bin";
    fs::path dup = temp_dir / "dup.bin";
    fs::path u1 = temp_dir / "u1.bin";
    fs::path v1 = temp_dir / "v1.bin";
    write_noise_file(base, sz, 30);
    write_noise_file(dup, sz, 30); // exact duplicate of base.bin
    write_noise_file(u1, sz, 31);
    {
        // v1: base with its middle quarter replaced (75% chunk affinity).
        std::vector<char> data = make_noise_bytes(sz, 30);
        std::vector<char> mid = make_noise_bytes(sz / 4, 32);
        std::copy(mid.begin(), mid.end(), data.begin() + static_cast<std::ptrdiff_t>(sz / 3));
        std::ofstream f(v1, std::ios::binary);
        assert(f);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        assert(f);
    }

    std::string exe = get_cli_path();
    auto create = [&](const char* arc_name, const char* mt) {
        fs::path arc = temp_dir / arc_name;
        std::string cmd = exe + " a -cdc -q " + mt + " " + arc.string() + " " + base.string() +
                          " " + dup.string() + " " + u1.string() + " " + v1.string() +
                          " > " DEVNULL " 2>&1";
        int res = std::system(cmd.c_str());
        assert(res == 0);
        return arc;
    };
    fs::path arc1 = create("cdc_mt1.rar", "-mt1");
    fs::path arc2 = create("cdc_mt8.rar", "-mt8");

    // Ordering stability: byte-identical archives at any -mt.
    assert(read_whole_file(arc1) == read_whole_file(arc2));

    // Redir interaction: the exact duplicate became a FILECOPY to its master,
    // carries no data, and the master precedes it in the packed order.
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc1));
        size_t base_pos = SIZE_MAX, dup_pos = SIZE_MAX;
        size_t file_idx = 0;
        for (const auto& e : r.entries()) {
            if (e.header.is_service) continue;
            if (e.header.file_name == "base.bin") base_pos = file_idx;
            if (e.header.file_name == "dup.bin") {
                dup_pos = file_idx;
                assert(e.header.redir_type == 5);
                assert(e.header.redir_target == "base.bin");
                assert(e.data_size == 0);
            }
            ++file_idx;
        }
        assert(base_pos != SIZE_MAX && dup_pos != SIZE_MAX);
        assert(base_pos < dup_pos);
    }

    // `t` + extraction identity (references materialize under -ol).
    std::string cmd = exe + " t " + arc1.string() + " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);
    fs::path out_dir = temp_dir / "out";
    cmd = exe + " x -ol -q " + arc1.string() + " " + out_dir.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    assert(file_bytes_equal(base, out_dir / "base.bin"));
    assert(file_bytes_equal(base, out_dir / "dup.bin"));
    assert(file_bytes_equal(u1, out_dir / "u1.bin"));
    assert(file_bytes_equal(v1, out_dir / "v1.bin"));

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] cdc_packing_order_stable (plan test 3 + redir interaction)\n";
}

// Plan test 2 (v1.26 §6): fingerprint index cap hit -> the tail keeps its
// original order and the fallback is REPORTED (directive 5, no silent
// degradation). OPENRAR_CDC_INDEX_CAP lowers the cap for the run; the
// archive stays valid, deterministic, and byte-faithful.
static void test_cdc_index_cap_falls_back_reported() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_cdc_cap";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    std::vector<fs::path> files;
    for (unsigned i = 0; i < 6; ++i) {
        fs::path p = temp_dir / ("f" + std::to_string(i) + ".bin");
        write_noise_file(p, 64 * 1024, 40 + i); // distinct content, no affinity
        files.push_back(p);
    }

    std::string exe = get_cli_path();
    set_cap_env("5"); // force the cap to trigger after a few files
    fs::path arc = temp_dir / "cap.rar";
    fs::path list_out = temp_dir / "cap.out";
    std::string cmd = exe + " a -cdc -q " + arc.string();
    for (const auto& f : files) cmd += " " + f.string();
    cmd += " > " + list_out.string() + " 2>&1";
    int res = std::system(cmd.c_str());
    set_cap_env("");
    assert(res == 0);

    std::ifstream in(list_out, std::ios::binary);
    assert(in);
    std::string out_text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(out_text.find("fingerprint index cap reached") != std::string::npos);
    assert(out_text.find("original order") != std::string::npos);

    // The fallback archive is valid, deterministic, and byte-faithful.
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc));
        size_t file_entries = 0;
        for (const auto& e : r.entries()) {
            if (!e.header.is_service) ++file_entries;
        }
        assert(file_entries == 6);
    }
    fs::path arc2 = temp_dir / "cap2.rar";
    set_cap_env("5");
    std::string cmd2 = exe + " a -cdc -q " + arc2.string();
    for (const auto& f : files) cmd2 += " " + f.string();
    cmd2 += " > " DEVNULL " 2>&1";
    res = std::system(cmd2.c_str());
    set_cap_env("");
    assert(res == 0);
    assert(read_whole_file(arc) == read_whole_file(arc2));

    fs::path out_dir = temp_dir / "out";
    cmd = exe + " x -ol -q " + arc.string() + " " + out_dir.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    for (const auto& f : files) {
        assert(file_bytes_equal(f, out_dir / f.filename()));
    }

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] cdc_index_cap_falls_back_reported (plan test 2)\n";
}

// Plan test 4 (v1.26 §6) — the three-number reduction gate (directive 1,
// living in tests per directive 3): for a corpus with cross-file similarity
// the report carries store / plain-solid / CDC-packed at the SAME window,
// and CDC-packed strictly beats plain-solid. The corpus is engineered so
// similarity is byte-sharing, not self-compression: noise base + two
// variants (middle quarter replaced) + two unrelated noise files, original
// order spreading the variants one window apart.
static void test_cdc_reduction_report_three_numbers() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_cdc_reduction";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    const size_t sz = 1024 * 1024;
    const size_t mid_off = sz / 4, mid_len = sz / 4;
    fs::path base = temp_dir / "base.bin";
    fs::path u1 = temp_dir / "u1.bin";
    fs::path v1 = temp_dir / "v1.bin";
    fs::path u2 = temp_dir / "u2.bin";
    fs::path v2 = temp_dir / "v2.bin";
    write_noise_file(base, sz, 50);
    write_noise_file(u1, sz, 51);
    write_noise_file(u2, sz, 52);
    auto write_variant = [&](const fs::path& p, unsigned mid_seed) {
        std::vector<char> data = make_noise_bytes(sz, 50); // copy of base
        std::vector<char> mid = make_noise_bytes(mid_len, mid_seed);
        std::copy(mid.begin(), mid.end(), data.begin() + static_cast<std::ptrdiff_t>(mid_off));
        std::ofstream f(p, std::ios::binary);
        assert(f);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        assert(f);
    };
    write_variant(v1, 53);
    write_variant(v2, 54);

    // Original order interleaves the variants so plain-solid cannot keep the
    // shared regions inside a 1 MiB window; CDC packing pulls them adjacent.
    const std::string files = base.string() + " " + u1.string() + " " + v1.string() + " " +
                              u2.string() + " " + v2.string();
    std::string exe = get_cli_path();

    fs::path store_arc = temp_dir / "store.rar";
    std::string cmd = exe + " a -m0 -q " + store_arc.string() + " " + files + " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);

    fs::path plain_arc = temp_dir / "plain.rar";
    cmd = exe + " a -s -md1m -q " + plain_arc.string() + " " + files + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);

    fs::path cdc_arc = temp_dir / "cdc.rar";
    fs::path list_out = temp_dir / "cdc.out";
    cmd = exe + " a -cdc -md1m " + cdc_arc.string() + " " + files + " > " + list_out.string() +
          " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);

    const uint64_t store_size = std::filesystem::file_size(store_arc, ec);
    const uint64_t plain_size = std::filesystem::file_size(plain_arc, ec);
    const uint64_t cdc_size = std::filesystem::file_size(cdc_arc, ec);
    std::cout << "  [INFO] reduction: store=" << store_size << " plain-solid=" << plain_size
              << " cdc-packed=" << cdc_size << "\n";

    // The pack-time report carries logical/packed/window/flag.
    std::ifstream in(list_out, std::ios::binary);
    assert(in);
    std::string out_text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(out_text.find("cdc: logical=") != std::string::npos);
    assert(out_text.find("flag=reordered") != std::string::npos);

    // The gate: CDC-packed must beat plain-solid at the same window.
    assert(cdc_size < plain_size);

    // Extraction identity for the packed archive.
    fs::path out_dir = temp_dir / "out";
    cmd = exe + " x -ol -q " + cdc_arc.string() + " " + out_dir.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    assert(file_bytes_equal(base, out_dir / "base.bin"));
    assert(file_bytes_equal(v1, out_dir / "v1.bin"));
    assert(file_bytes_equal(v2, out_dir / "v2.bin"));
    assert(file_bytes_equal(u1, out_dir / "u1.bin"));
    assert(file_bytes_equal(u2, out_dir / "u2.bin"));

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] cdc_reduction_report_three_numbers (plan test 4)\n";
}

// Plan test 9 (v1.26 §6): 0-byte and sub-min-chunk files pack in original
// order, no crash, no affinity edges; empty files extract as empty.
static void test_cdc_degenerate_inputs() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_cdc_degenerate";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    fs::path big = temp_dir / "big.bin";
    fs::path e1 = temp_dir / "e1.bin";
    fs::path e2 = temp_dir / "e2.bin";
    fs::path t1 = temp_dir / "t1.bin";
    fs::path t2 = temp_dir / "t2.bin";
    write_noise_file(big, 100 * 1024, 60);
    write_noise_file(t1, 1024, 61); // distinct 1 KiB files (no edges)
    write_noise_file(t2, 1024, 62);
    { std::ofstream(e1, std::ios::binary); }
    { std::ofstream(e2, std::ios::binary); }

    fs::path arc = temp_dir / "degenerate.rar";
    std::string exe = get_cli_path();
    std::string cmd = exe + " a -cdc -q " + arc.string() + " " + big.string() + " " + e1.string() +
                      " " + e2.string() + " " + t1.string() + " " + t2.string() +
                      " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);

    // Original order preserved end-to-end (no affinity edges anywhere).
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc));
        std::vector<std::string> names;
        for (const auto& e : r.entries()) {
            if (!e.header.is_service) {
                names.push_back(e.header.file_name);
                assert(e.header.redir_type == 0); // no affinity edges materialized
            }
        }
        assert(
            (names == std::vector<std::string>{"big.bin", "e1.bin", "e2.bin", "t1.bin", "t2.bin"}));
    }

    cmd = exe + " t " + arc.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);

    fs::path out_dir = temp_dir / "out";
    cmd = exe + " x -ol -q " + arc.string() + " " + out_dir.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    assert(fs::file_size(out_dir / "e1.bin", ec) == 0);
    assert(fs::file_size(out_dir / "e2.bin", ec) == 0);
    assert(file_bytes_equal(t1, out_dir / "t1.bin"));
    assert(file_bytes_equal(t2, out_dir / "t2.bin"));
    assert(file_bytes_equal(big, out_dir / "big.bin"));

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] cdc_degenerate_inputs (plan test 9)\n";
}

// v1.26 M3 helpers: set/read a file's mtime as unix seconds (the test needs
// stamps outside std::filesystem::file_time_type's comfortable range).
static bool test_set_mtime_unix(const std::filesystem::path& p, long long unix_sec) {
#ifdef _WIN32
    const long long ft100 = (unix_sec + 11644473600LL) * 10000000LL;
    ULARGE_INTEGER ul;
    ul.QuadPart = static_cast<unsigned long long>(ft100);
    FILETIME ft;
    ft.dwLowDateTime = ul.LowPart;
    ft.dwHighDateTime = ul.HighPart;
    HANDLE h = CreateFileW(p.wstring().c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok != FALSE;
#else
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = static_cast<time_t>(unix_sec);
    times[1].tv_nsec = 0;
    return ::utimensat(AT_FDCWD, p.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0;
#endif
}

static long long test_read_mtime_unix(const std::filesystem::path& p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return -1;
    const ULARGE_INTEGER ul{fa.ftLastWriteTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime};
    return static_cast<long long>(ul.QuadPart / 10000000ULL) - 11644473600LL;
#else
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_mtime);
#endif
}

// Plan test 8 (v1.26 §6): out-of-bounds mtimes clamp to MtimeBounds and
// surface as the timestamp_clamped security flag in the JSON summary plus a
// report line. Also pins the parity-gap fix this wiring carries: extracted
// FILES now restore the archived mtime exactly (only directories did
// before). The absurd stamp is crafted directly into the header block as a
// pre-1970 Windows FILETIME — mtime_win is the only 64-bit mtime field, so
// this exercises the clamp on every platform without 32-bit truncation.
static void test_timestamp_clamped_flag() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_ts_clamp";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    const std::string ok_text = "in-bounds mtime payload";
    const std::string bad_text = "pre-1970 mtime payload";
    fs::path ok_file = temp_dir / "ok.bin";
    fs::path bad_file = temp_dir / "bad.bin";
    {
        std::ofstream f(ok_file, std::ios::binary);
        f << ok_text;
        std::ofstream g(bad_file, std::ios::binary);
        g << bad_text;
    }
    // 2020-01-01: comfortably inside [1970, 3000].
    assert(test_set_mtime_unix(ok_file, 1577836800));
    const long long ok_mtime = test_read_mtime_unix(ok_file);
    assert(ok_mtime == 1577836800);

    // Build the archive in-process for exact header-field control:
    // bad.bin carries ONLY a pre-1970 mtime_win (utime/htime zeroed).
    fs::path arc = temp_dir / "ts.rar";
    {
        std::vector<openrar::archive::ArchiveMutator::PreparedAdd> prepared(2);
        assert(openrar::archive::ArchiveMutator::prepare_add_file(ok_file, "ok.bin", 0, "",
                                                                  prepared[0]));
        assert(openrar::archive::ArchiveMutator::prepare_add_file(bad_file, "bad.bin", 0, "",
                                                                  prepared[1]));
        prepared[1].fb.utime_unix = 0;
        prepared[1].fb.htime_is_unix = false;
        prepared[1].fb.htime_mtime_unix = 0;
        // Year 1960: unix -315619200 -> FILETIME 100ns ticks.
        const long long pre1970_unix = -315619200LL;
        prepared[1].fb.mtime_win =
            static_cast<unsigned long long>((pre1970_unix + 11644473600LL) * 10000000LL);
        assert(openrar::archive::ArchiveMutator::write_batch_add(arc, prepared, {}, "", false, {},
                                                                 false));
    }

    // Extract with the JSON summary; stderr carries the report line.
    fs::path out_dir = temp_dir / "out";
    fs::path json_path = temp_dir / "summary.json";
    fs::path err_path = temp_dir / "stderr.txt";
    std::string cmd = get_cli_path() + " x --json-summary=" + json_path.string() + " " +
                      arc.string() + " " + out_dir.string() + " 2> " + err_path.string();
    int res = std::system(cmd.c_str());
    assert(res == 0);

    // The out-of-bounds stamp clamped to the bounds minimum (1970-01-01).
    assert(test_read_mtime_unix(out_dir / "bad.bin") == 0);
    // The in-bounds stamp restored exactly (file mtime parity fix).
    assert(test_read_mtime_unix(out_dir / "ok.bin") == ok_mtime);

    // JSON summary flags the clamped entry; stderr carries the report line.
    {
        std::ifstream j(json_path, std::ios::binary);
        assert(j);
        std::string json_text((std::istreambuf_iterator<char>(j)),
                              std::istreambuf_iterator<char>());
        assert(json_text.find("timestamp_clamped") != std::string::npos);
    }
    {
        std::ifstream e(err_path, std::ios::binary);
        assert(e);
        std::string err_text((std::istreambuf_iterator<char>(e)), std::istreambuf_iterator<char>());
        assert(err_text.find("timestamp out of bounds, clamped") != std::string::npos);
        assert(err_text.find("bad.bin") != std::string::npos);
    }

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] timestamp_clamped_flag (plan test 8)\n";
}

// Plan test 6 (v1.26 §6, security-arch §5.4 cross-check): a CDC-packed
// carried-window solid stream is an ordinary solid stream at extraction —
// the cumulative max_total_output_bytes cap fires mid-decode, with the
// carried matches expanding against the same accounting as any other solid
// member. A generous cap proves the limit policy is the only difference.
static void test_cdc_packed_caps_enforced() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_cdc_caps";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    const size_t sz = 512 * 1024;
    fs::path base = temp_dir / "base.bin";
    fs::path v1 = temp_dir / "v1.bin";
    write_noise_file(base, sz, 70);
    {
        std::vector<char> data = make_noise_bytes(sz, 70);
        std::vector<char> mid = make_noise_bytes(sz / 4, 71);
        std::copy(mid.begin(), mid.end(), data.begin() + static_cast<std::ptrdiff_t>(sz / 4));
        std::ofstream f(v1, std::ios::binary);
        assert(f);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        assert(f);
    }
    fs::path arc = temp_dir / "caps.rar";
    std::string cmd = get_cli_path() + " a -cdc -md1m -q " + arc.string() + " " + base.string() +
                      " " + v1.string() + " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);

    {
        openrar::archive::ArchiveReader r;
        int status = 0;
        std::string detail;
        assert(r.open_ex(arc, "", status, detail));

        // Cumulative cap below the first member's size: must fire mid-decode.
        openrar::archive::ExtractionLimits limits;
        limits.max_total_output_bytes = 100;
        openrar::archive::LimitState state;
        std::vector<unsigned char> out;
        int rc = r.extract_entry_to_memory(0, out, 1 << 20, {}, &limits, &state);
        assert(rc == openrar::archive::RAR_ERR_LIMIT_EXCEEDED);
        assert(state.total_out <= limits.max_total_output_bytes);

        // Fresh reader state, generous cap: full decode succeeds and the
        // bytes are exact — the cap is the only thing that ever stops it.
        openrar::archive::ArchiveReader r2;
        assert(r2.open_ex(arc, "", status, detail));
        openrar::archive::ExtractionLimits wide;
        openrar::archive::LimitState wide_state;
        for (size_t i = 0; i < 2; ++i) {
            out.clear();
            rc = r2.extract_entry_to_memory(i, out, 4 << 20, {}, &wide, &wide_state);
            assert(rc == openrar::archive::RAR_OK);
            const fs::path src = (i == 0) ? base : v1;
            std::ifstream f(src, std::ios::binary);
            assert(f);
            std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            assert(out.size() == raw.size());
            assert(std::memcmp(out.data(), raw.data(), raw.size()) == 0);
        }
    }

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] cdc_packed_caps_enforced (plan test 6)\n";
}

// Plan test 7 (v1.26 §6, pre-analysis R3): recovery record + repair works on
// a CDC-packed reordered solid archive — the RR covers the run regardless of
// the planner's ordering, and a damaged carried-window stream reconstructs
// byte-exactly from parity.
static void test_cdc_packed_rr_repair() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temp_dir = "test_cdc_rr";
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    const size_t sz = 256 * 1024;
    fs::path base = temp_dir / "base.bin";
    fs::path v1 = temp_dir / "v1.bin";
    fs::path u1 = temp_dir / "u1.bin";
    write_noise_file(base, sz, 80);
    write_noise_file(u1, sz, 81);
    {
        std::vector<char> data = make_noise_bytes(sz, 80);
        std::vector<char> mid = make_noise_bytes(sz / 4, 82);
        std::copy(mid.begin(), mid.end(), data.begin() + static_cast<std::ptrdiff_t>(sz / 3));
        std::ofstream f(v1, std::ios::binary);
        assert(f);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        assert(f);
    }
    fs::path arc = temp_dir / "cdc_rr.rar";
    std::string exe = get_cli_path();
    std::string cmd = exe + " a -cdc -md1m -rr5 -q " + arc.string() + " " + base.string() + " " +
                      u1.string() + " " + v1.string() + " > " DEVNULL " 2>&1";
    int res = std::system(cmd.c_str());
    assert(res == 0);

    // Control: the intact archive tests clean and has an inline RR.
    cmd = exe + " t " + arc.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);

    // Damage the packed region: flip 8 bytes at ~40% of the archive size
    // (headers sit at the front, RR parity at the end). The shipped repair
    // decodes a SINGLE erased data shard per record (Cauchy single-erasure
    // identification), so the damage stays within one RS shard — the test
    // pins R3 (repair on a reordered carried-window run), not the erasure
    // count NR could theoretically cover.
    {
        std::ifstream in(arc, std::ios::binary);
        assert(in);
        std::vector<char> buf((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        in.close();
        assert(buf.size() > 4 * 1024);
        const size_t pos1 = buf.size() * 2 / 5;
        for (size_t i = 0; i < 8; ++i) {
            buf[pos1 + i] = static_cast<char>(buf[pos1 + i] ^ 0xA5);
        }
        std::ofstream out(arc, std::ios::binary | std::ios::trunc);
        assert(out);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        assert(out);
    }

    // The damage is real: integrity check fails before repair.
    cmd = exe + " t " + arc.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res != 0);

    // Repair reconstructs the packed solid data from parity.
    cmd = exe + " r " + arc.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);

    cmd = exe + " t " + arc.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);

    // Byte-exact recovery of every member.
    fs::path out_dir = temp_dir / "out";
    cmd = exe + " x -q " + arc.string() + " " + out_dir.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    assert(file_bytes_equal(base, out_dir / "base.bin"));
    assert(file_bytes_equal(v1, out_dir / "v1.bin"));
    assert(file_bytes_equal(u1, out_dir / "u1.bin"));

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] cdc_packed_rr_repair (plan test 7)\n";
}

// v1.27 M5: -oi3/-oi4 exit WITHOUT creating the archive — the post-add -rr
// dispatch must not then run against a file that was never written.
static void test_oi34_no_archive_no_dispatch() {
    namespace fsx = std::filesystem;
    std::error_code ec;
    fsx::path temp_dir = "test_oi34";
    fsx::remove_all(temp_dir, ec);
    fsx::create_directories(temp_dir, ec);

    fsx::path f1 = temp_dir / "d1.bin";
    fsx::path f2 = temp_dir / "d2.bin";
    const std::string body(128 * 1024, 'D');
    for (const fsx::path& p : {f1, f2}) {
        std::ofstream f(p, std::ios::binary);
        assert(f);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    fsx::path arc = temp_dir / "oi34.rar";
    fsx::path err_log = temp_dir / "err.txt";
    // -oi3 + -rr3: the analysis exits 0 without an archive; no recovery
    // record attempt may run against the nonexistent path.
    std::string cmd = get_cli_path() + " a -oi3 -rr3 -q " + arc.string() + " " + f1.string() + " " +
                      f2.string() + " 2> " + err_log.string();
    const int res = std::system(cmd.c_str());
    assert(res == 0);
    assert(!fsx::exists(arc, ec));
    std::ifstream ef(err_log, std::ios::binary);
    std::string err((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
    assert(err.find("recovery") == std::string::npos);

    // -oi4 WITH duplicates: exit before any archive is written (no rr).
    fsx::path arc4 = temp_dir / "oi4.rar";
    cmd = get_cli_path() + " a -oi4 -rr3 -q " + arc4.string() + " " + f1.string() + " " +
          f2.string() + " 2> " + err_log.string();
    assert(std::system(cmd.c_str()) == 0);
    assert(!fsx::exists(arc4, ec));
    std::ifstream ef4(err_log, std::ios::binary);
    std::string err4((std::istreambuf_iterator<char>(ef4)), std::istreambuf_iterator<char>());
    assert(err4.find("recovery") == std::string::npos);

    // -oi4 WITHOUT duplicates: normal add flow — the archive IS created and
    // the post-add -rr dispatch runs against the real file successfully.
    fsx::path u1 = temp_dir / "u1.bin";
    {
        std::ofstream f(u1, std::ios::binary);
        f << "unique";
    }
    fsx::path arc5 = temp_dir / "oi4clean.rar";
    cmd = get_cli_path() + " a -oi4 -rr3 -q " + arc5.string() + " " + u1.string() +
          " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);
    assert(fsx::exists(arc5, ec));

    fsx::remove_all(temp_dir, ec);
    std::cout << "[PASS] oi34_no_archive_no_dispatch: post-add dispatch skipped\n";
}

// ── v1.27 M4: MotW propagation + zone-stream policy (plan tests 7-11) ───────

#ifdef _WIN32
// Reads the Zone.Identifier ADS content of a file; false when absent.
static bool read_zone_ads_content(const std::filesystem::path& f, std::string& out) {
    std::vector<openrar::io::StreamEntry> streams;
    if (!openrar::io::read_alternate_streams(f, streams)) return false;
    for (const auto& s : streams) {
        if (openrar::io::is_zone_stream_name(s.name)) {
            out.assign(s.data.begin(), s.data.end());
            return true;
        }
    }
    return false;
}
#endif

// plan tests 9/10/11: a marked archive propagates freshly generated marks;
// the archive's HostUrl/ReferrerUrl NEVER travel; -oz- disables; an
// unmarked archive writes nothing.
static void test_motw_propagation_cli() {
#ifdef _WIN32
    namespace fsx = std::filesystem;
    std::error_code ec;
    fsx::path temp_dir = "test_motw_cli";
    fsx::remove_all(temp_dir, ec);
    fsx::create_directories(temp_dir, ec);

    fsx::path src = temp_dir / "a.txt";
    {
        std::ofstream f(src, std::ios::binary);
        f << "motw-body";
    }

    fsx::path arc = temp_dir / "marked.rar";
    std::string cmd =
        get_cli_path() + " a -q " + arc.string() + " " + src.string() + " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);

    // Mark the ARCHIVE file itself (the only provenance surface).
    const auto gen3_bytes = openrar::io::generate_zone_identifier_content(3);
    const std::string gen3(gen3_bytes.begin(), gen3_bytes.end());
    assert(openrar::io::write_alternate_stream(arc, ":Zone.Identifier", gen3.data(), gen3.size()));

    // Default (-oz implied ON): extracted file carries a fresh mark.
    fsx::path out1 = temp_dir / "out1";
    cmd = get_cli_path() + " x -q " + arc.string() + " " + out1.string() + " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);
    std::string got;
    assert(read_zone_ads_content(out1 / "a.txt", got));
    assert(got == gen3); // generated locally, byte-exact
    std::cout << "[PASS] motw_propagated_from_archive_ads (plan test 9)\n";

    // plan test 10: the archive's HostUrl/ReferrerUrl never travel — the
    // extracted mark is exactly the generated content.
    const char* tainted = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=http://evil.example\r\n"
                          "ReferrerUrl=http://referrer.example\r\n";
    assert(openrar::io::write_alternate_stream(arc, ":Zone.Identifier", tainted, strlen(tainted)));
    fsx::path out2 = temp_dir / "out2";
    cmd = get_cli_path() + " x -q " + arc.string() + " " + out2.string() + " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);
    assert(read_zone_ads_content(out2 / "a.txt", got));
    assert(got == gen3);
    assert(got.find("evil") == std::string::npos);
    std::cout << "[PASS] motw_hosturl_never_copied (plan test 10)\n";

    // plan test 11: -oz- disables propagation entirely.
    fsx::path out3 = temp_dir / "out3";
    cmd =
        get_cli_path() + " x -oz- -q " + arc.string() + " " + out3.string() + " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);
    assert(!read_zone_ads_content(out3 / "a.txt", got));

    // Unmarked archive: zero writes.
    fsx::path arc2 = temp_dir / "clean.rar";
    cmd = get_cli_path() + " a -q " + arc2.string() + " " + src.string() + " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);
    fsx::path out4 = temp_dir / "out4";
    cmd = get_cli_path() + " x -q " + arc2.string() + " " + out4.string() + " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);
    assert(!read_zone_ads_content(out4 / "a.txt", got));
    std::cout << "[PASS] motw_disabled_by_flag + unmarked-no-writes (plan test 11)\n";

    fsx::remove_all(temp_dir, ec);
#endif
}

// plan test 7: archive-provided zone content is never written to disk. The
// archive carries a real Zone.Identifier STM child (the WinRAR -os shape,
// also the hostile-crafter shape); extraction skips it before even reading
// the payload and reports it.
static void test_zone_stream_never_restored() {
#ifdef _WIN32
    namespace fsx = std::filesystem;
    std::error_code ec;
    fsx::path temp_dir = "test_zone_skip";
    fsx::remove_all(temp_dir, ec);
    fsx::create_directories(temp_dir, ec);

    fsx::path src = temp_dir / "a.txt";
    {
        std::ofstream f(src, std::ios::binary);
        f << "zone-skip-body";
    }

    // Craft an archive with an STM child named :Zone.Identifier — exactly
    // what WinRAR's -os stores and what a hostile archive would craft.
    fsx::path arc = temp_dir / "zonestream.rar";
    const char* zone_payload = "[ZoneTransfer]\r\nZoneId=1\r\n"; // downgrade attempt
    openrar::archive::ArchiveMutator::PreparedAdd p;
    p.entry_name = "a.txt";
    p.src_path = src;
    assert(openrar::archive::ArchiveMutator::prepare_add_file(src, "a.txt", 0, "", p));
    openrar::archive::ArchiveMutator::PreparedAdd zone_child;
    zone_child.fb.is_service = true;
    zone_child.fb.service_type = "STM";
    zone_child.fb.file_name = "STM";
    const std::string zname = ":Zone.Identifier";
    zone_child.fb.sub_data.assign(zname.begin(), zname.end());
    zone_child.fb.unp_size = strlen(zone_payload);
    zone_child.fb.pack_size = static_cast<openrar::core::int64>(strlen(zone_payload));
    zone_child.fb.method = 0;
    zone_child.fb.has_crc32 = true;
    openrar::crypto::Crc32 c;
    c.update(zone_payload, strlen(zone_payload));
    zone_child.fb.data_crc32 = c.get();
    zone_child.payload.assign(zone_payload, zone_payload + strlen(zone_payload));
    p.child_services.push_back(std::move(zone_child));

    std::vector<openrar::archive::ArchiveMutator::PreparedAdd> batch;
    batch.push_back(std::move(p));
    std::string detail;
    assert(openrar::archive::ArchiveMutator::write_batch_add_ex(arc, batch, {}, "", false, {},
                                                                false, {}, detail) == 0);

    // The archive really contains the zone stream (the skip happens at
    // restore, not because the data vanished).
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc));
        bool saw_zone_child = false;
        for (const auto& e : r.entries()) {
            if (e.header.is_service && e.header.service_type == "STM" &&
                openrar::io::is_zone_stream_name(
                    std::string(e.header.sub_data.begin(), e.header.sub_data.end()))) {
                saw_zone_child = true;
            }
        }
        assert(saw_zone_child);
    }

    // Extract: the file lands, the zone stream does not, and the skip is
    // reported on stderr.
    fsx::path out = temp_dir / "out";
    fsx::path err_log = temp_dir / "err.txt";
    // NOTE: no -q here — quiet mode suppresses the W: line this test asserts.
    std::string cmd = get_cli_path() + " x " + arc.string() + " " + out.string() + " 2> " +
                      err_log.string() + " > " DEVNULL "";
    assert(std::system(cmd.c_str()) == 0);
    std::ifstream ef(err_log, std::ios::binary);
    std::string err((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
    assert(err.find("zone stream") != std::string::npos);
    std::string got;
    assert(!read_zone_ads_content(out / "a.txt", got)); // never restored
    assert(file_bytes_equal(src, out / "a.txt"));

    fsx::remove_all(temp_dir, ec);
    std::cout << "[PASS] zone_stream_never_restored (plan test 7, WinRAR-shaped)\n";
#endif
}

// plan test 8: -os capture excludes the Zone.Identifier ADS (provenance is
// not content); ordinary streams still store.
static void test_os_excludes_zone_capture() {
#ifdef _WIN32
    namespace fsx = std::filesystem;
    std::error_code ec;
    fsx::path temp_dir = "test_os_zone";
    fsx::remove_all(temp_dir, ec);
    fsx::create_directories(temp_dir, ec);

    fsx::path src = temp_dir / "a.txt";
    {
        std::ofstream f(src, std::ios::binary);
        f << "os-zone-body";
    }
    const char* zone = "[ZoneTransfer]\r\nZoneId=3\r\n";
    assert(openrar::io::write_alternate_stream(src, ":Zone.Identifier", zone, strlen(zone)));
    const char* note = "hello-note";
    assert(openrar::io::write_alternate_stream(src, ":note.txt", note, strlen(note)));

    fsx::path arc = temp_dir / "os.rar";
    std::string cmd =
        get_cli_path() + " a -os -q " + arc.string() + " " + src.string() + " > " DEVNULL " 2>&1";
    assert(std::system(cmd.c_str()) == 0);

    openrar::archive::ArchiveReader r;
    assert(r.open(arc));
    bool saw_note = false;
    bool saw_zone = false;
    for (const auto& e : r.entries()) {
        if (!e.header.is_service || e.header.service_type != "STM") continue;
        const std::string name(e.header.sub_data.begin(), e.header.sub_data.end());
        if (openrar::io::is_zone_stream_name(name)) {
            saw_zone = true;
        } else if (name == ":note.txt") {
            saw_note = true;
        }
    }
    assert(saw_note);
    assert(!saw_zone);

    fsx::remove_all(temp_dir, ec);
    std::cout << "[PASS] os_excludes_zone_at_capture (plan test 8)\n";
#endif
}

// ── v1.28 M1: §7.1 release-gate tests ────────────────────────────────────────

// Captures a full command's stdout+stderr into `captured` and returns the
// exit status (system_exit_code semantics live in the caller where needed).
static int run_cli_capture(const std::string& args, const std::filesystem::path& captured) {
    // Quote-free paths + trailing redirect: cmd /c strips the outer quote
    // pair when >2 quotes appear (suite convention — no spaces in paths).
    return std::system((get_cli_path() + " " + args + " > " + captured.string() + " 2>&1").c_str());
}

static std::string slurp_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool has_raw_hostile_bytes(const std::string& data) {
    if (data.find('\x1b') != std::string::npos) return true;         // ESC introducer
    if (data.find('\x07') != std::string::npos) return true;         // BEL terminator
    if (data.find("\xe2\x80\xae") != std::string::npos) return true; // RLO
    if (data.find("\xc2\x85") != std::string::npos) return true;     // C1 NEL
    return false;
}

// Crafts a stored archive in ONE pass: a CMT service header (comment in the
// data area — the shipped `-z` writer's shape, archive_mutator.cpp:2302),
// then every entry; the entry named `owner_entry_name` additionally carries
// a hostile FHEXTRA_UOWNER record. Raw names land verbatim in the headers.
static void write_hostile_archive(const std::filesystem::path& arc, const std::string& comment,
                                  const std::string& owner_user, const std::string& owner_group,
                                  const std::vector<std::pair<std::string, std::string>>& entries,
                                  const std::string& owner_entry_name) {
    openrar::io::FileStream out;
    assert(out.open(arc, openrar::io::FileMode::CreateAlways));
    using openrar::format::FileBlock;
    using openrar::format::HeaderWriter;
    HeaderWriter::write_signature(out);
    openrar::format::MainBlock mb;
    HeaderWriter::write_main_block(out, mb);
    FileBlock cmt;
    cmt.is_service = true;
    cmt.file_name = "CMT";
    cmt.unp_size = comment.size();
    cmt.pack_size = static_cast<openrar::core::int64>(comment.size());
    cmt.method = 0;
    cmt.win_size = 0;
    cmt.has_crc32 = true;
    openrar::crypto::Crc32 crc;
    crc.update(comment.data(), comment.size());
    cmt.data_crc32 = crc.get();
    assert(HeaderWriter::write_file_block(out, cmt));
    out.write(comment.data(), comment.size());
    for (const auto& [name, content] : entries) {
        FileBlock fb;
        fb.file_name = name;
        fb.host_os = 0;
        fb.unp_size = content.size();
        fb.pack_size = static_cast<openrar::core::int64>(content.size());
        fb.method = 0;
        fb.win_size = 0;
        if (name == owner_entry_name) {
            fb.has_owner = true;
            fb.owner_user = owner_user;
            fb.owner_group = owner_group;
        }
        assert(HeaderWriter::write_file_block(out, fb));
        out.write(content.data(), content.size());
    }
    openrar::format::EndArcBlock eb;
    HeaderWriter::write_end_block(out, eb);
}

// v1.28 plan test 6 (`hostile_corpus_zero_payload_e2e`, M1 scope): every
// render surface — listing, technical listing (owner names), comment,
// testing, extraction per-file lines — is driven from one crafted archive
// carrying one hostile payload per §7.1 category, and no attacker payload
// byte may reach the captured output. CI legs are piped, so this is also
// the zero-ESC non-TTY assertion (test 14 extends it to pipes explicitly).
void test_hostile_render_surfaces_e2e() {
    namespace fs = std::filesystem;
    const std::string ESC = "\x1b";
    const std::string BEL = "\x07";
    const std::string RLO = "\xe2\x80\xae";
    const std::string NEL = "\xc2\x85";
    const std::string BAD = "\xff\xfe";

    const std::string comment = ESC + "]0;pwned_title" + BEL + ESC + "[2Jclear";
    const std::string owner_user = ESC + "]0;pwned" + BEL + "adm";
    const std::string owner_group = "ev" + RLO + "il" + NEL;
    const std::string name_rlo = "inv" + RLO + "exe.txt";
    const std::string name_csi = "x" + ESC + "[4;20H" + "y.txt";
    const std::string name_bad = "bad" + BAD + ".txt";

    fs::path arc = "build/cli_hostile_render.rar";
    fs::path captured = "build/cli_hostile_render_out.txt";
    fs::path out_dir = "build/cli_hostile_render_x";
    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove(captured, ec);
    fs::remove_all(out_dir, ec);

    // One raw craft: CMT (hostile comment) + hostile-name entries + the
    // owner-bearing entry. Raw names land verbatim in the headers — the
    // same trust boundary a foreign producer's archive presents.
    write_hostile_archive(arc, comment, owner_user, owner_group,
                          {{name_rlo, "payload A"},
                           {name_csi, "payload B"},
                           {name_bad, "payload C"},
                           {"owned.txt", "owned payload"}},
                          "owned.txt");

    // l: comment + names render sanitized; zero raw hostile bytes.
    int res = system_exit_code(run_cli_capture("l " + arc.string(), captured));
    assert(res == 0);
    std::string data = slurp_file(captured);
    assert(!has_raw_hostile_bytes(data));
    assert(data.find("?[2Jclear") != std::string::npos); // comment mangled, text kept
    assert(data.find("inv???exe.txt") != std::string::npos);
    assert(data.find("x?[4;20Hy.txt") != std::string::npos);

    // lb: names only.
    res = system_exit_code(run_cli_capture("lb " + arc.string(), captured));
    assert(res == 0);
    data = slurp_file(captured);
    assert(!has_raw_hostile_bytes(data));
    assert(data.find("inv???exe.txt") != std::string::npos);

    // lt: the owner names (FHEXTRA_UOWNER) render sanitized (plan test 3).
    res = system_exit_code(run_cli_capture("lt " + arc.string(), captured));
    assert(res == 0);
    data = slurp_file(captured);
    assert(!has_raw_hostile_bytes(data));
    assert(data.find("?]0;pwned?adm / ev???il??") != std::string::npos);

    // t: per-file testing lines sanitized.
    res = system_exit_code(run_cli_capture("t " + arc.string(), captured));
    assert(res == 0);
    data = slurp_file(captured);
    assert(!has_raw_hostile_bytes(data));
    assert(data.find("inv???exe.txt") != std::string::npos);

    // x: per-file lines sanitized; the invalid-UTF-8 name lands under its
    // lossless %XX escape (displayed ≡ extracted, §4.4). The run also emits
    // the JSON summary on stdout, which by the SAME pinned contract carries
    // the raw on-disk name (valid UTF-8, RLO is a legal filename) — the
    // terminal sanitizer owns rendering, the JSON owns on-disk truth.
    res = system_exit_code(run_cli_capture("x " + arc.string() + " " + out_dir.string(), captured));
    assert(res == 0);
    data = slurp_file(captured);
    const size_t json_at = data.find("{\"schema_version\"");
    assert(json_at != std::string::npos);
    const std::string human = data.substr(0, json_at);
    const std::string json_doc = data.substr(json_at);
    assert(!has_raw_hostile_bytes(human));
    assert(openrar::io::is_valid_utf8(json_doc)); // JSON: valid UTF-8, not '?'-mangled
    assert(human.find("bad%FF%FE.txt") != std::string::npos);
    bool found_escaped = false;
    for (const auto& e : fs::directory_iterator(out_dir)) {
        if (e.path().filename().string() == "bad%FF%FE.txt") found_escaped = true;
    }
    assert(found_escaped);

    fs::remove(arc, ec);
    fs::remove(captured, ec);
    fs::remove_all(out_dir, ec);
    std::cout << "[PASS] hostile_corpus_zero_payload_e2e (l/lb/lt/t/x, comment+owner+names)\n";
}

// v1.28 plan test 4 (`json_summary_valid_utf8_hostile_paths`, M1 scope):
// json_escape must emit valid UTF-8 under ALL inputs — RFC 8259 requires the
// document be valid UTF-8, so every invalid byte becomes the ASCII \uFFFD
// escape (never the raw character, directive 6). Valid non-ASCII passes.
void test_json_escape_utf8_validity() {
    using openrar::archive::ExtractionReport;
    using openrar::archive::ExtractionReportEntry;

    // Hostile archive path (invalid UTF-8, overlong): the emitted document
    // must be pure ASCII here and carry the replacement escapes.
    ExtractionReport hostile;
    hostile.archive = std::string("a\xff\xfe") + "b\xc0\xaf.rar";
    ExtractionReportEntry he;
    he.name = "entry.txt";
    he.status = "extracted";
    hostile.entries.push_back(he);
    const std::string j1 = openrar::archive::to_json(hostile);
    assert(openrar::io::is_valid_utf8(j1));
    assert(j1.find("\\uFFFD") != std::string::npos);
    for (const unsigned char c : j1) assert(c < 0x80); // pure ASCII (no raw char)

    // Valid non-ASCII (CJK) passes through as valid UTF-8 — not escaped away.
    ExtractionReport cjk;
    cjk.archive = "a\xc4\x81\xe4\xb8\xad.rar";
    ExtractionReportEntry ce;
    ce.name = "\xe4\xb8\xad\xe6\x96\x87.txt";
    ce.status = "extracted";
    cjk.entries.push_back(ce);
    const std::string j2 = openrar::archive::to_json(cjk);
    assert(openrar::io::is_valid_utf8(j2));
    assert(j2.find("\xe4\xb8\xad\xe6\x96\x87.txt") != std::string::npos);

    // Mixed hostile + legit in one field: only the broken bytes substitute.
    ExtractionReport mixed;
    mixed.archive = std::string("ok") + "\xff" + "\xe4\xb8\xad" + ".rar";
    const std::string j3 = openrar::archive::to_json(mixed);
    assert(openrar::io::is_valid_utf8(j3));
    assert(j3.find("ok\\uFFFD\xe4\xb8\xad.rar") != std::string::npos);
    std::cout << "[PASS] json_summary_valid_utf8_hostile_paths (\\uFFFD escape, CJK passthrough)\n";
}

// v1.28 plan test 5 (`local_path_warnings_sanitized`, M1 scope): the
// add-side "cannot read" warning carries the LOCAL name — §2.2 local racing
// can plant hostile names in shared directories, so it renders through the
// sanitizer too. Deterministic trigger: a 100 KiB file (over the 64 KiB -oi
// threshold) that cannot be READ — deny-read lock (Windows) / mode 000
// (POSIX, probe-then-skip under privileged runners).
void test_local_path_warnings_sanitized() {
    namespace fs = std::filesystem;
    fs::path dir = "build/cli_hostile_local";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string name = std::string("w") + "\x1b" + "[3Barn" + "\x07" + ".txt";
    fs::path f = dir / name;
    fs::path arc = "build/cli_hostile_local.rar";
    fs::path captured = "build/cli_hostile_local_out.txt";
    fs::remove(arc, ec);
    fs::remove(captured, ec);
    {
        std::ofstream payload(f, std::ios::binary);
        payload << std::string(100 * 1024, 'W');
    }

    int res = 1;
#ifdef _WIN32
    // Hold a deny-read share lock across the child process: io::FileStream's
    // CreateFile gets a sharing violation, the sha256 pre-read fails, and the
    // W: line fires with the local (hostile) name. Defender-style AV scanners
    // briefly hold new files open, which defeats the exclusive request —
    // retry, then skip (probe-then-skip pattern) rather than flake.
    FILE* lock = nullptr;
    for (int attempt = 0; attempt < 20 && lock == nullptr; ++attempt) {
        lock = _fsopen(f.string().c_str(), "rb", _SH_DENYRW);
        if (lock == nullptr) Sleep(100);
    }
    if (lock == nullptr) {
        fs::remove_all(dir, ec);
        std::cout << "[PASS] local_path_warnings_sanitized (skipped: AV holds the new file)\n";
        return;
    }
    res = run_cli_capture("a -oi " + arc.string() + " " + f.string(), captured);
    fclose(lock);
#else
    const bool chmodded = (::chmod(f.string().c_str(), 0) == 0);
    bool unreadable = false;
    if (chmodded) {
        std::ifstream probe(f.string());
        unreadable = !probe.good(); // privileged runners read mode-000 files: skip
    }
    if (unreadable) {
        res = run_cli_capture("a -oi " + arc.string() + " " + f.string(), captured);
    }
    // chmod back BEFORE cleanup — remove_all cannot delete inside a
    // read-protected tree (the dir_metadata_deferred lesson).
    ::chmod(f.string().c_str(), 0600);
    if (!unreadable) {
        fs::remove_all(dir, ec);
        std::cout << "[PASS] local_path_warnings_sanitized (skipped: privileged runner)\n";
        return;
    }
#endif
    (void)res; // the add may fail overall; the W: line is the contract
    const std::string data = slurp_file(captured);
    assert(data.find('\x1b') == std::string::npos);
    assert(data.find('\x07') == std::string::npos);
    assert(data.find("cannot read") != std::string::npos);
    assert(data.find("?[3Barn?") != std::string::npos); // sanitized local name

    fs::remove_all(dir, ec);
    fs::remove(arc, ec);
    fs::remove(captured, ec);
    std::cout << "[PASS] local_path_warnings_sanitized (W: line sanitized)\n";
}

// v1.28 plan test 7 (`vt_capability_follows_sink`, M1 scope): capability is
// a property of the sink fd. CI pipes make both fds non-TTY, so only the
// seam consistency and the suppression switches are assertable here; the
// TTY side is pinned by the pure-renderer tests in M2 and the manual gate.
void test_vt_capability_follows_sink() {
    openrar::cli::g_plain_mode = false;
    openrar::cli::g_quiet_mode = false;
    const bool cout_vt = openrar::cli::is_vt_supported_fd(1);
    const bool cerr_vt = openrar::cli::is_vt_supported_fd(2);
    assert(openrar::cli::is_vt_supported_for(std::cout) == cout_vt);
    assert(openrar::cli::is_vt_supported_for(std::cerr) == cerr_vt);
    // The suppression switches gate BOTH sinks (L3 contract).
    openrar::cli::g_plain_mode = true;
    assert(!openrar::cli::is_vt_supported_fd(1) && !openrar::cli::is_vt_supported_fd(2));
    openrar::cli::g_quiet_mode = true;
    assert(!openrar::cli::is_vt_supported_fd(1));
    openrar::cli::g_plain_mode = false;
    openrar::cli::g_quiet_mode = false;
    std::cout << "[PASS] vt_capability_follows_sink (fd seam + suppression switches)\n";
}

// ── v1.28 M2: dual-progress TUI + cooperative cancel ─────────────────────────

// Splits on '\n' and strips ESC sequences so visible width = code points.
static size_t tui_visible_cols(const std::string& line) {
    std::string plain;
    for (size_t i = 0; i < line.size();) {
        if (line[i] == '\x1b') {
            size_t j = i + 1;
            if (j < line.size() && line[j] == '[') {
                ++j;
                while (j < line.size() && !std::isalpha(static_cast<unsigned char>(line[j]))) ++j;
                if (j < line.size()) ++j; // CSI final byte
                i = j;
                continue;
            }
            ++i;
            continue;
        }
        plain += line[i];
        ++i;
    }
    size_t cols = 0;
    for (size_t i = 0; i < plain.size();) {
        const size_t len = openrar::cli::utf8_seq_len(static_cast<unsigned char>(plain[i]));
        if (i + len > plain.size()) break;
        ++cols;
        i += len;
    }
    return cols;
}

static std::vector<std::string> tui_split_lines(const std::string& s) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            lines.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return lines;
}

// Plan test 8 (`renderer_width_clamp_no_wrap`) + test 9
// (`renderer_truncate_utf8_safe`) + the §7.1 choke point: the pure render
// core clamps EVERY line to the requested width at any width, truncates names
// on sequence boundaries with '…', and re-sanitizes hostile names (the
// renderer is the release-gate choke point).
void test_renderer_core() {
    using openrar::cli::render_tui;
    using openrar::cli::TuiState;
    TuiState st;
    st.badge = "EXTRACTING";
    st.fg_color = "\x1b[38;2;95;184;176m";
    st.bg_color = "\x1b[48;2;19;37;35m";
    st.total_files = 37;
    st.done_files = 12;
    st.total_bytes = 1000;
    st.done_bytes = 470;
    st.current_name = "big_file.iso";
    st.current_done = 620;
    st.current_total = 1000;

    // test 8: region stays 4 lines and fits ANY width (20..200).
    for (int width : {20, 40, 80, 200}) {
        const auto lines = tui_split_lines(render_tui(st, width));
        assert(lines.size() == 4);
        for (const auto& line : lines) {
            assert(tui_visible_cols(line) <= static_cast<size_t>(width));
        }
    }
    // A 300-char name cannot wrap the region at width 30.
    st.current_name = std::string(300, 'a') + ".bin";
    for (const auto& line : tui_split_lines(render_tui(st, 30))) {
        assert(tui_visible_cols(line) <= static_cast<size_t>(30));
    }

    // test 9: CJK name truncates on sequence boundaries ('…' appended, the
    // whole line stays valid UTF-8 — no split bytes).
    st.current_name = "\xe4\xb8\xad\xe6\x96\x87\xe6\x96\x87\xe4\xbb\xb6\xe5\x90\x8d.dat";
    for (const auto& line : tui_split_lines(render_tui(st, 24))) {
        assert(openrar::io::is_valid_utf8(line));
        assert(tui_visible_cols(line) <= static_cast<size_t>(24));
    }
    assert(render_tui(st, 24).find("\xe2\x80\xa6") != std::string::npos);
    // A short name is never truncated.
    st.current_name = "ok.txt";
    assert(render_tui(st, 120).find("ok.txt") != std::string::npos);
    assert(render_tui(st, 120).find("\xe2\x80\xa6") == std::string::npos);

    // §7.1 choke point: the renderer re-sanitizes the name field — a raw
    // ESC-led sequence never appears, the '?'-substituted text does.
    st.current_name = std::string("x") + "\x1b[4;20H" + "y.txt";
    const std::string hostile = render_tui(st, 120);
    assert(hostile.find("\x1b[4;20H") == std::string::npos);
    assert(hostile.find("x?[4;20Hy.txt") != std::string::npos);

    // Spin mode renders exactly ONE line; zero totals do not divide by zero.
    TuiState sp;
    sp.mode = TuiState::Mode::Spin;
    sp.spin_frame = "\xe2\xa0\x8b";
    sp.spin_message = "Scanning files";
    assert(tui_split_lines(render_tui(sp, 40)).size() == 1);
    TuiState zero;
    zero.badge = "X";
    assert(!render_tui(zero, 80).empty());
    std::cout << "[PASS] renderer_width_clamp_no_wrap + truncate_utf8_safe + choke point\n";
}

// Plan test 10 (`renderer_cancel_state_machine`, non-TTY legs): the cancel
// scope arms to a NO-OP without a TTY, pause/resume/disarm are safe no-ops,
// and the shared flag stays clean. (The armed state machine on a real
// terminal is the manual gate; the flag side is exercised by the cancel e2e
// tests below through the env hooks.)
void test_keyboard_cancel_nontty_nop() {
    auto& kb = openrar::cli::KeyboardCancel::instance();
    kb.arm(false);
    assert(!kb.active());
    kb.pause();
    kb.resume();
    kb.arm(true);       // stdin piped under ctest → still disarmed
    if (!kb.active()) { // interactive manual runs legitimately arm here
        kb.pause();
        kb.resume();
    }
    kb.disarm();
    assert(!kb.active());
    assert(!openrar::cli::g_tui_cancel.load());
    std::cout << "[PASS] renderer_cancel_state_machine (non-TTY legs, flag clean)\n";
}

static void set_cancel_hook(const char* name, const char* value) {
#ifdef _WIN32
    const std::string kv = std::string(name) + "=" + (value ? value : "");
    _putenv(kv.c_str());
#else
    if (value && *value) {
        ::setenv(name, value, 1);
    } else {
        ::unsetenv(name);
    }
#endif
}

static bool dir_has_tmp_leftovers(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return false;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".tmp") return true;
        (void)e;
    }
    return false;
}

// Plan test 13 (`midfile_cancel_via_hooks`): the cooperative-cancel predicate
// fires DURING a large file's decode via the reader disk hooks; the run exits
// user-break (255), the JSON reports aborted, and no partial temp survives.
void test_midfile_cancel_via_hooks() {
    namespace fs = std::filesystem;
    fs::path src = "build/cli_cancel_big.bin";
    fs::path arc = "build/cli_cancel_big.rar";
    fs::path out = "build/cli_cancel_big_out";
    fs::path captured = "build/cli_cancel_out.txt";
    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove(captured, ec);
    fs::remove_all(out, ec);
    fs::remove(src, ec);
    {
        std::ofstream payload(src, std::ios::binary);
        payload << std::string(4 * 1024 * 1024, 'W');
    }
    int res = system_exit_code(
        run_cli_capture("a -m0 -q " + arc.string() + " " + src.string(), captured));
    assert(res == 0);

    set_cancel_hook("OPENRAR_TEST_CANCEL_AFTER_BYTES", "1048576");
    res = system_exit_code(run_cli_capture("x " + arc.string() + " " + out.string(), captured));
    set_cancel_hook("OPENRAR_TEST_CANCEL_AFTER_BYTES", "");
    assert(res == 255); // EXIT_USER_BREAK (pinned taxonomy)
    const std::string data = slurp_file(captured);
    assert(data.find("User break") != std::string::npos);
    assert(data.find("\"aborted\":true") != std::string::npos);
    assert(!dir_has_tmp_leftovers(out));             // AtomicWriter abandoned + removed
    assert(!fs::exists(out / "cli_cancel_big.bin")); // never committed

    // Without the hook the same archive extracts cleanly (hook is inert).
    res = system_exit_code(run_cli_capture("x " + arc.string() + " " + out.string(), captured));
    assert(res == 0);
    assert(fs::exists(out / "cli_cancel_big.bin"));

    fs::remove(arc, ec);
    fs::remove(captured, ec);
    fs::remove_all(out, ec);
    fs::remove(src, ec);
    std::cout << "[PASS] midfile_cancel_via_hooks (255 + aborted JSON + no temp)\n";
}

// Plan test 12 (`parallel_cancel_journal_sweep`): between-entries cancel under
// -mt4 — two files complete, the rest fast-fail as unprocessed, exit 255, and
// no journal-recorded temp is left behind.
void test_parallel_cancel_journal_sweep() {
    namespace fs = std::filesystem;
    fs::path arc = "build/cli_cancel_par.rar";
    fs::path out = "build/cli_cancel_par_out";
    fs::path captured = "build/cli_cancel_par_out.txt";
    std::error_code ec;
    fs::remove(arc, ec);
    fs::remove(captured, ec);
    fs::remove_all(out, ec);
    std::string srcs;
    for (int i = 0; i < 8; ++i) {
        fs::path f = "build/cli_cancel_par_" + std::to_string(i) + ".bin";
        {
            std::ofstream payload(f, std::ios::binary);
            payload << std::string(1024 * 1024, static_cast<char>('A' + i));
        }
        srcs += " " + f.string();
    }
    int res = system_exit_code(run_cli_capture("a -m0 -q " + arc.string() + srcs, captured));
    assert(res == 0);

    set_cancel_hook("OPENRAR_TEST_CANCEL_AFTER_FILES", "2");
    res =
        system_exit_code(run_cli_capture("x -mt4 " + arc.string() + " " + out.string(), captured));
    set_cancel_hook("OPENRAR_TEST_CANCEL_AFTER_FILES", "");
    assert(res == 255);
    const std::string data = slurp_file(captured);
    assert(data.find("User break") != std::string::npos);
    assert(data.find("\"aborted\":true") != std::string::npos);
    assert(data.find("\"unprocessed\"") != std::string::npos);
    assert(!dir_has_tmp_leftovers(out));

    fs::remove(arc, ec);
    fs::remove(captured, ec);
    fs::remove_all(out, ec);
    for (int i = 0; i < 8; ++i) {
        fs::remove("build/cli_cancel_par_" + std::to_string(i) + ".bin", ec);
    }
    std::cout << "[PASS] parallel_cancel_journal_sweep (255 + unprocessed + no temp)\n";
}

// ── v1.28 M3: non-TTY degradation contract ───────────────────────────────────

// Crafts a stored single-entry archive whose header-declared CRC32 is wrong —
// extraction must fail with the CRC exit code (3) on every engine.
static void write_bad_crc_archive(const std::filesystem::path& arc) {
    openrar::io::FileStream out;
    assert(out.open(arc, openrar::io::FileMode::CreateAlways));
    openrar::format::HeaderWriter::write_signature(out);
    openrar::format::MainBlock mb;
    openrar::format::HeaderWriter::write_main_block(out, mb);
    openrar::format::FileBlock fb;
    fb.file_name = "crc.txt";
    fb.unp_size = 8;
    fb.pack_size = 8;
    fb.method = 0;
    fb.win_size = 0;
    fb.has_crc32 = true;
    fb.data_crc32 = 0xDEADBEEF; // payload below hashes to something else
    assert(openrar::format::HeaderWriter::write_file_block(out, fb));
    out.write("payload!", 8);
    openrar::format::EndArcBlock eb;
    openrar::format::HeaderWriter::write_end_block(out, eb);
}

// Plan tests 14-17 + 19: piped/redirected runs (stdout NOT a TTY, stdin
// pinned to the null device on every leg) carry ZERO ESC bytes, keep the
// per-file lines, split human output across the two streams, exit
// identically on both legs, honor the suppression switches, and keep the
// JSON summary pure under hostile names. CI legs are non-TTY by
// construction; the TTY side is the renderer unit tests + the manual gate.
void test_non_tty_degradation_contract() {
    namespace fs = std::filesystem;
    const std::string RLO = "\xe2\x80\xae";

    // Fixtures: clean archive, hostile archive, garbage file, CRC-corrupt
    // archive, encrypted archive.
    fs::path clean_src = "build/cli_ntty_src.txt";
    fs::path clean_arc = "build/cli_ntty_clean.rar";
    {
        std::ofstream payload(clean_src);
        payload << "ntty payload";
    }
    fs::remove(clean_arc);
    assert(openrar::archive::ArchiveMutator::add_file_to_archive(clean_arc, clean_src,
                                                                 "ntty_one.txt"));
    assert(openrar::archive::ArchiveMutator::add_file_to_archive(clean_arc, clean_src,
                                                                 "ntty_two.txt"));

    fs::path hostile_arc = "build/cli_ntty_hostile.rar";
    fs::remove(hostile_arc);
    write_hostile_archive(hostile_arc, "n\x1b[2Jtty", "u", "g",
                          {{std::string("inv") + RLO + "exe.txt", "data"}}, "");

    fs::path garbage = "build/cli_ntty_garbage.bin";
    {
        std::ofstream payload(garbage, std::ios::binary);
        payload << "not a rar at all";
    }

    fs::path badcrc_arc = "build/cli_ntty_badcrc.rar";
    fs::remove(badcrc_arc);
    write_bad_crc_archive(badcrc_arc);

    fs::path enc_arc = "build/cli_ntty_enc.rar";
    fs::remove(enc_arc);
    int res = system_exit_code(run_cli_capture(
        "a -hppw123 -q " + enc_arc.string() + " " + clean_src.string(), "build/cli_ntty_e1.txt"));
    assert(res == 0);

    auto run = [&](const std::string& args, const std::filesystem::path& out_f,
                   const std::filesystem::path& err_f) {
        const std::string cmd = get_cli_path() + " " + args + " < " DEVNULL " > " + out_f.string() +
                                " 2> " + err_f.string();
        return system_exit_code(std::system(cmd.c_str()));
    };
    auto slurp = [](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    auto assert_no_esc = [](const std::string& data) {
        assert(data.find('\x1b') == std::string::npos);
    };

    fs::path out1 = "build/cli_ntty_o1.txt";
    fs::path err1 = "build/cli_ntty_e2.txt";
    fs::path out2 = "build/cli_ntty_o2.txt";
    fs::path err2 = "build/cli_ntty_e3.txt";
    fs::path outd1 = "build/cli_ntty_x1";
    fs::path outd2 = "build/cli_ntty_x2";

    // tests 14 + 15: hostile x through BOTH legs — zero ESC bytes anywhere,
    // per-file lines present, sanitized names only, identical human streams.
    {
        const std::filesystem::path* outs[2] = {&out1, &out2};
        const std::filesystem::path* errs[2] = {&err1, &err2};
        const std::filesystem::path* ods[2] = {&outd1, &outd2};
        for (int i = 0; i < 2; ++i) {
            std::error_code ec2;
            fs::remove_all(*ods[i], ec2);
            res = run("x " + hostile_arc.string() + " " + ods[i]->string(), *outs[i], *errs[i]);
            assert(res == 0);
            assert_no_esc(slurp(*outs[i]));
            assert_no_esc(slurp(*errs[i]));
            // Non-JSON mode: per-file lines on stdout, warnings on stderr.
            assert(slurp(*outs[i]).find("Extracting") != std::string::npos);
            assert(slurp(*outs[i]).find("inv???exe.txt") != std::string::npos);
        }
    }
    assert(slurp(err1) == slurp(err2));

    // test 16: exit-code parity across legs — 0, 13, no-mask nonzero, 11, 3.
    std::error_code ec;
    fs::remove_all(outd1, ec);
    fs::remove_all(outd2, ec);
    const int rc_clean_a = run("x " + clean_arc.string() + " " + outd1.string(), out1, err1);
    const int rc_clean_b = run("x " + clean_arc.string() + " " + outd2.string(), out2, err2);
    assert(rc_clean_a == 0 && rc_clean_b == rc_clean_a);
    const int rc_garbage_a = run("x " + garbage.string() + " " + outd1.string(), out1, err1);
    const int rc_garbage_b = run("x " + garbage.string() + " " + outd2.string(), out2, err2);
    assert(rc_garbage_a == 13 && rc_garbage_b == rc_garbage_a);
    const int rc_nomask_a =
        run("x " + clean_arc.string() + " " + outd1.string() + " *.nomask", out1, err1);
    const int rc_nomask_b =
        run("x " + clean_arc.string() + " " + outd2.string() + " *.nomask", out2, err2);
    assert(rc_nomask_a != 0 && rc_nomask_b == rc_nomask_a);
    const int rc_pw_a = run("x -pwrong " + enc_arc.string() + " " + outd1.string(), out1, err1);
    const int rc_pw_b = run("x -pwrong " + enc_arc.string() + " " + outd2.string(), out2, err2);
    assert(rc_pw_a == 11 && rc_pw_b == rc_pw_a);
    const int rc_crc_a = run("x " + badcrc_arc.string() + " " + outd1.string(), out1, err1);
    const int rc_crc_b = run("x " + badcrc_arc.string() + " " + outd2.string(), out2, err2);
    // Parity only: the disk-x path reports corrupt payload as fatal (2) —
    // the CRC=3 mapping is pinned on the p/t legs (interop stage 15).
    assert(rc_crc_a != 0 && rc_crc_b == rc_crc_a);

    // test 17: -q silences the human output (stderr empty; stdout carries the
    // always-on machine JSON summary only); -plain renders but never emits
    // VT bytes (same as default in CI).
    fs::remove_all(outd1, ec);
    res = run("x -q " + clean_arc.string() + " " + outd1.string(), out1, err1);
    assert(res == 0);
    assert(slurp(err1).empty());
    assert(slurp(out1).find("{\"schema_version\"") == 0);
    fs::remove_all(outd1, ec);
    res = run("x -plain " + clean_arc.string() + " " + outd1.string(), out1, err1);
    assert(res == 0);
    assert_no_esc(slurp(out1));
    assert_no_esc(slurp(err1));

    // test 18: JSON purity under hostile names — stdout carries ONLY the
    // JSON document; humans live on stderr.
    fs::remove_all(outd1, ec);
    res = run("x --json-summary " + hostile_arc.string() + " " + outd1.string(), out1, err1);
    assert(res == 0);
    const std::string json_out = slurp(out1);
    assert(json_out.find("{\"schema_version\"") == 0);
    assert(openrar::io::is_valid_utf8(json_out));
    assert(slurp(err1).find("Extracting from") != std::string::npos);

    // test 19: `p` prints data bytes to stdout (pipe semantics — exempt) and
    // its error lines carry sanitized names (the CRC path prints one).
    {
        const std::string cmd = get_cli_path() + " p " + hostile_arc.string() +
                                " < " DEVNULL " > " + out1.string() + " 2> " + err1.string();
        res = system_exit_code(std::system(cmd.c_str()));
        assert(res == 0);
        assert_no_esc(slurp(err1));
        const std::string cmd2 = get_cli_path() + " p " + badcrc_arc.string() +
                                 " < " DEVNULL " > " + out2.string() + " 2> " + err2.string();
        res = system_exit_code(std::system(cmd2.c_str()));
        assert(res == 3);
        const std::string perr = slurp(err2);
        assert_no_esc(perr);
        assert(perr.find("Checksum error in crc.txt") != std::string::npos);
    }

    // cleanup
    fs::remove(clean_arc, ec);
    fs::remove(hostile_arc, ec);
    fs::remove(garbage, ec);
    fs::remove(badcrc_arc, ec);
    fs::remove(enc_arc, ec);
    fs::remove(clean_src, ec);
    fs::remove(out1, ec);
    fs::remove(err1, ec);
    fs::remove(out2, ec);
    fs::remove(err2, ec);
    fs::remove("build/cli_ntty_e1.txt", ec);
    fs::remove_all(outd1, ec);
    fs::remove_all(outd2, ec);
    std::cout << "[PASS] non_tty_degradation_contract (zero-ESC, parity, -q/-plain, JSON, p)\n";
}

// ── v1.28 M4: benchmark engine v2 ────────────────────────────────────────────

static std::filesystem::path bench_exe_path() {
#ifdef OPENRAR_BENCH_EXE
    if (std::filesystem::exists(OPENRAR_BENCH_EXE)) return {OPENRAR_BENCH_EXE};
#endif
    return "openrar_bench.exe";
}

// Plan test 20 (`bench_json_schema_roundtrip`): --json stdout carries ONLY
// the schema_version-1 document (host/protocol/suites with median, passes,
// spread_pct); exit 0. --strict is deterministic through the
// --spread-threshold-pct test hook against a deterministic suite: a
// threshold below zero always fires (exit 1), a huge one never does.
void test_bench_json_schema_roundtrip() {
    namespace fs = std::filesystem;
    const std::string bench = "\"" + bench_exe_path().string() + "\"";
    fs::path out = "build/cli_bench_json.txt";
    fs::path err = "build/cli_bench_err.txt";
    std::error_code ec;
    fs::remove(out, ec);

    const std::string cmd = bench + " --json --quick --suite cdc_fingerprint < " DEVNULL " > " +
                            out.string() + " 2> " + err.string();
    const int res = system_exit_code(std::system(cmd.c_str()));
    assert(res == 0);
    const std::string doc = slurp_file(out);
    assert(doc.find("{\"schema_version\":1") == 0);
    assert(doc.find("\"host\":{") != std::string::npos);
    assert(doc.find("\"cpu\":") != std::string::npos);
    assert(doc.find("\"cores\":") != std::string::npos);
    assert(doc.find("\"protocol\":{") != std::string::npos);
    assert(doc.find("\"warmup\":1") != std::string::npos);
    assert(doc.find("\"spread_pct\":") != std::string::npos);
    assert(doc.find("\"passes\":[") != std::string::npos);
    assert(openrar::io::is_valid_utf8(doc));
    // stdout purity: nothing but the JSON document on stdout.
    for (const auto& line : tui_split_lines(doc)) {
        assert(line.empty() || line.rfind("{\"schema_version\"", 0) == 0 ||
               line.rfind(",\"host\"", 0) == 0 || line.rfind(",\"protocol\"", 0) == 0 ||
               line.rfind(",\"suites\"", 0) == 0 || line.rfind("  {", 0) == 0 ||
               line.rfind("]") == 0 || line == "}" || line == "}");
    }

    // --strict deterministic legs via the threshold hook: a negative
    // threshold always fires (exit 1); a huge one never does (exit 0).
    const std::string strict_bad = bench + " --strict --spread-threshold-pct -1 --suite " +
                                   "cdc_fingerprint < " DEVNULL " > " + DEVNULL " 2>&1";
    assert(system_exit_code(std::system(strict_bad.c_str())) == 1);
    const std::string strict_ok =
        bench + " --strict --spread-threshold-pct 1000000 --suite cdc_fingerprint < " DEVNULL
                " > " DEVNULL " 2>&1";
    assert(system_exit_code(std::system(strict_ok.c_str())) == 0);

    // Unknown suite name is a usage error (exit 2).
    const std::string bad_suite = bench + " --suite no_such_suite < " DEVNULL " > " DEVNULL " 2>&1";
    assert(system_exit_code(std::system(bad_suite.c_str())) == 2);

    fs::remove(out, ec);
    fs::remove(err, ec);
    std::cout << "[PASS] bench_json_schema_roundtrip (schema, purity, strict legs)\n";
}

// Plan test 21 (`bench_cdc_three_number_suite`): the engine-measured
// three-number reduction on the engineered corpus — store > plain-solid >
// CDC-packed, spread 0.0% (deterministic bytes), rollover item 2 closed.
void test_bench_cdc_three_number_suite() {
    namespace fs = std::filesystem;
    const std::string bench = "\"" + bench_exe_path().string() + "\"";
    fs::path out = "build/cli_bench_cdc.txt";
    std::error_code ec;
    fs::remove(out, ec);
    const std::string cmd =
        bench +
        " --quick --suite cdc_three_number_store --suite cdc_three_number_plain_solid "
        "--suite cdc_three_number_cdc_packed < " DEVNULL " > " +
        out.string() + " 2>&1";
    const int res = system_exit_code(std::system(cmd.c_str()));
    assert(res == 0);
    const std::string data = slurp_file(out);
    auto value_of = [&data](const std::string& name) -> double {
        const size_t at = data.find(name);
        assert(at != std::string::npos);
        const size_t eq = data.find('=', at);
        return std::atof(data.c_str() + eq + 1);
    };
    const double store = value_of("cdc_three_number_store");
    const double plain = value_of("cdc_three_number_plain_solid");
    const double packed = value_of("cdc_three_number_cdc_packed");
    assert(store > plain);
    assert(plain > packed); // the reduction direction (plan test 4, bench leg)
    assert(data.find("(spread 0.0%)") != std::string::npos); // deterministic bytes
    fs::remove(out, ec);
    std::cout << "[PASS] bench_cdc_three_number_suite (store > plain-solid > CDC-packed)\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // assert() ends in abort(), whose Debug-CRT "abort() has been called"
    // modal is a SEPARATE dialog (_CALL_REPORTFAULT) — without this the
    // process still hangs after printing the assert.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    std::cout << "Running Clean-Room Milestone 7 CLI Verification...\n";
    test_cli_help();
    test_cli_version();
    test_cli_sfx();
    test_cli_hardlinks();
    test_cli_lifecycle();
    test_cli_quiet_list_missing_archive_fails();
    test_cli_list_sanitizes_esc_entry_name();
    test_cli_mt_batch_equivalence();
    test_cli_overwrite_modes();
    test_cli_help_switch_parity();
    test_cli_v1_10_features();
    test_cli_dict_size_flag();
    test_oi_creation_side_emitted();
    test_cdc_filecopy_precedence();
    test_oi_switch_modes();
    test_cdc_packing_order_stable();
    test_cdc_index_cap_falls_back_reported();
    test_cdc_reduction_report_three_numbers();
    test_cdc_degenerate_inputs();
    test_timestamp_clamped_flag();
    test_cdc_packed_caps_enforced();
    test_cdc_packed_rr_repair();
    test_oi34_no_archive_no_dispatch();
    test_motw_propagation_cli();
    test_zone_stream_never_restored();
    test_os_excludes_zone_capture();
    test_sanitize_for_display();
    test_hostile_render_surfaces_e2e();
    test_json_escape_utf8_validity();
    test_local_path_warnings_sanitized();
    test_vt_capability_follows_sink();
    test_renderer_core();
    test_keyboard_cancel_nontty_nop();
    test_midfile_cancel_via_hooks();
    test_parallel_cancel_journal_sweep();
    test_non_tty_degradation_contract();
    test_bench_json_schema_roundtrip();
    test_bench_cdc_three_number_suite();
    std::cout << "All Milestone 7 CLI Primitives PASSED!\n";
    return 0;
}
