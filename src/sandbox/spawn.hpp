#ifndef OPENRAR_SANDBOX_SPAWN_HPP
#define OPENRAR_SANDBOX_SPAWN_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/sandbox/spawn.hpp — the per-OS privilege models for the worker spawn
//  (v1.30.0 §5.1; SECURITY_ARCHITECTURE). The broker/worker protocol and
//  policy are identical regardless of profile — the profile only wraps the
//  OS spawn/privilege layer:
//
//    Windows  — AppContainer (LPAC where the SDK/kernel supports it): the
//               child token carries no capability SIDs (no filesystem, no
//               network); inherited handles remain usable (access checks
//               happened at open time with the broker's token); Job Object
//               caps + mitigation policy ride along as defense-in-depth
//               (the v1.23 SFX pattern).
//    Linux    — fork/exec as usual; the CHILD installs its seccomp-BPF
//               allowlist (spawn.hpp install_linux_seccomp, called from
//               worker_main on --install-seccomp) after CRT init, before
//               any protocol I/O. Allowlist-as-data: the memory-management
//               family is INCLUDED (a sandbox must not kill malloc — Gate 0
//               directive 3); mmap PROT_EXEC is argument-filtered; default
//               action is RET_ERRNO(EPERM) so denials are observable.
//               CLONE_NEWUSER/NEWNS are probe-then-skip hardening, never
//               load-bearing (Ubuntu 24.04 AppArmor restricts them).
//    macOS    — not yet shipped (the honest-cut follow-up; the v1.29
//               directive needs TWO tested mechanisms, which AppContainer +
//               seccomp satisfy).
// ─────────────────────────────────────────────────────────────────────────────

#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h> // global scope only (ARCHITECTURE.md §3)
#endif

namespace openrar::sandbox {

enum class SpawnProfile {
    Unsandboxed,       // plain spawn (the control leg; also the loud fallback)
    PlatformSandboxed, // the OS privilege model below
};

// Cheap, side-effect-free probe backing the sandbox_mode_for() decision
// function. Per platform: Windows = the AppContainer profile APIs resolve;
// Linux = the kernel accepts a seccomp probe (PR_GET_SECCOMP); others false
// until a model ships.
bool platform_sandbox_available();

// Spawns the worker with `argv_tail` (the arguments after the executable
// path; quoting is the platform impl's job). Sets proc_out (HANDLE / pid as
// void*). The CHANNEL and VOLUME handles are the caller's concern (created
// inheritable before this call); this function only wraps the OS privilege
// layer. `sandboxed=false` must behave exactly like the plain spawn the
// unsandboxed e2e exercises.
bool spawn_worker_process(const std::string& exe, const std::vector<std::string>& argv_tail,
                          SpawnProfile profile, void** proc_out, std::string& err);

// Absolute path of the openrar_worker executable that ships beside the CLI
// binary (same directory; GetModuleFileName / /proc/self/exe). Empty when it
// cannot be located — the caller falls back in-process, loudly.
std::filesystem::path worker_exe_path();

#if defined(__linux__)
// Installs the seccomp allowlist. Called INSIDE the worker (post-CRT init,
// pre-protocol). Fail-closed: any install error terminates the worker with
// exit code 4 — a sandbox that cannot be installed must never fall back to
// an unsandboxed parse silently.
void install_linux_seccomp();
#endif

} // namespace openrar::sandbox

#endif // OPENRAR_SANDBOX_SPAWN_HPP
