# 🏛️ Principal Architect Challenge & Review — v1.23.0 Advanced SFX Scripting

**Plan Reviewed:** Deep Research Analysis: v1.23.0 — Advanced SFX Scripting &
Installer Directives (Revision 2, 2026-09-23) — the pre-analysis supplied by
the product owner, reviewed against `docs/SECURITY_ARCHITECTURE.md` §6
(normative), §3.1 (exit taxonomy), §7.1 (terminal sanitizer), and
`docs/ROADMAP.md` §v1.23.0. Codebase facts verified against
`src/cli/sfx_main.cpp` (472 lines, standalone extractor, no comment parsing),
`src/archive/archive_mutator.cpp` (`convert_to_sfx`, CMT *writing* only), and
`src/archive/archive_reader.hpp` (`sfx_offset()` exists; **no comment-read
API**).

**Architect Verdict:** 🟡 CONDITIONAL APPROVAL (REVISIONS REQUIRED)

The security *direction* is correct and §6-aligned. The plan is **not**
approved for implementation until the ten directives in §7 land — most
critically the Failure-Mode Matrix, the process-execution contract, and the
consent invariants.

---

## 1. Executive Summary & Core Posture

The pre-analysis internalized the hardest lesson this project has on file: SFX
auto-execution is the malware-delivery vehicle, so consent, containment, and
mitigations ship **in the same release**, not as a follow-up. The consent
framework (default focus "Don't Run", post-sanitization names, batch consent,
coalescing, hard cap → abort) matches §6.1 verbatim, the runtime policy matches
§6.2, and the dual-module split with one shared parser/security engine is the
right shape — one consent engine is also exactly the single-decision-function
posture the v1.21.1 audit demands.

The plan's core vulnerability is that it specifies the *happy consent flow*
well and everything else thinly. The consent prompt is the security boundary —
and the plan never defines what the prompt actually displays for `Setup=` /
`Presetup=` (which reference attacker-controlled *command strings*, not entry
names), never defines the process-spawn contract (a single `cmd.exe /c`
interpolation away from trivially violating everything else in the design),
never states what happens when containment setup fails, and omits the
reader-side comment plumbing its own phase 1 depends on. None of these are
design flaws — they are specification gaps, and for a feature whose threat
model is "the archive is hostile," unspecified is unauditable.

Verdict logic: §6 alignment is genuinely good; the Pillar-7 hard-gate artifacts
are absent; several fail-closed seams are open by omission. Conditional
approval with the revisions below is the correct gate outcome.

---

## 2. 🚨 Critical Red Flags & Fatal Flaws

- **Process-execution contract is undefined.** `Setup=`/`Presetup=` carry
  attacker-authored command strings. If the stub ever routes them through
  `cmd.exe /c` interpolation, `&`, `|`, redirection, and quoting tricks
  bypass every other control in the design. The plan must mandate
  `CreateProcessW` with explicit `lpApplicationName`/`lpCommandLine` built
  from validated tokens (no shell), state the metacharacter policy (reject or
  literal-only), and define the equivalent POSIX spawn contract.
- **Prompt content contract is undefined.** §6.1 says prompts show
  post-sanitization *entry names* — but `Setup=` is a *command*, not an entry.
  The prompt must show the verbatim command string (fixed-font, non-wrapping,
  no rich-text) AND the resolved executable path post-validation. Homoglyph /
  lookalike display tricks otherwise defeat the human in the loop.
- **Containment-failure semantics unstated.** If `CreateJobObject`,
  `SetInformationJobObject`, or the mitigation attribute fails — run
  uncontained? Fail-closed posture (§3, §6) requires: **any** containment-setup
  failure = refuse to execute that directive (abort per the prompt-cap
  semantics), never run uncontained. AMSI is the one deliberate exception
  (defense-in-depth only) and the plan must say so explicitly, with the
  failure visible in output + exit code.
