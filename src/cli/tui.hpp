#ifndef OPENRAR_CLI_TUI_HPP
#define OPENRAR_CLI_TUI_HPP
// v1.28.0 dual-progress TUI (docs/v1.28-implementation-plan.md §1.2):
//   - pure render core: render_tui(state, width) is unit-testable without a
//     terminal and width-clamps every line (the shipped renderer's fixed
//     BAR_WIDTH + unwrapped names garbled the redraw region on narrow
//     terminals once a long name wrapped).
//   - single-writer rule (challenge directive 2): every mutator holds the
//     state mutex ACROSS its render — whichever thread holds the lock is the
//     one writer of terminal bytes; workers mutate, never render unlocked.
//   - PROMPT state: pause_for_prompt() wipes the region and hands the
//     terminal to the prompt; resume() redraws (ask_overwrite fires mid-run).
//   - interactive cancel: KeyboardCancel reads ESC/'q' on a raw-stdin thread
//     and owns the ^C handler ONLY for the TUI scope (directive 3); ^C
//     outside keeps the terminate semantics. The thread never writes and
//     never touches terminal modes.
//
// §7.1 release gate: the renderer re-sanitizes every name-shaped field with
// the idempotent sanitize_for_display (directive 7) — callers may pre-
// sanitize, the renderer does not trust them.

#include "progress.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#include <io.h>
#else
#include <csignal>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace openrar::cli {

// Cooperative-cancel flag shared by the keyboard thread, the ^C handler and
// the reader disk hooks. Set once; never cleared mid-run.
inline std::atomic<int> g_tui_cancel{0};

// ── pure render core ─────────────────────────────────────────────────────────

struct TuiState {
    enum class Mode { Bars, Spin };
    Mode mode = Mode::Bars;

    std::string badge;    // state name, e.g. "EXTRACTING" (trusted, ours)
    std::string fg_color; // palette escapes (trusted, ours)
    std::string bg_color;

    size_t done_files = 0;
    size_t total_files = 0;
    core::uint64 done_bytes = 0;
    core::uint64 total_bytes = 0;

    std::string current_name; // DISPLAY name — re-sanitized by the renderer
    core::uint64 current_done = 0;
    core::uint64 current_total = 0;

    // Spin mode only.
    std::string spin_frame;
    std::string spin_message;
    core::int64 spin_count = -1;

    double elapsed_s = 0.0; // renderer derives ETA
};

// Columns ≈ UTF-8 code points (an approximation that is safe for truncation:
// cutting at a code-point boundary never splits a sequence). Truncation
// appends a single '…' (U+2026) and always leaves room for it.
inline size_t utf8_seq_len(unsigned char c) {
    return c >= 0xF0 ? size_t(4) : c >= 0xE0 ? size_t(3) : c >= 0xC0 ? size_t(2) : size_t(1);
}

inline std::string truncate_utf8(const std::string& s, size_t max_cols) {
    if (max_cols == 0) return std::string();
    size_t total = 0;
    for (size_t i = 0; i < s.size();) {
        const size_t len = utf8_seq_len(static_cast<unsigned char>(s[i]));
        if (i + len > s.size()) break; // truncated tail: not counted
        ++total;
        i += len;
    }
    if (total <= max_cols) return s;
    const size_t budget = max_cols - 1; // room for the ellipsis itself
    size_t cols = 0;
    size_t i = 0;
    while (i < s.size() && cols < budget) {
        const size_t len = utf8_seq_len(static_cast<unsigned char>(s[i]));
        if (i + len > s.size()) break;
        ++cols;
        i += len;
    }
    return s.substr(0, i) + "\xe2\x80\xa6";
}

inline int format_eta_secs(double seconds) {
    if (seconds < 0 || seconds > 360000) return -1;
    return static_cast<int>(seconds + 0.5);
}

inline std::string format_eta(double seconds) {
    const int s = format_eta_secs(seconds);
    if (s < 0) return "--:--";
    const int h = s / 3600;
    const int m = (s % 3600) / 60;
    const int sec = s % 60;
    std::ostringstream oss;
    if (h > 0) {
        oss << h << ":" << std::setfill('0') << std::setw(2) << m << ":" << std::setfill('0')
            << std::setw(2) << sec;
    } else {
        oss << m << ":" << std::setfill('0') << std::setw(2) << sec;
    }
    return oss.str();
}

