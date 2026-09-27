#include "sandbox_mode.hpp"
#include "spawn.hpp"

#include <cstdlib>
#include <string>

namespace openrar::sandbox {

// platform_sandbox_available() lives in spawn.cpp (the per-OS privilege
// models own it; M3a/b flip it when a model's proof-of-denial e2e is green
// on the CI leg that owns that model).

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
