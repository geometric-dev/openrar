#include "sandbox_mode.hpp"

#include <cstdlib>
#include <string>

namespace openrar::sandbox {

bool platform_sandbox_available() {
    // M3a/b/c flip this per platform when the model's e2e proof-of-denial
    // suite is green on the CI leg that owns it (plan §5 M3 decision point;
    // kill signals pre-analysis §3). Until then every host takes the
    // in-process path — the wiring ships inert, behavior unchanged.
    return false;
}

SandboxMode sandbox_mode_for(bool in_proc_flag) {
    if (in_proc_flag) return SandboxMode::InProc;
    const char* env = std::getenv("OPENRAR_IN_PROC");
    if (env != nullptr && env[0] != '\0' && std::string(env) != "0") {
        return SandboxMode::InProc;
    }
    if (platform_sandbox_available()) return SandboxMode::Worker;
    return SandboxMode::InProc;
}

} // namespace openrar::sandbox