// Renders the region: 1 line (spin) or 4 lines (badge / overall bar /
// current-file bar + name / meta), each prefixed with clear-line + CR and
// separated by \n, NO trailing newline after the final line (the renderer
// adds it). Every line is clamped to `width` columns of VISIBLE text.
inline std::string render_tui(const TuiState& st, int width) {
    std::ostringstream out;
    const auto emit_line = [&out](const std::string& content) {
        out << "\x1b[2K\r  " << content;
    };

    if (st.mode == TuiState::Mode::Spin) {
        std::string line = std::string("\x1b[38;2;95;184;176m") + st.spin_frame + "\x1b[0m" +
                           "  \x1b[38;2;109;114;128m" + st.spin_message + "\x1b[0m";
        if (st.spin_count >= 0) {
            line += "  \x1b[38;2;69;73;85m" + std::to_string(st.spin_count) + " found\x1b[0m";
        }
        emit_line(truncate_utf8(line, width > 8 ? static_cast<size_t>(width) - 4 : 8));
        return out.str();
    }

    double pct = 0.0;
    if (st.total_bytes > 0) {
        pct = static_cast<double>(st.done_bytes) / static_cast<double>(st.total_bytes) * 100.0;
    } else if (st.total_files > 0) {
        pct = static_cast<double>(st.done_files) / static_cast<double>(st.total_files) * 100.0;
    }
    pct = std::max(0.0, std::min(100.0, pct));

    double eta = -1;
    if (pct > 0.5 && st.elapsed_s > 0.2) {
        eta = st.elapsed_s / (pct / 100.0) - st.elapsed_s;
        if (eta < 0) eta = 0;
    }

    // 1. badge
    std::string badge_line = "\x1b[1m" + st.fg_color + st.bg_color + " " + st.badge + " \x1b[0m";

    // 2. overall bar (fixed 28-block bar like the shipped renderer; the
    // clamp below bounds the WHOLE line so narrow terminals never wrap).
    const int kBarWidth = 28;
    int filled = static_cast<int>((pct / 100.0) * kBarWidth + 0.5);
    filled = std::max(0, std::min(kBarWidth, filled));
    std::string bar_line = "[";
    bar_line += st.fg_color;
    for (int i = 0; i < filled; ++i) bar_line += "\xe2\x96\x88"; // █
    bar_line += "\x1b[38;2;69;73;85m";
    for (int i = 0; i < kBarWidth - filled; ++i) bar_line += "\xe2\x96\x91"; // ░
    bar_line += "\x1b[0m]  \x1b[38;2;231;229;223m" + std::to_string(static_cast<int>(pct + 0.5)) +
                "%\x1b[0m";

    // 3. current-file bar + name. The name is re-sanitized here: the §7.1
    // choke point (idempotent — see test_sanitize_for_display).
    std::string file_line;
    {
        double fpct = 0.0;
        if (st.current_total > 0) {
            fpct = static_cast<double>(st.current_done) / static_cast<double>(st.current_total) *
                   100.0;
        }
        fpct = std::max(0.0, std::min(100.0, fpct));
        const int kFileBar = 12;
        int ffilled = static_cast<int>((fpct / 100.0) * kFileBar + 0.5);
        ffilled = std::max(0, std::min(kFileBar, ffilled));
        file_line += "\x1b[38;2;95;184;176m[";
        for (int i = 0; i < ffilled; ++i) file_line += "\xe2\x96\x88";
        for (int i = 0; i < kFileBar - ffilled; ++i) file_line += "\xc2\xb7"; // ·
        file_line += "]\x1b[0m ";
        file_line += sanitize_for_display(st.current_name);
    }

    // 4. meta
    std::string meta_line = "\x1b[38;2;109;114;128m";
    if (st.total_files > 0) {
        meta_line += std::to_string(st.done_files) + " / " + std::to_string(st.total_files) +
                     " files  ·  ETA " + format_eta(eta);
    } else {
        meta_line += std::to_string(st.done_files) + " files  ·  ETA " + format_eta(eta);
    }
    meta_line += "\x1b[0m";

    const size_t budget = width > 8 ? static_cast<size_t>(width) - 4 : 8;
    emit_line(truncate_utf8(badge_line, budget));
    out << "\n";
    emit_line(truncate_utf8(bar_line, budget));
    out << "\n";
    emit_line(truncate_utf8(file_line, budget));
    out << "\n";
    emit_line(truncate_utf8(meta_line, budget));
    return out.str();
}

