# 🗺️ OpenRAR Engineering Roadmap: v1.21.1 → v1.30.0 (Tight Base → Enterprise & High-Throughput Era)

> **Provenance & Supersession.** This document re-baselines the original "Enterprise & High-Throughput Era"
> blueprint after the full **v1.6.0 → v1.21.0 architect walkthrough audit** (2026-09-20). The audit verified
> 19/19 CTest suites green, a genuinely frozen additive C ABI, and real feature delivery across 15 releases —
> but also surfaced **16 P1 and ~30 P2 defects**, concentrated in error paths, multi-object interactions, the
> parallel/WASM surface, and CHANGELOG claims that do not match the code. This roadmap therefore inserts a
> **stabilization gate (v1.21.1)** before any new feature work, re-scopes three items whose baselines were
> mis-stated (v1.22 SIMD, v1.24 hardlinks, v1.27 metadata), and adds format-legality and security gates to
> the two highest-risk blueprints (v1.26 CDC, v1.23 SFX). It supersedes the external blueprint document.

---

## Corrected Baseline: What v1.21.0 Actually Is

Verified by the audit (build clean, 19/19 suites, six parallel subsystem reviews):

**Solid.**
- Frozen additive C ABI: `OPENRAR_DLL_API_VERSION = 1` unchanged since v1.6.0; 64-byte
  `openrar_archive_entry_t` byte-identical, pinned by size + field-offset static asserts; feature bits
  `1<<7`…`1<<15` uniquely assigned and all exposed; every `extern "C"` export exception-contained.
- Plan/schedule/execute separation (`compress_plan.hpp`) is load-bearing (CLI admission gating).
- Chunk-parallel `-mt` pipeline is thread-safe by construction (per-chunk encoder instances, ordered
  FIFO emission, captured exceptions rethrown on main).
- Key-material hygiene: every derived copy wiped; constant-time password compares; PBKDF2 count guards.
- Dual-oracle interop gate (14 stages) against WinRAR/UnRAR 7.20.

