#ifndef OPENRAR_SFX_PROCESS_EXEC_HPP
#define OPENRAR_SFX_PROCESS_EXEC_HPP

#include <functional>
#include <string>

namespace openrar::sfx {

// v1.23.0 process-execution contract (plan §5): the ONLY code path in the
// codebase that spawns SFX directive programs. No shell is involved on any
// platform — metacharacters in a directive command line are inert by
// construction. Containment is established BEFORE spawn; a containment-setup
// failure refuses execution (spawned=false, error set) — the process is
// never spawned uncontained (plan §2 invariant 2).

struct ExecResult {
    bool spawned{false};   // the process actually ran
    bool contained{false}; // containment established (true whenever spawned)
    bool cancelled{false}; // wait aborted via the cancellation callback
    int exit_code{0};      // child exit code (valid when spawned && !cancelled)
    std::string error;     // human-readable failure ("" on success)
};

// Resolves the executable named by the first token of a directive command
// line. Windows: tokenization per CreateProcessW rules (quoted or unquoted);
// a token with a drive/device prefix is used as-is when present, otherwise
// the destination directory (when allow_dest) and then PATH are searched.
// POSIX: '/'-absolute tokens are checked for executability; otherwise dest
// (when allow_dest) then $PATH. Returns the resolved path, or "" when the
// executable cannot be found (callers skip the directive without prompting).
std::string resolve_command_executable(const std::string& command, const std::string& dest_dir,
                                       bool allow_dest);

// Spawns the command (verbatim command line; resolved executable) contained
// in an OS job (Windows: Job Object with kill-on-close, per-process memory
// cap, active-process cap, ACG mitigation policy; POSIX: own process group,
// CPU/address-space/process-count rlimits, PDEATHSIG on Linux) and waits for
// exit, polling `cancelled`. On cancellation the child tree is killed and
// result.cancelled is set. AMSI scans the command line post-consent/pre-spawn
// as defense-in-depth; an AMSI failure is the documented fail-open and is
// reported via error (the spawn proceeds).
ExecResult spawn_contained(const std::string& command, const std::string& resolved_exe,
                           const std::string& working_dir,
                           const std::function<bool()>& cancelled = {});

// AMSI scan (Windows; no-op elsewhere => always true, amsi_failed=false).
// Returns false when AMSI flags the command as hostile. AMSI being
// unavailable sets amsi_failed=true and returns true — the documented
// defense-in-depth fail-open (plan §2 invariant 2).
bool amsi_scan_command(const std::string& command, bool& amsi_failed);

} // namespace openrar::sfx

#endif // OPENRAR_SFX_PROCESS_EXEC_HPP
