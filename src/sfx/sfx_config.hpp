#ifndef OPENRAR_SFX_SFX_CONFIG_HPP
#define OPENRAR_SFX_SFX_CONFIG_HPP

#include "../core/types.hpp"

#include <string>
#include <vector>

namespace openrar::sfx {

// v1.23.0 directive-engine config, parsed from the archive's CMT service
// header by the SFX stub (docs/sfx-v1.23-implementation-plan.md §4).
//
// Grammar: Key=Value lines, keys case-insensitive (normalized to lower),
// LF/CRLF line endings, UTF-8 (a UTF-8 BOM is tolerated and stripped; an
// invalid-UTF-8 comment disables all directives). Multiplicity: setup /
// presetup / delete / shortcut / text / license accumulate in order; path /
// overwrite / silent / tempmode / title are last-wins. Unknown keys are
// counted and otherwise ignored (forward compatibility); prose lines without
// '=' are ignored silently.
//
// Caps (plan §4; exceeding degrades that line, never aborts extraction):
//   - at most kMaxDirectiveLines recognized directive lines (truncated flag)
//   - at most kMaxValueBytes per value (line ignored)
//   - the decoded comment itself is capped reader-side at 1 MiB
//
// The struct is plain data and is treated as immutable after parse: the stub
// parses once per run and consumes it as const.

inline constexpr size_t kMaxDirectiveLines = 64;
inline constexpr size_t kMaxValueBytes = 4096;
inline constexpr size_t kMaxShortcutFields = 6;
inline constexpr size_t kMaxShortcutFieldBytes = 1024;

enum class OverwriteDirective { Ask, OverwriteAll, SkipExisting };
enum class SilentMode { Off, HideStart, Headless };

struct ShortcutDirective {
    // Raw fields as authored; semantic validation (folder enum, target
    // resolution inside the destination) happens at execution time (M4).
    std::string target;
    std::string folder;
    std::string name;
    std::string description;
    std::string icon_file;
    std::string icon_index;
};

struct SfxConfig {
    // Accumulating directives (order preserved).
    std::vector<std::string> setup;
    std::vector<std::string> presetup;
    std::vector<std::string> delete_patterns;
    std::vector<ShortcutDirective> shortcuts;
    std::vector<std::string> text_lines;
    std::vector<std::string> license_lines;

    // Last-wins directives.
    std::string path;
    OverwriteDirective overwrite{OverwriteDirective::Ask};
    SilentMode silent{SilentMode::Off};
    bool tempmode{false};
    std::string title;

    // Parse report (surfaced by the stub per the observable-suppression
    // contract; never a reason to abort extraction).
    size_t ignored_unknown_keys{0};     // Key=Value lines with unrecognized key
    size_t ignored_over_limit_lines{0}; // value/field caps exceeded
    bool truncated{false};              // kMaxDirectiveLines exceeded
    bool invalid_utf8{false};           // comment not valid UTF-8: no directives

    bool has_directives() const {
        return !setup.empty() || !presetup.empty() || !delete_patterns.empty() ||
               !shortcuts.empty() || !text_lines.empty() || !license_lines.empty() ||
               !path.empty() || overwrite != OverwriteDirective::Ask || silent != SilentMode::Off ||
               tempmode || !title.empty();
    }
};

// Parses the raw decoded comment bytes into an SfxConfig. Never fails:
// every degradation is reported inside the returned config.
SfxConfig parse_sfx_config(const std::vector<core::byte>& comment);

} // namespace openrar::sfx

#endif // OPENRAR_SFX_SFX_CONFIG_HPP
