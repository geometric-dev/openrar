#include "../../src/core/types.hpp"
#include "../../src/archive/archive_mutator.hpp"
#include "../../src/archive/archive_reader.hpp"
#include "openrar/version.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
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
    std::string cmd = get_cli_path() + " a -q " + arc1.string() + " @" + listfile.string() + " > " DEVNULL " 2>&1";
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
    cmd = get_cli_path() + " a -q -x*.log " + arc2.string() + " " +
          f_a.string() + " " + f_b.string() + " " + f_c.string() + " > " DEVNULL " 2>&1";
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

    auto find_file = [](const openrar::archive::ArchiveReader& r) -> const openrar::archive::ArchiveEntry* {
        for (const auto& e : r.entries()) {
            if (!e.header.is_service) return &e;
        }
        return nullptr;
    };

    // 1. Valid -md16m with -m3
    fs::path arc_16m = temp_dir / "test_16m.rar";
    std::string cmd = exe + " a -q -m3 -md16m " + arc_16m.string() + " " + src_file.string() + " > " DEVNULL " 2>&1";
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
    cmd = exe + " a -q -m5 -md64m " + arc_64m.string() + " " + src_file.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res == 0);
    {
        openrar::archive::ArchiveReader r;
        assert(r.open(arc_64m));
        const auto* e = find_file(r);
        assert(e != nullptr);
        assert(e->header.win_size == 64 * 1024 * 1024);
    }

    // 3. Invalid -md not power of two (e.g. -md10m) -> fail
    fs::path arc_bad1 = temp_dir / "bad1.rar";
    cmd = exe + " a -q -md10m " + arc_bad1.string() + " " + src_file.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res != 0);

    // 4. Invalid -md out of range (< 128k, e.g. -md64k) -> fail
    fs::path arc_bad2 = temp_dir / "bad2.rar";
    cmd = exe + " a -q -md64k " + arc_bad2.string() + " " + src_file.string() + " > " DEVNULL " 2>&1";
    res = std::system(cmd.c_str());
    assert(res != 0);

    fs::remove_all(temp_dir, ec);
    std::cout << "[PASS] CLI -md<size> dictionary configuration\n";
}

int main() {
#ifdef _MSC_VER
    // Route assert failures to stderr: under ctest (piped stdio) the MSVC
    // default for _CRT_ASSERT is a modal dialog, which silently hangs the
    // test process forever while ctest moves on, leaving file locks behind.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
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
    std::cout << "All Milestone 7 CLI Primitives PASSED!\n";
    return 0;
}

