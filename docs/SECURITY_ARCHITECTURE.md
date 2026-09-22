# OpenRAR Security & Hardening Architecture

> **Status:** Normative target architecture (v1.22.0 → v1.30.0). This revision
> integrates four policy corrections required by shipped behavior — see
> §3.1 (exit taxonomy), §5.3 (argv passwords), §4.3 (ownership restore),
> §7.2 (MSan scoping) — each marked **[Revised]** with rationale. Items
> already implemented in shipped releases are marked **[Basline: done]** so
> the document is honest about the starting point. Sequencing lives in
> `docs/ROADMAP.md`.

## 1. Executive Summary

This document defines the security architecture, threat model, and hardening
strategy for **OpenRAR**, a C++ archive parsing and extraction engine. Because
archive extractors bridge untrusted data and the local filesystem, the
architecture adopts a defense-in-depth, fail-closed posture. It addresses
historical archive vulnerabilities (CVE-2025-8088 path traversal,
CVE-2023-38831 spoofing, CVE-2023-40477 recovery-volume overflow) and
establishes containment boundaries at the OS syscall and process levels.

## 2. Threat Model

### 2.1. Trust Boundaries & Assets
* **Untrusted Input:** Any archive file, its headers, structural metadata
  (including ZIP Central Directory vs Local Header relationships), recovery
  volumes (`.rev`), passwords, and embedded execution directives (SFX).
* **Assets to Protect:** The host filesystem (no arbitrary writes/overwrites
  outside the target directory), system memory, CPU/disk resources, and user
  execution consent.

### 2.2. Adversary Capabilities (In-Scope)
* **Malicious Archive Construction:** Mathematically impossible headers,
  cyclic offsets, polyglots, decompression bombs, weaponized legacy-format
  features (RAR 2.x/3.x VM filters — which OpenRAR does not implement, see
  §5.3).
* **Local Racing:** A different local user pre-planting symlinks/junctions in
  the target directory or manipulating `TempMode` extraction paths (TOCTOU).
* **Terminal Manipulation:** Filenames carrying ANSI escape sequences
  (ESC/CSI/OSC), RTL overrides (U+202E), or invalid UTF-8 crafted to deceive
  users in CLI environments.

### 2.3. Out of Scope
* **Elevated Execution:** OpenRAR assumes standard user privileges. Defending
  against a root/SYSTEM attacker compromising the OpenRAR process is out of
  scope.

## 3. Unified Security Policy & Posture

### 3.1. Violation Response & Exit Taxonomy **[Revised]**
Fail-closed, with granularity split between the **process exit code** and the
**per-entry report**:

* **Exit codes use the WinRAR/unrar taxonomy already pinned by interop-gate
  stage 15** (fatal=2, bad-archive=13, CRC=3, usage=7, bad-password=11,
  no-files=10, user-break=255). Security violations map into it: path
  traversal and structural integrity failures → 2 (or 13 where the archive is
  unparseable); **no new exit-code namespace is introduced** — scripts and
  oracles already depend on the taxonomy, and a parallel security namespace
  would break exit-code parity that the interop gate certifies.
* **Per-entry granularity** (`extracted`, `modified`, `skipped`, `failed`,
  `unprocessed` for encrypted headers awaiting a password) is carried by a
  **machine-readable JSON summary** written at the end of the run — the
  security detail lives there, not in the exit code.
* Classification:
  * *Abort run:* path traversal attempts, structural bounds failures,
    CD-vs-LFH metadata mismatch, names empty after trimming — integrity
    failures of the archive as a whole.
  * *Skip-or-modify entry, report:* checksum failures, clamped timestamps,
    losslessly escaped filenames, ENOSPC mid-extraction.

### 3.2. Overwrite & Collision Policy
* Default clobbering is prohibited. Overwrites require explicit user consent
  or CLI flags (`-o+`). `FILE_ATTRIBUTE_READONLY` is respected.
* **Archive-internal collisions are integrity failures:** duplicate identical
  entries, case-insensitive collisions (`Readme` vs `README`), macOS
  NFC/NFD normalization collisions, and file-vs-directory type collisions
  within one archive. (Baseline: v1.21.2 already case-folds extraction
  targets to defeat the parallel-write race; the policy here strengthens
  detection-from-rejection and covers in-archive duplicates.)

### 3.3. Atomic Extraction
* Files are extracted to cryptographically unique temporary names
  (`filename.<random>.tmp`) created **in the destination directory** (same
  volume) so completion is an atomic rename. On Windows the rename uses
  POSIX-semantics `FileRenameInformationEx`.
* *Cleanup:* `FILE_FLAG_DELETE_ON_CLOSE` prevents atomic renaming, so
  abandoned temps rely on best-effort disposition-on-abort
  (`FILE_DISPOSITION_INFO`). Startup sweeps delete only temps recorded in a
  **per-run journal manifest** — never pattern-matched sweeps, which an
  attacker could feed pre-planted victims.

