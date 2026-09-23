#ifndef OPENRAR_CLI_PROGRESS_HPP
#define OPENRAR_CLI_PROGRESS_HPP
#include "../core/types.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace openrar::cli {

extern bool g_plain_mode;
extern bool g_quiet_mode;

// L11: entry names come from untrusted archives. Sanitize before echoing a
// name to the terminal (v1.22.0 sweep hardening; the per-category contract
// is pinned by tests/unit/cli_tests.cpp test_sanitize_for_display):
//   - C0 controls (0x00-0x1F) and DEL: ESC-led CSI/OSC injection (title
//     rewrite, cursor hide, OSC 52 clipboard writes) -> '?'
//   - C1 controls (U+0080..009F, incl. 8-bit CSI U+009B) -> '?'
//   - invalid UTF-8 (bad leads, lone continuations, overlong forms,
//     surrogates, > U+10FFFF, truncated tails) -> '?' per byte, so
//     terminals cannot be pushed out of UTF-8 state
//   - bidi/direction attacks: RTL/LTR overrides (U+202A..202E), isolates
//     (U+2066..2069), directional marks (U+200E/200F), line/paragraph
//     separators (U+2028/2029), soft hyphen (U+00AD) -> '?' per source byte
//   - valid non-ASCII text (accents, CJK, emoji) passes through untouched
// Display-only helper: never feed its result back into filesystem operations.
inline std::string sanitize_for_display(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    const size_t n = name.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c <= 0x1F || c == 0x7F) {
            out += '?';
            ++i;
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }

        // Strict multi-byte decode: bad lead, bad continuation, overlong,
        // surrogate, or out-of-range all degrade to '?' for this byte; any
        // following continuation bytes then fail as bad leads, so a broken
        // sequence costs one '?' per byte.
        int len = 0;
        core::uint32 cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07u;
        } else {
            out += '?';
            ++i;
            continue;
        }
        bool ok = i + static_cast<size_t>(len) <= n;
        for (int k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(name[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        static const core::uint32 kMinCp[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (!ok || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu) || cp < kMinCp[len]) {
            out += '?';
            ++i;
            continue;
        }

        const bool hostile = (cp >= 0x202Au && cp <= 0x202Eu) || // LRE/LRO/RLE/RLO/PDF overrides
                             (cp >= 0x2066u && cp <= 0x2069u) || // LRI/RLI/FSI/PDI isolates
                             cp == 0x200Eu || cp == 0x200Fu ||   // LRM/RLM directional marks
                             cp == 0x2028u || cp == 0x2029u ||   // LINE/PARAGRAPH SEPARATOR
                             cp == 0x00ADu ||              // SOFT HYPHEN (invisible in filenames)
                             (cp >= 0x80u && cp <= 0x9Fu); // C1 controls
        if (hostile) {
            out.append(static_cast<size_t>(len), '?');
        } else {
            out.append(name, i, static_cast<size_t>(len)); // validated sequence verbatim
        }
        i += static_cast<size_t>(len);
    }
    return out;
}

inline bool is_vt_supported() {
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

class CLIProgress {
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
    // Batch operations prepare files on worker threads, so start_file /
    // update_bytes arrive concurrently with each other. All state below is
    // touched under mu_; rendering itself stays serialized because every
    // mutator holds the lock across its render_locked() call.
    std::mutex mu_;

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
        std::lock_guard<std::mutex> lk(mu_);
        clear_locked();
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
        active_ = !g_quiet_mode;
    }

    void set_totals(size_t total_files, core::uint64 total_bytes) {
        std::lock_guard<std::mutex> lk(mu_);
        total_files_ = total_files;
        total_bytes_ = total_bytes;
    }

    void spin(const std::string& message, int64_t count = -1) {
        if (g_quiet_mode || !is_vt_supported()) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (lines_drawn_ > 0) {
            std::cout << "\x1b[" << lines_drawn_ << "A";
        }
        std::cout << "\x1b[2K\r  \x1b[38;2;95;184;176m" << SPIN_FRAMES[spin_frame_]
                  << "\x1b[0m  \x1b[38;2;109;114;128m" << message << "\x1b[0m";
        if (count >= 0) {
            std::cout << "  \x1b[38;2;69;73;85m" << count << " found\x1b[0m";
        }
        std::cout << "\n" << std::flush;
        lines_drawn_ = 1;
        spin_frame_ = (spin_frame_ + 1) % 10;
    }

    void start_file(const std::string& filename, size_t file_index) {
        std::lock_guard<std::mutex> lk(mu_);
        current_file_ = sanitize_for_display(filename);
        processed_files_ = file_index;
        render_locked();
    }

    // Batch-operation completion from worker threads: counts one finished
    // file and its bytes, showing the most recently finished name. Safe to
    // call concurrently; completion order defines the displayed name.
    void note_file_done(const std::string& filename, core::uint64 bytes) {
        if (g_quiet_mode || !is_vt_supported()) return;
        std::lock_guard<std::mutex> lk(mu_);
        processed_files_++;
        processed_bytes_ += bytes;
        current_file_ = sanitize_for_display(filename);
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render_time_)
                    .count() >= 30 ||
            processed_bytes_ >= total_bytes_) {
            render_locked();
        }
    }

    void update_bytes(core::uint64 bytes_added) {
        std::lock_guard<std::mutex> lk(mu_);
        processed_bytes_ += bytes_added;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render_time_)
                    .count() >= 30 ||
            processed_bytes_ >= total_bytes_) {
            render_locked();
        }
    }

    void render() {
        std::lock_guard<std::mutex> lk(mu_);
        render_locked();
    }

