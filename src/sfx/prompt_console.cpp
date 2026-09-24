#include "prompt_console.hpp"

#include "../core/types.hpp"

#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace openrar::sfx {

bool ConsolePromptBackend::interactive() const {
    // Piped/closed stdin cannot answer a consent prompt: the engine denies
    // every ask without calling ask(). This is the fail-closed counterpart of
    // the CLI overwrite prompt's !isatty auto-Yes — extraction convenience
    // must never become execution consent.
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

ConsentAnswer ConsolePromptBackend::ask(const ConsentRequest& req) {
    // Rendering contract (plan §5/§6): the verbatim command line and the
    // resolved path are shown as-is (fixed-font consoles); names inside the
    // request arrive post-sanitization from the caller.
    std::cout << "\nSFX wants to " << req.verb << ".\n";
    if (!req.command.empty()) std::cout << "  Command: " << req.command << "\n";
    if (!req.resolved_path.empty()) std::cout << "  Program: " << req.resolved_path << "\n";
    if (!req.detail.empty()) std::cout << "  " << req.detail << "\n";
    std::cout << "Run? [R]un once, [A]lways run this type, [D]eny (default), "
                 "de[N]y this type, [Q]uit: "
              << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) return ConsentAnswer::Deny; // EOF/closed stdin
    if (line.size() == 1) {
        switch (line[0]) {
        case 'R':
        case 'r':
            return ConsentAnswer::Run;
        case 'A':
        case 'a':
            return ConsentAnswer::RunAll;
        case 'N':
        case 'n':
            return ConsentAnswer::DenyAll;
        case 'Q':
        case 'q':
            return ConsentAnswer::Abort;
        default:
            break;
        }
    }
    return ConsentAnswer::Deny; // empty input, 'D', or anything unrecognized
}

} // namespace openrar::sfx
