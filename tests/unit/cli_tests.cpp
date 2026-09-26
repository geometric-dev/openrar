#include "../../src/core/types.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
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
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#endif
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

// Null device for output redirection: "nul" is only a device on Windows; on
// POSIX it would silently create a regular file named "nul" in the CWD.
#ifdef _WIN32
#define DEVNULL "nul"
#else
#define DEVNULL "/dev/null"
#endif

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

    std::cout << "[PASS] sanitize_for_display negative coverage (ESC/CSI/OSC, C1, bidi, "
                 "invalid UTF-8)\n";
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
        "-ed", "-ep",  "-ep1",  "-ep2", "-ep3", "-ol",      "-ol-",      "-os",  "-ow", "-plain",
        "-q",  "-r",   "-r-",   "-s",   "-sfx", "-y",       "-kb",       "-o+",  "-o-", "-vp",
        "-m3", "-mt1", "-md1m", "-tsm", "-v1k", "-psecret", "-hpsecret", "-rr3",
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
        int res = std::system(cmd.c_str());
        assert(res == 7); // EXIT_USAGE
        assert(!fs::exists(arc));
    }

    // -oi with multi-volume is refused fail-closed (exit 7), no archive.
    {
        fs::path arc = temp_dir / "vol.rar";
        std::string cmd = exe + " a -oi1 -v1m " + arc.string() + " " + m1.string() + " " +
                          m2.string() + " > " DEVNULL " 2>&1";
        int res = std::system(cmd.c_str());
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
    test_sanitize_for_display();
    std::cout << "All Milestone 7 CLI Primitives PASSED!\n";
    return 0;
}
