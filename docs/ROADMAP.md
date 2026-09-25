# 🗺️ OpenRAR Engineering Roadmap: v1.21.x → v1.30.0 (Tight Base → Enterprise & High-Throughput Era)

> **Provenance & Supersession.** Third revision. The first revision inserted
> the v1.21.1 stabilization gate after the v1.6.0 → v1.21.0 architect audit;
> the second folded in the residual P2 front-load (shipped as v1.21.2). This
> revision integrates the **Security & Hardening Architecture**
> (`docs/SECURITY_ARCHITECTURE.md`) — every security item now has a home in a
> specific release, with the policy conflicts it surfaced resolved in the
> document itself (exit taxonomy, argv passwords, ownership restore, MSan
> scoping, legacy-VM drop). It also carried one **OPEN P1** blocking the
> v1.22.0 release gate — since fixed (see CLOSED P1 below).

## Shipped Baseline (v1.21.x / v1.22.0 / v1.23.0 / v1.24.0)

- **v1.24.0 (current):** extraction containment & integrity shipped —
  syscall-level containment with write-through-handle atomic extraction
  (per-directory journals with a validated sweep), archive-internal
  collision detection (gate 2), --json-summary with stdout purity,
  links default-deny + session-scoped hardlinks + deferred dir metadata,
  unknown-extra preservation, generated Unicode 15.1.0 tables. Plan:
  docs/v1.24-implementation-plan.md (pre-analysis parsed critically —
  §0 verdicts; gaps found during the arc recorded in §0/§12).

- **v1.23.0:** advanced SFX scripting shipped with its security
  architecture — directive engine, consent framework with prompt-fatigue
  controls, TempMode hardening, Job Object runtime policy, WinGUI.SFX,
  -sfxnoexec kill switch, sandbox e2e suite (gate 2). convert_to_sfx
  POSIX exec-bit fix found by the e2e suite. See CHANGELOG 1.23.0.

- **v1.21.1 "Tight Base":** all 16 audit P1s closed; commit atomicity; `.rev`
  repair stem-anchoring + legacy `.rNN`; DLL CAS busy-claim; CLI fail-closed
  `t`; WASM hooks; changelog claim ledger.
- **v1.21.2:** residual P2 ledger (FCI-grid window snap, `:$DATA` rejection,
  owner-record conformance, oracle-mapped exit codes + interop stage 15,
  switch-dispatch tightening).
- **v1.21.25:** P1 roundtrip divergence closed (filter chunk-boundary root
  cause, see CLOSED P1); decompress-side window defaults aligned with the
  compressor's; platform-identified release asset names; linux-gcc-arm64
  (Raspberry Pi 64-bit) release package.
- **v1.22.0 (current):** SIMD arc complete and natively benchmarked —
  AVX-512 match kernel, GFNI RS16 fold (19.19× scalar), NEON RS16 fold
  (5.72× scalar); security baseline sweep complete (safe-integer math,
  terminal sanitization, KDF ceilings); `tools/../tests/bench` throughput
  harness in CI. See the v1.22.0 section below and CHANGELOG 1.22.0.