- **Reader-side comment plumbing is missing from Files Touched.** Verified:
  `ArchiveReader` exposes `sfx_offset()` but has **no** comment-read API; the
  mutator only *writes* CMT. Phase 1 ("read the archive's main comment
  block") has no implementation home. Either extend the reader's
  library-mode parse or parse the CMT service header directly in the stub
  against `format/` — but it must be named, sized, and capped.
- **`Silent=2` is one consent prompt away from a dropper UX.** The plan
  implies consent cannot be suppressed; it never states the invariant.
  Mandate explicitly: **no directive, including `Silent=2`, suppresses the
  consent prompts** — `Silent` only governs progress UI. Silent=2 + consent =
  unattended *after* consent, never before.

---

## 3. ⚠️ Weak Assumptions & Fragile Invariants

- **"The comment block is readable"** — see above; no read API exists.
  Also unspecified: CMT size cap (a 16 MiB comment must not become a 16 MiB
  parse), directive-count cap, per-directive string-length caps feeding the
  prompt cap, and the comment's encoding (RAR5 CMT is UTF-8; the parser must
  be UTF-8-aware end to end).
- **"Job Object containment" is Windows-only.** The plan is silent on POSIX.
  There is no Job Object on Linux/macOS. Either define the POSIX containment
  contract (process groups + `RLIMIT_CPU`/`RLIMIT_AS`/`RLIMIT_NPROC`,
  kill-on-close via PR_SET_PDEATHSIG) or explicitly refuse side-effecting
  directives on POSIX in v1.23 (fail-closed, message + exit code). Silence
  here = shipping an uncontained execution path.
- **`Overwrite=` directive vs §3.2 policy engine.** The directive must map
  INTO the existing overwrite/collision policy engine, not bypass it — one
  decision function for overwrite semantics across CLI, library, and SFX
  (audit learning #8). An SFX-only overwrite path is a divergence by
  construction.
- **`Delete=` scoping.** Unspecified. Must be: destination-directory-scoped,
  operates on pattern-matched files within it, symlink-hostile (never
  resolves out of the destination per §4.2), consent-gated, and reported
  per-entry (`deleted`/`failed`).
- **`Presetup=` executes code chosen by the archive from the host disk.**
  Consent covers it; validation must additionally resolve the path and
  confirm existence *before* the prompt, and display the resolved path —
  "the archive told me to run `C:\Windows\System32\cmd.exe`" must be visible
  as such.
- **Exit codes.** The plan cites external `SetupCode` handling but never maps
  phase failures into §3.1's pinned taxonomy. Every failure (consent refused,
  cap exceeded, containment refused, spawn failure, Setup nonzero exit) needs
  a defined exit code from {2, 3, 10, 11, 13, 255} — no new namespace.

---

## 4. 🧱 Happy-Path Bias Screen (Pillar 7)

- **Failure-Mode Matrix**: **HIT** — absent. Required before implementation,
  per phase (parse → Presetup → extraction → Setup → Delete → cleanup) ×
  failure class (malformed directive, OOM, disk-full, missing privilege,
  spawn failure, containment failure, consent refused, cap exceeded,
  cancellation mid-extraction, crafted comment).
- **Negative-Test Plan**: **HIT** — the sandbox e2e list is a good skeleton
  but is scenario-level, not per-failure-branch. Enumerate deterministic
  tests by name for every P0/P1-capable branch (see §7 directive 1).
- **Parallel/Streaming divergence (dual-module analogue)**: **HIT (pending)**
  — the plan's "differ only in UI layer" needs a parity trace table: every
  control (consent, batch, coalescing, cap, `-sfxnoexec`, sanitization)
  × {Default.SFX, WinGUI.SFX} × engine state.
- **Destructive commit paths**: **HIT (spec gap)** — `Delete=` is destructive
  by nature; the commit protocol (what is deleted, when, and what survives a
  failure mid-delete) must be stated.
- **Fail-open verification**: **HIT (by omission)** — `-sfxnoexec` must be
  observable: when set, suppressed directives are reported (output + exit
  code), never silently swallowed. Same for refused-consent outcomes.
- **Golden-path-only verification**: **HIT** — covered by the first two
  directives; "tests to follow" is a rejection criterion.
- **Changelog-as-contract drift**: **CLEAN** (pre-release; anchor discipline
  applies at release time).
- **Binding-layer blindness**: **CHECK AT PLAN FINALIZATION** — if
  `convert_to_sfx` / comment injection is (or becomes) reachable through the
  C ABI or WASM surface, the plan must name the cross-language conformance
  test; directive *execution* must never be reachable from the library
  surface at all (stub-only, by design — state it).
