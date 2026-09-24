#include "sfx_consent.hpp"

#include <algorithm>

namespace openrar::sfx {

SfxConsentEngine::SfxConsentEngine(IPromptBackend& backend, size_t prompt_cap)
    : backend_(backend), prompt_cap_(prompt_cap) {}

bool SfxConsentEngine::batch_approved(DirectiveType type) const {
    return std::find(run_all_types_.begin(), run_all_types_.end(), type) != run_all_types_.end();
}

bool SfxConsentEngine::batch_denied(DirectiveType type) const {
    return std::find(deny_all_types_.begin(), deny_all_types_.end(), type) != deny_all_types_.end();
}

ConsentDecision SfxConsentEngine::ask(const ConsentRequest& req) {
    // Batch answers resolve without prompting, regardless of the cap.
    if (batch_approved(req.type)) return ConsentDecision::Run;
    if (batch_denied(req.type)) return ConsentDecision::Deny;

    // Abort latches: every later ask is denied and the pipeline aborts.
    if (aborted_) return ConsentDecision::Deny;

    // Non-interactive backend: deny without prompting (plan §2 invariant 4).
    if (!backend_.interactive()) return ConsentDecision::Deny;

    // Prompt cap: exceeded => deny and latch aborted (the pipeline aborts the
    // whole run; plan §6 "hard per-run prompt cap beyond which extraction
    // aborts").
    if (prompts_used_ >= prompt_cap_) {
        aborted_ = true;
        return ConsentDecision::Deny;
    }

    ++prompts_used_;
    const ConsentAnswer answer = backend_.ask(req);
    switch (answer) {
    case ConsentAnswer::Run:
        return ConsentDecision::Run;
    case ConsentAnswer::RunAll:
        run_all_types_.push_back(req.type);
        return ConsentDecision::Run;
    case ConsentAnswer::DenyAll:
        deny_all_types_.push_back(req.type);
        return ConsentDecision::Deny;
    case ConsentAnswer::Abort:
        aborted_ = true;
        return ConsentDecision::Deny;
    case ConsentAnswer::Deny:
    default:
        return ConsentDecision::Deny;
    }
}

} // namespace openrar::sfx