- **GFNI groundwork (merged PR #1):** AVX-512 match kernel + GFNI RS16 fold
  kernel, empirically calibrated, validated under Intel SDE in CI
  (`simd-validation` job); cross-implementation bit-exactness gate; local
  preflight harness (`make preflight`) covering every CI leg that can run
  locally.

## CLOSED P1 (was blocking v1.22.0) — FIXED

**Core-codec roundtrip divergence** — deterministic repro retained in
`tests/fuzz/crashes/` (rng(42) sweep, iteration 727, 80519 bytes; decoded
output diverged at offset 65535; cross-validation abort at pos 12364,
dist 12114 was the same defect seen through the match validator).
Root cause: `compress_buffer()` derived its pre-transform chunk length from
the *requested* window (1 MiB default) while the embedded packer emitted
filter tokens chunked by its *clamped* window (pow2 clamp, 128 KiB floor →
64 KiB tokens). The decoder un-transforms one token region per token and its
scan skips any CALL whose operand crosses the region end, so a boundary-
crossing E8 was transformed by the encoder's wider window and never reverted
(divergence at 65535: operand `41 10 ef 00` → `40 10 f0 00`, i.e. `addr +
0xFFFF`). The same mismatch let encoder matches cross decoder token regions,
tripping the cross-validation raw-source check (pos 12364). Two adjacent
defects fixed in the same pass: the large-input `mem_src` branch re-
transformed already-pre-transformed data (double transform), and the
pre-transform/token chunk length logic existed in two drifting copies — now
a single `filter_max_chunk()` derived from the packer's actual window
(`window_size()`), plus a `filter_pretransformed_` latch. Regression tests:
`test_e8_filter_chunk_boundary_symmetry`, `test_filter_chunk_boundary_roundtrip_p1`,
`test_filter_multi_token_multiblock_roundtrip`; the roundtrip fuzzer now
decodes with the matching raw-stream window (0x200000). Follow-up (done):
all decompress-side defaults were aligned to the compressor's 2 MiB default
(see Risk Register item 1).

---

## Guiding Principles

1. **Stabilization before velocity.** No feature release starts until the
   prior release's error-path debt is closed and negative-path tested.
2. **Format-legality gates** for anything that changes emitted bytes,
   reviewed against the RAR5 spec and the UnRAR oracle *before*
   implementation (`architect-challenge`).
3. **Definition of Done** per release: Failure-Mode Matrix + Negative-Test
   Plan; config plumbing traced through all engines; binding conformance
   tests for cross-language surfaces; changelog claim ledger with file:line;
   atomic commit paths.
4. **Security posture** (normative in `docs/SECURITY_ARCHITECTURE.md`):
   fail-closed; syscall-level containment over string sanitization;
   default-deny for links; never trust archive-provided metadata where the
   OS/transport can attest it locally; every policy decision surfaced to the
   user (output + exit code), never silent.

---

## v1.22.0 — Hardware Vectorization + Security Baseline Sweep

**SIMD (as re-scoped):** ✅ complete, measured natively
(`tools/openrar_bench`, informational CI step on native-hardware legs):
- RS16 `.rev` fold: **GFNI 19.19× scalar** (55.4 GiB/s, ubuntu runner,
  Ice Lake+ — the 5–10× claim is exceeded) and **NEON 5.72× scalar**
  (14.6 GiB/s, macOS Apple Silicon) vs the scalar table fold.
- Match-length: **AVX2 2.68× scalar** (79.5 GiB/s, ubuntu), SSE2 1.78×,
  NEON 0.77× on Apple Silicon (the compiler auto-vectorizes the scalar
  loop there — honest number, kept).
- AVX-512 match kernel: correctness-validated under SDE only; no native
  silicon in CI or locally, so no normative numbers (dispatching proven,
  ratio labeled non-normative).
Bit-exactness gate ✅ green across every dispatch path each leg can
execute (Scalar/SSE2/AVX2 x86-native, AVX-512 + GFNI under SDE, NEON on
Apple Silicon + QEMU aarch64).

**Blocking gates:**
1. ~~OPEN P1 roundtrip divergence~~ **FIXED** (see CLOSED P1 above) — root-
   caused, fixed, and pinned by regression tests built from the retained
   artifact. A codec correctness bug outranks everything.
2. Bit-exactness across all dispatch paths including the multi-chunk and
   final-partial-chunk boundaries; parity fuzz across dispatch paths.

**Security baseline sweep (§5.2/§5.3/§7.1 — small, fits the boundary-
discipline theme):** ✅ complete (v1.22.0).
- Safe-integer-math audit ✅ — untrusted-value arithmetic verified end to
  end: `read_vint` bounded with a tested overflow contract, subtractive
  bounds + explicit clamps in the header parser, size caps at every layer,
  UB-free unaligned readers. The existing check forms are already the
  overflow-safe ones; helper-conversion would be churn without a gain.
- Terminal-sanitization coverage audit ✅ — `sanitize_for_display`
  hardened (C0/C1, invalid UTF-8 per byte, bidi/isolates/marks, soft
  hyphen; valid non-ASCII passes through) with negative tests per
  category (`test_sanitize_for_display`).
- KDF cap verification ✅ — PBKDF2 `lg2_count` ceilings pinned at both
  header paths (`test_kdf_cap_pinned`: hostile lg2=25 refused fail-closed
  through a valid-CRC header, 24 accepted, 25..255 refused without
  derivation).

---

## v1.23.0 — Advanced SFX Scripting (Security-First Delivery)

The SFX directive engine ships **with** its security architecture — §6 is the
spec, not a follow-up:

- **Directive consent:** explicit UI consent for every side-effecting
  directive (`Setup=`, `Presetup=`, `Delete=`, `Shortcut=`, `Silent=`),
  default focus "Don't Run", post-sanitization names only.
- **Prompt fatigue controls:** batch consent, prompt coalescing, hard
  per-run prompt cap → abort.
- **TempMode hardening:** cryptographically random subdirectories,
  restrictive ACLs, defined cleanup — defeating local TOCTOU planting.
- **Runtime policy:** Job Objects (process/memory/CPU caps, kill-on-close);
  `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY`; stub compiled with CET/ACG/
  PIE-ASLR/CFG; AMSI scan of command lines as defense-in-depth.
- Dual SFX modules (`Default.SFX` console, `WinGUI.SFX` native dialogs).
- Sandbox e2e test executing directives in temp dirs; security design review
  via `architect-challenge` before implementation.

---

## v1.24.0 — Extraction Containment & Integrity (Security Release) ✅ SHIPPED

All six milestones landed with their gates green (TOCTOU fault injection +
collision matrix, 28/28 on MSVC and gcc-WSL). Full detail:
docs/v1.24-implementation-plan.md §12 (milestones) and §0 (pre-analysis
verdicts + the ten gaps found and closed during the arc). Highlights:

**Syscall-level containment (§4.1) ✅:**
- Windows: open-once NtCreateFile traversal rejecting reparse points per
  component, `GetFinalPathNameByHandle` containment assertion, **write
  through the same handle** (closes assert-then-write TOCTOU).
- POSIX: `openat2` `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS` (direct syscall,
  cached probe) with `openat` + `O_NOFOLLOW` dirfd walk fallback and a
  kernel-path prefix assertion on the final anchor (exercised for real by
  the WSL test leg).
- Namespace traps: 8.3 alias-shaped names (`NAME~X.ext`) rejected
  fail-closed in the walk and skipped-with-report in the CLI.

**Atomic extraction (§3.3) ✅:** crypto-random temp-in-destination writes,
journal-fsynced before each temp exists, atomic no-follow rename cascade
(NTFS POSIX-semantics rename anchored to the verified parent; renameat2 /
renamex_np / link-cascade on POSIX), per-directory journal manifests with a
forging-resistant validated sweep, FD use bounded by parallelism. This
landed as a deliberate drift alignment: the CLI's direct-to-destination
writes and the DLL's predictable-named temps were converged onto one
io-layer mechanism (ExtractionSession + AtomicWriter, two thin entry
points) — see Risk Register item 2 for the full record.

**Collision & overwrite policy (§3.2) ✅:** duplicate-identical, case-fold,
NFC/NFD, and file-vs-directory collisions abort with the structural exit
code before anything is written (gate 2); READONLY destinations fail the
commit everywhere.

**Encoding, trimming, UI invariant (§4.4) ✅:** displayed-name ≡
extracted-name pinned end-to-end; undecodable names percent-encoded
losslessly (the escaped name IS the filename); timestamp clamp bounds
parameterized (MtimeBounds).

**Exit policy + JSON summary (§3.1) ✅:** `--json-summary[=path]` with
strict stdout purity; statuses extracted/modified/skipped/failed/
unprocessed; security flags machine-readable; no new exit codes.

**Metadata fidelity (§6/§7) ✅:** links default-deny with decoupled opt-in;
session-scoped hardlinks with link-count/inode oracles; cross-device
hardlink fallback debiting the byte caps; SUID/SGID/sticky masked unless
`--preserve-suid`, modes umask-bounded; deferred directory metadata applied
bottom-up; unknown extra records preserved verbatim across mutations.

---

## v1.25.0 — Memory-Mapped Read Engine (Re-scoped)

Unchanged from the previous revision, now normatively backed by
SECURITY_ARCHITECTURE §5.2: **mmap for listing/header-scanning/random-read
only — never for extraction inputs** (POSIX files cannot be locked against
truncation; SIGBUS recovery is hostile to multithreaded engines). Buffered
`pread`/`ReadFile` remains the extraction path. Windows SEH → `RAR_ERR_IO`;
POSIX uses truncation-aware pre-flight + read fallback instead of signal
handlers. Limits (`max_header_bytes`/`max_total_bytes`) enforced on the
mapped path. New C ABI behind `OPENRAR_ABI_FEATURE_MMAP (1ull << 16)`.
Gates: 50 GB listing benchmark, truncation fault injection, limits-not-
bypassable.

---

## v1.26.0 — Content-Defined Chunking (format-legality gated)

Unchanged: Gate 0 format-legality review (cross-file backward references are
not legal RAR5 outside solid chains); filters disabled/bounded per CDC chunk
(same invariant class as `-mt` boundaries); bounded fingerprint index with
honest memory model fed to `compress_plan.hpp`. Cross-check: cumulative
output caps from `ExtractionLimits` apply to dedup-expanded streams
(SECURITY_ARCHITECTURE §5.4).

---

## v1.27.0 — Extended Attributes, Quarantine & MotW

Now normatively shaped by §4.3:
1. **MotW `-oz` policy:** zone data read exclusively from the archive file's
   own filesystem metadata / transport context; `Zone.Identifier` content
   **generated locally** — never parsed from archive-provided streams.
2. POSIX `user.*`/`security.*` xattrs as a new extra-record type (format-
   legality gate applies; graceful degradation on stock WinRAR).
3. macOS quarantine xattr + Finder tags; resource forks deferred to 2.1.
4. **Ownership policy normative:** setuid/setgid/sticky never restored;
   UID/GID/name restore remains explicit opt-in (`-ow`/`-og`/euid==0).
5. Prerequisites from v1.21.2 (`:$DATA` rejection, CRC-drop warnings)
   already landed — build on them.

---

## v1.28.0 — Interactive TUI & Benchmark Engine

As planned; security/UX addenda:
- **Terminal injection hardening is a release gate:** every rendered string
  passes the §7.1 sanitizer (ESC/CSI/OSC, RTL, invalid UTF-8) — the dual-
  progress TUI renders attacker-controlled filenames by construction.
- Non-TTY degradation (pipe/file/CI) as negative-path tests.
- Benchmark reproducibility (<5% variance) + `--json` output for CI trends.

---

## v1.29.0 — Archive Migration & Transcoder (`cv`) — Phase 1: ZIP / TAR / GZIP

Scope correction stands (7z/LZMA2 deferred to post-2.0), now with the
security constraints normative:
- **ZIP reader hardening (§2.1):** Central-Directory vs Local-Header
  metadata mismatch → integrity failure (abort), not best-effort — the CD/LF
  split is a classic weaponization surface.
- **Legacy RAR constraint (§5.3):** the RAR 2.x/3.x VM filter subsystem is
  not implemented; VM-filtered legacy entries fail with an explicit
  per-entry error. No partial VM emulation, ever.
- **Undecodable names:** losslessly escaped → skip-with-report through the
  transcode pipeline.
- Metadata translation layer (perms/attrs/UTF-8/timestamps/comments);
  `-df` verify-before-delete; bit-for-bit extraction equivalence + WinRAR
  readability oracle.

---

## v1.30.0 — OpenRAR 2.0 LTS: Enterprise Stability, Sandboxing & Universal SDKs

**Freeze prerequisites (blocking):**
1. ABI: bit-level conventions documented; `static_assert` coverage for all
   public structs; feature-bit registry current; the `solid`-parameter
   cleanup landed (✅ v1.21.2).
2. **Sandboxed worker mode (§5.1):** parser engine in a sandboxed worker
   process (AppContainer on Windows, seccomp-bPF on Linux, Seatbelt on
   macOS) with a broker holding file handles; library mode keeps bounds
   checks and path containment **non-disableable**, documented residual
   risk for embedders.
3. **Binding conformance policy:** per-binding C-ABI harness gates Python
   and C# SDKs (the WASM double-free/signature defects are the standing
   cautionary tale).