// Current terminal width for the progress sink; probe failure → 80.
inline int terminal_width_for(std::ostream& os) {
#ifdef _WIN32
    (void)os;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        const int w = static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
        if (w >= 20) return w;
    }
    return 80;
#else
    if (&os != &std::cerr) {
        struct winsize ws;
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20) return ws.ws_col;
    }
    return 80;
#endif
}

// ── DualProgress: CLIProgress's API + within-file progress + PROMPT state ────

class DualProgress {
private:
    static const int BAR_WIDTH = 28;
    static constexpr const char* SPIN_FRAMES[10] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                                    "⠴", "⠦", "⠧", "⠇", "⠏"};

    std::string state_name_;
    std::string fg_color_;
    std::string bg_color_;
    size_t total_files_ = 0;
    size_t processed_files_ = 0;
    core::uint64 total_bytes_ = 0;
    core::uint64 processed_bytes_ = 0;
    std::string current_file_;
    core::uint64 current_done_ = 0;
    core::uint64 current_total_ = 0;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_render_time_;
    int spin_frame_ = 0;
    int lines_drawn_ = 0;
    bool active_ = false;
    bool paused_ = false;
    std::mutex mu_;

    static std::string eta_str(double seconds) { return format_eta(seconds); }

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
        current_done_ = 0;
        current_total_ = 0;
        start_time_ = std::chrono::steady_clock::now();
        last_render_time_ = std::chrono::steady_clock::now();
        spin_frame_ = 0;
        lines_drawn_ = 0;
        paused_ = false;
        active_ = !g_quiet_mode;
    }

    void set_totals(size_t total_files, core::uint64 total_bytes) {
        std::lock_guard<std::mutex> lk(mu_);
        total_files_ = total_files;
        total_bytes_ = total_bytes;
    }

    void spin(const std::string& message, core::int64 count = -1) {
        if (g_quiet_mode || !is_vt_supported()) return;
        std::lock_guard<std::mutex> lk(mu_);
        TuiState st;
        st.mode = TuiState::Mode::Spin;
        st.spin_frame = SPIN_FRAMES[spin_frame_];
        st.spin_message = message;
        st.spin_count = count;
        render_state_locked(st);
        spin_frame_ = (spin_frame_ + 1) % 10;
    }

    void start_file(const std::string& filename, size_t file_index) {
        std::lock_guard<std::mutex> lk(mu_);
        current_file_ = sanitize_for_display(filename);
        processed_files_ = file_index;
        current_done_ = 0;
        current_total_ = 0;
        render_locked();
    }

    // Parallel workers: show the most recently started file without touching
    // the counts (the counts advance via note_file_done on completion).
    void flash_current(const std::string& filename) {
        std::lock_guard<std::mutex> lk(mu_);
        current_file_ = sanitize_for_display(filename);
        current_done_ = 0;
        current_total_ = 0;
        maybe_render_locked();
    }

    // Disk-hook progress: within-file byte progress (≤64 KiB granularity).
    void update_current(core::uint64 done, core::uint64 total) {
        std::lock_guard<std::mutex> lk(mu_);
        current_done_ = done;
        current_total_ = total;
        maybe_render_locked();
    }

    void note_file_done(const std::string& filename, core::uint64 bytes) {
        if (g_quiet_mode || !is_vt_supported()) return;
        std::lock_guard<std::mutex> lk(mu_);
        processed_files_++;
        processed_bytes_ += bytes;
        current_file_ = sanitize_for_display(filename);
        current_done_ = 0;
        current_total_ = 0;
        maybe_render_locked();
    }

    void update_bytes(core::uint64 bytes_added) {
        std::lock_guard<std::mutex> lk(mu_);
        processed_bytes_ += bytes_added;
        maybe_render_locked();
    }

    void render() {
        std::lock_guard<std::mutex> lk(mu_);
        render_locked();
    }

    // PROMPT state (challenge directive 2): wipe the region, hand the
    // terminal to the prompt; resume() redraws. No-op when not rendering.
    void pause_for_prompt() {
        std::lock_guard<std::mutex> lk(mu_);
        clear_locked();
        paused_ = true;
    }

    void resume() {
        std::lock_guard<std::mutex> lk(mu_);
        paused_ = false;
        last_render_time_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(1000);
        render_locked();
    }

    core::uint64 processed_bytes() {
        std::lock_guard<std::mutex> lk(mu_);
        return processed_bytes_;
    }

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
            (*g_prog_out)
                << "\x1b[38;2;123;193;127m✓\x1b[0m  \x1b[1;38;2;123;193;127;48;2;25;39;26m "
                   "DONE \x1b[0m\n"
                << "\x1b[1;38;2;231;229;223m" << total_files
                << "\x1b[0m \x1b[38;2;109;114;128mfiles " << verb
                << " in\x1b[0m \x1b[1;38;2;231;229;223m" << oss.str() << "s\x1b[0m";
            if (!arc_name.empty()) {
                (*g_prog_out) << " \x1b[38;2;109;114;128m→\x1b[0m \x1b[1;38;2;231;229;223m"
                              << arc_name << "\x1b[0m";
            }
            (*g_prog_out) << ratio_str << "\n\n" << std::flush;
        } else {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << elapsed;
            (*g_prog_out) << "Done. " << total_files << " files " << verb << " in " << oss.str()
                          << "s";
            if (!arc_name.empty()) (*g_prog_out) << " -> " << arc_name;
            (*g_prog_out) << "\n" << std::flush;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        clear_locked();
    }