- **Decision-function determinism**: **CLEAN (must be preserved)** — one
  consent engine, one overwrite policy engine; both modules and the CLI
  consume them. Name them in the plan.
- **Coordinate-frame stability**: **HIT (minor)** — the stub reads itself at
  `sfx_offset()`; state the frame (absolute file offsets vs archive-relative)
  for the comment parse and keep conversions at the boundary.

---

## 5. ⚡ High-Performance & Mechanical Sympathy Gaps

Not a hot path; extraction dominates any directive cost. Two notes only:
- Parse the comment once into an immutable `SfxConfig`; no re-parsing between
  phases.
- Job Object + mitigation attribute setup is ~10 syscalls total; do not
  cache or pool processes — containment correctness outranks spawn latency,
  and the kill-on-close lifetime model is incompatible with pooling.

---

## 6. 🔍 Omissions, Edge Cases & Recovery Gaps

- Duplicate directives (`Setup=` twice, `Delete=` overlapping patterns):
  coalescing spec must define ordering, dedup, and prompt grouping.
- Cancellation mid-extraction with `Setup=` pending: abort semantics must
  leave Setup unexecuted (extraction incomplete ⇒ post-phase never runs —
  state it; no partial-tree installer runs).
- `Setup=` exit-code propagation: nonzero child exit ⇒ SFX exit code? Per
  §3.1, map to the taxonomy and surface per-entry/per-phase reports.
- TempMode cleanup on crash: kill-on-close covers children; the temp
  directory itself needs a defined orphan story (next-run sweep of
  `TempMode`-prefixed dirs older than N? state it — orphaned crypto-random
  dirs must still be owner-ACL'd so orphans are harmless).
- WinGUI.SFX has no console: its error-reporting path (message box vs
  exit-code-only) must be defined, and it must never attach a console.
- `Shortcut=` targets: desktop/start-menu write locations need the same
  containment review as extraction paths (shell-folder resolution, no
  archive-controlled absolute paths outside user scope).

---

## 7. 🛠️ Actionable Revision Directives

1. **Add the Failure-Mode Matrix** (phases × failure classes above) and the
   **Negative-Test Plan** with named, deterministic tests per P0/P1 branch —
   including: consent refused → skip + report; prompt cap → abort with
   taxonomy exit code; containment-setup failure → refuse-to-execute;
   `-sfxnoexec` → all directives suppressed AND reported; malformed
   directive → parse abort; crafted CMT at size cap; TempMode TOCTOU probe
   (pre-planted predictable dir + symlink) → unaffected; hostile command
   string (metacharacters) → refused or literal-safe.
2. **Write the process-execution contract**: `CreateProcessW` with explicit
   application name, no shell, metacharacter policy, POSIX story (defined
   containment or explicit v1.23 refusal), and the prompt display contract
   (verbatim command + resolved path, fixed font).
3. **State the consent invariants as normative**: no directive suppresses
   prompts (incl. `Silent=2`); containment-setup failure ⇒ refuse; AMSI is
   the only documented fail-open (defense-in-depth) and its failure is
   visible; default focus "Don't Run" is a tested property of BOTH UIs.
4. **Add reader-side CMT access to Files Touched** with the chosen module,
   size/count/length caps, UTF-8 handling, and the coordinate frame for
   self-offset reading.
5. **Define the §3.1 exit-code mapping** for every phase failure.
6. **Spec `Delete=` scoping** (destination-only, symlink-hostile, engine
   mediated) and map `Overwrite=` into the §3.2 policy engine.
7. **Add the dual-module parity trace table** and the WinGUI no-console
   error-reporting story.
8. **Define resource caps** for the CMT and directives feeding the prompt
   cap; define TempMode orphan cleanup.
9. **Make `-sfxnoexec` observable** (reporting + exit code).
10. **Specify child-process lifetime**: Job Object flags (kill-on-close,
    caps), children auto-join, and the cancellation rule that an aborted or
    failed extraction never reaches `Setup=`.

---

## Gate Outcome

Per `docs/ROADMAP.md` §v1.23.0, this review is blocking gate 1. The plan is
**conditionally approved**: implement the ten directives into the
implementation plan, then the sandbox e2e test suite (gate 2) is built
against the revised plan. Implementation of the directive parser may proceed
in parallel only for the parser/config layer (§4.1) — **not** for any
execution, consent, or containment code, which is gated on the revised plan.