**Not solid (the audit's P1 ledger — all owned by a release below).**
- Compression: `Filters50::detect_filter` 32-bit wraparound → heap OOB read (SIGSEGV) on crafted content;
  `StreamDecoder` per-block decode breaks on filter regions spanning block boundaries (reproduced);
  >4 GiB-window OOB in the batched hash-insert path; decompressor filter-budget/ordering regressions on
  crafted streams.
- Mutation/commit: `lock_archive` destroys the original on rename failure (remove→rename, not
  `atomic_replace`); unchecked `write` returns can commit truncated archives on disk-full; deleted files
  orphan NTFS ADS/ACL children onto the next surviving entry; multi-volume lock is non-atomic across the
  chain; volume rewrite drops `MHEXTRA_METADATA`; `-mc` silently ignored on the parallel path for
  >16 MiB files; 32-bit slice-size truncation.
- Recovery: `.rev` repair adopts foreign recovery sets and renames valid volumes `.bad` (reproduced);
  legacy `.r00`-numbered volumes invisible to repair (reproduced).
- DLL: `set_limits` TOCTOU data race; repair exports return `RAR_ERR_ABORTED` after the mutation completed.
- CLI: `t` reports OK / exit 0 for encrypted entries with no password (fail-open, pre-existing).
- WASM: `createArchive` double-frees the hooks pointer on every hooks-using call; progress callback
  registered with wrong signature (`'vijj'` vs `'vjji'`) → module trap on first C-ABI callback.

**CHANGELOG claims corrected by the audit** (must be fixed in the v1.21.1 changelog pass):
`win_size = min(dict, chunk)` header recording (not implemented) · "worker counts clamped to 16" (switch
clamp is 64; 16 only in the chunk pipeline) · v1.14 "O(dictionary window)" multi-volume ceiling (actual:
O(vol_size) slice buffer; guard is `< 1024`, not `>= 4096`) · v1.20 "residual buffer contamination" bug
(not reproducible; memsets are defense-in-depth) · v1.9.3 `RAR_ERR_BAD_DATA` (code maps UTF-8 rejection to
`RAR_ERR_TRUNCATED`).

---

## Guiding Principles (the "Tight Base" Discipline)

1. **Stabilization before velocity.** No feature release starts until the prior release's error-path debt
   is closed and negative-path tested.
2. **Format-legality gates for format-touching features.** Anything that changes what bytes we emit
   (CDC, SFX directives, transcoding) gets an explicit legality/interop review against the RAR5 spec and
   UnRAR oracle *before* implementation, via the `architect-challenge` skill.
3. **Definition of Done for every release** (enforced by `architect-walkthrough`):
   - **Failure-Mode Matrix** in the plan: for each new code path — disk-full / short write, rename
     failure, OOM, cancellation mid-operation, crafted/malformed input, missing privilege.
   - **Negative-Path Tests:** at least one deterministic test per P0/P1-capable failure branch.
     "Skipped verification" must be visible in output *and* reflected in the exit code. Fail-open is a
     defect by definition.
   - **Config-Plumbing Trace:** every new switch/flag traced through *all* execution engines
     (sequential, spool, direct-stream, parallel, WASM). A knob honored on one path and silently dropped
     on its parallel/streaming variant is a P1.
   - **Binding Conformance:** every C-ABI export and JS/TS/Py binding change ships with a test that
     exercises the actual cross-language path (the v1.21.0 audit proved C++-green does not protect the
     JS layer).
   - **Changelog as Auditable Contract:** every `Added`/`Fixed` bullet verified against code with
     `file:line` evidence before the release commit is made.
   - **Commit Atomicity:** originals are never removed/renamed until the replacement is fully written
     *and* validated; repair flows validate the adopted set before any destructive step.
4. **Exit-code parity is an interop concern.** WinRAR/unrar's exit-code taxonomy is part of CLI parity;
   scripts must be able to distinguish corrupt-archive from warning from password-error. Add as an
   interop-gate stage.

---

## v1.21.1 — Stabilization: "Tight Base" (blocking gate for all feature work)

**Goal:** close every audit P1, the P2s that gate later roadmap items, and the changelog corrections.
**No feature work may start in v1.22.0 until this release ships.**

### Track A — Compression engine (audit agent: compression)
1. `Filters50::detect_filter`: fix `pe_off + 6 < size` 32-bit wraparound → 64-bit arithmetic
   (`src/compress/filters50.cpp:50`). Add crafted-`e_lfanew` regression test (SIGSEGV repro exists).
2. `Decompressor50::decompress_block` / `StreamDecoder`: carry `filters_` + `file_base_` across
   per-block calls in solid mode; defer flush failure for incomplete regions. Add
   `StreamEncoder`(filters)`→StreamDecoder` roundtrip test (currently zero filter coverage).
3. Restore a window-based filter budget and ordering validation in `SIZE_MAX`-output mode
   (`decompressor50.cpp:741-745`); reject overlapping/backward `block_start`.
4. Guard the batched hash-insert fast path against `is_large_window_` (`compressor50.cpp:1235-1242`).
5. Defend `StreamDecoder(0)` / `Decompressor50(0)`; clamp or reject in constructors.
6. Plumb `FilterConfig` into the parallel pipeline or refuse/fall back with a warning when filters are
   requested with `-mt>1` (kills the `-mt1`/`-mt4` output divergence).
7. Decide and document the real header `win_size` policy for chunked files (implement the claimed
   `min(dict, chunk)` or correct the changelog).

### Track B — Mutation & commit atomicity (audit agent: mutator)
1. `lock_archive`: use `atomic_replace()` (single-volume) and a two-phase rename (all temp files written,
   then committed) for the multi-volume chain.
2. Check every `FileStream::write` / `write_signature` / `write_main_block` / `write_end_block` return on
   commit paths; bail with `RAR_ERR_IO` (≈14 sites in `archive_mutator.cpp`).
3. `delete_entries` / `delete_entries_by_index`: mark trailing ADS/ACL child services of removed entries
   (mirror the replace path at `write_batch_add_ex`).
4. `add_file_to_archive_vol`: seed the new `MainBlock` from the existing head volume (preserve
   `MHEXTRA_METADATA` and retained arc flags).
5. Reject `vol_size` > `SIZE_MAX` (32-bit slice truncation in `read_packed_slice`).
6. Hygiene: `mutation_temp_path` + `CreateNew` in `convert_to_sfx`; temp-file litter in
   `delete_entries_impl`; exempt `RR` from trailing-service replace marking; guard the append-path
   QO locator under header encryption; remove dead `plan_batch` / duplicate `move_file_to_archive_vol`
   overload; check `copy_stream_region` seek failure; fix error-code mismatch on `-hp` append.

### Track C — Recovery & volumes (audit agent: recovery)
1. `.rev` scan in `repair_rev_volumes`: anchor candidates to the archive stem; adopt a set only after at
   least one present volume validates against its table; defer `.bad` renames until then.
2. Admit legacy `.{irr}NN` extensions in the data-volume scan (wire up the dead `slot_for_name` legacy
   branch); treat present-but-unmapped volumes consistently.
3. `RevCleanupGuard`: only delete `final_path` for outputs this run actually renamed.
4. Fix the output-stream leak on `CreateAlways` failure in `repair_rev_volumes`; cap `nr × CHUNK` parity
   allocation via `calculate_parity_buffer_size`; correct CLI diagnostic attribution on repair/rv paths;
   probe 5-digit `.partNNNNN` widths in `vol_name_to_first_name`.
5. Add the missing tests: multi-chunk roundtrip in C++ (mjs-only today), legacy-numbering set, foreign-
   `.rev` poisoning, and an automated UnRAR-consumes-OpenRAR-parity oracle test.

### Track D — DLL ABI (audit agent: ABI)
1. `set_limits`: CAS-claim the busy flag (fixes the TOCTOU *and* same-handle reentrancy); make
   extract/test paths CAS-claim too.
2. Repair-family exports: drop post-operation cancel checks (or thread a cancel flag into
   `RecoveryWriter`); never report `RAR_ERR_ABORTED` for a committed mutation.
3. `openrar_archive_create_file_ex`: remove the undocumented `solid`-parameter bit-smuggling
   (`is_solid = (solid != 0)`); filters enter via the documented filter channel or a new export.
4. `entry_owner`: fail `RAR_ERR_NOMEM` on allocation failure (zero flags, free siblings); add
   `static_assert(sizeof(openrar_entry_owner_t) == 20)`.
5. `set_limits`: define/reset `LimitState` semantics or document handle-lifetime accumulation.

### Track E — CLI & UX
1. `t` on encrypted entries without a password: print `SKIPPED (encrypted — no password)` and exit
   non-zero (fail closed).
2. Harden `-md` parsing (reject non-finite/trailing garbage; range-check before cast); fix
   `std::string::back()` UB on empty `x`/`e` args; case-normalize the duplicate-target set on Windows;
   restore stdout mode after `p`.
3. Exit-code taxonomy: map to WinRAR/unrar conventions (fatal/CRC/locked/open/CLI/password/user-break)
   **and** add exit-code parity as interop-gate stage 15.
4. P3 switch-hygiene batch (`rr*`/`s*` prefix over-acceptance, bare `-x@`, `-ver*` prefix, silent
   `-md` shrink warnings, help-text lag for `-og`/`--owner=`/`-rv`/`-qo`/`-am(s)`).

### Track F — WASM & bindings
1. `createArchive`: drop `hooksPtr` from the `finally` free list (double free).
2. Register the progress callback with signature `'vjji'` and convert BigInt args.
3. `MHEXTRA_METADATA` name-length overflow check (subtraction form, `header_reader.cpp:388`) —
   wasm-aborting today.
4. Add a JS conformance test that passes `onProgress`/`signal` through the real C ABI (the existing
   "double-free regression" test passes `hooksPtr=0` and is dormant); fix `RarEntry.method` TS type.

### Track G — Changelog corrections
Correct the five inaccurate claims listed under *Corrected Baseline*; backfill audit-derived notes where
the shipped description would mislead future maintainers.

**Gate:** all 19 suites + every fix's negative-path test green; full dual-oracle re-run (stages 1–14);
WASM JS suite incl. new hooks conformance; clang-format/tidy gates.

---

## v1.22.0 — Hardware Vectorization: AVX-512, GFNI & NEON (re-scoped)

**Already exists (do not re-plan):** runtime CPU dispatch (`core::cpu.hpp` `get_cpu_features()` /
`describe_acceleration()`), SSE2/AVX2/NEON match finder (`src/compress/arch/match_simd.hpp`, v1.11).

**Net-new scope:**
- AVX-512BW match kernels + runtime function-pointer dispatch extension; GFNI
  (`_mm512_gf2p8affine_epi64_epi8`) decomposition of $GF(2^{16})$ Cauchy RS math; NEON `vmull_p64`
  polynomial path.
- Vectorize `RecoveryWriter`/`rs16` parity generation and decode (5–10× `.rev` target).

**Gates (per Guiding Principles):**
- Bit-exactness: Scalar == AVX2 == AVX-512 == NEON on identical inputs, including the multi-chunk and
  final-partial-chunk boundaries (the exact code paths v1.21.1 hardens — land v1.21.1 first or the
  bit-exactness gate enshrines broken reference behavior).
- Parity fuzzing: random sector corruption → 100% recovery across all dispatch paths.
- TSan stress on the dispatch warm-up path (function-pointer swap vs. in-flight workers).

---

## v1.23.0 — Advanced SFX Scripting & Installer Directives (security-gated)

Scope as originally planned (directive parser: `Path`/`Setup`/`Presetup`/`Silent`/`Overwrite`/`Title`/
`Text`/`License`; `Default.SFX` + `WinGUI.SFX` dual modules) **plus a mandatory security design review:**

- `Setup=`/`Presetup=` auto-execution from archive comments is the canonical malware-launcher pattern.
  Required guardrails before implementation: explicit user consent surface in interactive mode (never a
  bare "run?" prompt habituated by default-Enter), `Silent=2` policy documented as a trust decision,
  an opt-in CLI kill-switch (`-sfxnoexec` or equivalent) for CI/CD, and docs that state the threat model.
- Prerequisite from v1.21.1: `convert_to_sfx` temp-name hardening already landed — build on it.
- Headless sandbox integration test executing directives in a temp dir (paths, overwrite policies,
  setup commands) as originally specified.

---

## v1.24.0 — Metadata & Mutation Fidelity Hardening (re-baselined)

**Baseline correction:** hardlink dedup (`-oh`) shipped in v1.8.0 — `FHEXTRA_REDIR` emission
(`archive_mutator.hpp`), Windows `FILE_ID_INFO`/128-bit fallback + POSIX `(st_dev, st_ino)` identity
(`main.cpp`), three-phase link restoration. This release is *hardening*, not delivery:

1. Link-failure fallback: cross-volume/unsupported-filesystem link creation falls back to full copy with
   a non-fatal warning (verify + test; fix if absent).
2. Link-count verification oracle: `fsutil hardlink list` / `stat -c %h` in the dual-oracle suite.
3. Unknown-extra-record preservation policy: header re-serialization currently drops extras the parser
   doesn't model (audit structural finding). Define the policy (preserve raw bytes where possible),
   implement, and test with third-party-written extras.
4. Mutation-level test debt: versioning pipeline, multi-volume lock, and metadata preservation currently
   have no C++ mutation tests (format-level only) — close the gap the audit flagged.
5. WASM binding hardening follow-through: extend the v1.21.1 JS conformance suite into a reusable
   per-binding harness (preparation for v1.30 SDKs).

---

## v1.25.0 — Zero-Copy Memory-Mapped Read Engine (re-scoped)

**Scope reduction:** mmap for **listing, header scanning, and random-read extraction** only. Not for
sequential extraction. Rationale: POSIX `SIGBUS` handlers are process-wide and async-signal-safety-hostile
in a multithreaded engine; the sequential extraction path is already buffered and bounded.

- Windows: `CreateFileMappingW` + `MapViewOfFile` with SEH `__try/__except` → `RAR_ERR_IO` (acceptable).
  POSIX: mmap with truncation-aware pre-flight (size check + conservative read fallback on short/truncated
  files) instead of `SIGBUS` handlers.
- Zero-copy vint/header decode directly off the page cache behind the existing `HeaderReader` interface.
- Integration requirement: mapped header bytes still count against `ExtractionLimits`
  (`max_header_bytes`/`max_total_bytes`) — limits must not be bypassable by the fast path.
- C ABI: `openrar_archive_open_mmap(...)` behind `OPENRAR_ABI_FEATURE_MMAP (1ull << 16)`.
- Gates: 50 GB listing benchmark vs. streaming; fault injection (truncate mid-read, network drop) proving
  clean error paths; limits enforcement under mmap.

---

## v1.26.0 — Content-Defined Chunking Stream Deduplication (format-legality gated)

**Gate 0 — format legality (blocking, via `architect-challenge`):** the original blueprint claims
"format-legal backward copy reference" across *different files in non-solid archives*. A non-solid
entry's LZ stream can only reference its own earlier output; cross-file dedup requires solid chains or
same-stream co-location. The design review must resolve this before any implementation — candidate
shapes: (a) solid-chain-scoped dedup, (b) same-stream (single large file) CDC, (c) emit-duplicate-bytes
fallback when out of window. The >50% reduction claim is re-validated against whichever shape survives.

**Implementation invariants carried from the `-mt` audit:**
- CDC chunk boundaries interact with pre-processing filters exactly like `-mt` chunk boundaries: filters
  are disabled or transform-bounded *per chunk*, never across boundaries (same bug class as the
  parallel-filter defect fixed in v1.21.1 Track A6).
- Bounded fingerprint index (LRU ≤ 64 MB) with an honest memory estimate fed into `compress_plan.hpp`
  so admission gating stays truthful (the current 5W+6MB model already underestimates >4 GiB windows —
  fix that model in this release).
- Byte-identical extraction in UnRAR 7.20 on the dedup-shaped stream; ≥50% reduction re-measured on
  versioned build trees.

---

## v1.27.0 — Extended Attributes, Quarantine & MotW (re-scoped)

**Baseline correction:** NTFS ADS archiving/extraction shipped in v1.7 (`win32_meta` models
`:Zone.Identifier`); net-new is policy + platforms:

1. `-oz` MotW extraction policy: default strips Zone.Identifier on extraction; `-oz+` preserves for
   audit workflows. Document the SmartScreen rationale.
2. POSIX `user.*`/`security.*` xattrs (`getxattr`/`setxattr`, `listxattr`) as a new extra-record type —
   note: **this is a format extension beyond WinRAR's emitted set**; run the format-legality gate and
   define graceful degradation when archives carrying it meet stock WinRAR.
3. macOS quarantine xattr + Finder tags; resource forks explicitly *deferred* to 2.1 (scope control).
4. Prerequisite from v1.21.1: the `:$DATA` stream-name rejection and silent CRC-drop warnings in
   `restore_children` are already fixed — build on them.
5. Multi-OS roundtrip matrix (SELinux labels, quarantine bits, Zone.Identifier) per the original plan.

---

## v1.28.0 — Interactive Dual-Progress TUI & Benchmark Engine (`b`)

As originally planned; low risk. Notes:
- VT detection and ANSI rendering infrastructure already exist (`is_vt_supported()`, `sfx_main` progress
  renderer) — reuse, don't duplicate.
- Non-TTY degradation (pipe/file/CI) must be a negative-path test, not an afterthought.
- Benchmark reproducibility gate (<5% variance) and a `--json` output for CI trend tracking.

---

## v1.29.0 — Archive Migration & Transcoder (`cv`) — Phase 1: ZIP / TAR / GZIP

**Scope correction:** the original plan underestimates 7z enormously (LZMA2 + filter chains + encrypted
headers + CRC64, from scratch, zero-dependency). Phase it:

- **v1.29.0:** ZIP (DEFLATE decoder), TAR, GZIP stream readers → RAR5 via the existing streaming
  compressor; metadata translation layer (Unix perms, DOS attrs, UTF-8 names, timestamps, comments);
  `-df` with verify-before-delete; bit-for-bit extraction equivalence gate + WinRAR readability oracle.
- **Deferred (post-2.0 / 2.1):** 7z/LZMA2 — schedule only after a dedicated feasibility review.

---

## v1.30.0 — OpenRAR 2.0 LTS: Enterprise Stability & Universal SDKs

**Freeze prerequisites (all blocking):**
1. ABI: v1.21.1's `solid`-parameter cleanup landed; every bit-level convention on public parameters
   documented in `openrar_dll.h`; `static_assert` coverage for all public structs (incl. the 20-byte
   owner struct); feature-bit registry comment current through bit 16.
2. Binding conformance policy: the reusable per-binding harness (from v1.24.0 item 5) gates Python and
   C# SDKs — the WASM double-free/signature defects are the standing cautionary tale. Each SDK ships
   with C-ABI-level tests, not just C++ suites.
3. Fuzzing campaign: cost the "1-billion iteration" claim honestly (nightly minutes-scale today);
   either budget the compute or state a realistic certification target. ASan/MSan/UBSan matrices.
4. Interop gate: 15 stages (14 + exit-code parity from v1.21.1 Track E3) at 100%.
5. Multi-OS regression matrix (Win 10/11/Server, Ubuntu/RHEL/Alpine, macOS x86_64/ARM64) green.

**SDKs:** C/C++ header-only wrapper (exists), WASM NPM (exists), Python PyPI wheels, C# NuGet
(`OpenRAR.NET`, async streaming P/Invoke).

---

## 📊 Release Matrix

| Release | Primary Focus | Audit Items Retired | Blocking Gate |
| :--- | :--- | :--- | :--- |
| **v1.21.1** | Stabilization ("tight base") | All 16 P1s + gate-relevant P2s + changelog corrections | Negative-path tests per fix; dual-oracle 1–14; WASM hooks conformance |
| **v1.22.0** | AVX-512 / GFNI / RS16 vectorization | — (consumes Track A hardening) | 4-way bit-exactness incl. chunk boundaries; parity fuzz across dispatch |
| **v1.23.0** | SFX installer directives | — | Security design review (Setup= consent model); sandbox e2e |
| **v1.24.0** | Metadata & mutation fidelity hardening | Structural test debt (versioning/lock/metadata tests) | Unknown-extra preservation; hardlink oracle |
| **v1.25.0** | mmap read engine (listing/random-read) | — | Limits-not-bypassable; fault injection; no SIGBUS handlers |
| **v1.26.0** | CDC deduplication | — | **Format-legality gate 0**; filter/boundary invariants; honest memory model |
| **v1.27.0** | xattr / quarantine / MotW | — | Legality gate for the xattr record; multi-OS matrix |
| **v1.28.0** | TUI + benchmark | — | Non-TTY degradation tests; reproducibility <5% |
| **v1.29.0** | Transcoder Phase 1 (ZIP/TAR/GZIP) | — | Bit-for-bit equivalence; 7z formally deferred |
| **v1.30.0** | 2.0 LTS + SDKs | — | Freeze prerequisites 1–5 above |

## ⚠️ Risk Register (top items)

1. **v1.26 CDC format legality** — the flagship reduction claim may not survive the legality gate in its
   original (non-solid, cross-file) form. Resolve Gate 0 before announcing.
2. **v1.23 SFX auto-execution** — security-sensitive by construction; guardrails are scope, not polish.
3. **v1.25 POSIX fault tolerance** — signal-based fault translation rejected by design; fallback strategy
   must be proven against truncation races.
4. **v1.30 freeze with latent P2s** — the audit's P2 list (≈30 items) must be triaged: anything touching
   the frozen surface gets fixed *before* the freeze.
5. **Changelog drift** — five incorrect claims already shipped; Track G fixes history, and the
   Definition-of-Done changelog gate prevents recurrence.
