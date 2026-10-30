#ifndef OPENRAR_CLI_INSPECT_HPP
#define OPENRAR_CLI_INSPECT_HPP

#include <string>

namespace openrar::cli {

struct InspectOptions {
    std::string arc_path;
    std::string password;
    bool json{false};
};

// Inspects the internal block structures, Cauchy RS16 geometry, encryption parameters,
// and service records of an archive with resilient fallback on truncated archives.
int inspect_archive(const InspectOptions& opt);

} // namespace openrar::cli

#endif // OPENRAR_CLI_INSPECT_HPP
