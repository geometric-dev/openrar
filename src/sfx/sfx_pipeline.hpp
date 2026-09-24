#ifndef OPENRAR_SFX_SFX_PIPELINE_HPP
#define OPENRAR_SFX_SFX_PIPELINE_HPP

#include "sfx_config.hpp"
#include "sfx_consent.hpp"

#include <functional>
#include <string>
#include <vector>

namespace openrar::sfx {

// v1.23.0 phase pipeline (plan §7/§8/§9): destination resolution with Path=
// containment, Presetup -> extraction -> Setup -> Delete -> TempMode cleanup,
// the §8 exit-code mapping, and the observable -sfxnoexec kill switch.
//
// Extraction itself is injected (the stub's extraction loop): it receives the
// effective destination and the effective overwrite policy (the §3.2 engine
// remains the decision function for every byte) and returns the extraction
// exit code. A nonzero extraction result aborts the remaining phases — an
// incomplete extraction never reaches Setup or Delete.

struct PipelineOptions {
    bool no_exec{false};   // -sfxnoexec / OPENRAR_SFX_NOEXEC=1
    std::string argv_dest; // explicit user destination (overrides Path=)
};

struct DirectiveReportLine {
    std::string phase;  // "path" | "presetup" | "setup" | "delete" | "tempmode"
    std::string status; // "executed" | "skipped-consent" | "skipped-noexec" |
                        // "skipped-not-found" | "refused-containment" |
                        // "refused-amsi" | "refused-containment-path" | "failed"
    std::string detail;
};

struct PipelineResult {
    int exit_code{0};
    std::string effective_dest;
    bool aborted{false}; // consent cap or user abort mid-run
    std::vector<DirectiveReportLine> report;
    size_t suppressed_directives{0}; // -sfxnoexec side-effecting directive count
};

// Wildcard match for Delete= patterns ('*' any run, '?' one char;
// case-insensitive on Windows, case-sensitive elsewhere).
bool match_wildcard(const std::string& pattern, const std::string& name);

// Resolves the effective extraction destination (plan §7 Path= containment):
// argv_dest (explicit user choice) overrides everything; TempMode ignores
// Path= (the caller creates the temp dir and passes it as argv_dest-equivalent
// by leaving both empty and using tempmode handling); a Path= outside the
// user profile is refused with a report line and the fallback destination is
// used. Returns "" when no usable destination could be determined.
std::string resolve_sfx_destination(const SfxConfig& cfg, const PipelineOptions& opts,
                                    std::vector<DirectiveReportLine>& report);

// Creates the TempMode extraction directory: <temp>/OpenRAR-<128-bit-hex>,
// owner-only (POSIX 0700 / Windows owner-only DACL). Returns "" on failure.
std::string make_tempmode_dir();

PipelineResult run_sfx_pipeline(
    const SfxConfig& cfg, SfxConsentEngine& consent, const PipelineOptions& opts,
    const std::function<int(const std::string& dest, OverwriteDirective overwrite)>& extract_fn);

} // namespace openrar::sfx

#endif // OPENRAR_SFX_SFX_PIPELINE_HPP