## 4. Extraction Boundaries & File System Security

### 4.1. Syscall-Level Path Containment
String sanitization (stripping `../`) is defense-in-depth only. Primary
containment is enforced at the OS syscall level:
* **POSIX:** `openat2` with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS` where
  available; fallback to `openat()` tree walks anchored to a pinned root
  `dirfd`, strictly applying `O_NOFOLLOW`.
* **Windows:** path resolution rejects reparse points on every component; the
  engine opens the file once via that traversal, calls
  `GetFinalPathNameByHandle` as a final containment assertion, and **writes
  through that same already-open handle** — closing the assert-then-write
  TOCTOU gap.
* **Windows namespace traps:** drive-relative paths, reserved device names
  (`CON`), colons (ADS operators), UNC/`\\.\` device paths, and alias-shaped
  names (`NAME~X.ext`) are rejected to prevent resolution through 8.3
  short-name alias space. (Baseline: DOS-device stems and colon rules exist
  since v1.9.0/v1.21.2; the alias-space rejection is new.)

### 4.2. Entry Semantics (Symlinks, Hardlinks)
* **Default deny:** symlinks, hardlinks, and NTFS junctions are never
  extracted by default.
* **Decoupled opt-in:** with symlink extraction explicitly enabled, absolute
  links and links traversing above the extraction root are unconditionally
  rejected. **Link-creation policy is decoupled from path-resolution policy:**
  every subsequent file write still goes through the no-follow walk of §4.1.
  Hardlinks, if opted in, may only point to files created within the same
  extraction session.

### 4.3. Metadata, Ownership & MotW **[Revised]**
* **Ownership & security bits:** setuid/setgid/sticky bits are **never**
  restored at any privilege level. Numeric UID/GID and name restoration
  (shipped in v1.17, `-ow`/`-og`) remains available **only as explicit
  opt-in** (`-ow`/`-og` or euid == 0) for backup-restore workflows —
  matching WinRAR behavior — and is documented as a trust decision. The
  original draft's unconditional "never restore ownership" contradicted
  shipped, oracle-tested behavior; the defensible line is
  *default-deny + explicit opt-in + never privilege bits*.
* ACL restoration remains heavily-flagged opt-in.
* **MotW & quarantine forgery:** Mark-of-the-Web (Windows `Zone.Identifier`)
  and macOS quarantine attributes are first-class. Zone data is read
  exclusively from the **archive file's own filesystem metadata** (or
  transport context); fresh `Zone.Identifier` content is **generated
  locally** per extracted file. OpenRAR never parses or trusts
  zone/quarantine stream content from inside the archive.

### 4.4. Encoding, Normalization & Timestamps
* **Filename encoding:** names in legacy formats or invalid UTF-8 are decoded
  per format spec; undecodable names are losslessly escaped and feed the
  skip-with-report path.
* **UI invariant (CVE-2023-38831 mitigation):** the displayed entry name must
  exactly match the name of the file extracted to disk; the listing UI never
  executes files.
* **Trimming:** on Windows, trailing spaces and dots are trimmed **before
  both** UI rendering and extraction-path evaluation.
* **Timestamps:** absurd file/directory `mtime` values are clamped (bounds
  parameterized); clamps feed skip-with-report.

## 5. Memory Robustness, Parser Security & Limits

### 5.1. Parser Sandboxing & Library Mode
* **Sandboxed worker:** the parsing engine runs in a sandboxed worker process
  (AppContainer / seccomp-bPF / Seatbelt) while a broker holds file handles.
* **Library mode:** for mail gateways and scanners embedding OpenRAR as a
  library, where no AppContainer worker exists, memory bounds checking,
  entry-count caps, and path containment are **strictly non-disableable by
  embedders**.

### 5.2. Memory & Hardware Faults
* **No mmap for extraction inputs:** a POSIX file cannot be locked against
  truncation by local racers even opened read-only, and SIGBUS-based recovery
  is fragile in multithreaded engines. Buffered `pread`/`ReadFile` I/O is
  used universally. (Normative since the v1.25.0 re-scope in
  `docs/ROADMAP.md`; mmap remains allowed for listing/random-read with
  pre-flight size checks.)
* **Safe math & bounds checking:** overflow-checked arithmetic
  (`__builtin_*_overflow` family) is mandatory for vint decoding, size
  calculations, and offset arithmetic — preventing `malloc(size+1) → 0`
  classes. Hardened accessors for all span-like views.
* **Parity bounds (CVE-2023-40477 mitigation):** all `.rev` parity matrix
  integers (block counts, stripe sizes) are bounds-checked against sane
  maximums before SIMD allocations. (Baseline: workspace caps and geometry
  guards shipped in v1.21.1/v1.21.2.)

### 5.3. Crypto & Format Internals
* **KDF caps:** RAR5 PBKDF2 iteration counts are capped to prevent per-entry
  CPU DoS. (Baseline: `lg2_count ≤ 24` enforced at both header paths since
  v1.9.x.)
* **Password hygiene — [Revised]:** memory is zeroized post-use; all password
  checks are constant-time (both baseline). **argv passwords are retained**
  for WinRAR CLI parity (`-p<password>`; scripts depend on it) with the
  exposure documented; additional channels are provided — interactive prompt,
  environment variable, and stdin — and the docs state that command-line
  arguments are visible to same-user local processes. The original draft's
  "never accepted on argv" broke shipped, oracle-tested CLI parity.
* **Encrypted headers:** path/extension policy runs strictly *after* header
  decryption.
* **Legacy formats:** the RAR 2.x/3.x **Virtual Machine filter subsystem is
  not implemented and will not be** (historically the vector for
  malicious unpack filters). Consequence for the v1.29.0 transcoder: legacy
  RAR archives are read for migration, and entries whose compression requires
  VM filters fail with an explicit per-entry error (see ROADMAP v1.29.0).

### 5.4. Resource Limits & DoS Prevention
* **Decompression bombs:** declared unpacked sizes are checked against
  absolute hard caps before decompression; cumulative output caps and
  wall-clock timeouts enforced. (Baseline: `ExtractionLimits` +
  `MAX_STREAM_OUTPUT` shipped v1.9.3/v1.12.)
* **Nested archives:** no recursion by default. Opt-in embedded scanning
  (mail-gateway mode) enforces depth, cumulative-size, and timeout caps.
* **Exhaustion limits:** entry-count caps, header-walk loop bounds,
  multi-volume cumulative caps, index-memory caps, and file-descriptor
  exhaustion limits; decompressor dictionary/window sizes capped
  independently of output size. (Baseline: caps shipped v1.9.3/v1.12/v1.13;
  FD limits are new.)

## 6. Execution & SFX Malware Delivery

### 6.1. Comprehensive SFX Consent
* **All side-effecting directives require explicit UI consent** — `Setup=`,
  `Presetup=`, `Delete=`, `Shortcut=`, `Silent=` — with default focus on
  **"Don't Run"** and prompts using the post-sanitization entry name only.
* **Prompt fatigue:** batch consent ("apply to all"), prompt coalescing, and
  a hard per-run prompt cap beyond which extraction aborts.
* **TempMode races:** `TempMode` extractions use cryptographically random
  subdirectories with restrictive ACLs and defined cleanup routines.

### 6.2. Process & Runtime Policy
* **Job Objects & mitigations:** SFX payloads run under OS Job Objects
  (process/memory/CPU caps, kill-on-close). Child restriction via
  `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY`. The SFX stub compiles with
  CET/Shadow Stack, ACG, PIE/ASLR, and CFG.
* **AMSI:** Windows AMSI scans command-line strings strictly as
  defense-in-depth.

## 7. UI/Terminal, Assurance & Residual Risk

### 7.1. Terminal Injection & Isolation
* **Sanitization:** all filenames printed to terminals are stripped of
  ESC/CSI/OSC sequences, RTL overrides, and invalid UTF-8. (Baseline:
  `sanitize_for_display` exists; a coverage audit against this list is
  scheduled — ROADMAP v1.22.0/v1.28.0.)
* **Parallel extraction isolation:** multithreaded extraction maintains
  atomic cumulative counters, per-entry failure isolation, and guaranteed
  temp-name uniqueness across workers.

### 7.2. Assurance & Vulnerability Management
* **Persistent fuzzing:** the parsing layer runs under persistent
  LibFuzzer/oss-fuzz campaigns with corpus retention, coverage targets, and
  crash dedup. (Baseline: nightly libFuzzer + cross-validation + retained
  crash artifacts since v1.21.x; the nightly surfaced the current OPEN P1
  roundtrip divergence — working as designed.)
* **Sanitizers — [Revised]:** CI runs ASan, UBSan, and TSan as primary legs.
  **MSan is scoped to the Linux/clang leg as best-effort** — it requires an
  instrumented libc++ and cannot gate the MSVC matrix; treating it as a
  first-class gate was impractical as drafted.
* **Testing:** CI carries an attack regression corpus (historical archive
  CVE PoCs re-run) and differential testing against `unrar` and `7z`.
* **Supply chain:** release binaries are signed, shipped with an SBOM, and
  updaters hardened against MITM/RCE. `SECURITY.md` defines the disclosure
  policy.

### 7.3. Residual Risk
Strict containment against format exploits and filesystem boundaries leaves
residual risk where users actively consent to executing malicious SFX
payloads, and for zero-day OS kernel exploits bridging the sandboxes.
**Library-mode embedders lose the AppContainer broker/worker containment**
and inherit in-process parsing risk; they are strictly advised to wrap the
OpenRAR library in their own OS-level sandbox.
