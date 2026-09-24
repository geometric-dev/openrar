#include "../core/types.hpp"
#include "../io/file_stream.hpp"
#include "../io/path_util.hpp"
#include "../format/headers.hpp"
#include "../archive/archive_reader.hpp"
#include "../sfx/sfx_config.hpp"
#include "../sfx/sfx_consent.hpp"
#include "../sfx/sfx_pipeline.hpp"
#include "../sfx/process_exec.hpp"
#include "../sfx/prompt_console.hpp"
#include "progress.hpp" // shared ANSI progress; SFX extraction mirrors cli via CLIProgress reuse for creation

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace openrar::sfx {

bool g_plain_mode = false;
bool g_quiet_mode = false;

bool is_vt_supported() {
    if (g_plain_mode || g_quiet_mode) return false;
    if (getenv("NO_COLOR") != nullptr) return false;
#ifdef _WIN32
    HANDLE out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (_isatty(_fileno(stdout)) && GetConsoleMode(out_handle, &mode)) {
        SetConsoleMode(out_handle, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
        return true;
    }
    return false;
#else
    const char* term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) return false;
    return isatty(fileno(stdout));
#endif
}

class SFXProgress {
private:
    static const int BAR_WIDTH = 28;
    const char* SPIN_FRAMES[10] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

    std::string state_name_;
    std::string fg_color_;
    std::string bg_color_;
    size_t total_files_ = 0;
    size_t processed_files_ = 0;
    core::uint64 total_bytes_ = 0;
    core::uint64 processed_bytes_ = 0;
    std::string current_file_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_render_time_;
    int spin_frame_ = 0;
    int lines_drawn_ = 0;
    bool active_ = false;

    static std::string format_eta(double seconds) {
        if (seconds < 0 || seconds > 360000) return "--:--";
        int s = static_cast<int>(seconds + 0.5);
        int h = s / 3600;
        int m = (s % 3600) / 60;
        int sec = s % 60;
        std::ostringstream oss;
        if (h > 0) {
            oss << h << ":" << std::setfill('0') << std::setw(2) << m << ":" << std::setfill('0')
                << std::setw(2) << sec;
        } else {
            oss << m << ":" << std::setfill('0') << std::setw(2) << sec;
        }
        return oss.str();
    }

public:
    void init(const std::string& state_name, const std::string& fg, const std::string& bg) {
        clear();
        state_name_ = state_name;
        fg_color_ = fg;
        bg_color_ = bg;
        total_files_ = 0;
        processed_files_ = 0;
        total_bytes_ = 0;
        processed_bytes_ = 0;
        current_file_ = "";
        start_time_ = std::chrono::steady_clock::now();
        last_render_time_ = std::chrono::steady_clock::now();
        spin_frame_ = 0;
        lines_drawn_ = 0;
        active_ = true;
    }

    void set_totals(size_t total_files, core::uint64 total_bytes) {
        total_files_ = total_files;
        total_bytes_ = total_bytes;
    }

    void spin(const std::string& message) {
        if (!is_vt_supported()) return;
        if (lines_drawn_ > 0) {
            std::cout << "\x1b[" << lines_drawn_ << "A";
        }
        std::cout << "\x1b[2K\r  \x1b[38;2;95;184;176m" << SPIN_FRAMES[spin_frame_]
                  << "\x1b[0m  \x1b[38;2;109;114;128m" << message << "\x1b[0m\n"
                  << std::flush;
        lines_drawn_ = 1;
        spin_frame_ = (spin_frame_ + 1) % 10;
    }

    void start_file(const std::string& filename, size_t file_index) {
        // L11: SFX entry names are archive-controlled; strip control chars
        // before they reach the ANSI-rendered progress lines (display only).
        current_file_ = openrar::cli::sanitize_for_display(filename);
        processed_files_ = file_index;
        render();
    }

    void update_bytes(core::uint64 bytes_added) {
        processed_bytes_ += bytes_added;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render_time_)
                    .count() >= 30 ||
            processed_bytes_ >= total_bytes_) {
            render();
        }
    }

    void render() {
        if (!active_ || !is_vt_supported()) return;
        last_render_time_ = std::chrono::steady_clock::now();

        double pct = 0.0;
        if (total_bytes_ > 0)
            pct = static_cast<double>(processed_bytes_) / static_cast<double>(total_bytes_) * 100.0;
        else if (total_files_ > 0)
            pct = static_cast<double>(processed_files_) / static_cast<double>(total_files_) * 100.0;

        if (pct < 0.0) pct = 0.0;
        if (pct > 100.0) pct = 100.0;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        double eta = -1;
        if (pct > 0.5 && elapsed > 0.2) {
            double total_time = elapsed / (pct / 100.0);
            eta = total_time - elapsed;
            if (eta < 0) eta = 0;
        }

        // Line 1: Badge
        std::string badge_line = "\x1b[1m" + fg_color_ + bg_color_ + " " + state_name_ + " \x1b[0m";

        // Line 2: Progress bar + %
        int filled = static_cast<int>((pct / 100.0) * BAR_WIDTH + 0.5);
        if (filled < 0) filled = 0;
        if (filled > BAR_WIDTH) filled = BAR_WIDTH;
        int empty = BAR_WIDTH - filled;

        std::string bar_line = "[";
        bar_line += fg_color_;
        for (int i = 0; i < filled; ++i) bar_line += "█";
        bar_line += "\x1b[38;2;69;73;85m";
        for (int i = 0; i < empty; ++i) bar_line += "░";
        bar_line += "\x1b[0m]  \x1b[38;2;231;229;223m" +
                    std::to_string(static_cast<int>(pct + 0.5)) + "%\x1b[0m";

        // Line 3: Meta
        std::string eta_str = format_eta(eta);
        std::string meta_line = "\x1b[38;2;109;114;128m";
        if (total_files_ > 0) {
            meta_line += std::to_string(processed_files_) + " / " + std::to_string(total_files_) +
                         " files  ·  ETA " + eta_str;
        } else {
            meta_line += std::to_string(processed_files_) + " files  ·  ETA " + eta_str;
        }
        meta_line += "\x1b[0m";

        // Line 4: Current file path
        std::string file_line = "\x1b[38;2;69;73;85m" + current_file_ + "\x1b[0m";

        if (lines_drawn_ > 0) {
            std::cout << "\x1b[" << lines_drawn_ << "A";
        }

        std::cout << "\x1b[2K\r  " << badge_line << "\n"
                  << "\x1b[2K\r  " << bar_line << "\n"
                  << "\x1b[2K\r  " << meta_line << "\n"
                  << "\x1b[2K\r  " << file_line << "\n"
                  << std::flush;
        lines_drawn_ = 4;
    }

    void done(size_t total_files, const std::string& verb, const std::string& sfx_name,
              const std::string& extra = "") {
        clear();
        active_ = false;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();

        std::string extra_str =
            extra.empty() ? "" : (" \x1b[38;2;109;114;128m— " + extra + "\x1b[0m");

        if (is_vt_supported()) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << elapsed;
            std::cout << "\x1b[38;2;123;193;127m✓\x1b[0m  \x1b[1;38;2;123;193;127;48;2;25;39;26m "
                         "DONE \x1b[0m\n"
                      << "\x1b[1;38;2;231;229;223m" << total_files
                      << "\x1b[0m \x1b[38;2;109;114;128mfiles " << verb
                      << " in\x1b[0m \x1b[1;38;2;231;229;223m" << oss.str() << "s\x1b[0m";
            if (!sfx_name.empty()) {
                std::cout << " \x1b[38;2;109;114;128mfrom\x1b[0m \x1b[1;38;2;231;229;223m"
                          << sfx_name << "\x1b[0m";
            }
            std::cout << extra_str << "\n\n" << std::flush;
        } else {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << elapsed;
            std::cout << "Done. " << total_files << " files " << verb << " in " << oss.str() << "s";
            if (!sfx_name.empty()) std::cout << " from " << sfx_name;
            std::cout << "\n" << std::flush;
        }
    }

    void clear() {
        if (lines_drawn_ > 0 && is_vt_supported()) {
            std::cout << "\x1b[" << lines_drawn_ << "A";
            for (int i = 0; i < lines_drawn_; ++i) {
                std::cout << "\x1b[2K\r\n";
            }
            std::cout << "\x1b[" << lines_drawn_ << "A" << std::flush;
            lines_drawn_ = 0;
        }
    }
};