4. **Assurance:** attack regression corpus (historical archive CVE PoCs),
   differential testing vs `unrar`/`7z`, ASan/UBSan/TSan primary legs,
   **MSan best-effort on the Linux/clang leg**, persistent fuzzing with
   corpus retention and crash dedup.
5. **Supply chain:** signed release binaries, SBOM, `SECURITY.md`
   disclosure policy, hardened updaters.
6. Interop gate (15+ stages incl. exit-code parity) at 100%; multi-OS
   regression matrix green.
7. Audit P2 ledger triaged — anything touching the frozen surface fixed
   before the freeze.

**SDKs:** C/C++ wrapper (exists), WASM NPM (exists), Python PyPI wheels,
C# NuGet (`OpenRAR.NET`).

---

## 📊 Release Matrix

| Release | Primary Focus | Security Items | Blocking Gate |
| :--- | :--- | :--- | :--- |
| **v1.22.0** | SIMD + security baseline sweep | Safe-math audit, sanitization audit, KDF caps | ~~OPEN P1 roundtrip divergence~~ FIXED; bit-exactness gates |
| **v1.23.0** | SFX scripting | Full §6 consent/runtime policy | Security design review; sandbox e2e |
| **v1.24.0** | Extraction containment & integrity ✅ | §3/§4 syscall containment, atomic extraction, collisions, JSON summary | TOCTOU fault injection ✅; collision matrix ✅ |
| **v1.25.0** | mmap read engine (listing/random-read) | §5.2 normative no-mmap-for-extraction | Limits-not-bypassable; fault injection |
| **v1.26.0** | CDC deduplication | Cumulative caps on dedup streams | **Format-legality gate 0** |
| **v1.27.0** | xattr / quarantine / MotW | §4.3 MotW local-generation; ownership policy | Legality gate; multi-OS matrix |
| **v1.28.0** | TUI + benchmark | §7.1 terminal-injection gate | Non-TTY degradation; reproducibility |
| **v1.29.0** | Transcoder (ZIP/TAR/GZIP) | ZIP CD/LF hardening; legacy-VM drop; name escaping | Bit-for-bit equivalence |
| **v1.30.0** | 2.0 LTS + SDKs | §5.1 sandboxed worker; §7.2 assurance; supply chain | Freeze prerequisites 1–7 |

