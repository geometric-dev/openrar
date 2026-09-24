# v1.23.0 SFX Implementation Plan (Revision 2 — post design review)

**Status:** Resolves all ten directives in `docs/sfx-v1.23-design-review.md`
(🟡 conditional approval). Traceability table at §14. Directive-parser /
config-layer work (§3, §4) is cleared to proceed immediately; execution,
consent, and containment code follows this plan as revised.

Normative inputs: `docs/SECURITY_ARCHITECTURE.md` §3.1/§3.2/§3.3/§4/§6/§7.1,
`docs/ROADMAP.md` §v1.23.0. Codebase facts verified: `convert_to_sfx` is
CLI-only (never in the DLL/WASM surface); `ArchiveReader` has no comment-read
API today (§3 adds one); `FileBlock::sub_data` exists in the format layer for
service headers.

---

## 1. Scope & module layout

| Module | Path | Role |
| :--- | :--- | :--- |
| Shared engine | `src/sfx/sfx_config.hpp/.cpp` | Directive grammar, parser, `SfxConfig`, caps |
| Consent engine | `src/sfx/sfx_consent.hpp/.cpp` | The ONE consent decision function (both UIs consume it) |
| Prompt backends | `src/sfx/prompt_console.cpp`, `src/sfx/prompt_wingui.cpp` | `IPromptBackend` implementations (console stdin; Win32 dialogs) |
| Process contract | `src/sfx/process_exec.hpp/.cpp` | Spawn/wait/contain (Windows + POSIX), the only execution path in the codebase |
| Job containment | `src/sfx/job_object.hpp/.cpp` | Windows Job Object wrapper; POSIX rlimit/process-group counterpart |
| Stub wiring | `src/cli/sfx_main.cpp` (console), `src/cli/sfx_gui_main.cpp` (new, WinGUI) | Phase pipeline, `-sfxnoexec` |
| Reader comment | `src/archive/archive_reader.hpp/.cpp` | `read_archive_comment()` (§3) |

Directive *execution* lives only in the SFX stubs. It is unreachable from the
C ABI, WASM, or any library API — stated as a design invariant; the DLL/WASM
surfaces never gain it.

---

## 2. Normative consent invariants (§6.1 — restated as contract)

1. **No directive suppresses prompts.** `Silent=1|2` governs progress/dialog
   UI only. With `Silent=2`, consent prompts still appear (they are the only
   interactive moments), then everything else proceeds unattended.
2. **Containment-setup failure ⇒ refuse-to-execute.** If the Job Object or
   mitigation attributes cannot be established, that directive is refused
   (reported; exit mapping §8) and the process is never spawned. AMSI is the
   single documented fail-open (defense-in-depth only); its failure is
   recorded in the per-run report.