std::filesystem::path get_self_path(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len > 0) {
        return std::filesystem::path(buf);
    }
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::filesystem::path(buf);
    }
#endif
    if (argv0 != nullptr && argv0[0] != '\0') {
        return std::filesystem::path(argv0);
    }
    return "";
}

} // namespace openrar::sfx

// L10: extraction touches the filesystem throughout (current_path(), target
// directory creation, ...). An escaping std::filesystem/library exception must
// produce a diagnostic and a nonzero exit, not std::terminate. Argv parsing
// above cannot throw.
static int sfx_main_impl(int argc, char* argv[]) {
    std::filesystem::path self_path = openrar::sfx::get_self_path(argc > 0 ? argv[0] : "");
    std::string sfx_filename = self_path.filename().string();

    std::string dest_dir = "";
    std::string password = "";
    bool test_mode = false;
    bool silent = false;
    bool no_exec = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t") {
            test_mode = true;
        } else if (arg == "-sfxnoexec") {
            // CI/CD kill switch: extraction only, every directive suppressed
            // and REPORTED (plan §9 — suppressed never means silent).
            no_exec = true;
        } else if (arg == "-s" || arg == "-silent" || arg == "--silent" || arg == "-q" ||
                   arg == "-quiet" || arg == "--quiet") {
            silent = true;
            openrar::sfx::g_quiet_mode = true;
        } else if (arg == "-plain" || arg == "--plain" || arg == "-idp" || arg == "--no-color") {
            openrar::sfx::g_plain_mode = true;
        } else if (arg.rfind("-d", 0) == 0) {
            // INTENDED (SFX switch syntax): switches are
            // prefix-attached (-d<path>, -p<pass>), so an argument like
            // "-dumb" or "-pkg" parses as a -d/-p switch rather than an
            // unknown flag. That is the compatibility contract; do not
            // "fix" this into exact-argument matching.
            if (arg.size() > 2)
                dest_dir = arg.substr(2);
            else if (i + 1 < argc)
                dest_dir = argv[++i];
        } else if (arg.rfind("-p", 0) == 0 && arg.size() > 2) {
            password = arg.substr(2);
        } else if (arg[0] != '-' && dest_dir.empty()) {
            dest_dir = arg;
        }
    }

    openrar::archive::ArchiveReader reader;
    if (!reader.open(self_path, password)) {
        if (reader.has_bad_password()) {
            std::cerr << "Cannot decrypt SFX archive: BADPSW (bad password)\n";
            return 1;
        }
        if (!silent) {
            // No [-y] here: the module has no interactive queries (it
            // overwrites silently), so advertising it would repeat the B8
            // class — help promising behavior nothing implements.
            std::cout << "\nOpenRAR 5.0 Self-Extracting Archive Module (SFX)\n"
                      << "Copyright (c) 2026 OpenRAR Project\n\n"
                      << "Usage: " << sfx_filename
                      << " [-d<destination_directory>] [-p<password>] [-t] [-s] [-plain] [-q]\n";
        }
        return 0;
    }

    size_t total_entries = 0;
    openrar::core::uint64 total_bytes = 0;
    for (const auto& entry : reader.entries()) {
        if (!entry.header.is_service) {
            total_entries++;
            total_bytes += entry.header.unp_size;
        }
    }

    openrar::sfx::SFXProgress prog;

    if (test_mode) {
        if (!silent && !openrar::sfx::is_vt_supported()) {
            std::cout << "Testing SFX archive: " << sfx_filename << "\n\n";
        }
        if (!silent) {
            prog.init("TESTING", "\x1b[38;2;95;184;176m", "\x1b[48;2;19;37;35m");
            prog.set_totals(total_entries, total_bytes);
        }

        size_t error_count = 0;
        size_t idx = 0;
        for (const auto& entry : reader.entries()) {
            if (entry.header.is_service) continue;
            idx++;
            if (!silent) prog.start_file(entry.header.file_name, idx);

            if (!silent && !openrar::sfx::is_vt_supported()) {
                std::cout << "Testing     "
                          << openrar::cli::sanitize_for_display(entry.header.file_name) << "... ";
            }

            if (reader.test_entry(entry)) {
                if (!silent && !openrar::sfx::is_vt_supported()) std::cout << "OK\n";
            } else {
                if (!silent && !openrar::sfx::is_vt_supported()) std::cout << "FAILED\n";
                error_count++;
            }
            if (!silent) prog.update_bytes(entry.header.unp_size);
        }

        if (!silent) {
            prog.done(total_entries, "tested", sfx_filename,
                      error_count == 0 ? "all OK" : (std::to_string(error_count) + " errors"));
        }
        return error_count == 0 ? 0 : 1;
    } else {
        if (!silent && !openrar::sfx::is_vt_supported()) {
            std::cout << "Extracting from SFX archive: " << sfx_filename << "\n\n";
        }

        // ── v1.23.0 directive pipeline (plan §7) ────────────────────────────
        // Comment directives drive Presetup/Setup/Delete/TempMode around the
        // extraction loop; the consent engine gates every side effect. With
        // no comment the pipeline degenerates to plain extraction.
        std::vector<openrar::core::byte> comment;
        const bool have_comment = reader.read_archive_comment(comment);
        openrar::sfx::SfxConfig cfg;
        if (have_comment) {
            cfg = openrar::sfx::parse_sfx_config(comment);
            if (cfg.invalid_utf8)
                std::cout << "[sfx] directives disabled: comment is not valid UTF-8\n";
        }

        openrar::sfx::PipelineOptions pipe_opts;
        pipe_opts.no_exec = no_exec || getenv("OPENRAR_SFX_NOEXEC") != nullptr;
        pipe_opts.argv_dest = dest_dir; // explicit user choice; overrides Path=

        // Silent directives suppress progress UI only — consent prompts still
        // appear (plan §2 invariant 1).
        const bool extraction_silent = silent || cfg.silent != openrar::sfx::SilentMode::Off;

        openrar::sfx::ConsolePromptBackend console_backend;
        openrar::sfx::SfxConsentEngine consent(console_backend);

        auto extract_loop = [&](const std::string& pipe_dest,
                                openrar::sfx::OverwriteDirective overwrite) -> int {
            if (extraction_silent) openrar::sfx::g_quiet_mode = true;
            std::filesystem::path out_root = pipe_dest.empty() ? std::filesystem::current_path()
                                                               : std::filesystem::path(pipe_dest);
            std::error_code mk_ec;
            std::filesystem::create_directories(out_root, mk_ec);

            size_t idx = 0;
            for (const auto& entry : reader.entries()) {
                if (entry.header.is_service) continue;
                idx++;
                // Zip-Slip guard (same rationale as cli/main.cpp extract_archive)
                std::string safe_name = openrar::io::sanitize_archive_path(entry.header.file_name);
                if (safe_name.empty()) {
                    if (!extraction_silent)
                        std::cout << "Skipping entry with unsafe empty path: "
                                  << openrar::cli::sanitize_for_display(entry.header.file_name)
                                  << "\n";
                    continue;
                }
                std::filesystem::path target = out_root / std::filesystem::path(safe_name);
                if (!openrar::io::is_lexically_contained(target, out_root)) {
                    if (!extraction_silent)
                        std::cout << "Skipping entry escaping extraction directory: "
                                  << openrar::cli::sanitize_for_display(entry.header.file_name)
                                  << "\n";
                    continue;
                }

                // §3.2 overwrite policy: directives may only de-escalate.
                // Ask (the default) and SkipExisting skip existing targets in
                // the non-interactive stub and report them; OverwriteAll (only
                // reachable with explicit escalation consent) keeps the
                // historical overwrite behavior.
                if (overwrite != openrar::sfx::OverwriteDirective::OverwriteAll &&
                    std::filesystem::exists(target)) {
                    if (!extraction_silent)
                        std::cout << "Skipping existing: "
                                  << openrar::cli::sanitize_for_display(target.string()) << "\n";
                    continue;
                }

                if (!extraction_silent) prog.start_file(entry.header.file_name, idx);

                if (!extraction_silent && !openrar::sfx::is_vt_supported()) {
                    std::cout << "Extracting  "
                              << openrar::cli::sanitize_for_display(target.string()) << " ... ";
                }

                if (reader.extract_entry(entry, target, password)) {
                    if (!extraction_silent && !openrar::sfx::is_vt_supported()) std::cout << "OK\n";
                } else {
                    if (!extraction_silent && !openrar::sfx::is_vt_supported()) {
                        std::cout << "FAILED\n";
                    }
                    return 1;
                }
                if (!extraction_silent) prog.update_bytes(entry.header.unp_size);
            }

            if (!extraction_silent) {
                prog.done(total_entries, "unpacked", sfx_filename);
            }
            return 0;
        };

        openrar::sfx::PipelineResult result =
            openrar::sfx::run_sfx_pipeline(cfg, consent, pipe_opts, extract_loop);

        // Observable report (plan §2 invariant 6): every consent/containment/
        // suppression outcome is user-visible. Details are post-sanitization.
        for (const auto& line : result.report) {
            std::cout << "[sfx] " << line.phase << ": " << line.status;
            if (!line.detail.empty())
                std::cout << " - " << openrar::cli::sanitize_for_display(line.detail);
            std::cout << "\n";
        }
        if (result.suppressed_directives > 0)
            std::cout << "[sfx] directives suppressed: " << result.suppressed_directives
                      << " (-sfxnoexec)\n";
        std::cout << std::flush;
        return result.exit_code;
    }
}

#ifdef _WIN32
static std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 1) return {};
    std::string str(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size_needed, nullptr, nullptr);
    return str;
}
#endif

int main(int argc, char* argv[]) {
    // L10: see sfx_main_impl - no exception may escape main() uncaught.
    try {
#ifdef _WIN32
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv) {
            std::vector<std::string> utf8_args;
            std::vector<char*> utf8_argv;
            utf8_args.reserve(wargc);
            utf8_argv.reserve(wargc + 1);
            for (int i = 0; i < wargc; ++i) {
                utf8_args.push_back(wide_to_utf8(wargv[i]));
            }
            LocalFree(wargv);
            for (auto& s : utf8_args) {
                utf8_argv.push_back(&s[0]);
            }
            utf8_argv.push_back(nullptr);
            return sfx_main_impl(wargc, utf8_argv.data());
        }
#endif
        return sfx_main_impl(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "openrar: error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "openrar: error: unknown non-standard exception\n";
        return 1;
    }
}
