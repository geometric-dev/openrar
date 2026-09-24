#ifndef OPENRAR_SFX_PROMPT_CONSOLE_HPP
#define OPENRAR_SFX_PROMPT_CONSOLE_HPP

#include "sfx_consent.hpp"

namespace openrar::sfx {

// Console consent backend (Default.SFX). Renders the request to stdout and
// reads the answer from stdin:
//   R = run once        A = run all of this directive type
//   Enter / D = deny    N = deny all of this directive type
//   Q = abort the whole run
// Enter (empty input) is Deny — the "Don't Run" default-focus invariant.
// Non-interactive stdin (piped/closed) reports interactive() == false; the
// engine then denies every ask without prompting (plan §2 invariant 4).
class ConsolePromptBackend : public IPromptBackend {
public:
    bool interactive() const override;
    ConsentAnswer ask(const ConsentRequest& req) override;
};

} // namespace openrar::sfx

#endif // OPENRAR_SFX_PROMPT_CONSOLE_HPP
