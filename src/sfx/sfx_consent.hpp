#ifndef OPENRAR_SFX_SFX_CONSENT_HPP
#define OPENRAR_SFX_SFX_CONSENT_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace openrar::sfx {

// v1.23.0 consent engine (plan §6) — THE single consent decision function.
// Both SFX modules (console + WinGUI) consume it; only the prompt backend
// differs. Normative invariants enforced HERE, not in the UIs (plan §2):
//   - no directive suppresses prompts (Silent never reaches this layer)
//   - non-interactive backend => every ask resolves to Deny without prompting
//   - prompt cap exceeded => engine latches aborted; pipeline aborts the run
//   - batch answers (RunAll/DenyAll) are remembered per directive TYPE
// The engine never displays anything itself: callers pass post-sanitization
// display text plus the verbatim command line and resolved path.

enum class DirectiveType { Presetup, Setup, Delete, Shortcut, OverwriteEscalation };

struct ConsentRequest {
    DirectiveType type;
    std::string verb;          // e.g. "run before extraction", "delete files"
    std::string command;       // verbatim directive command line
    std::string resolved_path; // resolved absolute executable ("" for Delete)
    std::string detail;        // extra display lines (e.g. pattern count)
};

enum class ConsentAnswer { Run, RunAll, Deny, DenyAll, Abort };

// One interactive prompt. Implementations must default to Deny ("Don't Run")
// and must not render anything except what the request carries.
class IPromptBackend {
public:
    virtual ~IPromptBackend() = default;
    // False when no interactive surface exists (piped stdin, headless
    // session): the engine then denies every ask without calling ask().
    virtual bool interactive() const = 0;
    virtual ConsentAnswer ask(const ConsentRequest& req) = 0;
};

enum class ConsentDecision { Run, Deny };

class SfxConsentEngine {
public:
    explicit SfxConsentEngine(IPromptBackend& backend, size_t prompt_cap = 8);

    // Resolves one consent request. Never prompts once a batch answer for the
    // request's type exists, once aborted, or once the cap is exhausted.
    ConsentDecision ask(const ConsentRequest& req);

    // True when the user aborted or the prompt cap latched the run aborted.
    bool aborted() const { return aborted_; }
    size_t prompts_used() const { return prompts_used_; }
    size_t prompt_cap() const { return prompt_cap_; }

private:
    bool batch_approved(DirectiveType type) const;
    bool batch_denied(DirectiveType type) const;

    IPromptBackend& backend_;
    size_t prompt_cap_;
    size_t prompts_used_{0};
    bool aborted_{false};
    std::vector<DirectiveType> run_all_types_;
    std::vector<DirectiveType> deny_all_types_;
};

} // namespace openrar::sfx

#endif // OPENRAR_SFX_SFX_CONSENT_HPP
