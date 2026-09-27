#ifndef OPENRAR_SANDBOX_SANDBOX_MODE_HPP
#define OPENRAR_SANDBOX_SANDBOX_MODE_HPP

// ─────────────────────────────────────────────────────────────────────────────
//  src/sandbox/sandbox_mode.hpp — THE single sandbox-mode decision function
//  (Gate 0 directive 1; the is_vt_supported_for lesson from v1.28: one
//  decision function, every site consumes it, no scattered spawn attempts).
//
//  Inputs, in priority order (first match wins):
//    1. --in-proc flag            → InProc (explicit kill-switch)
//    2. OPENRAR_IN_PROC=1 env     → InProc (env kill-switch, the
//                                   OPENRAR_NO_MMAP pattern)
//    3. platform model probe      → Worker when the OS model is available
//                                   (AppContainer/LPAC, seccomp-BPF,
//                                   Seatbelt — probe must be CHEAP and
//                                   SIDE-EFFECT-FREE)
//    4. otherwise                 → InProc
//
//  Spawn failures at RUNTIME are not a mode decision: they are the loud
//  fallback (one W: notice per run + sandbox_active disclosure in the JSON
//  summary) — policy-visible, never silent (plan §1.3, directive 6).
// ─────────────────────────────────────────────────────────────────────────────

namespace openrar::sandbox {

enum class SandboxMode {
    Worker, // parse/decode in the sandboxed worker process
    InProc, // in-process (today's engine; the documented fallback posture)
};

// The ONE decision function consumed by every command path.
// `in_proc_flag` carries the --in-proc CLI switch (false when absent).
SandboxMode sandbox_mode_for(bool in_proc_flag);

// Platform probe: does THIS OS provide a worker sandbox model that this
// build can engage? Cheap, side-effect-free, no process spawned. Returns
// false on every platform until the per-OS models land (M3a/b/c) — the CLI
// wiring can ship against this stub with zero behavior change, and each
// model flips its own probe when its e2e proof-of-denial suite is green.
bool platform_sandbox_available();

} // namespace openrar::sandbox

#endif // OPENRAR_SANDBOX_SANDBOX_MODE_HPP