private:
    // Caller must hold mu_. Rendering assumes a single stdout writer: every
    // public entry point takes the lock before reaching here.
    void render_locked() {
        if (!active_ || g_quiet_mode || !is_vt_supported()) return;
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

        std::string badge_line = "\x1b[1m" + fg_color_ + bg_color_ + " " + state_name_ + " \x1b[0m";

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

        std::string eta_str = format_eta(eta);
        std::string meta_line = "\x1b[38;2;109;114;128m";
        if (total_files_ > 0) {
            meta_line += std::to_string(processed_files_) + " / " + std::to_string(total_files_) +
                         " files  ·  ETA " + eta_str;
        } else {
            meta_line += std::to_string(processed_files_) + " files  ·  ETA " + eta_str;
        }
        meta_line += "\x1b[0m";

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

public:
    void done(size_t total_files, const std::string& verb, const std::string& arc_name,
              core::uint64 unp_bytes = 0, core::uint64 packed_bytes = 0,
              const std::string& extra = "") {
        std::lock_guard<std::mutex> lk(mu_);
        clear_locked();
        active_ = false;
        if (g_quiet_mode) return;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();

        std::string ratio_str = "";
        if (unp_bytes > 0 && packed_bytes > 0) {
            double diff =
                (1.0 - static_cast<double>(packed_bytes) / static_cast<double>(unp_bytes)) * 100.0;
            if (diff >= 0) {
                ratio_str = " \x1b[38;2;109;114;128m— " +
                            std::to_string(static_cast<int>(diff + 0.5)) + "% smaller\x1b[0m";
            } else {
                ratio_str = " \x1b[38;2;109;114;128m— " +
                            std::to_string(static_cast<int>(-diff + 0.5)) + "% larger\x1b[0m";
            }
        } else if (!extra.empty()) {
            ratio_str = " \x1b[38;2;109;114;128m— " + extra + "\x1b[0m";
        }

        if (is_vt_supported()) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << elapsed;
            std::cout << "\x1b[38;2;123;193;127m✓\x1b[0m  \x1b[1;38;2;123;193;127;48;2;25;39;26m "
                         "DONE \x1b[0m\n"
                      << "\x1b[1;38;2;231;229;223m" << total_files
                      << "\x1b[0m \x1b[38;2;109;114;128mfiles " << verb
                      << " in\x1b[0m \x1b[1;38;2;231;229;223m" << oss.str() << "s\x1b[0m";
            if (!arc_name.empty()) {
                std::cout << " \x1b[38;2;109;114;128m→\x1b[0m \x1b[1;38;2;231;229;223m" << arc_name
                          << "\x1b[0m";
            }
            std::cout << ratio_str << "\n\n" << std::flush;
        } else {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << elapsed;
            std::cout << "Done. " << total_files << " files " << verb << " in " << oss.str() << "s";
            if (!arc_name.empty()) std::cout << " -> " << arc_name;
            std::cout << "\n" << std::flush;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        clear_locked();
    }

private:
    // Caller must hold mu_.
    void clear_locked() {
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

} // namespace openrar::cli

#endif // OPENRAR_CLI_PROGRESS_HPP
