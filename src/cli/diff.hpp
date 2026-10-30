#ifndef OPENRAR_CLI_DIFF_HPP
#define OPENRAR_CLI_DIFF_HPP

#include <string>

namespace openrar::cli {

struct DiffOptions {
    std::string arc1_path;
    std::string arc2_path;
    std::string password;
    bool verbose{false}; // -v: also list identical files
    bool json{false};    // --json: emit RFC 8259 structured diff
};

// Performs a zero-extraction structural diff of two archives.
// Returns POSIX diff exit codes:
//   0: Archives are content-identical.
//   1: Structural or content differences detected.
//   2: Operational or syntax error (file missing, bad archive, bad password).
int diff_archives(const DiffOptions& opt);

} // namespace openrar::cli

#endif // OPENRAR_CLI_DIFF_HPP