## ⚠️ Risk Register (top items)

1. **Raw-stream window contract** — RESOLVED: all decompress-side defaults
   (core `Decompressor50::DEFAULT_WIN_SIZE`, dll `openrar_decompress`/
   `decompress2`/`decompress_block`, wasm raw + stream-decoder fallbacks) now
   mirror the compressor's 2 MiB default; raw-stream consumers decoding
   non-default-window streams still pass the recorded dictionary size
   explicitly (archive/dll layers do). The P1 roundtrip divergence this item
   used to track is fixed (see CLOSED P1 above).
2. **v1.24 containment depth** — RESOLVED. The arc's premise was real
   drift, both directions: the CLI wrote directly at the destination
   (CreateAlways, no temp, destination truncated before content verified)
   while the DLL's `durable_write_to` did temp+rename with predictable
   `.openrar-tmp.<pid>.<seq>` names, no journal, and no no-clobber mode —
   and write-through-handle containment (M2) means whoever opens the write
   handle must run the containment walk, so two openers would have meant
   two drifting containment implementations. Converged on ONE mechanism in
   the io layer — ExtractionSession + AtomicWriter (crypto-random temps,
   per-directory journals, no-follow atomic rename cascade) — with two
   thin entry points (the reader's write paths for the CLI;
   `durable_write_to` for the DLL) and per-surface policy kept at the
   surface (CLI overwrite consent, DLL caller-chosen destination). The
   alternative — converting the CLI to stream through
   `extract_entry_stream` plus a DLL-style wrapper — was rejected: bigger
   refactor, less security gain, and the reader must own the handle
   anyway. WASM is in-memory (no drift by construction). Design
   refinement over the pre-analysis: per-directory journals instead of one
   root journal (plan §2.3, gap 6) — exact sweep coverage regardless of
   who knew the root, FD use bounded by parallelism, and the
   forged-journal attack closed by same-directory + exact-temp-shape
   record validation.
3. **v1.23 SFX auto-execution** — security-sensitive by construction;
   guardrails are scope, not polish.
4. **v1.26 CDC format legality** — the flagship reduction claim may not
   survive the legality gate in its original non-solid form.
5. **v1.30 sandboxed worker** — three OS sandbox models is enterprise-scale
   work; scope-kill criteria should be agreed at v1.29 review.
6. **Environment-dependent tests** — macOS jetsam / allocator-lazy-commit
   behavior differs from Linux/Windows; tests probing allocation failure
   must probe-then-skip (pattern established in v1.21.2).