private:
    // Caller must hold mu_.
    bool throttle_due_locked() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - last_render_time_)
                       .count() >= 30 ||
               (total_bytes_ > 0 && processed_bytes_ >= total_bytes_);
    }

    void maybe_render_locked() {
        if (!throttle_due_locked()) return;
        render_locked();
    }

    // Writes a TuiState region. Caller must hold mu_ and hold the WRITER
    // right (single-writer rule: mu_ serializes every terminal write).
    void render_state_locked(const TuiState& st) {
        if (!active_ || g_quiet_mode || paused_ || !is_vt_supported()) return;
        last_render_time_ = std::chrono::steady_clock::now();
        const int width = terminal_width_for(*g_prog_out);
        const std::string region = render_tui(st, width);
        if (lines_drawn_ > 0) {
            (*g_prog_out) << "\x1b[" << lines_drawn_ << "A";
        }
        (*g_prog_out) << region << "\n" << std::flush;
        lines_drawn_ = (st.mode == TuiState::Mode::Spin) ? 1 : 4;
    }

    void render_locked() {
        if (!active_ || g_quiet_mode || paused_ || !is_vt_supported()) return;
        TuiState st;
        st.mode = TuiState::Mode::Bars;
        st.badge = state_name_;
        st.fg_color = fg_color_;
        st.bg_color = bg_color_;
        st.done_files = processed_files_;
        st.total_files = total_files_;
        st.done_bytes = processed_bytes_;
        st.total_bytes = total_bytes_;
        st.current_name = current_file_;
        st.current_done = current_done_;
        st.current_total = current_total_;
        st.elapsed_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
        render_state_locked(st);
    }

    // Caller must hold mu_.
    void clear_locked() {
        if (lines_drawn_ > 0 && is_vt_supported()) {
            (*g_prog_out) << "\x1b[" << lines_drawn_ << "A";
            for (int i = 0; i < lines_drawn_; ++i) {
                (*g_prog_out) << "\x1b[2K\r\n";
            }
            (*g_prog_out) << "\x1b[" << lines_drawn_ << "A" << std::flush;
            lines_drawn_ = 0;
        }
    }
};

// ── Interactive cancel (TUI scope only) ──────────────────────────────────────

// Reads ESC / 'q' from a raw-mode stdin thread and sets g_tui_cancel; owns
// the ^C handler for the TUI scope (directive 3: armed ONLY around long TUI
// operations, disarmed on exit — ^C outside keeps the terminate semantics).
// arm() is a no-op when stdin is not a TTY (CI, pipes), so the class is safe
// to arm unconditionally around long operations. RAII nets: disarm() (and the
// process-exit destruction) restore terminal mode and the handler.
class KeyboardCancel {
public:
    static KeyboardCancel& instance() {
        static KeyboardCancel k;
        return k;
    }

