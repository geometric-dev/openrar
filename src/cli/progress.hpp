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

// Output stream for all progress rendering. --json-summary (stdout-purity
// mode, v1.24 plan §5.2) points this at stderr so stdout carries only JSON.
inline std::ostream* g_prog_out = &std::cout;
inline void set_prog_out(std::ostream& os) {
    g_prog_out = &os;
}

// v1.28 §7.1: VT capability is a property of the SINK stream, not of stdout.
// In --json-summary mode progress renders to stderr while stdout carries the
// JSON, so the probe must follow the fd actually written to. The fd-taking
// core is the testable seam; the ostream wrapper resolves the two sinks the
// CLI uses (g_prog_out points at std::cout or std::cerr only).
inline bool is_vt_supported_fd(int fd) {
    if (g_plain_mode || g_quiet_mode) return false;
    if (getenv("NO_COLOR") != nullptr) return false;
#ifdef _WIN32
    HANDLE out_handle = GetStdHandle(fd == 2 ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (_isatty(fd) && GetConsoleMode(out_handle, &mode)) {
        SetConsoleMode(out_handle, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
        return true;
    }
    return false;
#else
    const char* term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) return false;
    return ::isatty(fd);
#endif
}

inline bool is_vt_supported_for(std::ostream& os) {
    return is_vt_supported_fd(&os == &std::cerr ? 2 : 1);
}

inline bool is_vt_supported() {
    return is_vt_supported_for(*g_prog_out);
}

} // namespace openrar::cli

#endif // OPENRAR_CLI_PROGRESS_HPP