3. **Default focus is "Don't Run"** — a tested property of BOTH backends
   (console: `Enter` alone = Deny; dialog: default button = Don't Run).
4. **Non-interactive stdin ⇒ deny.** If the console backend detects stdin is
   not a TTY, every prompt resolves to Deny-with-report. There is no
   auto-consent anywhere (deliberately stricter than the CLI's overwrite
   `!isatty → Yes` behavior, which extracts — extraction ≠ execution).
5. **Prompts display only post-sanitization text** (§7.1 sanitizer) plus the
   verbatim command line and the resolved absolute executable path (fixed
   font, non-wrapping, no rich text).
6. **Every consent outcome is visible** in the per-run report (per §3.1
   granularity): `executed`, `skipped-consent`, `skipped-noexec`,
   `refused-containment`, `failed`.

---

## 3. Reader-side comment plumbing (review directive 4)

`ArchiveReader::read_archive_comment(std::vector<core::byte>& out)`:

- Locates the CMT service header already walked during scan (the scan reads
  every header; service entries are retained with `is_service=true`), and
  streams its data area through the existing payload machinery (CMT may be
  stored or compressed). **Multiple CMT headers (crafted archives): the
  first in archive order wins; subsequent ones are ignored + reported** —
  deterministic, matches the mutator's replace-on-write semantics.
- Caps: decoded comment > 1 MiB → returns `false`, comment treated as absent,
  directives disabled with a report line (a hostile CMT must not become a
  parse DoS; extraction itself is unaffected — fail-closed for *execution*,
  not for extraction).
- The stub opens its own file exactly as today (`sfx_offset()` frame:
  archive-relative offsets, converted to absolute only inside the reader —
  coordinate-frame rule honored; the stub never does offset math itself).
- UTF-8 end to end. A UTF-16/BOM comment is not RAR5-conformant; BOM-stripped
  and validated; invalid UTF-8 in the comment disables directives (report).
- Tests: `test_reader_archive_comment_roundtrip` (mutator-written CMT →
  reader accessor), `test_reader_comment_size_cap` (oversized → false),
  `test_reader_comment_absent` (no CMT → false, no error).

---

## 4. Directive grammar & parser (review directive 8)

- Grammar: lines `Key=Value`; keys case-insensitive (normalized to lower);
  LF or CRLF line endings; UTF-8; a UTF-8 BOM is tolerated and stripped.
- Keys: `setup`, `presetup`, `delete`, `shortcut`, `silent`, `path`,
  `overwrite`, `title`, `text`, `license`, `tempmode`, `exitcode`? — no:
  `exitcode` is not a directive (external `SetupCode` context only; we map
  child exits per §8). Unknown keys: counted + one aggregated report line
  (`ignored N unrecognized directives`), never an error (forward compat).
- Multiplicity: `setup`, `presetup`, `delete`, `shortcut` accumulate in
  order; `path`, `overwrite`, `silent`, `title`, `text`, `license`,
  `tempmode` — last occurrence wins (`text`/`license` accumulate as dialog
  body lines).
- Caps (enforced at parse; exceeding → that directive line ignored + report,
  total cap exceeded → directives disabled + report, extraction unaffected):
  ≤ 64 directive lines, ≤ 4096 bytes per value, decoded comment ≤ 1 MiB (§3).
- `SfxConfig` is immutable after parse — one parse per run, no re-parse
  between phases (mechanical-sympathy note from the review).

---

## 5. Process-execution contract (review directive 2)

**Windows:**
- `CreateProcessW`, never a shell. The directive string is used verbatim as
  `lpCommandLine`; `lpApplicationName = NULL` (Windows tokenization rules);
  metacharacters (`&`, `|`, `<`, `>`, `^`, `%`) are **inert by construction**
  because no `cmd.exe` exists in the path — documented as the metacharacter
  policy. Redirection/pipes are impossible by design.
- Resolution before the prompt: the executable token (per CreateProcess
  tokenization rules, including quoted first token) is resolved — `Setup`:
  destination dir first, then PATH; `Presetup`: PATH (or absolute) only, and
  it must exist pre-extraction. Unresolvable ⇒ skip + report (no prompt for
  something that cannot run). The resolved absolute path is what the prompt
  displays alongside the verbatim command.
- `lpCurrentDirectory = <resolved destination>` for `Setup` (WinRAR
  semantics); for `Presetup`, the original working directory.
- Containment: Job Object (§6) created and configured BEFORE spawn;
  `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY` on the child via attribute list
  (disables dynamic code, restricts child processes per §6.2); handle
  inheritance limited to nothing (no stdio inheritance into GUI children;
  console children inherit the stub's console handles explicitly).
- AMSI: `AmsiScanString` on the resolved command line, post-consent,
  pre-spawn, per §6.2 as defense-in-depth. Failure to initialize or scan is
  the one documented fail-open: the spawn proceeds, the failure is recorded
  in the report. Containment NEVER fails open (§2 invariant 2). A POSITIVE
  AMSI verdict (flagged command) triggers an additional warning prompt —
  default focus Don't Run, counted against the prompt cap — and declining
  skips the directive; an automatic refusal would lock out legitimate
  installers at AMSI's false-positive rate (measured: a benign self-probe
  command line was flagged on a stock Windows host).
- **Residual risk documented:** the resolved executable can be swapped
  between resolution and spawn by a local racer (classic TOCTOU, present in
  every OS shell-execution design including WinRAR). Containment limits the
  blast radius; noted in §12 residual risks, not silently ignored.

**POSIX (defined contract, not silence — review directive 2):**
- `fork`/`execv` (no shell). Child: `setpgid(0,0)` (own process group),
  `RLIMIT_CPU` (600 s), `RLIMIT_AS` (2 GiB), `RLIMIT_NPROC` (64) applied
  pre-exec. Linux: `PR_SET_PDEATHSIG(SIGKILL)`. macOS: no pdeathsig — the
  parent polls and kills the process group on abort/exit (kill-on-close
  analogue, documented delta).
- Same resolution, display, and working-directory rules as Windows.

**Child lifetime:** the stub waits for exit (WinRAR `SetupCode` semantics —
a hung installer hangs the SFX; documented). On stub death, Windows
kill-on-close + Linux pdeathsig kill the tree; macOS kills the process group
in the parent's exit handler where reachable. Children auto-join the job on
Windows (default).

---

## 6. Consent engine (review directives 2, 3)

- One engine, `SfxConsentEngine`, consuming an `IPromptBackend`:
  `Prompt ask(const ConsentRequest&)` with `Answer { Run, Deny, RunAll,
  DenyAll, Abort }`. Both modules differ ONLY in the backend (parity table
  §11).
- **Prompt cap: 8** prompts per run (fixed constant). Exceeding ⇒ abort the
  entire SFX run (extraction included if it is mid-flight — extraction is
  cancelled, children killed, exit 2 with report). The cap is counted per
  *prompt shown*, not per directive: coalescing reduces prompts below the
  directive count by design.
- Batch semantics: `RunAll`/`DenyAll` apply to the remainder of the SAME
  directive type within the run (a `RunAll` for `Setup` does not cover
  `Delete`). `Abort` ⇒ immediate cancellation, exit 255 (user-break per
  §3.1).
- Coalescing: all `Delete=` patterns are one prompt (one consent for the
  whole delete set). `Overwrite=1` (escalation) is one coalesced prompt
  reporting the count of colliding existing files. `Presetup`/`Setup`
  prompts occur in their phases and cannot be coalesced across time.
- In-variants from §2 are enforced IN the engine, not in the backends, so
  both UIs inherit them.

---

## 7. Phase pipeline & cancellation

Order: parse → `Presetup` (consent → contain → spawn → wait) → extraction
(existing secure pipeline: temp-name atomic extraction, zip-slip guard,
symlink policy — unchanged) → `Setup` (consent → contain → spawn with dest
CWD → wait) → `Delete` (consent → execute) → TempMode cleanup.

Cancellation & failure rules:
- Cancellation (Ctrl+C) or extraction failure at any point ⇒ remaining
  phases never run. **An incomplete extraction never reaches `Setup=` or
  `Delete=`** (no partial-tree installers).
- Consent refused (`Deny`) ⇒ that directive skipped + reported; extraction
  continues. This is user choice, not an error.
- TempMode: extraction target is `%TEMP%\OpenRAR-<128-bit-hex>` (POSIX:
  `mkdtemp` template), created 0700/owner-only ACE before any write; `Setup`
  runs with that dir as CWD; post-`Setup` cleanup deletes the subtree via
  the same containment-checked delete path; Job kill-on-close covers child
  cleanup on crash.
- **TempMode orphan policy (§3.3-compliant):** no pattern-matched sweeps of
  the temp root — §3.3 forbids them (pre-planted victims). Orphaned
  `OpenRAR-<hex>` dirs from killed runs are accepted residual risk: random
  128-bit names, owner-only ACLs, extracted-file content only. Roadmap note
  added for a future manifest-based sweep if telemetry justifies it.

`Delete=` scoping: patterns match against destination-relative paths only;
each match is resolved and containment-checked (must resolve inside dest,
symlink-hostile per §4.2) before deletion; per-file report lines
(`deleted` / `failed`); one coalesced consent for the set. Not restricted to
files extracted this run (WinRAR parity — consent + containment are the
controls), deviation-free.

`Overwrite=` mapping into the §3.2 engine (one decision function): `2` =
skip-existing (de-escalation, no prompt beyond the directive consent);
`0` = per-file ask (console) / dialog (WinGUI) via the engine; `1` =
overwrite-all is an ESCALATION and requires its own coalesced consent
prompt stating the collision count; without that consent it de-escalates to
`0`. The engine (not the stub) remains the overwrite decision function for
every extracted byte.

`Shortcut=`: fixed field grammar (Target, DestFolder, Name, Description,
IconFile,IconIndex — comma-separated, ≤ 6 fields, each ≤ 1024 bytes).
`DestFolder` is a restricted enum (DESKTOP, STARTMENU, PROGRAMS, QUICKLAUNCH)
resolved via `SHGetKnownFolderPath` — never a free-form path from the
archive. `Target`/`IconFile` must resolve inside the destination (post-
extraction) or be refused. Consent-gated like the others.

`Path=` **destination containment** (self-review gap — the directive proposes
where EVERY extracted byte lands, and `Silent=2` hides the start dialog that
would otherwise show it):
- `Path=` may only propose a directory inside the user's profile
  (`%USERPROFILE%` / `$HOME`, resolved) — system directories, other users'
  profiles, and drive roots are refused with a report line, before any
  extraction begins.
- The argv-provided destination (an explicit user choice) overrides `Path=`
  unconditionally; `TempMode` present ⇒ `Path=` is ignored entirely.
- Refused `Path=` ⇒ default destination (cwd-based, as today) + report;
  extraction proceeds.
- This is a containment check, NOT a consent prompt: the archive does not
  get to propose `C:\Windows\System32` even for the user to decline.

---

## 8. Exit-code mapping (review directive 5 — §3.1 taxonomy, no new codes)

| Event | Exit | Report |
| :--- | :--- | :--- |
| Extraction success, no directives | 0 | — |
| Directives present, all consented/denied, extraction complete | 0 | per-directive lines |
| `-sfxnoexec` active | 0 | `directives suppressed: -sfxnoexec` + count (observable, never silent) |
| Consent refused (any/all) | 0 | `skipped-consent` lines |
| Malformed/oversized CMT or directives | 0 | `directives disabled: malformed comment` (no execution; extraction unaffected) |
| Containment-setup failure (any directive) | 2 | `refused-containment` line(s); remaining directives refused |
| Prompt cap exceeded | 2 | `abort: prompt cap` (run cancelled, children killed) |
| Spawn failure (resolved path vanished/unspawnable) | 2 | `failed` line for the directive |
| `Setup` child exited nonzero | raw child code propagated (first nonzero across multiple `Setup=` wins) | `setup exit=N` (WinRAR `SetupCode` parity, documented) |

Propagation note: a child exiting 255 collides with the taxonomy's
user-break code — accepted and documented (WinRAR propagates raw codes the
same way); the per-run report disambiguates for anyone scripting against it.
| Cancellation (user) | 255 | per §3.1 user-break |
| Extraction failure (pre-existing semantics) | 2/3/11/13 per cause | unchanged |

---

## 9. `-sfxnoexec` kill switch (review directive 9)

Accepted as stub argv flag `-sfxnoexec` AND env `OPENRAR_SFX_NOEXEC=1`
(CI systems control env more easily than argv). When active: the phase
pipeline runs parse → extraction → cleanup ONLY; every directive is counted
and reported (`directives suppressed: -sfxnoexec (N)`), consent prompts never
appear, no process is ever spawned, exit 0. The suppression is always
printed (stdout for console stub; for WinGUI a non-modal report line in the
progress UI + exit code), satisfying the fail-open-verification gate:
suppressed ≠ silent.

---

## 10. Compiler mitigations (§6.2, unchanged from pre-analysis, now pinned)

Both stubs (console + WinGUI): `/guard:cf` (CFG), `/CETCOMPAT` (CET shadow
stack, x64), `/DYNAMICBASE` + `/HIGHENTROPYVA` (ASLR), `/NXCOMPAT` (DEP),
ACG via the child-process mitigation policy on the SFX process itself at
startup (`SetProcessMitigationPolicy(ProcessDynamicCodePolicy)`), stack
canaries default-on MSVC; MinGW/POSIX builds: `-fstack-protector-strong`,
`-D_FORTIFY_SOURCE=2`, PIE. CI asserts the mitigation presence via a build-
log grep (like the GFNI activation gate) so a toolchain change cannot
silently drop them.

---

## 11. Dual-module parity trace (review directive 7)

| Control | Default.SFX (console) | WinGUI.SFX (dialogs) |
| :--- | :--- | :--- |
| Parser / `SfxConfig` | shared | shared |
| Consent engine | shared | shared |
| Consent UI | stdin/stdout prompt; **Enter = Deny**; non-TTY stdin ⇒ Deny | MessageBox-style task dialog; **default button = Don't Run**; never attaches a console; backend/dialog failure ⇒ Deny + report (fail-closed) |
| Batch options | `A=run all / N=deny all` keys | task-dialog command buttons |
| Prompt cap | engine (8) | engine (8) |
| `-sfxnoexec` | argv + env, printed | argv + env, progress-UI line |
| Sanitizer | §7.1 on every echoed name | §7.1 before any dialog text |
| `Silent` handling | suppresses progress bar only | suppresses progress dialog only |
| Error reporting (no console case) | stderr | message box on fatal + exit code |
| Exit codes | §8 | §8 |

---

## 12. Failure-Mode Matrix (review directive 1)

Phases: P=parse, C=consent, PR=Presetup, E=extract, S=Setup, D=delete, T=TempMode cleanup.
Failure classes: malformed input, OOM, disk-full, privilege, spawn failure, containment failure, cap exceeded, cancellation, crafted archive.

| Phase \ Class | malformed input | OOM / disk-full | privilege | spawn failure | containment failure | cap exceeded | cancellation | crafted archive |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| P | directives disabled + report, extraction continues (exit 0) | directives disabled + report | — | — | — | CMT >1 MiB ⇒ disabled + report | — | hostile CMT ⇒ same as malformed; CRC gates already reject corrupt headers |
| C | n/a (parser handled) | deny + report | — | — | — | **abort whole run, exit 2** | exit 255 | forged names ⇒ sanitizer; non-TTY ⇒ deny |
| PR | skip + report | child OOM ⇒ child's exit | spawn error ⇒ skip + exit 2 | skip + report + exit 2 | **refuse, report, exit 2** | via C | kill group, no extraction starts | unresolved path ⇒ skip (no prompt) |
| E | existing pipeline semantics | existing pipeline (abort, no S/D) | existing pipeline | — | — | via C | existing cancel; **no S/D ever** | existing zip-slip/symlink guards |
| S | skip + report | child OOM | exit 2 | skip + report + exit 2 | refuse, report, exit 2 | via C | kill job; no D | resolved-in-dest check |
| D | pattern parse fail ⇒ skip + report | partial delete ⇒ per-file failed lines | ACL-denied ⇒ per-file failed | — | refuse, report, exit 2 | via C | stop deleting, report | containment check per match |
| T | — | best-effort cleanup, report | — | — | — | — | best-effort | random name + owner ACL make orphans inert |

Every `skip + report` and every abort is user-visible in output AND exit code
(fail-open-verification gate).

---

## 13. Negative-Test Plan (review directive 1 — named, deterministic)

Run in the sandbox e2e harness (gate 2); all spawn paths exercised with a
benign probe executable and a probe sentinel:

1. `sfx_consent_default_focus_console` — Enter alone denies; probe NOT run.
2. `sfx_consent_default_focus_dialog` — WinGUI default button denies (UI
   automation probe).
3. `sfx_prompt_cap_aborts` — 9 side-effecting directives ⇒ abort at 8, exit 2.
4. `sfx_consent_refused_reports` — Deny ⇒ `skipped-consent` lines, exit 0.
5. `sfx_noexec_suppresses_and_reports` — flag + env paths, exit 0, count
   reported, zero spawns (spawn counter in probe).
6. `sfx_containment_failure_refuses` — fault-injected Job Object creation
   ⇒ refused-containment, exit 2, no spawn.
7. `sfx_presetup_requires_existing_path` — unresolved Presetup ⇒ skip, no
   prompt.
8. `sfx_setup_runs_in_dest_cwd` — probe asserts its CWD == dest.
9. `sfx_delete_scoped_to_dest` — pattern matching a file OUTSIDE dest is
   not touched; symlink escaping dest is not followed.
10. `sfx_tempmode_random_and_acl` — two runs produce different dir names;
    ACL denies a second-user probe (or mode 0700 on POSIX).
11. `sfx_tempmode_toctou_probe` — pre-planted `OpenRAR-<predictable>` dir
    with a symlink is untouched by the run.
12. `sfx_overwrite_escalation_needs_consent` — `Overwrite=1` denied ⇒ falls
    back to ask/skip, never silent overwrite.
13. `sfx_overwrite_maps_policy_engine` — collision semantics identical to
    CLI extraction of the same archive (one decision function).
14. `sfx_malformed_cmt_disables_directives` — garbage comment ⇒ extraction
    succeeds, exit 0, directives-disabled report, zero spawns.
15. `sfx_shortcut_enum_only` — archive-controlled DestFolder outside the
    enum is refused.
16. `sfx_cancel_kills_setup` — cancel during Setup wait ⇒ job killed (probe
    child observed dead), exit 255, no Delete.
17. `sfx_extraction_failure_blocks_setup` — induced extraction failure ⇒ no
    spawn of Setup.
18. `sfx_noninteractive_denies` — piped stdin ⇒ all directives denied with
    report, exit 0.
19. `sfx_child_exit_code_propagates` — probe exits 7 ⇒ SFX exits 7.
20. `sfx_mitigations_present` — build-log gate: CFG/CET/ACG flags asserted
    (grep gate like the GFNI activation check).
21. `sfx_path_system_dir_refused` — `Path=C:\Windows\System32` (POSIX:
    `/usr/bin`) ⇒ refused with report, extraction lands in the default
    destination.
22. `sfx_path_outside_profile_refused` — `Path=` to another user's profile
    or a drive root ⇒ refused + report.
23. `sfx_path_argv_overrides` — explicit destination argument beats a valid
    `Path=`.
24. `sfx_tempmode_overrides_path` — `TempMode` present ⇒ `Path=` ignored.
25. `sfx_multiple_cmt_first_wins` — two CMT headers ⇒ first parsed,
    second ignored + reported.

---

## 14. Directive traceability & sequencing

| Review directive | Resolved in |
| :--- | :--- |
| 1 FMM + negative tests | §12, §13 |
| 2 execution contract | §5 |
| 3 consent invariants | §2, §6 |
| 4 CMT plumbing | §3 |
| 5 exit-code mapping | §8 |
| 6 Delete/Overwrite scoping | §7 |
| 7 parity table | §11 |
| 8 resource caps | §4 |
| 9 `-sfxnoexec` observability | §9 |
| 10 child lifetime | §5, §7 |

Milestones: **M1** parser + `SfxConfig` + reader comment accessor (+tests §3,
§4) — cleared to start now. **M2** consent engine + console backend.
**M3** process contract + job containment (+ fault-injection tests).
**M4** phase pipeline, Delete/Overwrite/TempMode. **M5** WinGUI backend.
**M6** sandbox e2e suite (gate 2). **M7** docs, changelog with file:line
anchors, interop notes.

Residual risks accepted and documented: Setup-exe swap TOCTOU (§5), TempMode
orphans (§7), macOS kill-on-close delta (§5).