    void arm(bool enable) {
        if (armed_) return;
        if (!enable || !stdin_isatty()) return;
        install_signal_handler();
        if (!begin_raw_mode()) {
            // A terminal that refuses raw mode still gets the ^C handler —
            // cooperative cancel works, key-cancel does not.
            remove_signal_handler();
            return;
        }
        armed_ = true;
        worker_ = std::thread(&KeyboardCancel::run, this);
    }

    void disarm() {
        if (worker_.joinable()) {
            stop_.store(1, std::memory_order_relaxed);
            worker_.join();
            stop_.store(0, std::memory_order_relaxed);
        }
        end_raw_mode();
        remove_signal_handler();
        armed_ = false;
    }

    void pause() {
        paused_.store(1, std::memory_order_relaxed);
        if (armed_) end_raw_mode(); // prompt paths expect cooked stdin
    }

    void resume() {
        if (!armed_) return;
        paused_.store(0, std::memory_order_relaxed);
        begin_raw_mode();
    }

    bool active() const { return armed_; }

private:
    KeyboardCancel() = default;
    ~KeyboardCancel() { disarm(); }
    KeyboardCancel(const KeyboardCancel&) = delete;
    KeyboardCancel& operator=(const KeyboardCancel&) = delete;

    static bool stdin_isatty() {
#ifdef _WIN32
        return _isatty(_fileno(stdin)) != 0;
#else
        return ::isatty(STDIN_FILENO) != 0;
#endif
    }

#ifdef _WIN32
    static BOOL WINAPI ctrl_handler(DWORD type) {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
            g_tui_cancel.store(1, std::memory_order_relaxed);
            return TRUE; // handled: cooperative cancel, not terminate
        }
        return FALSE;
    }
    void install_signal_handler() { SetConsoleCtrlHandler(ctrl_handler, TRUE); }
    void remove_signal_handler() { SetConsoleCtrlHandler(ctrl_handler, FALSE); }
    bool begin_raw_mode() { return true; } // conio reads need no mode change
    void end_raw_mode() {}
#else
    static void sigint_handler(int) { g_tui_cancel.store(1, std::memory_order_relaxed); }
    void install_signal_handler() {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigint_handler;
        ::sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, &saved_sigint_);
        handler_installed_ = true;
    }
    void remove_signal_handler() {
        if (handler_installed_) {
            ::sigaction(SIGINT, &saved_sigint_, nullptr);
            handler_installed_ = false;
        }
    }
    bool begin_raw_mode() {
        if (!::isatty(STDIN_FILENO)) return false;
        termios raw{};
        if (::tcgetattr(STDIN_FILENO, &raw) != 0) return false;
        saved_termios_ = raw;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
        raw_active_ = true;
        return true;
    }
    void end_raw_mode() {
        if (raw_active_) ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios_);
        raw_active_ = false;
    }
    termios saved_termios_{};
    bool raw_active_ = false;
    bool handler_installed_ = false;
    sigaction saved_sigint_{};
#endif

    void run() {
        while (stop_.load(std::memory_order_relaxed) == 0) {
            if (paused_.load(std::memory_order_relaxed) != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
#ifdef _WIN32
            while (_kbhit()) {
                const int c = _getch();
                if (c == 0x00 || c == 0xE0) {
                    (void)_getch(); // extended key: two-byte sequence, skip both
                    continue;
                }
                if (c == 0x1B || c == 'q') g_tui_cancel.store(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
#else
            pollfd pfd{STDIN_FILENO, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 100);
            if (pr <= 0) continue;
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break; // EOF: exit quietly
            char c = 0;
            while (::read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 0x1B || c == 'q') g_tui_cancel.store(1, std::memory_order_relaxed);
            }
#endif
        }
    }

    std::atomic<int> stop_{0};
    std::atomic<int> paused_{0};
    bool armed_ = false;
    std::thread worker_;
};

} // namespace openrar::cli

#endif // OPENRAR_CLI_TUI_HPP
