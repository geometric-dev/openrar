# Changelog

All notable changes to OpenRAR are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.26.0] - 2026-09-26

CDC-Driven Solid-Chain Packing — the arc the Gate 0 format-legality review
cleared as Design A (docs/v1.26-pre-analysis.md §5): ordinary RAR5 solid
compression with packer-side ordering only. Reduction is window-bounded and
measured against a plain-solid same-window baseline (three-number gate);
every decoder — OpenRAR, WinRAR, UnRAR — reads the emitted archives natively
(interop gate Track 9).

### Added

- **`-cdc` — CDC-driven solid-chain packing** (`src/cli/main.cpp` switch
  dispatch; `src/compress/cdc_planner.{hpp,cpp}`): content-defined chunking
  fingerprints the batch inputs and orders high-affinity files adjacent
  within the solid chain. Implies solid mode and identical-file references
  (explicit `-oi0` wins); `-ver` is refused (versioned adds cannot be
  reordered); `-oi`/`-cdc` with `-v` refused fail-closed. Pack-time report
  `cdc: logical=X packed=Y window=W flag=[reordered|original-order]`
  (plan §1b directive 3); the three-number baseline gate lives in
  tests/bench, not at pack time.
- **CDC fingerprinting engine** (`src/compress/cdc_chunker.{hpp,cpp}`):
  FastCDC-style Gear-hash chunker with two-level normalization (min
  16 KiB, avg 64 KiB, max 256 KiB), fixed splitmix64 table — the same
  bytes always produce the same chunk list. `CdcStreamChunker` is the
  single implementation of the cut rules and `chunk()` delegates through
  it: block-wise feeds yield byte-identical chunk sequences (the CLOSED-P1
  no-drift-copies lesson), pinned by a 13-partition × 3-seed equivalence
  gate plus the `cdc_chunk_boundary_fuzz` locality gate.
- **Affinity planner** (`src/compress/cdc_planner.cpp`): greedy grouping
  over chunk-hash overlap — a file joins its best earlier partner when
  they share ≥ 30% of the smaller file's chunks (integer-permille math,
  smallest-index tie-break, deterministic at any `-mt`). Groups emit
  members in original order; 0-byte and sub-min-chunk inputs never match
  and keep original order (plan directive 7). Fingerprint index capped at
  2,000,000 entries (32 MiB, plan §1.2); cap hit → the tail keeps original
  order and the fallback is reported on stdout (directive 5 — no silent
  degradation). `OPENRAR_CDC_INDEX_CAP` may lower the cap for tests, never
  raise it.
- **Encoder-side solid window carry** (`src/compress/compressor50.cpp`
  `begin_stream(continue_window)`, `src/compress/stream_encoder.cpp`
  `pack_solid_*`, `src/compress/solid_packer.{hpp,cpp}`): the write side
  now mirrors the reader's carried-window decode (which has always
  honored FCI_SOLID) — fresh tables/tokens/CRC per file with the LZ
  window, hash chains, and rep distances carried across chain members.
  Until this release every encoder started from an empty window, so
  OpenRAR's solid archives were header-solid but never window-solid
  (self-contained encodes are carry-neutral, which is why round-trips
  still passed). Solid batches pack through one `SolidPacker` session
  (threads already forced to 1); the store fallback is FORBIDDEN for
  session members — a stored member's bytes never enter the decoder's
  window, so storing one would desynchronize the chain (block-codec
  overhead for incompressible members is a few per-mille). Fresh
  sessions, appends onto an existing chain, and entries after FILECOPY
  gaps pack self-contained — always safe by the suffix-aligned-window
  argument. `snap_window_to_fci_grid` moved to `compress_plan.hpp` so the
  packer's window and the recorded `win_size` share one copy.
- **`-oi` creation side — identical files as references**
  (`src/cli/main.cpp` switch parse + detection pass,
  `src/archive/archive_mutator.cpp` `prepare_add_filecopy`): the README
  `-oi[0-4][:<size>]` row is now true (plan §1b critical finding /
  Pillar 7.6 drift fix). Modes: 0 off, 1 silent, 2 list, 3 list+exit
  (no archive), 4 exit only when duplicates were found; default 64 KiB
  comparison threshold, `-oi:<size>` overrides. Detection: size buckets +
  streaming SHA-256 prefilter, then a full byte-compare confirmation —
  the archive never claims an identity a hash collision could fake. The
  first occurrence in the entry-name-sorted batch becomes the stored
  master; later identical files become `FHEXTRA_REDIR` type-5 entries
  with no data area.
- **Interop gate Track 9** (`tools/interop_gate.py`): `-cdc` and `-oi1`
  archives cross-decoded by the local WinRAR/UnRAR oracles
  byte-identically — carried-window solid streams and FILECOPY references
  are ordinary RAR5 to the reference decoder (CI falls back to
  self-roundtrip per the gate's oracle-availability contract).

### Changed

- **Solid-run breaking around FILECOPY references** (`src/compress/compress_plan.hpp`
  `breaks_solid_chain`): reference entries carry no data area, are never
  chain members, and the run BREAKS around them — the next data-bearing
  entry starts a fresh chain (plan §2.2). The prepare-side plan and the
  writer's re-plan share the semantics (a divergence would desynchronize
  the packer window from the header bits).
- **File mtimes are restored on extraction** (`src/archive/archive_reader.cpp`
  M3 wiring): files never received the archived mtime before (only
  deferred directory metadata did) — a parity gap found while wiring the
  timestamp clamp. Regular-file paths now apply the archived mtime to the
  extraction temp before the commit rename, with the shipped
  FHEXTRA_HTIME > utime > FILETIME precedence.

- **Solid chains are denser — deletion/replace refusals now cover any
  non-suffix member**: with window-solid packing the store fallback is
  forbidden inside the chain (a stored member's bytes never enter the
  decoder's window), so members that previously degraded to `method 0`
  and fragmented solid runs now remain compressed chain members. The
  suffix-only deletion contract (docs/invariants.md §1) therefore bites
  exactly where it always stated: only the run's last chain member (or
  the whole tail) deletes; a mid-chain delete is refused fail-closed.
  The writer-conformance suite's solid-delete subtest was aligned with
  this contract and now also pins the mid-chain refusal.

### Security

- **Timestamp clamping wired** (`MtimeBounds`/`clamp_mtime`, shipped v1.24
  with zero call sites until now): absurd archive mtimes clamp to the
  parameterized bounds (default 1970-01-01 .. 3000-01-01) at file commit
  AND in the deferred dir-meta build; clamps surface as the
  `timestamp_clamped` security flag in `--json-summary` plus a report
  line on stderr (security-arch §4.4 reconciliation, plan test 8).
- **Caps cross-check (§5.4)**: a CDC-packed carried-window solid stream is
  an ordinary solid stream at extraction — the cumulative
  `max_total_output_bytes` cap fires mid-stream exactly as for any other
  solid archive (plan test 6).
- **RR on CDC-packed sets (pre-analysis R3)**: recovery record + repair
  verified on a reordered carried-window solid archive — damaged packed
  data fails the integrity check, single-erasure repair reconstructs it
  from parity, members recover byte-exactly (plan test 7).

### Measured

- Three-number gate on the engineered similarity corpus of
  `tests/unit/cli_tests.cpp` (`cdc_reduction_report_three_numbers`, plan
  test 4; 5 × 1 MiB members sharing 75%-of-bytes base content, 1 MiB
  window): store 5,243,438 / plain-solid original order 4,226,802 /
  CDC-packed 3,672,842 bytes — the packer's ordering buys a further 13.1%
  below the same-window plain-solid baseline, on top of the solid gain
  over store. Reduction is window-bounded by construction (Gate 0 §3):
  results are corpus-dependent and claimed per corpus only.

## [1.25.0] - 2026-09-26

Memory-Mapped Read Engine (Re-scoped): listing, header-scanning and
random-read gain a memory-mapped engine (SECURITY_ARCHITECTURE §5.2
normative scope — extraction inputs stay buffered), behind the same
fail-open contract as the rest of the engine.

### Added

- **Mapped read engine** (`src/io/mapped_file`): read-only mapping with an
  open-time size pin (truncation-aware pre-flight) and fault-guarded
  bounded reads — Windows SEH leaf converts access violations/in-page
  errors into clean short reads; POSIX bounds every access against the
  mapped length with a fresh `fstat` (no signal handlers, per §5.2).
  `io::ReadSource` — a minimal random-access read interface implemented
  by both `FileStream` (buffered) and `MappedFile` — lets the header
  scanner run unchanged on either engine (zero scanner duplication).
- **Mapped scanner wiring** (`select_volume_source`): the single
  mapped/buffered decision function picks a mapped view per volume when
  enabled and mappable, buffered fail-open otherwise; the view is
  scan-scoped and released before payload/extraction reads. `--no-mmap`
  / `OPENRAR_NO_MMAP=1` force the buffered engine; `OPENRAR_DEBUG_MMAP=1`
  makes fallbacks visible.
- **Random-read region export** (`OPENRAR_ABI_FEATURE_MMAP` bit 16 +
  `openrar_archive_handle_read_entry_region`): random-read region of a
  stored entry's payload, mapped view per region when available, buffered
  pread otherwise; encrypted/compressed entries refused; whole-payload
  ranges CRC/BLAKE2sp-verified, partial ranges unverified by contract.
- **Listing benchmark** (`openrar_bench`): sparse ~50 GB corpus (100k
  stored entries × 500 KiB holes, FSCTL sparse-marked on Windows) listed
  with the mapped engine vs the buffered engine; volumes refusing the
  FSCTL (ReFS: ERROR_INVALID_FUNCTION) skip the full-scale corpus, and
  the full-scale run happens on the CI ubuntu leg (ext4 sparse native).

### Changed

- **Scanner source abstraction**: `HeaderReader::read_block_raw` now takes
  `io::ReadSource&` instead of `io::FileStream&` — existing `FileStream`
  call sites compile unchanged; the mapped engine is a drop-in source.

## [1.24.0] - 2026-09-25

Extraction Containment & Integrity — the extraction pipeline itself, the
surface every crafted-archive attack actually hits, rebuilt to be *provably
correct*: syscall-level path containment with write-through-handle atomic
extraction, archive-internal collision rejection, a machine-readable
per-entry JSON report, and metadata fidelity. Risk Register item 2 (the
largest refactor of the arc) closed.

### Added

- **Syscall-level path containment** (§4.1, `src/io/containment`): string
  sanitization is defense-in-depth only; primary containment is the OS
  syscall chain. Every archive-controlled directory component is opened
  (or created) no-follow from a pinned root handle — Windows: NtCreateFile
  relative to the verified parent with FILE_OPEN_REPARSE_POINT and a
  per-component reparse-point rejection; Linux: openat2
  (RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS) via direct syscall with a cached
  availability probe, and an openat walk fallback (O_NOFOLLOW per
  component) whose final anchor is verified against the kernel-resolved
  root path (/proc/self/fd; F_GETPATH on macOS). An LRU cache (64) of
  verified directory handles amortizes the walk; cache hits skip the walk
  but never the final containment assertion
  (GetFinalPathNameByHandleW, \\?\-canonicalized both sides).
- **Write-through-handle atomic extraction** (§3.3/§4.1, `src/io/
  extraction_journal`): every payload write path (stored, compressed,
  encrypted, in-memory, multi-volume) writes to a crypto-random
  `<name>.<128-bit-hex>.tmp` in the destination directory and commits with
  an atomic no-follow rename cascade — Windows: POSIX-semantics
  FileRenameInformationEx via NtSetInformationFile (symlink leaf replaced,
  never followed; READONLY respected); Linux: renameat2(RENAME_NOREPLACE)
  with arch-slot fallbacks; macOS: renamex_np(RENAME_EXCL); POSIX
  fallback: link()+unlink(). A failed commit never leaves the destination
  touched (READONLY destinations fail the commit, plan test 14).
- **Per-directory journal manifests** (§3.3): every destination directory
  the run touches gets a `.openrar_journal_<pid>_<time>_<nonce>.tmp` under
  an exclusive advisory lock, with durable-first record ordering (the
  record is fsynced before the temp exists) and per-record CRC32. Journals
  close+unlink at zero in-flight temps, so descriptor use is bounded by
  extraction parallelism, not tree size. The startup sweep replays only
  unlocked journals and only deletes records that (a) lie in the journal's
  own directory and (b) match this build's exact temp shape — closing the
  forged-journal attack a naive sweep design would allow.
- **Archive-internal collision detection** (§3.2, gate 2,
  `src/archive/collision_detector`): the final merged entry list (service
  headers filtered) is checked for duplicate-identical names, simple
  case-fold collisions (Readme vs README), NFC normalization collisions
  (NFC vs NFD café), and file-vs-directory prefix conflicts — before
  anything is written. The CLI aborts with the structural exit code; DLL
  and WASM refuse implicated entries identically.
- **Unicode 15.1.0 tables** (`src/unicode`, ~48 KiB binary): simple case
  folding, canonical decompositions, composition pairs and combining
  classes generated by the checked-in `tools/gen_unicode_tables.py` from
  pinned UCD files (SHA256-verified); Hangul handled algorithmically.
  The Unicode version is surfaced by the collision detector.
- **`--json-summary[=path]`** (§3.1/§5): machine-readable per-entry
  extraction report — statuses extracted|modified|skipped|failed|
  unprocessed, human reasons, machine security flags (name_escaped,
  traversal_attempt, ...), abort semantics (entries processed before an
  abort keep their real status, the rest finalize as unprocessed). Without
  a path, ALL human output (banner, per-file lines, prompts, progress)
  routes to stderr and stdout carries only the JSON object; with a path,
  the JSON is written there.
- **Undecodable-name escaping** (§4.3): invalid UTF-8 byte sequences in
  entry names are percent-encoded (%XX, uppercase hex) inside the
  sanitizer — the escaped name IS the on-disk name, reversible by
  hex-decoding, reported with the name_escaped flag. POLICY CHANGE: the
  header parser no longer rejects invalid-UTF-8 names (previously whole
  archives with such entries were unreadable); they are admitted
  byte-losslessly and escaped downstream.
- **POSIX mode restoration with privilege-bit masking** (§4.3/§7.1):
  archived unix modes are restored umask-bounded (thread-safe cached
  umask — no transient umask(0) windows during parallel extraction);
  SUID/SGID/sticky are masked unless the explicit `--preserve-suid` admin
  opt-in is given. Modes are honored only when the producer stored a real
  st_mode (file-type bits 0170000 present) — DOS-style attribute blobs
  would otherwise become garbage restrictive modes.
- **Deferred directory metadata** (§7.4): directory (mode, mtime) pairs
  stack per extraction session and are applied bottom-up (deepest first)
  after all writes complete — directory mtimes survive child writes and
  read-only directories receive children while permissive. The CLI
  applies at run end; DLL/WASM calls apply previous-call metadata at each
  call start and flush at handle close, so restrictive modes never lock
  out children arriving in later calls.
- **Unknown-extra preservation** (§7.3): unrecognized extra records are
  captured verbatim (type vint + size vint + payload) into
  `FileBlock::unknown_extras` and re-serialized byte-identically during
  mutations — a roundtrip through OpenRAR never destroys data a newer
  producer wrote.
- **Extraction test suites** (blocking gates): extraction_atomic_tests
  (journal lifecycle, orphan sweep validation, no-clobber/read-only
  commits, flat-CWD key consistency), extraction_containment_tests (gate
  1 TOCTOU fault-injection: symlink target race, mid-path junction,
  rename-leaf symlink, fallback-forced double pass, 8.3 aliases;
  Linux runs every suite twice — openat2 and the forced walk fallback),
  collision_matrix_tests (gate 2: detector matrix, CLI exit-2 aborts with
  no partial output, service-header exemption, multi-volume span),
  extraction_report_tests (stdout purity, displayed ≡ extracted,
  percent-encoding roundtrip), extraction_fidelity_tests (SUID stripping,
  hardlink session scoping with link-count/inode oracles, deferred dir
  metadata), unknown_extras_tests (mutation roundtrip).

### Fixed

- **No-match extractions exited 0** (found during M4): the
  `return EXIT_NO_FILES` for a command that matched no files had been
  mangled into a comment since v1.21.2 — `x` with a mask matching nothing
  exited 0. Restored, placed before the -o- pre-filter so runs whose
  every target was skipped still exit 0 with each entry reported as
  skipped.
- **NTFS POSIX-semantics rename no-op over symlink leaves** (found by the
  gate-1 suite): FileRenameInformationEx reports SUCCESS without
  replacing when the target leaf is a reparse point whose target exists —
  the source temp is consumed and nothing changes. The commit detects the
  reparse leaf (anchored, no-follow) and unlinks the link object itself
  before the rename; NoClobber treats an existing link as a clean
  collision.
- **kernel32/NT information-class split** (found by the gate-1 suite):
  SetFileInformationByHandle rejects handles it did not create
  (ERROR_INVALID_PARAMETER for NtCreateFile-produced handles), and its
  class value 13 is FileDispositionInformation on the NT side — passing
  it there silently deletes the source file. The containment commit calls
  NtSetInformationFile with the correct NT classes (65 rename, 64
  disposition) directly.
- **Directory-handle rights for anchored renames** (gate-1 finding):
  walked directory handles carry FILE_ADD_FILE/FILE_ADD_SUBDIRECTORY and
  a full share mode — a too-restrictive share blocked every later
  write-intent open of the same directory.
- **Stale DLL error messages**: durable_write_to's failure paths now
  name the cause (temp creation vs atomic commit) instead of leaving the
  previous operation's error string in place.
- **Debug-CRT modal dialogs hung ctest**: test mains route _CRT_ASSERT
  and _CRT_ERROR to stderr and clear _CALL_REPORTFAULT — an assert prints
  and exits instead of popping a modal dialog forever.
- **Lost directory-creation races** (gate-1 finding): the containment
  walk's mkdir protocol reopens the winner's directory on
  STATUS_OBJECT_NAME_COLLISION, so parallel extraction (CLI -mt4 into a
  fresh tree) no longer fails on the first child of a just-created
  directory.

### Changed

- **Links are default-deny** (§6.1): symlinks, junctions, hardlinks and
  filecopy entries extract only with the explicit `-ol` opt-in
  (previously enabled by default). Opt-in remains decoupled from path
  resolution: even with links enabled, absolute/escaping links are
  rejected and every regular-file write goes through the containment
  walk. Hardlink targets must be files created in the same extraction
  session (§6.3) — a hardlink to any pre-existing user file is skipped.
- **Cross-device hardlinks fall back to a debited copy** (§7.2):
  EXDEV/ERROR_NOT_SAME_DEVICE falls back to a chunked copy whose bytes
  debit against ExtractionLimits::max_total_output_bytes; excess aborts
  the entry.

## [1.23.0] - 2026-09-24

Advanced SFX Scripting & Installer Directives, security-first per Security
Architecture §6: the directive engine ships with its full consent framework,
TempMode hardening, and runtime process policy in a single release.

### Added

- **SFX directive engine**: the Default.SFX stub parses the archive comment
  (`src/sfx/sfx_config`) for `Setup=`, `Presetup=`, `Delete=`, `Shortcut=`,
  `Silent=`, `Path`, `Overwrite`, `Title`, `Text`, `License`, `TempMode` and
  executes them through the phase pipeline
  (Presetup -> extraction -> Setup -> Delete -> TempMode cleanup). Unknown
  keys are counted and ignored (forward compatibility); malformed or
  oversized comments disable directives without aborting extraction.
- **Consent framework** (§6.1): every side-effecting directive requires
  explicit consent through one shared decision function
  (`src/sfx/sfx_consent`) consumed by both SFX modules. Default focus is
  always "Don't Run"; batch run-all/deny-all is per directive type; a hard
  per-run prompt cap (8) aborts the run with exit 2; non-interactive stdin
  denies without prompting. Prompts display the verbatim command line and
  the resolved absolute executable path.
- **TempMode hardening** (§6.1): extraction targets are
  `OpenRAR-<128-bit-hex>` directories under the temp root with owner-only
  permissions (0700 / owner DACL), created per run and cleaned up after
  `Setup`. No pattern-matched sweeps (§3.3).
- **Runtime process policy** (§6.2): directive programs spawn contained —
  Windows: Job Object with kill-on-close, 2 GiB per-process memory cap, 64
  active-process cap, ACG mitigation policy; POSIX: own process group with
  CPU/address-space/process-count rlimits verified pre-exec and PDEATHSIG on
  Linux. A containment-setup failure refuses execution (never spawn
  uncontained). AMSI scans the command line as defense-in-depth; a flagged
  verdict raises an additional warning prompt (default Don't Run), and AMSI
  unavailability is the documented fail-open.
- **Compiler mitigations**: both stubs build with CFG; the SFX process
  applies dynamic-code protection at startup; POSIX builds get
  `-fstack-protector-strong` (via the shared hardening flags).
- **`-sfxnoexec` kill switch** (flag + `OPENRAR_SFX_NOEXEC=1` env):
  extraction only — every directive is suppressed AND the suppression is
  reported with a count.
- **WinGUI.SFX**: native-dialog SFX stub (Windows, windowed subsystem)
  sharing the parser/consent/containment engine; MessageBox consent chain
  with the Don't-Run default focus.
- **Sandbox e2e suite** (blocking gate 2): runs the real Default.SFX stub on
  converted directive archives with scripted stdin — consented run, Don't-Run
  default focus, -sfxnoexec suppression + reporting, prompt-cap abort (exit
  2), and directive-free extraction. `OPENRAR_SFX_FORCE_INTERACTIVE` is the
  documented automation hook for scripted consent; CI/CD uses -sfxnoexec.

### Fixed

- **convert_to_sfx did not mark the module executable on POSIX** (found by
  the sandbox e2e suite on its first Linux run): the converted SFX carried
  the umask-derived 0644 mode and could not run. The owner-exec bit is added
  before the atomic commit. No-op on Windows.

### Changed

- **Silent directives suppress progress UI only** — consent prompts always
  appear; `Silent=2` runs unattended only after consent.
- **SFX overwrite policy** follows §3.2: archive directives may only
  de-escalate (skip-existing); overwrite-all requires the escalation consent
  prompt and otherwise falls back to per-file Ask behavior.

## [1.22.0] - 2026-09-23

Hardware vectorization + security baseline sweep: the RS16 `.rev` parity
fold and match-length kernels now dispatch to AVX-512/GFNI (x86) and NEON
(AArch64), measured natively; the security baseline sweep (safe-integer
math, terminal sanitization, KDF caps) is complete; the P1 roundtrip
divergence that blocked this gate shipped fixed in 1.21.25.

### Added

- **NEON RS16 fold kernel**: the Cauchy parity fold now dispatches to an
  AArch64 NEON path alongside the x86 GFNI kernel. Each byte × 16-bit
  constant product is a GF(2)-linear byte map, which splits over input
  nibbles into four 16-entry byte tables applied with `vqtbl1q` — 16 words
  per iteration from two loads, one deinterleave pair and eight table
  lookups. Bit-exact by construction (tables built from the same `gf_mul`
  the scalar fold uses), so no runtime calibration is needed; Advanced SIMD
  is architecturally mandatory on AArch64, making compile-time gating the
  whole dispatch. Design note: the roadmap's original `vmull_p64` sketch
  was replaced — a carry-less byte × degree-15 product needs a multi-step
  reduction mod P, so lane math only pays off for 32/128-bit fields (CRC,
  GHASH); for degree-16 constants the nibble-map lookups are fewer
  instructions per byte than any PMULL arrangement.
  `test_rs16_neon_bit_exactness` pins dispatched == scalar over 10 block
  classes x every Cauchy coefficient (exercised for real on the ARM64 CI
  legs).

- **Terminal-sanitization hardening (security sweep)**:
  `sanitize_for_display` now decodes UTF-8 and replaces, per category:
  C0/DEL (ESC-led CSI/OSC injection), C1 controls incl. the 8-bit CSI
  U+009B, invalid UTF-8 (bad leads, lone continuations, overlongs,
  surrogates, truncated tails — one '?' per byte so terminals cannot be
  pushed out of UTF-8 state), and bidi/direction attackers (RLO/LRE
  U+202A..202E, isolates U+2066..2069, LRM/RLM, line/paragraph
  separators, soft hyphen). Valid non-ASCII text passes through
  byte-identical. `test_sanitize_for_display` pins every category with
  negative tests.
- **Safe-integer-math audit (security sweep)**: verified the untrusted-
  value arithmetic surface end to end — `read_vint` is bounded at 10
  bytes with a tested 64-bit overflow contract; the header parser's size
  arithmetic uses subtractive bounds (`size - offset`) and an explicit
  overflow-safe clamp for locator records; body sizes are capped (2 MiB
  headers, 64 MiB locator payloads, 256 MiB heap extracts);
  `read_le32/64` are byte-wise (alignment-UB-free). The additive-check
  forms are already the overflow-safe ones; routing them through
  `__builtin_*_overflow` helpers would be churn without a safety gain.
- **Kernel benchmark harness** (`tests/bench/openrar_bench.cpp`, wired as
  an informational CI step on native-hardware legs): deterministic
  corpus, full-cap periodic compare (throughput, not early-exit),
  best-of-3, grep-able output. Measured natively in CI:
  RS16 `.rev` fold — **GFNI 19.19× scalar** (55.4 GiB/s, ubuntu runner)
  and **NEON 5.72× scalar** (14.6 GiB/s, macOS Apple Silicon); the
  5–10× roadmap claim is met and, for GFNI, exceeded. Match-length:
  **AVX2 2.68× scalar** (79.5 GiB/s), SSE2 1.78×, NEON 0.77× on Apple
  Silicon (the scalar loop is auto-vectorized there). AVX-512 match
  kernel numbers remain SDE-only (labeled non-normative) until native
  Ice Lake+ silicon appears in a measurable position.

- **PBKDF2 `lg2_count` ceilings pinned at both header paths** (security
  sweep): every KDF derivation site refuses `lg2_count > 24` —
  `HeaderCryptReader::init` / `HeaderCryptWriter::init_existing` for
  encrypted headers, and the entry-KDF gate for per-file FHEXTRA_CRYPT.
  `test_kdf_cap_pinned` drives a hand-built hostile file header with a
  valid CRC and `lg2_count = 25` through the full reader (fail-closed
  RAR_ERR_UNSUPPORTED_FEATURE, no derivation, no hang), pins 25..255
  refused and 24 accepted on the HEAD_CRYPT gates, and mirrors the
  boundary on the writer side.

- **Raspberry Pi / ARM Linux release packages (64-bit)**: the aarch64 QEMU
  leg now publishes its artifact, so releases gain a `linux-gcc-arm64`
  platform package (Raspberry Pi 3/4/5 on 64-bit Raspberry Pi OS). The arm
  legs run with warnings-as-errors, and the NEON RS16 kernel activation is
  asserted there rather than left implicit. 32-bit ARM (armv7) was
  evaluated and deliberately not packaged — modern 64-bit only is the
  platform policy (wasm32 remains the one ILP32 target, as the Emscripten
  product surface). The evaluation still paid for itself: it surfaced a
  class of ILP32 truncation bugs shared with the existing 32-bit code
  paths — `ALLOC_LIMIT` and `RAR_DICT_ALLOC_LIMIT` (64 GiB constants)
  truncated to 0 on a 32-bit `size_t`, `select_unpacker()` lost the
  oversized-dictionary signal to the same cast, and `pack_entries()`' u32
  width guard wrapped before it could fire. All are fixed, with
  `static_assert`s turning any recurrence into compile errors, and the
  compressor's window cap now mirrors the decompressor's 1 GiB ILP32
  limit so wasm32-produced streams stay decodable in-family.

## [1.21.25] - 2026-09-22

Core-codec correctness release: closes the OPEN P1 roundtrip divergence that
blocked the v1.22.0 gate, aligns every decompress-side window default with
the compressor's, and ships the v1.22.0 SIMD groundwork already merged to
master since v1.21.2.

### Fixed

- **P1 roundtrip divergence (fuzz iteration 727; nightly cross-validation
  abort at pos 12364)**: `compress_buffer()` derived its filter pre-transform
  chunk length from the *requested* window (2 MiB → 1 MiB chunks) while the
  embedded packer emitted filter tokens chunked by its *clamped* window
  (pow2 clamp, 128 KiB floor → 64 KiB tokens). The decoder un-transforms
  exactly one token region per token and its scan skips any CALL whose
  operand crosses the region end, so a boundary-crossing E8 was transformed
  by the encoder's wider window and never reverted (first divergence at
  65535: operand bytes shifted by the translation). The same mismatch let
  encoder matches cross decoder token regions, tripping the raw-source
  match validation. The pre-transform now derives its chunking from the
  packer's actual window via a single `filter_max_chunk()` helper shared
  with token emission. Regression tests: chunk-boundary encode/apply
  symmetry, boundary-crossing-CALL roundtrip, multi-token/multi-block and
  mem_src-branch roundtrip; the roundtrip fuzzer decodes with the matching
  raw-stream window (0x200000).
- **Double transform in the large-input `mem_src` branch**: pre-transformed
  data handed to a packer with `set_active_filter()` was transformed a
  second time inside `process_available()`; a `filter_pretransformed_` latch
  suppresses the in-loop transform for the `compress_buffer()` path.
- **CI cross-compiler build**: missing `<algorithm>` include and lambda
  capture fixed for the GCC/Clang legs; volume tests de-order-dependent on
  directory iteration.

### Changed

- **Window defaults aligned (embedder note)**: raw block streams carry no
  dictionary-size header, so every decompress-side default now mirrors the
  compressor's 2 MiB default — `Decompressor50::DEFAULT_WIN_SIZE` (core
  constructor default and explicit-0 fallback), dll `openrar_decompress` /
  `openrar_decompress2(0)`, wasm raw + stream-decoder fallbacks, and
  `decompress_block` in `include/openrar/openrar.hpp`. Streams decoded
  before are decoded byte-identically; streams with match distances or
  filter regions over 1 MiB now decode where they previously failed; cost
  is one extra MiB of lazily-allocated window per decoder. Consumers
  decoding non-default-window streams still pass the recorded dictionary
  size explicitly (archive/dll layers do).

### Added

- **AVX-512 match-length kernel (v1.22.0 groundwork)**: 64-byte-per-cycle
  match-length comparison in a dedicated translation unit compiled with
  `/arch:AVX512` (MSVC) / function-level target attributes (GCC, Clang) —
  512-bit intrinsics can never leak into baseline codegen. Runtime dispatch
  requires `cpu.avx512f` (CPUID + OS ZMM XSTATE, report M4 semantics) and the
  `OPENRAR_HAS_AVX512_KERNEL` compile-time capability flag. `GFNI` detection
  (leaf 7 ECX bit 8, ZMM-gated) added to `CpuFeatures` for the RS16 parity
  kernel. Cross-implementation bit-exactness gate
  (`test_match_length_bit_exactness`): every implementation the running CPU
  supports must match the scalar reference over 2300 boundary cases —
  Scalar == SSE2 == AVX2 == AVX-512 == NEON, exercised per machine.
- **GFNI RS16 fold kernel**: the Cauchy parity fold
  (`ReedSolomon16::update_ecc`) now dispatches to a `_mm512_gf2p8affine_epi64_epi8`
  kernel — each 16-bit multiply-by-constant decomposes into four 8x8 GF(2)
  byte matrices applied across 64 bytes/instruction. A one-time convention
  probe validates the instruction's matrix encoding against the scalar table
  fold and fails safe to it (worst case: no speedup, never wrong parity);
  `update_ecc_scalar` stays public as the bit-exactness reference.
  Validated in CI under **Intel SDE** (`simd-validation` job): the gate
  greps for kernel activation so an SDE/ISA mismatch fails loudly, and runs
  the interop quick gate end-to-end (real `.rev` parity through the kernel).
  `test_rs16_gfni_bit_exactness` pins dispatched == scalar over 11 block
  classes x every Cauchy coefficient.
- **Local CI-matrix preflight harness** (`tools/preflight.sh`): run the legs
  a machine can run locally; residual push-and-pray scope stays visible.
  Pre-commit runs the clang-format gate (CI parity); CI cancels superseded
  runs; the roundtrip fuzzer dumps divergence state; normative security
  architecture document added.

## [1.21.2] - 2026-09-21

Residual P2 front-load from the v1.6.0 → v1.21.0 audit (the remainder of the
P2 ledger not required for the v1.21.1 gate), plus the exit-code taxonomy
measured against the reference UnRAR oracle.

### Fixed

- **Off-grid dictionary windows (P2)**: a library caller passing an arbitrary
  `dict_size` (e.g. 4.1 GiB, which floor-quantizes to a 4 GiB header) got an
  encoder whose distance-slot table (446-slot) and match horizon were chosen
  from the REQUESTED window while the header recorded the quantized value —
  self-inconsistent archives the decoder (and WinRAR) cannot read. Window
  finalization now snaps to the exact FCI grid (base 128 KiB<<N, fraction
  steps of base/32, clamped at the maximum representable value) in both
  `prepare_add_file` and the multi-volume path, before the compressor is
  constructed. CLI `-md` values were already grid-exact. Regression test
  `test_off_grid_dict_snap_roundtrip`.
- **`:` / `:$DATA` stream names (P2, truncation vector)**: `write_alternate_stream`
  accepted archive-controlled stream names that resolve to the host file's
  DEFAULT data stream; the `CREATE_ALWAYS` open then truncated the
  just-extracted target's contents. Empty and `$DATA` (any case) stream parts
  are now rejected.
- **FHEXTRA_OWNER 255-byte name limit (P2, conformance)**: the reader clamped
  over-long name lengths and then parsed the remaining fields out of the
  middle of the name bytes (wrong ownership data flowing into `chown`); the
  writer never enforced the limit. Over-long records are now DISCARDED per
  spec on read, and the writer truncates names at 255 bytes.
- **`start_vol` orphan volume (P2)**: a failure during a new volume's header
  writes left the created file on disk (registered in the cleanup guard only
  after all writes succeeded); it is registered immediately after open.
- **`-hp` append error fidelity (P2)**: appending to a header-encrypted
  archive without a password reported `RAR_ERR_IO "cannot open existing
  archive"`; it now returns `RAR_ERR_UNSUPPORTED_FEATURE` with a precise
  message (parity with the delete surface), via `open_ex` status codes.
- **Non-throwing cleanup (P2)**: 72 bare `std::filesystem::remove(tmp_path)`
  calls on error paths (which could throw out of status-code APIs and mask
  the real failure) now use the `error_code` overload.
- **Hygiene (P3)**: removed the unreachable `//`-prefix check in
  `validate_archive_path`; the parallel pipeline no longer delivers
  completed chunks to the sink after cancellation is observed.

### Changed

- **WinRAR exit-code taxonomy (CLI)**: exit codes were measured against the
  reference UnRAR implementation (`errhnd.hpp RAR_EXIT`) and mapped:
  healthy=0, missing archive=10 (NO_FILES), unrecognized=13 (BADARC),
  checksum=3 (CRC), locked=4, open=6, usage=7, memory=8, no files matched=10
  for extraction/testing, wrong password=11, user break=255. Previously every
  failure collapsed to 1 — which in WinRAR semantics means "warning".
  Documented deviation: testing an encrypted archive WITHOUT a password
  returns 11 here where unrar surfaces 12 (READ); a wrong password is 11 on
  both. Enforced by interop-gate stage 15 (exit-code parity).
- **`l`/`lb`/`lt` and `t` file-mask support (P3)**: mask arguments were
  silently ignored by the list and test commands; both now filter by mask
  (`l` no-match exits 0 per the oracle; `t`/`x`/`e` no-match exits 10).
- **Switch dispatch tightening (P3)**: `rr*`/`s*`/`-ver*` prefix
  over-acceptance (typos silently became commands with default behavior)
  replaced with exact-or-validated dispatch; bare `-x@` is an error instead
  of an exclusion pattern `"@"`; `-md` values snapped down by the FCI grid
  cap print a warning.
- **ADS/ACL restore warnings (P3)**: child streams/security descriptors
  failing their CRC are reported (`W:`) instead of being dropped silently.

## [1.21.1] - 2026-09-20

Stabilization release ("Tight Base"): the blocking gate of the v1.22.0+ roadmap
(`docs/ROADMAP.md`). Closes the 16 P1 defects found by the v1.6.0 → v1.21.0
architect walkthrough audit — concentrated in error paths, commit atomicity,
the `.rev` repair scan, the parallel/WASM surface — and corrects five
inaccurate historical changelog claims (see "Corrected claims" below).

### Fixed

- **Compression — crafted-content OOB read (P1)**: `Filters50::detect_filter`
  computed `pe_off + 6 < size` in 32-bit arithmetic; a crafted `e_lfanew`
  near `UINT32_MAX` wrapped past the guard and read ~4 GiB out of bounds
  (reproduced SIGSEGV). The probe now uses 64-bit arithmetic; regression test
  `test_detect_filter_hostile_pe_offset`.
- **Compression — StreamDecoder filter-state carry (P1)**: the per-block
  decode path cleared the filter queue on every block and flushed
  `flush_all=true` at block end, so filter regions spanning RAR5 block
  boundaries aborted the stream (reproduced). Filter regions are now recorded
  in absolute file coordinates and carried across blocks of the same file;
  incomplete regions are applied when their data completes; the strict final
  flush runs only on the last block. Regression test
  `test_stream_encoder_decoder_filter_roundtrip` (m1/m3/m5).
- **Compression — StreamDecoder truncation fail-open (P1)**: `finish()`
  accepted a stream whose LastBlock-framed final block never arrived. It now
  fails closed.
- **Compression — crafted-stream filter amplification**: overlapping or
  backward filter regions are rejected at registration (disjoint, ordered
  regions only), removing the transform-amplification vector; the queue cap
  was raised 8192 → 65536 (large legitimately-filtered files emit one region
  per ~1 MiB and previously exhausted the budget).
- **Compression — >4 GiB-window OOB**: the 4-wide batched hash-insert loop
  wrote to the (empty) 32-bit tables on large-window builds; it is now
  skipped when `is_large_window_` (the 64-bit `insert_position` path handles
  those positions).
- **Compression — zero-window construction**: `StreamDecoder(0)` /
  `Decompressor50(0)` clamped after building the decompressor with
  `win_size_ == 0` (undefined window arithmetic); both now clamp in the
  constructor initialization path.
- **Mutation — commit atomicity (P1)**: `lock_archive` committed via
  `remove` → `rename`; a failed rename after a successful remove destroyed
  the archive. Single-volume commit now uses `atomic_replace`, and the
  multi-volume chain is committed in two phases (all temp files written
  first, then atomic replaces) so no volume is ever half-rewritten.
- **Mutation — checked commit-path writes (P1)**: ~20 unchecked
  `FileStream::write` / `HeaderWriter::write_*` calls on commit paths (entry
  payloads, ADS/ACL child payloads, signatures, main/end blocks) silently
  committed truncated archives on disk-full; every one is now checked and
  fails with the temp file removed.
- **Mutation — ADS/ACL orphaning on delete (P1)**: `delete_entries` /
  `delete_entries_by_index` marked only non-service entries, so a deleted
  file's trailing NTFS stream/security child records survived and extraction
  reattached them to the next surviving file. Child services now die with
  their file (`mark_trailing_child_services`); RR records are exempt and
  remain RecoveryWriter's responsibility. Regression test
  `test_delete_removes_ads_children`.
- **Mutation — volume-rewrite metadata loss (P1)**: `add_file_to_archive_vol`
  built a fresh `MainBlock` per volume, dropping `MHEXTRA_METADATA` and
  retained arc flags on rewrite; the new chain now seeds from the existing
  head volume's parsed main block.
- **Mutation — 32-bit slice truncation**: `read_packed_slice` resized through
  `size_t` from a full `uint64` length; oversized slices are now rejected,
  and `-v` sizes above `SIZE_MAX` are refused up front.
- **Recovery — foreign `.rev` adoption (P1)**: the `.rev` scan had no stem
  anchoring, so a sibling set's recovery volumes supplied the authoritative
  table and repair renamed valid volumes to `.bad` (reproduced). Candidates
  are stem-anchored (same rule as `check_has_rev_files`), a table under which
  every present volume fails CRC is refused, and `.bad` renames are the only
  destructive step. Regression test `test_rev_foreign_set_refused`.
- **Recovery — legacy `.rNN` invisibility (P1)**: the data-volume scan
  admitted only `.rar`/`.exe`, so legacy old-numbering sets were invisible to
  repair (reproduced refusal; overwrite-from-parity without `.bad`
  preservation). `.{letter}NN` extensions are now admitted and mapped.
  Regression test `test_rev_legacy_numbering`.
- **Recovery — `RevCleanupGuard` deleting pre-existing `.rev` files**: the
  failure path removed every output's `final_path`, including `.rev` files
  from an earlier successful run that this run never touched. Only outputs
  published *this run* are removed, and publication is a single atomic
  replace (no remove-then-rename window).
- **Recovery — resource bounds**: the output-stream leak on a failed
  `CreateAlways` open in `repair_rev_volumes` (left volumes locked open on
  Windows) is fixed; the `.rev` parity workspace is capped at 1 GiB with an
  honest failure instead of `bad_alloc` from absurd `-rv` requests.
- **DLL — `set_limits` TOCTOU race (P1)**: the busy flag was checked-then-
  acted; a concurrent extraction could start between the check and the
  non-atomic limit writes, racing on the limit state. Claiming is now an
  atomic CAS on both handle types, and every extract/test/list path claims
  through the same CAS (also closing same-handle callback reentrancy).
- **DLL — repair exports misreported committed mutations (P1)**:
  `openrar_archive_repair`, `openrar_archive_create_rev_volumes` and
  `openrar_archive_add_recovery_record` sampled the cancel callback *after*
  the operation and returned `RAR_ERR_ABORTED` for mutations that had already
  committed. The post-operation polls are removed (pre-op polling remains).
- **DLL — `create_file_ex` undocumented bit-smuggling**: bits 8-15 of the
  `solid` parameter were hijacked as a hidden filter-flag channel, so a
  `solid` value like `0x100` silently produced a NON-solid archive. `solid`
  now honors the documented "any non-zero value" contract; filters go
  through `create_file_opts` (`OPENRAR_FILTER_*` flags).
- **DLL — `entry_owner` OOM contract violation**: an allocation failure left
  the `HAS_USER`/`HAS_GROUP` flag set with a NULL string and returned
  `RAR_OK`; it now clears flags, frees any sibling string, and returns
  `RAR_ERR_NOMEM`. Added the missing `static_assert(sizeof(openrar_entry_owner_t) == 20)`.
- **CLI — `t` fail-open on encrypted entries (P1)**: `openrar t enc.rar`
  without a password reported OK for every entry and exited 0 while nothing
  was verified. Encrypted entries without a password now print
  `SKIPPED (encrypted - no password)` and count as errors (non-zero exit).
- **CLI — `-md` parse hardening**: the numeric tail must now be fully
  consumed (`-md16mxyz` is an error), non-finite values are rejected before
  the double→uint64 cast (latent UB on `-md1e19` / `-mdinf`), and the range
  check runs pre-cast.
- **CLI — `x`/`e` argument UB**: `last.back()` on an empty string argument
  (`openrar x arc ""`) is guarded.
- **CLI — Windows case-insensitive extraction race**: the duplicate-target
  guard compared paths case-sensitively, so `ReadMe.txt` and `readme.txt`
  extracted as two parallel jobs writing one physical file; target identity
  is case-folded on Windows.
- **CLI — `p` stdout mode leak**: the binary translation mode set for raw
  `p` output is now restored on every exit path.
- **CLI — honest repair/rv diagnostics**: `rv` failures reported "non-volume
  archive" for IO/parity errors and `r` failures lumped `.rev`-set mismatches
  into one message; both now state the actual failure classes.
- **WASM — `createArchive` hooks double-free (P1)**: the hooks pointer was
  freed in `unwireHooks` *and* again in the `finally` block, double-freeing
  into the shared dlmalloc heap on every hooks-using call. Regression test
  added (`createArchive ... hooks conformance`).
- **WASM — progress callback signature trap (P1)**: the callback was
  registered as `'vijj'` while the C ABI is `(i64 done, i64 total, i32 user)`
  = `'vjji'`, trapping the module on the first C-ABI progress callback.
- **Format — `MHEXTRA_METADATA` name-length overflow**: an addition-form
  guard (`cur + name_len <= rec_end`) wrapped on crafted near-2^64 vint
  lengths and issued an unbounded `std::string::assign` (length_error
  natively, wasm-aborting); replaced with the subtraction form used by the
  filename path.
- **Hygiene**: SFX conversion uses the hardened unpredictable temp path with
  `CreateNew` (the old steady-clock name was predictable and pre-plantable);
  `copy_stream_region` checks its seek; dead `ArchiveMutator::plan_batch` and
  the ambiguous `move_file_to_archive_vol` overload were removed; a short
  SFX-stub read no longer silently zero-fills; the append-path QO locator is
  guarded under header encryption (matching the fresh-create path).

### Changed

- **Architect skills relocated out of the repository**: the `architect-challenge` and
  `architect-walkthrough` review workflows (bundled into the repo in 1.8.0) now live at
  the user level (`~/.agents/skills/`) where they apply across all workspaces; `.agents/`
  is untracked and gitignored. Historical releases retain their copies.
- **Parallel filter parity**: the chunk-parallel pipeline cannot honor
  pre-processing transforms (chunk-relative offsets would corrupt the
  position-dependent E8/E8E9/ARM transforms, and regions must not cross chunk
  boundaries). Content is now probed with the same leading-sample
  `detect_filter` call the sequential path uses: if a filter would trigger,
  the file takes the sequential path and the request is honored; otherwise it
  compresses chunk-parallel filter-free — which is what the sequential path
  would produce. `-mt1` and `-mt>1` now always take the same path for the
  same input (byte-identical output for filter-triggering content; regression
  test `test_parallel_filter_parity`).

### Corrected claims (historical entries)

- [1.21.0]: header windows record the full (adaptively clamped) dictionary,
  NOT `min(dict, chunk_size)` — the claimed reduced-RAM recording was never
  implemented (it requires grid-snapping the encoder window to the FCI
  quantization first; see the code comment at the recording site). The
  `-mt` switch itself clamps to 64; the 16-worker clamp applies to the
  single-file chunk pipeline only.
- [1.20.0]: the described "per-chunk parity zeroing" defects were not
  observable in shipped v1.19.0 (rs16 already zeroes on the first fold);
  the memsets added in v1.20.0 are defense-in-depth, not a repair.
- [1.19.0]/[1.14.0]: the multi-volume slicing stage buffers one volume slice
  (O(vol_size)), not O(dictionary window); the minimum volume-size guard is
  `vol_size < 1024` → reject, not `>= 4096`.
- [1.9.3]: strict UTF-8 filename rejection maps to `RAR_ERR_TRUNCATED`
  (`-3`); the claimed `RAR_ERR_BAD_DATA` code does not exist.

## [1.21.0] - 2026-09-20

### Added

- **High-Throughput Multi-Threaded Compression (`-mt`) & Block Pipeline**:
  - **Format-Legal Chunk-Parallel RAR5 Compression**:
    - Leverages RAR5's self-contained block bitstream framing (per-block Huffman tables and explicit `LastBlock` flags) to divide single large files into independent 2–4 MiB chunks compressed across multiple CPU cores.
    - Guarantees 100% compatibility with official `UnRAR.exe` 7.20 and native `Decompressor50` without any format extensions or unpacker modifications.
  - **Exclusive Concurrency Dimension Architecture**:
    - Eliminates nested thread-pool deadlock hazards by strictly enforcing the single-dimension concurrency rule: multi-file batches parallelize across files with single-threaded compression per file (`file_threads = 1`), while single files parallelize across chunks (`chunk_threads = mt_threads`).
    - Resolves bare `-mt` to `core::hardware_thread_hint()`, with `-mt1` forcing single-threaded mode. (Corrected in 1.21.1: the switch-level clamp is 64; the 16-worker clamp applies to the single-file chunk pipeline only.)
  - **Bounded-Memory Streaming Block Pipeline (`ParallelBlockPipeline`)**:
    - Implements streaming pipeline for files $> 16\text{ MiB}$ with in-order chunk emission and bounded in-flight memory throttled to $2 \times \text{threads}$.
    - Memory footprint is strictly bounded by clamping worker dictionary windows to chunk size ($\le 16\text{ MiB}$ per worker), with instant vector deallocation after block emission.
    - Enforces match-finder clamping at chunk boundaries (`src_loaded_ = chunk_len`) and disables filters in chunked mode (`FilterMode::DisableAll`) to prevent cross-boundary corruptions.
    - Sentinel repeat match initialization (`old_dist_ = -1`) mathematically prevents cross-chunk distance state contamination.
    - Header windows record the full (adaptively clamped) dictionary. (Corrected in 1.21.1: the claimed `win_size = min(dict, chunk_size)` recording was never implemented; see also the claim ledger in the 1.21.1 entry.)
  - **Additive C DLL ABI Parallel Interfaces**:
    - Added `#define OPENRAR_ABI_FEATURE_PARALLEL_COMPRESS (1ull << 15)` in `openrar_dll.h`.
    - Exported `openrar_archive_create_file_opts_mt` supporting caller-specified thread counts, with legacy `openrar_archive_create_file_opts` delegating to it with `threads = 1`.
    - Maintained frozen `OPENRAR_DLL_API_VERSION = 1` ABI contract and entry struct layouts.
  - **Comprehensive Verification & Canonical Interop Gate**:
    - Added unit test suite `tests/unit/parallel_compress_tests.cpp` covering 16 KiB framing spike, determinism, roundtrip methods 1..5, streaming pipeline, mid-stream cancellation, and small file bypass.
    - Added Node.js test suite `tools/tests/parallel_compression.tests.mjs` verifying multi-threaded chunk compression, < 3% ratio delta vs `-mt1`, multi-file batch exclusive concurrency, and bare `-mt`.
    - Added Stage 14 to canonical `tools/interop_gate.py`, verifying 100% pass rate against official reference `UnRAR.exe` 7.20 across all 14 stages.

## [1.20.0] - 2026-09-20

### Added

- **Standalone Recovery Volumes (`.rev` / `-rv`) Generation & Engine Parity**:
  - Multi-Chunk Reed-Solomon Parity Accumulation Integrity:
    - Added per-chunk parity zeroing to `RecoveryWriter::write_rev_volumes` for multi-chunk volume sets (>1 MiB per volume). (Corrected in 1.21.1: the described residual contamination was not observable in shipped 1.19.0; the zeroing is defense-in-depth.)
    - Added per-chunk reconstruction zeroing to `RecoveryWriter::repair_rev_volumes` during Cauchy Reed-Solomon decode passes (defense-in-depth; see 1.21.1 corrections).
    - Guarantees byte-for-byte exact parity encoding and reconstruction for multi-volume archives of arbitrary size.
  - Missing-Volume Direct Repair Entry Parity:
    - Prioritized `has_rev_files` check ahead of volume existence in `RecoveryWriter::repair`, permitting recovery volume reconstruction when passed missing volume paths (e.g. `openrar r archive.part02.rar`).
  - Standalone `rv` Command Path Normalization:
    - Extended `vol_name_to_first_name` to probe existing disk files across candidate digit widths (`.part1.rar`, `.part01.rar`, `.part001.rar`), restoring full WinRAR CLI parity when passing archive base names (`openrar rv1 archive.rar`).
  - Additive C DLL ABI Recovery Interfaces:
    - Added `#define OPENRAR_ABI_FEATURE_REC_VOL (1ull << 14)` in `openrar_dll.h`.
    - Added `openrar_archive_create_rev_volumes` and `openrar_archive_add_recovery_record` DLL exports.
    - Updated `openrar_archive_repair` to support reconstructing missing archive volumes via `.rev` files without failing existence prechecks.
    - Preserved frozen `OPENRAR_DLL_API_VERSION = 1` ABI contract and entry struct layouts.
  - Comprehensive Verification & Dual-Oracle Interop:
    - Authored `tools/tests/recovery_volumes.tests.mjs` verifying multi-chunk volume repair, direct missing volume repair, CLI base name normalization, and dual-oracle cross-validation against official WinRAR / UnRAR 7.20.

## [1.19.0] - 2026-09-20

### Added

- **Multi-Volume Header Encryption (`-hp` with `-v`) & Metadata Parity (`-z`, `-k`)**:
  - Multi-Volume Header Encryption (`-hp` combined with `-v`):
    - Emits plaintext RAR5 signature immediately followed by a canonical `HEAD_CRYPT` block on every volume in a multi-volume chain.
    - Shares identical `CryptBlock` (salt, IV, iteration count, password check) derived via PBKDF2 across all volumes.
    - Encrypts all subsequent block structures (MainBlock, CMT service blocks, FileBlocks, EndArcBlock) via AES-256-CBC, each preceded by its own 16-byte random IV.
    - Preserves 16-byte alignment on intermediate volume slice boundaries (`slice = (slice / 16) * 16`), ensuring clean decryption without fractional block carryover across volume extents.
  - Archive Comment (`-z`) on Multi-Volume Sets:
    - Writes `CMT` service block right after `MainBlock` on the head volume (`vol_idx == 0`).
    - Respects header encryption when `-hp` is active, re-encrypting the comment block.
    - `openrar lt` technical listing reads and displays the archive comment.
  - Archive Lock (`-k` and command `k`) on Multi-Volume Sets:
    - Automatically marks `MHFL_LOCK` (`0x0004`) in `MainBlock.arc_flags` across all volumes.
    - Stream-based multi-volume mutation in `ArchiveMutator::lock_archive`: opens each volume individually, updates `MainBlock`, and preserves all file extents, service records, and EndArc blocks verbatim.
    - Protects against subsequent file addition or mutation across both single and multi-volume archives.
  - Additive DLL ABI Integration:
    - Added `#define OPENRAR_ABI_FEATURE_VOL_ENCRYPT (1ull << 13)` in `openrar_dll.h`.
    - Exposed feature flag in `openrar_abi_features()`.
  - Comprehensive Test Suite & Dual-Oracle Cross-Validation:
    - Authored `tools/tests/volume_encryption.tests.mjs` verifying multi-volume header encryption, comments, locking, and combinations against official WinRAR / UnRAR 7.20.

## [1.18.0] - 2026-09-20

### Added

- **RAR 7.0 Fractional & Non-Power-of-Two Dictionary Sizing (bits 15–19 $F$, `FCI_RAR5_COMPAT`)**:
  - Full implementation of RAR 7.0 non-power-of-two dictionary sizing and fractional 1/32 dictionary steps:
    - Encodes discrete window coordinates $D = \text{base} + (\text{base} / 32) \times F$ into bits 10–14 (base $N$) and bits 15–19 (fraction $F$).
    - Supports canonical compression info flags: `FCI_ALGO_MASK` (`0x003F`), `FCI_SOLID` (`0x0040`), `FCI_METHOD_MASK` (`0x0380`), `FCI_DICT_MASK` (`0x7C00`), `FCI_DICT_FRACT_MASK` (`0xF8000`), and `FCI_RAR5_COMPAT` (`0x100000`).
    - Sets `FCI_RAR5_COMPAT` (`0x100000`) for all RAR 7 dictionary-sized archives to guarantee official WinRAR and UnRAR 7.20+ cleanly decouple extended dictionary sizing from the decompression stream algorithm, reporting `RAR 5.0(v50)` and unpacking with zero checksum errors.
  - CLI switch enhancement:
    - `-md` now supports non-power-of-two values (e.g. `-md24m`, `-md48m`), fractional/decimal inputs (e.g. `-md1.5g`), and unit-less numeric arguments defaulting to MB.
    - Technical listing (`openrar lt`) now displays formatted dictionary size for each archive entry.
  - Additive DLL ABI Integration:
    - Added `#define OPENRAR_ABI_FEATURE_DICT_EX (1ull << 12)` in `openrar_dll.h`.
    - Exposed feature flag in `openrar_abi_features()`.
  - Comprehensive Test Suite & Dual-Oracle Cross-Validation:
    - Authored `tools/tests/dict_sizing.tests.mjs` verifying creation, self-test, roundtrip extraction, and dual-oracle cross-validation against official WinRAR/UnRAR 7.20.

## [1.17.0] - 2026-09-20

### Added

- **POSIX User & Group Ownership (`FHEXTRA_OWNER` `0x06`, `-ow`, `-og`)**:
  - Implemented complete format serialization and deserialization for RAR5 owner extra record `0x06` (`FHEXTRA_OWNER`).
  - Flag handling: `0x01` (user name string), `0x02` (group name string), `0x04` (numeric UID), `0x08` (numeric GID).
  - CLI switch support:
    - `-og` / `-og<group>`: Store group name or numeric GID in archive extra records.
    - `--group=<group>` / `--owner=<user>`: Store symbolic user/group names or numeric UID/GID overrides.
    - Technical listing (`lt`): Displays user name, group name, UID, and GID attributes when present.
    - Extraction: Restores POSIX ownership attributes on Unix/POSIX targets when run as root (`euid == 0`) or when `-ow` / `-og` is requested.
  - Additive DLL ABI Integration:
    - Added `#define OPENRAR_ABI_FEATURE_OWNER (1ull << 11)` in `openrar_dll.h`.
    - Added `#define OPENRAR_ENTRY_FLAG_HAS_OWNER (1u << 10)` in `openrar_dll.h`.
    - Defined 20-byte packed struct `openrar_entry_owner_t` allowing zero-allocation numeric UID/GID queries.
    - Exported `openrar_archive_handle_entry_owner` and `openrar_archive_entry_owner_free`.
  - Comprehensive Test Suite & Dual-Oracle Cross-Validation:
    - Authored `tools/tests/owner.tests.mjs` verifying symbolic group, numeric GID, combined user/group overrides, technical listing, and dual-oracle cross-validation against official WinRAR 7.20.

## [1.16.0] - 2026-09-19

### Added

- **RAR5 File Versioning (`-ver[n]`) & Historical Version Pipeline**:
  - Implemented RAR5 file versioning support according to format specification and WinRAR parity.
  - Extra record `0x04` (`FHEXTRA_VERSION`) handling:
    - Encodes 64-bit VINT flags (`0x00` default) and 64-bit VINT version number.
    - Historical versions carry `has_file_version = true` and `file_version = 1, 2, ...`.
    - Active unversioned entries represent the latest active revision without `FHEXTRA_VERSION`.
  - Zero-recompression mutating pipeline in `ArchiveMutator::write_batch_add` & `write_batch_add_ex`:
    - Converting active entries into historical versions promotes existing entries without recompression or re-encoding.
    - Zero-overhead payload byte copying via `copy_stream_region` directly from existing offsets, preserving bit-exact payload CRC32 and memory efficiency.
    - Pruning enforcement with `-vern`: limits total historical versions to $n$, pruning oldest historical versions while preserving solid chain invariants.
- **CLI `-ver[n]` Switch & Extraction Semantics**:
  - Added `-ver` argument parsing precedence strictly evaluated before `-v` to prevent volume switch collisions.
  - Archive listing (`l`, `lt`) appends `;version` to historical versions and outputs `File version: <v>` in technical listings.
  - Extraction semantics:
    - Default extraction (`x` / `e`): extracts only latest active versions, skipping historical versions unless explicitly targeted by name with `;`.
    - `-ver`: extracts all versions with `;version` suffixes appended to avoid file collisions on disk.
    - `-verN`: extracts specifically version $N$ without suffix.
- **Additive DLL ABI Flag (`OPENRAR_ENTRY_FLAG_HAS_VERSION`)**:
  - Added `OPENRAR_ENTRY_FLAG_HAS_VERSION = (1u << 9)` in `include/openrar/openrar_dll.h`.
  - Populated in `FileArchiveHandle::entry_ex` while preserving frozen 64-byte `openrar_archive_entry_t` struct layout.
- **Comprehensive Test Suite & Dual-Oracle Cross-Validation**:
  - Authored `tools/tests/versioning.tests.mjs` verifying version accumulation, `-vern` pruning, default extraction, `-ver` multi-extraction, and targeted version extraction.
  - Verified bidirectional compatibility against official WinRAR 7.20 (`rar.exe` and `UnRAR.exe`).

## [1.15.0] - 2026-09-19

### Added

- **Pre-Processing Filter Pipeline & Compression Ratio Parity**:
  - Implemented full forward filter transform pipeline in `Filters50` (`encode_e8`, `encode_arm`, `encode_delta`) matching RAR5 specification and WinRAR 7.20 bitstream rules.
  - Added x86 CALL/JMP (`E8`, `E8E9`) jump address translation with circular dictionary wrap-around and relative offset calculation.
  - Added ARM BL relative instruction translation with PC-relative branch decoding.
  - Added multi-byte / multi-channel delta transform (`Delta`) with automatic channel detection (1..32 channels) and stride-based difference filtering for raw audio, imagery, and columnar binary data.
  - Integrated in-band filter token emission in `Compressor50` (`FilterToken`, slot 256 execution records, block length vint encoding) with bounded sliding window invariants.
- **First-Class `-mc` Switch Engine in CLI**:
  - Added full support for the WinRAR `-mc` switch family:
    - `-mc-`: Disable all pre-processing filters.
    - `-mc[param]E[+|-]`: Configure / force / disable x86 executable filter (`E8`/`E8E9`).
    - `-mc[param]A[+|-]`: Configure / force / disable ARM branch filter.
    - `-mc[param]D[+|-]`: Configure / force / disable multi-channel delta filter with channel stride override (e.g. `-mc16:4D+`).
    - `-mc[param]L[+|-]` & `-mc[param]X[+|-]`: Tolerant acceptance for long-range and exhaustive matching switches.
    - Compound multi-filter switch syntax support (e.g. `-mcE+D-`).
- **Additive DLL API Filter Negotiation (`OPENRAR_ABI_FEATURE_FILTERS`)**:
  - Exported `openrar_archive_create_file_opts` under new additive feature bit `OPENRAR_ABI_FEATURE_FILTERS = (1ull << 10)` in `include/openrar/openrar_dll.h`.
  - Added bitmask flags `OPENRAR_FILTER_DISABLE_ALL`, `OPENRAR_FILTER_FORCE_E8`, `OPENRAR_FILTER_DISABLE_E8`, `OPENRAR_FILTER_FORCE_ARM`, `OPENRAR_FILTER_DISABLE_ARM`, `OPENRAR_FILTER_FORCE_DELTA`, and `OPENRAR_FILTER_DISABLE_DELTA`.
  - Preserved strict backward ABI stability (`OPENRAR_DLL_API_VERSION = 1`, 64-byte `openrar_archive_entry_t` unchanged).
- **Solid Archive & `StreamEncoder` Filter Invariants**:
  - Enforced per-file filter isolation in solid archives: filter token bounds and transforms strictly reset at entry boundaries while preserving continuous LZ sliding dictionary history across solid chains.
  - Integrated in-place filter transformations into `StreamEncoder` and `Compressor50` bounded memory windows, producing bit-identical compressed streams with zero unbounded RAM growth.
  - Added WASM / C API streaming encoder export `openrar_stream_create_ex(method, win_size, filter_flags)` and updated npm package (`wasm/js/openrar.js`, `openrar.d.ts`) with `filterMode: 'auto' | 'none' | 'e8' | 'arm' | 'delta'` in `compressStream` and `compressStreamChunks`.
- **Dual-Oracle Cross-Validation**:
  - Expanded dual-oracle interop gate (Track 2) with full bidirectional filter test matrix against official WinRAR 7.20 (`rar.exe`) and UnRAR 7.20 (`UnRAR.exe`).

## [1.14.0] - 2026-09-19

### Added

- **First-Class Native Archive Creation in C ABI / DLL (`OPENRAR_ABI_FEATURE_CREATE`)**:
  - Exported `openrar_archive_create_file` and `openrar_archive_create_file_ex` under additive feature bit `OPENRAR_ABI_FEATURE_CREATE = (1ull << 9)` (`docs/versioning.md`), preserving the frozen 64-byte `openrar_archive_entry_t` ABI contract.
  - Supports non-existent target bootstrapping, atomic durability replacement, exact dictionary window sizes (`dict_size`), password encryption, solid chaining, and real-time progress/cancellation callbacks.
  - Added RAII C++ convenience wrapper `openrar::Archive::create`.
  - Relaxed `openrar_archive_add_files_file` to accept compression methods 0–5, modern dictionary sizes up to 64 GiB, and automatic creation for non-existent archive targets.
- **Streaming Multi-Volume Creation (`-v<size>`) Without Whole-File RAM Buffering**:
  - Overhauled `ArchiveMutator::add_file_to_archive_vol` to guarantee an invariant $O(\text{dictionary window})$ memory ceiling during multi-volume creation, completely eliminating `uncompressed.resize(file_sz)` whole-file RAM buffering.
  - Stream-compresses large files through bounded spool buffers and slices payloads across volume boundaries using 64-bit extents and offsets.
  - Connected the `-md` custom dictionary switch to multi-volume archiving and preserved transactional `.mv_bak` sidecar replacement. (Corrected in 1.21.1: the minimum volume-size guard is `vol_size < 1024` -> reject, not `>= 4096`.)
- **Direct-to-Archive Streaming Compression (Zero Double-Spooling via Fixed-Width vint Back-Patching)**:
  - Eliminated temporary disk spool files (`spool_tmp`) for large unencrypted files during single-file and sequential additions, cutting disk write I/O by 50% and peak scratch disk usage to 1x payload.
  - Implemented 10-byte fixed-width vint (`push_vint_fixed`) header back-patching in `HeaderWriter::serialize_file_block`, allowing in-place updating of `pack_size`, `data_crc32`, and header CRC without shifting byte offsets or violating RAR5 specification leniency.
  - Integrated deterministic store fallback truncation (`out.truncate(orig_pos)`) for payloads that expand during compression.
- **`StreamEncoder` Store-Mode RAM Uncapping & Bidirectional WASM Streaming**:
  - Uncapped store mode (`method == 0`) streaming in `StreamEncoder`, passing chunks directly to `flush_cb_` or yielding via `take_output` with zero whole-stream RAM buffering.
  - Added incremental block pulling (`take_output`) to `StreamEncoder`.
  - Exported streaming compressor C ABI functions (`openrar_stream_compress_new`, `openrar_stream_compress_feed`, `openrar_stream_compress_pull`, `openrar_stream_compress_finish`, `openrar_stream_compress_free`).
  - Added `compressStreamChunks` async generator to the WebAssembly npm wrapper, achieving full bidirectional streaming symmetry (`compressStream`, `compressStreamChunks`, `decompressStream`, `decompressStreamChunks`).
  - Bounded WASM compression dictionary allocations to $\le 64\text{ MiB}$ under `__EMSCRIPTEN__` to prevent 32-bit linear memory exhaustion.

## [1.13.0] - 2026-09-19

### Added

- **64-Bit In-Memory Extraction Buffers**:
  - Raised `MAX_STREAM_OUTPUT` (`Decompressor50`) and `MAX_TOTAL_OUTPUT` (`BufferArchive`) from 4 GiB to **64 GiB** on 64-bit native platforms (`sizeof(void*) >= 8`), guarded with `std::bad_alloc` exception handling mapping to `AllocationFailed` / `RAR_ERR_NOMEM`.
  - Preserved defensive 2 GiB bounds on 32-bit / Emscripten WASM builds.
- **64-Bit Recovery Parity Buffer Scaling**:
  - Raised `MAX_PARITY_BUFFER_CAP` in `RecoveryRecord` from 2 GiB to **64 GiB** on 64-bit platforms, enabling multi-gigabyte RS-parity blocks for large archives while preserving 2 GiB bounds on 32-bit platforms.
- **64-Bit Compression Match Finder Horizon**:
  - Implemented 64-bit match-finder tables (`head64_` / `prev64_`) in `Compressor50` when `win_size_ > 4 GiB`, eliminating 32-bit distance truncation in sliding-window match searches across $> 4\text{ GiB}$ horizons.
  - Dynamically allocates 64-bit tables only for $> 4\text{ GiB}$ dictionaries, maintaining zero overhead and 32-bit cache locality for standard dictionaries ($\le 4\text{ GiB}$).
- **Dynamic CLI Concurrency RAM Budget**:
  - Scaled CLI concurrency `PREPARE_BUDGET` dynamically based on detected host physical RAM (`GlobalMemoryStatusEx` on Windows, `sysconf` on POSIX), allocating 25% of system RAM clamped between 1 GiB and 32 GiB.
  - Concurrency throttling for large dictionaries now dynamically adjusts worker threads against available physical memory.

## [1.12.0] - 2026-09-19

### Added

- **Elimination of Arbitrary Memory & Window Ceilings**:
  - Uncapped `ALLOC_LIMIT` in `Decompressor50` and `format::headers` to **64 GiB** on 64-bit native hosts (`sizeof(void*) >= 8`), matching WinRAR 7.0 max profile, while preserving defensive 1 GiB / 2 GiB bounds on 32-bit / Emscripten WASM.
  - Raised `MAX_WIN_SIZE` in ABI contract (`src/api/abi_contract.hpp`) and C DLL (`src/dll/dll_api.cpp`) to 64 GiB on 64-bit native platforms.
  - Implemented lazy window allocation in `Decompressor50` with `try/catch(const std::bad_alloc&)` mapping allocation exhaustion to `DecompressErrorCode::AllocationFailed`.
- **64-Bit Distance Bit Decoding**:
  - Implemented `core::uint64 BitReader::get_bits64(unsigned int count)` leveraging the 64-bit accumulator register (`acc_`).
  - Fixed 32-bit distance truncation bug in `Decompressor50::decompress_internal`: extra-distance slots 68–79 (`d_bits > 36`, distances $> 4\text{ GiB}$) now decode via `get_bits64(d_bits - 4)` into `core::uint64 extra` without truncation or assertion failures.
- **Exact Byte Dictionary Parameterization**:
  - Modernized `ArchiveMutator::prepare_add_file` to accept `core::uint64 dict_size` directly instead of a lossy `window_log2`.
  - Maintained backward compatibility: inputs 1..15 decode to `0x20000ULL << (val - 1)`, while values > 15 are treated directly as byte counts.
  - Removed 15-iteration cap loop in CLI `main.cpp`, passing `opt_dict_size` directly to preserve fractional non-power-of-two dictionaries (`-mdx48m`, `-mdx96m`) and large dictionaries (`-md4g`..`-md64g`).
- **Archive Reader Error Fidelity**:
  - `ArchiveReader` and `BufferArchive` explicitly map decompressor `AllocationFailed` to `RAR_ERR_NOMEM (-5)` instead of falling through to misleading `RAR_ERR_TRUNCATED (-3)`.
  - Oversized dictionaries cleanly map to `RAR_ERR_LIMIT_EXCEEDED (-15)`.

## [1.11.0] - 2026-09-19

### Added

- **Incremental Streaming Decompressor (`StreamDecoder`) & WASM API**:
  - Bounded memory decoder architecture decoupling bit-reading from stream chunk boundaries via stateful input FIFO staging (`in_queue_`).
  - Native WebAssembly C ABI exports (`openrar_stream_decompress_*`) and async TypeScript generators (`decompressStream`, `decompressStreamChunks`).
  - Enforced defensive 64 MiB window allocation ceiling under `__EMSCRIPTEN__` to prevent linear memory exhaustion.
- **RAR7 Format Level Emission (`unp_ver = 1`) & Fractional Dictionaries**:
  - Exact formula serialization for RAR7 dictionary fractions `(win_size - pow2) * 32 / pow2` with 31 ceiling clamp in `HeaderWriter`.
  - Decoupled `unp_ver` from distance slot table sizing: `TABLE_SIZEX = 446` reserved strictly for $> 4\text{ GiB}$ dictionaries, retaining `TABLE_SIZE = 430` for intermediate non-power-of-two dictionaries $\le 4\text{ GiB}$.
- **Multi-Volume `.rev` Cauchy Parity Repair in C DLL ABI**:
  - Added `openrar_archive_repair` and ABI feature bit `OPENRAR_ABI_FEATURE_REPAIR = (1ull << 8)` supporting inline Recovery Records and external Cauchy Reed-Solomon `.rev` parity reconstruction.
- **Vectorized SIMD Match Acceleration**:
  - Boundary-guarded AVX2, SSE2, and ARM Neon match-finding loops in `src/compress/arch/match_simd.hpp`.

## [1.10.0] - 2026-09-18

### Added

- **Engine Streaming & Unbounded File Size**:
  - Removed internal 1 GiB file size cap via 16 MiB spooling threshold (`SPOOL_MEMORY_THRESHOLD = 16 MiB`) enabling multi-gigabyte file mutations without unbounded memory consumption.
  - RAII `SpoolFileGuard` ensuring guaranteed temporary file unlinking and zero temp leakage across exceptions, cancelation, and write aborts.
  - In-flight AES-256-CBC encryption to spool when archive encryption is enabled, ensuring zero unencrypted plaintext touches disk.
  - Large uncompressed files (> 16 MiB) stream directly from source path into archive writes without allocating intermediate disk spool files.
  - Single-pass store streaming with in-flight CRC calculation and header back-patching, accelerating uncompressed `-m0` throughput from 74.7 MB/s to 373.9 MB/s (5x speedup, outperforming WinRAR 7.20 by 25.3%).
  - Upgraded transfer and streaming buffers to 1 MiB across `copy_stream_region` and payload ingestion loops.
- **Adaptive Dictionary Window Sizing & CLI `-md<size>` Switch**:
  - Scaled default compression dictionaries: `-m3` to 8 MiB (was 2 MiB), `-m4` to 16 MiB (was 4 MiB), `-m5` to 64 MiB (was 16 MiB).
  - Adaptive dictionary clamping for non-solid files: clamps window size down to the nearest power of two of file size (floor 128 KiB) to avoid allocating oversized dictionaries for small files.
  - CLI `-md<size>` (e.g. `-md16m`, `-md64m`) supporting 128 KiB to 1 TiB dictionary sizes with dynamic thread concurrency throttling under `PREPARE_BUDGET` (1 GiB).
  - Accurately gated compressor workspace memory estimation ($5W + 6\text{ MB}$) in `compress_plan.hpp`.
- **True Cross-File Solid Compression Safety**:
  - Enforced single-worker constraint (`threads = 1`) for solid batch compression in `run_batch_add`, eliminating cross-file dictionary race conditions.
- **CLI Ergonomics & Parity**:
  - `p` command: stream archive entries directly to stdout in raw binary mode (`_O_BINARY` on Windows) via `ArchiveReader::extract_entry_sink`.
  - Positional `@<list>` listfile argument expansion with UTF-8 BOM removal and `#`, `;`, `//` comment stripping.
  - `-x<pattern>` and `-x@<list>` file exclusion filtering supported across all CLI commands (`a`, `u`, `f`, `m`, `x`, `e`, `l`, `t`).
- **C DLL ABI Additions**:
  - `openrar_archive_handle_set_limits`: dynamic runtime extraction limits on open archive handles with `std::atomic<bool> busy` concurrency protection returning `RAR_ERR_BUSY = -14` during active extractions.
  - ABI feature discovery bit `OPENRAR_ABI_FEATURE_SET_LIMITS = (1ull << 7)`.
  - Static assertion parity between C DLL ABI and core engine `RAR_ERR_BUSY`.

## [1.9.3] - 2026-09-18

### Added

- **Resource Limits API (`ExtractionLimits`, `LimitState`, `RAR_ERR_LIMIT_EXCEEDED = -15`)**:
  - Fine-grained resource limit enforcement in extraction pipeline: `max_member_bytes`, `max_total_bytes`, and `max_header_bytes`.
  - In-flight dynamic budget tracking across streaming decompression chunks (`kStreamChunk = 64 KiB`) providing deterministic aborts against decompression bombs even with `FHFL_UNPUNKNOWN`.
  - Cumulative header and decompression limits across multi-volume sets.
  - Mirrored in C DLL ABI with `RAR_ERR_LIMIT_EXCEEDED = -15` (with static assertions in `abi_contract.hpp`) and TypeScript definitions in `wasm/js/openrar-archive.d.ts`.
- **Formal Extraction Contract (`docs/EXTRACTION_CONTRACT.md`)**:
  - Architectural contract specifying cancellation granularity, CRC/BLAKE2sp checksum verification ordering, solid archive replay semantics, memory and byte budget limits, and multi-volume boundaries.
- **RFC 3629 UTF-8 Filename Validation Hardening**:
  - Implemented strict RFC 3629 UTF-8 validator in `core::is_valid_utf8` rejecting non-shortest forms, surrogate code points (`U+D800`..`U+DFFF`), and out-of-range values.
  - Enforced in `HeaderReader` for RAR5 file header filenames, rejecting corrupted or malicious archives. (Corrected in 1.21.1: the failure maps to `RAR_ERR_TRUNCATED` (-3); no `RAR_ERR_BAD_DATA` code exists.)
- **Golden Fixture Harness & Deterministic Tooling**:
  - Cross-platform golden fixture suite with portable `=key=value` naming scheme to avoid Windows NTFS alternate data stream collisions (`tests/fixtures/README.md`).
  - Automated generator (`tools/generate_golden.ps1`) and hash verifier (`tools/check_golden.ps1`) validating writer and mutator archives against companion `.sha256` files.
  - CTest test harness integration in `tests/unit/golden_fixtures_tests.cpp`.
- **Writer Plan/Schedule/Execute Separation (`src/compress/compress_plan.hpp`)**:
  - Refactored `ArchiveMutator` and archive writing architecture cleanly separating pre-execution planning (`CompressPlan`, `ExecutionPlan`, `EntryPlan`), concurrency/resource scheduling, and low-level byte serialization.

## [1.9.2] - 2026-09-18

### Added

- **7 Automated Interop Boundary Quality Gates (`tools/interop_gate.py`)**:
  - Track 1: Solid mixed-stream invariants (`-s -m3` mixing 0B files, store fallbacks, and compressed files without dictionary corruption).
  - Track 2: RAR5 executable filter decoding (`-mc`) across 512K/1M circular ring-buffer boundaries.
  - Track 3: High-precision 64-bit Windows FILETIME timestamps with pre-1970 negative Unix epoch and post-2038 rollover protection.
  - Track 4: Multi-byte UTF-8 password and key derivation (`-p` / `-hp`) across German umlauts, French accents, CJK characters, and 4-byte UTF-8 emojis.
  - Track 5: QuickOpen (QO) table invalidation and locator stripping upon external mutation (`openrar d`, `u`, `f`, `k`).
  - Track 6: Recovery volume (`.rev`) Cauchy erasure coding $GF(2^{16})$ parity compatibility with official WinRAR repair (`rar rc`).
  - Track 7: 64-bit VINT size bounds and in-memory heap allocation exhaustion guards.
- **Local UnRAR Integration (`dev\unrar`)**:
  - Automatically discovers and prefers local `UnRAR.exe` builds alongside official WinRAR installations for zero-setup local conformance verification.

### Fixed

- **RAR5 Executable Filter Transform & Deserialization**:
  - Fixed bitstream deserialization in `Decompressor50`: filter length and offset parameters now decode via official 2-bit length prefix + LE32 (`read_filter_data`), eliminating bitstream desynchronization previously caused by LEB128 parsing.
  - Modified `flush_pending_blocks` to transform filtered data out-of-place directly into the output callback rather than writing back to `window_`, guaranteeing that subsequent LZ77 string matches reference raw un-transformed dictionary history.
- **Pre-1970 Negative Timestamp Arithmetic Underflow**:
  - Fixed Windows `FILETIME` conversion in `ArchiveMutator` to store native 64-bit FILETIME values directly (`fa.ftLastWriteTime`) with `is_unix = false` and spec-compliant extra flags (`0x02` mtime, `0x04` ctime, `0x08` atime), preventing year 1965 from wrapping to year 2101 in WinRAR.
- **Windows CLI Argument Unicode Encoding**:
  - Replaced ANSI `char* argv[]` argument ingestion on Windows with `CommandLineToArgvW(GetCommandLineW())` converted to UTF-8 in `main.cpp` and `sfx_main.cpp`, guaranteeing that non-ASCII CLI passwords match WinRAR UTF-8 key derivation.

## [1.9.0] - 2026-09-18

### Added

- **`MHEXTRA_METADATA` Serialization & Mutation Preservation (`-ams` / `-am`)**:
  - Full deserialization in `HeaderReader::parse_main_block` and serialization in
    `HeaderWriter::write_main_block` for RAR5 main header extra record `0x02`.
  - Encodes archive software identity, nanosecond-precision ctime, and Unix epoch flags.
  - Complete mutation inheritance in `ArchiveMutator`: archive mutations (`d`, `u`,
    `f`, `m`, `k`, `s`, `-rr`) preserve `MHEXTRA_METADATA` from the original archive
    unless explicitly overridden.
  - Conformance test enabled in `tools/tests/format.tests.mjs` with 110/110 passing suite.
- **Dedicated Recovery Volumes (`-rv[N]` & `rv[N]`)**:
  - Support for `-rv[N]` switch and `rv[N]` standalone command for multi-volume recovery volume sets (`.rev` files).
  - Multi-threaded RS16 Cauchy parity computation (`-mt`) with deterministic byte output.
  - Strict validation: fast fail with diagnostic error message when attempting to generate recovery volumes on single-volume archives.
  - Robust RAII `RevCleanupGuard` ensuring `.rev` temporary files and incomplete artifacts are scrubbed on error or exception.
- **Win32 Reparse Point & Junction Hardening**:
  - Unprivileged creation of Windows directory junctions using `FSCTL_SET_REPARSE_POINT` with mandatory `\??\` NT namespace prefix for `SubstituteName` and Win32 path for `PrintName`.
  - Defensive parser hardening: strict bounds checking for `PrintNameOffset`, `PrintNameLength`, `SubstituteNameOffset`, `SubstituteNameLength` against `bytes_returned` and `ReparseDataLength`.
  - Traversal breakout protection: added `is_reparse_or_symlink()` inspecting `FILE_ATTRIBUTE_REPARSE_POINT` to prevent directory junctions from bypassing `has_symlink_parent()` extraction sandboxing.
- **Fuzzing Harness Expansion & Seed Corpus**:
  - Expanded `fuzz_archive` and `fuzz_file_handle` harnesses to ingest seed corpora dynamically.
  - Generated comprehensive seed corpus for QuickOpen, metadata extra records, recovery records, and recovery volume sets.
- **WASM / JS Packaging & TypeScript Validation Polish**:
  - Guarded Worker tests against missing build artifacts for headless/non-emscripten environments.
  - Maintained complete TypeScript definitions for the in-memory archive API.

## [1.8.0] - 2026-09-17

### Added

- **QuickOpen (QO) Engine**: Full reader acceleration and writer serialization
  for RAR5 QuickOpen service blocks.
  - Writer: Contiguous header cache arena (`qo_arena`), structure CRC32
    verification, pre-sized payload buffers, and 10-byte fixed-width locator
    backpatching into `MainBlock`.
  - Reader: Instantaneous archive open probing `MainBlock` locator offsets and
    seeking directly to tail QO cache with defensive bounds checking, structure
    CRC validation, monotonic offset verification, and transparent fallback to
    sequential scanning on corrupt or partial caches.
  - Spec-compliant stripping: All archive mutation operations (`d`, `u`, `f`,
    `m`, `k`, `s`, `-rr`) automatically strip QuickOpen blocks and locators.
- **Three-Phase Multi-Threaded Parallel Extraction**: Decoupled parallel
  extraction architecture:
  - Phase 1: Parallel file decompression across worker threads (`-mt`).
  - Phase 2: Sequential restoration of symlinks, hardlinks, and junctions,
    guaranteeing link target files exist on disk before link creation.
  - Phase 3: Bottom-up directory metadata and timestamp restoration in reverse
    topological order (deepest directories first), preventing parent directory
    `mtime` clobbering.
- **Hardlink Deduplication** (`-oh`): Identifies duplicate hardlinks across
  Windows FileID (64/128-bit) and POSIX `(dev, ino)` pairs, storing subsequent
  instances as hardlink redirections rather than duplicate payloads.
- **Unix Permissions & Ownership** (`-ow`): Preserves and restores Unix UID,
  GID, user name, group name, and file permission bits with unprivileged
  `lchown`/`chmod` fallbacks.
- **SFX In-Place Conversion** (`s`): Converts archives to self-extracting
  executables using default or custom SFX stubs (`openrar s archive.rar`).
- **Recovery Record CLI Parity (`rr[N]`) & Switch Parity**:
  - Native support for the `rr[N]` command (e.g. `openrar rr5% arc.rar`).
  - Support for applying `-rr` and `-k` on existing archives without requiring
    file arguments.
  - Case-insensitive acceptance of WinRAR switches (`-qo`, `-qo+`, `-qo-`,
    `-am`, `-ams`).
- **Architect Skills**: Bundled `architect-challenge` and `architect-walkthrough`
  principal solution architect auditing workflows in `.agents/skills/`.

### Fixed

- **QO+RR Locator Size Mismatch in `add_recovery_record`**: Fixed a critical
  header corruption bug where adding a recovery record to a QO-enabled archive
  preserved the QO block verbatim, creating a 10-byte locator expansion that
  shifted all internal entry offsets and broke WinRAR validation ("Main archive
  header is corrupt"). `add_recovery_record` now cleanly strips QO blocks and
  sets `locator_qo_offset = -1`.
- **RAII Temporary File Lifecycle**: Introduced `TempFileCleanupGuard` in
  `RecoveryWriter::add_recovery_record`, guaranteeing that temporary files are
  closed and unlinked on any error, early return, or unwound exception.
- **Cryptographic Memory Scrubbing**: Added `openrar::crypto::secure_wipe`
  (`SecureZeroMemory` on Windows / `explicit_bzero` on POSIX) preventing dead-store
  compiler elimination, and equipped `Rar5Keys` and `HeaderCryptReader` with
  automatic RAII memory-scrubbing destructors.
- **Path Traversal & Device Name Containment**: Hardened extraction target
  checking with purely algorithmic lexical containment (`is_lexically_contained`)
  and protected `make_safe_component` against null bytes, control codes,
  forbidden Windows characters, and trailing-whitespace DOS device stems.
- **Performance & Mechanical Sympathy**: Pre-sized QO serialization buffers in
  `ArchiveMutator::write_batch_add_ex`, eliminating $O(N)$ reallocations for
  large archives.

## [1.7.0] - 2026-09-17

### Added

- **NTFS Alternate Data Streams (ADS) Archiving & Extraction** (`-os`): Full
  support for NTFS alternate data streams on Windows. Archiving enumerates and
  stores streams as child service records (`HFL_CHILD` | `HFL_INHERITED`)
  with `FHEXTRA_SUBBLOCK` headers matching WinRAR 5 format. Extraction restores
  named data streams to destination files. Portable stubs ensure graceful
  fallback and non-Windows compatibility.
- **NTFS Security Access Control Lists (ACL) Archiving & Extraction** (`-ow`):
  Full support for Windows security descriptors on Windows. Archiving captures
  owner, group, DACL, and SACL descriptors into inherited security records;
  extraction applies stored security descriptors to created files and
  directories via Win32 security APIs.
- **BufferArchive Checksum Verification**: In-memory archive extractions now
  verify checksums authoritatively against BLAKE2sp digests, and CRC32
  checksums are verified (properly respecting the 0-sentinel flag for zeroed
  CRCs).
- **Regression suites**: Added automated tests for NTFS streams and security
  metadata, BufferArchive CRC/BLAKE2sp corruption detection, switch parity
  validation, C-ABI ↔ JS/TS error code synchronization (`errorsync.tests.mjs`),
  corrupted compressed payload extraction rejection, and streaming-verify over
  tweaked-checksum archives.

### Fixed

- **Multi-volume Writer Partial-write Cleanup**: Added RAII `VolumeCleanupGuard`
  to track created volume files during multi-volume archive creation, safely
  removing any orphaned volume parts if writing fails or is interrupted.
- **Header Writer Flag Preservation**: `HeaderWriter` now preserves `HFL_CHILD`
  and `HFL_INHERITED` flags when writing file and service headers, ensuring
  child records properly maintain hierarchical relationships.
- **Verification Asymmetry Sweep**: The bool `extract_entry` path verified
  stored payloads but wrote compressed payloads without CRC/BLAKE2sp validation.
  All extract paths now enforce uniform hash policies (BLAKE2sp authoritative,
  else CRC32). Encrypted entries with tweaked checksums (`0x0002`) are accepted
  cleanly after password verification. CLI `t` verifies encrypted entries via
  streaming when a password is provided.
- **Recovery & Mutator Temp Files**: Temp files (`.rr_tmp`, `.rep_tmp`,
  `.rev_tmp`) now use unique counter-based names created with `CreateNew` to
  prevent symlink pre-plant truncation attacks. Mutator delete/lock rewrite
  loops and `.rev` writers are exception-safe and remove temporary files on
  failure.
- **Header & Memory Allocation Caps**: `openrar_archive_handle_info` enforces a
  16 MiB allocation cap before reading archive comments from crafted header
  lengths.
- **Fuzzing Harnesses & CI**: Fixed allocation leak of `handle_list` in
  `fuzz_archive`, removed duplicate `main()` definition in `fuzz_file_handle`
  under libFuzzer builds, enabled duration parameter in `fuzz.yml`, staged seed
  fixtures properly, and pinned the CI formatting gate to `clang-format-18`.

## [1.6.0] - 2026-09-16

### Added

- **Installable CMake package**: `cmake --install` now ships the shared
  library, the CLI, the public headers, and a `find_package(openrar)` config
  exporting `openrar::openrar_dll` / `openrar::openrar` / `openrar::openrar_core`
  (GNUInstallDirs layout; library include dirs are export-clean via
  `BUILD_INTERFACE` generator expressions; `Threads` resolved through
  `find_dependency`). The public C ABI header moved from `src/dll/` to
  `include/openrar/openrar_dll.h` — consumers compiling with `-Iinclude` no
  longer reach into the source tree; `src/dll/openrar_dll.h` remains as a
  forwarding shim for internal translation units.
- **CLI overwrite query**: the documented default (`Prompt`) now actually
  asks — `existing file. Overwrite? [Y]es/[N]o/[A]lways/n[E]ver/[Q]uit` —
  instead of silently overwriting. `-y` answers Yes on every query;
  non-interactive stdin (pipes, CI runners) auto-answers Yes so scripted
  callers keep their previous behavior; `-o+` / `-o-` are now advertised in
  the help text, and `-o-` (skip existing) is applied as a pre-filter so the
  parallel extraction path honors it too.
- **CLI executable in release assets**: release zips now carry the CLI
  alongside the shared library and import library.

### Changed

- CI: the nightly fuzz job installs clang and passes it to CMake, so the
  `-fsanitize=fuzzer` harnesses really build in libFuzzer mode (they silently
  degraded to the standalone sweep under GCC), with a post-build guard that
  fails the job if any harness lacks libFuzzer. The writer-conformance job
  runs the full 11-suite Node test set; the WinRAR-oracle asserts in the
  volume/mutation/recovery/dictionary suites are `oracleAvailable()`-gated
  like roundtrip, and extraction destinations are platform-aware, so every
  suite is POSIX-clean while self-verification runs everywhere.

### Fixed

- **Extraction hardening**: `durable_write_to` retries the next temp suffix
  on `CreateNew` collision instead of aborting; every extraction-path
  filesystem call uses `error_code` overloads (including the multivolume
  chain scan's volume probes); a per-entry pre-open gate
  (`convert_self_links` + `has_symlink_parent` + destination-symlink removal)
  runs before any output stream is opened, so a symlink entry followed by a
  file entry can no longer divert the write; failed extractions remove their
  partial output (`FileUnlinker` RAII, `keep_broken` opt-in); the
  stored-payload file path (the CLI's extraction route) now verifies CRC32 —
  on both the contiguous and extent-stitch variants — instead of writing
  corrupted data successfully; encrypted entries stay unverified per the
  RAR5 rule that their header CRC32 does not hold the plaintext CRC.
- **Repair**: inline RR repair detects same-length payload corruption via
  parity syndromes, localizes damaged shards (cross-shard syndromes, header
  scan, CRC32 candidate verification) and reconstructs via Reed-Solomon,
  refusing ambiguous damage.
- **Format**: encrypted-header size VINT scan accepts up to 10 bytes with
  padded-VINT support (2 MiB headers no longer trip bad_password); AES-CBC
  primitives refuse non-block-aligned sizes instead of silently flooring
  (a short ciphertext on extraction now fails as corruption).
- **CLI/Windows**: wildcard arguments (`*`, `*.rar`) are expanded on Windows
  for add/update/freshen/move, `-r`-aware; `-r`, `-ol`/`-ol-`, `-ep1..3` and
  `--` are parsed and wired; the banner prints the project version instead of
  a hardcoded "1.0 (x64)"; the duplicate `-ed` help entry is gone.
- **Portability**: `openrar.hpp` compiles under C++20 (`u8_str` bridge for
  `path::u8string()`'s `char8_t` return); `/dev/urandom` opens with
  `O_CLOEXEC`.
- **API**: `extract_all` on an empty archive returns `RAR_OK` (DLL, C API and
  wasm layers) instead of a false `RAR_ERR_NOMEM`; parity buffer sizing is
  64-bit overflow-checked with a 2 GiB cap; the wasm JS/TS error surface now
  maps `MISSING_VOLUME` (-13) and `BUSY` (-14) instead of degrading them to
  generic `IO`.
- **Tests**: the golden BufferArchive verification block actually compiles
  now (its guard macro was defined nowhere); CLI tests stop creating a
  literal `nul` file on POSIX; new regressions for the corrupt-payload →
  `RAR_ERR_CRC_MISMATCH` mapping, partial-file removal, parent-is-file
  collisions, temp-collision retries, empty-archive extraction, padded VINTs,
  and parity buffer overflow caps.

## [1.5.0] - 2026-09-15

### Added

- **Extended entry metadata** (`OPENRAR_ABI_FEATURE_ENTRY_EX`, bit 5):
  `openrar_archive_handle_entry_ex` returns the fields the frozen 64-byte
  entry struct drops — host attributes, host OS, mtime/ctime/atime as
  FILETIMEs (UTC, 100 ns; both FHEXTRA_HTIME encodings supported —
  FILETIME-format passes through, unix-format converts), solid / encrypted /
  redirection / split / directory flags, dictionary size and the format
  version — with the redirection target as a malloc'd NUL-terminated string
  when present (`openrar_archive_entry_ex_free` frees it; `openrar_free`
  works too). File-mode handles only; buffer handles return
  `RAR_ERR_UNSUPPORTED_FEATURE`.
- **Archive-level info**: `openrar_archive_handle_info` reports main-header
  flags, volume index/count (always determinable — the file-mode open is
  strict), recovery record size, and the archive comment, which is read
  lazily at query time: stored-compressed comments decompress, and a
  payload failing its CRC or decode reports as absent with `RAR_OK` (a
  filesystem read failure is `RAR_ERR_IO`).
- **Refinements** (enhancement plan §4.3, all four): named window-size
  constants `OPENRAR_WINDOW_128K` … `OPENRAR_WINDOW_1M` with the
  bytes-vs-log2 unit difference documented;
  `RAR_ERR_PARTIAL_OK` documented verbatim (returned solely by
  `extract_all`); the thread-local error state documented on
  `openrar_last_error` / `openrar_archive_get_error`; the feature-bit
  registry now lists bits 0–5 and pins the reserve convention (future
  open-time options ship as `openrar_archive_open_file_ex` behind a new
  bit, never as signature changes).
- C++ wrapper: `ArchiveHandle::entry_ex(idx)` / `ArchiveHandle::info()` with
  `EntryEx` / `ArchiveInfo` value types (buffer handles throw
  `UNSUPPORTED_FEATURE`).
- Tests: `metadata_tests` (18th ctest target) — both htime encodings,
  flag coverage, redirection extra lifetime, lazy comment read, RR size,
  volume provenance, buffer-handle refusals.

### Notes

- All additive: `OPENRAR_DLL_API_VERSION` stays 1. Exports 43 → 46; feature
  bit 5 reserved and shipped. This completes the Crate request catalog —
  #6 (extended metadata) lands here; #1–#5 and #7 shipped in v1.2.0–v1.4.0.

## [1.4.0] - 2026-09-15

### Added

- **Atomic deletion** (`OPENRAR_ABI_FEATURE_MUTATION`, bit 4):
  `openrar_archive_delete_entries_file` deletes entries from an existing
  archive **by index** — indices are in the file-handle listing order (the
  sequence `openrar_archive_handle_list` reports on an `open_file` handle;
  file entries only, service headers never exposed). The DLL translates each
  index to the entry's header offset with its own strict reader and deletes
  by that identity, never by name, so entry names containing `*` / `?` are
  safe. The rewrite lands in a temp file, is flushed, and atomically
  replaces the original — untouched on any failure.
- **Batch add/replace**: `openrar_archive_add_files_file` appends files to an
  existing archive with 'u' semantics — incoming names override same-name
  entries (byte-exact UTF-8 compare after normalizing `\` to `/`; all prior
  instances stripped). `method ∈ {0,3,5}`, `window_log2 ∈ [1,4]` (create
  parity; ignored by stored entries). Directory sources become directory
  records (non-recursive). Added files are written unencrypted — password /
  `encrypt_headers` parameters are deliberately deferred.
- **Solid archives — suffix-only delete** (docs/invariants.md §1, now
  enforced): deleting a member of a solid run while a later member of that
  run is retained fails with `RAR_ERR_UNSUPPORTED_FEATURE` instead of
  silently orphaning the LZ chain — the same guard now backs the CLI's
  mask-based delete. Allowed shapes per run: untouched, suffix deletion,
  whole-run deletion. Replacement of any solid-block member (head included)
  is refused; replace the tail of a run via delete + add. Adding new names
  to a solid archive continues its stream.
- **`RAR_ERR_BUSY` (-14)**: the mutation exports pre-check every open
  file-mode handle in the process and fail up front when the target (or any
  volume of its set) is still held open — instead of an opaque Windows
  sharing violation during the final rename. Close handles, then mutate;
  hosts re-open after every mutation.
- **Refusals, all `RAR_ERR_UNSUPPORTED_FEATURE`**: locked (`MHFL_LOCK`),
  multi-volume (`MHFL_VOLUME`) and header-encrypted (`-hp`) archives (the
  mutation surface takes no password; "mutating header-encrypted archive
  requires password").
- Comment (CMT) preserved by both operations; QuickOpen locators stripped;
  recovery records copied verbatim (not recomputed — treat as absent after a
  mutation).
- C++ wrapper: `delete_entries(path, indices)` and `add_files(path, files,
  AddOptions)` free functions mirroring `create_archive`'s ergonomics.
- Tests: `mutation_tests` (17th ctest target) pins the index-space identity,
  solid delete/replace guards, 'u' semantics, atomicity on failed batches,
  the `RAR_ERR_BUSY` handle collision, validation parity and CMT/QO
  behavior. `fuzz_file_handle` gained a mutation leg (delete/add/re-open
  against arbitrary bytes).

### Notes

- All additive: `OPENRAR_DLL_API_VERSION` stays 1; no frozen export changed
  behavior. Exports 41 → 43; feature bit 4 reserved and shipped.
- The frozen `list_file` / `list_file_ex` walks surface comment/recovery
  service blocks as entries and are therefore NOT the delete index space on
  such archives — hosts must list via a file handle (docs/dll-integration-spec.md
  §6.12).

## [1.3.0] - 2026-09-15

### Added

- **File-mode handles** (`OPENRAR_ABI_FEATURE_FILE_HANDLE`, bit 3): the
  streaming reader is now reachable from the DLL. `openrar_archive_open_file`
  opens a scan-once handle over an archive **on disk** — the file stays open
  for the handle's lifetime, headers are walked exactly once, and
  encrypted/solid/multi-volume archives are supported (the buffer handle
  surface keeps its frozen MVP semantics). Passwords enter at open (required
  up front for `-hp`, verified lazily per entry via constant-time PswCheck
  for `-p`); progress/cancel cover the open-time scan; middle-volume paths
  rewind to the derived first volume. The existing handle exports dispatch
  on handle kind: in-memory extract on file handles is capped at 256 MiB
  (`RAR_ERR_NOMEM` above), `extract_all` is `UNSUPPORTED_FEATURE` on file
  handles, and `handle_list` exposes file entries only (service headers are
  internal blocks).
- **Streaming extraction with progress/cancel/durability**:
  `openrar_archive_handle_extract_to_path` writes straight to disk with byte
  progress (uncompressed produced vs `entry.size`), cancel per output chunk,
  and DLL-owned durability — `dest.openrar-tmp.<pid>.<seq>` opened
  CREATE_NEW, flushed (FlushFileBuffers/fsync), atomically renamed; abort or
  failure deletes the temp and never leaves a partial destination. Extracting
  onto the archive (or any volume of its set) is rejected up front.
- **Streaming integrity test**: `openrar_archive_handle_test` verifies
  CRC32 / BLAKE2sp without retaining output — fixed small RAM regardless of
  entry size (stored entries stream in 64 KiB chunks). Encrypted entries are
  verified through chunked AES-256-CBC decrypt (CBC IV carried across
  slices) via PswCheck/MAC — wrong password is `RAR_ERR_BAD_PASSWORD`, never
  `CRC_MISMATCH`, and plaintext is never surfaced.
- **Solid archives**: out-of-order and repeated extraction are correct — the
  reader transparently decodes the solid run prefix through a discard sink
  (catch-up); in-order extraction remains the fast path. Progress stays at
  (0, entry.size) during catch-up; the handle stays usable after any abort.
- **Multi-volume sets**: transparent extent stitching across `.partNN.rar`
  volumes (primary held open, secondaries opened per access); missing
  volumes fail the open or the extraction with the new
  `RAR_ERR_MISSING_VOLUME` (-13) and the offending path in the error detail.
- C++ wrapper: `ArchiveHandle(path, password, progress, cancel, user)`
  constructor plus `extract_to_path` / `test` methods.
- `docs/invariants.md`: the pinned engineering contracts (solid block, RAM
  ceiling, volume lifetime, callback/cancel, durability, key hygiene).
- Fuzzing: new `fuzz_file_handle` harness (file surface, hostile volume
  naming, password variants, decompression-bomb cancel guard) wired into the
  nightly fuzz job with the checked-in fixtures as seeds.

### Notes

- All additive: `OPENRAR_DLL_API_VERSION` stays 1; no frozen export changed
  behavior (the CLI's tolerant missing-volume open behavior is preserved;
  only the strict file-handle surface enforces complete sets). Exports
  38 → 41. See `docs/dll-integration-spec.md` §6.11.

## [1.2.0] - 2026-09-15

### Added

- **Password listing of header-encrypted archives**:
  `openrar_archive_list_file_pw` streams an archive from disk and, when it
  carries a `HEAD_CRYPT` block, derives keys from the supplied password
  (PBKDF2) and decrypts every following header (AES-256-CBC). Wrong password
  → `RAR_ERR_BAD_PASSWORD`; no password on a header-encrypted archive → the
  existing `RAR_ERR_ENCRYPTED` early signal. In this mode file entries with
  encrypted payloads are reported (`is_encrypted = 1`) and the walk
  continues — `-hp` implies encrypted file data, so rejecting them would
  defeat the purpose. Negotiated via `OPENRAR_ABI_FEATURE_LIST_PASSWORD`;
  the frozen v1.1.0 listing semantics are untouched. There is deliberately
  no password variant of the in-memory listing (documented in the header).
- **Callbacks on the handle-API scan**: `openrar_archive_open_ex` runs the
  scan that happens at open time with byte progress and cancel (cancelled →
  handle 0 with an "open aborted" detail). Negotiated via
  `OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS`.
- C++ wrapper: `list_archive_file(path, password, ...)` overload and
  `ArchiveHandle(data, size, progress, cancel, user)` constructors.

## [1.1.0] - 2026-09-14

### Added

- **Progress/cancel on the archive-listing APIs** (additive, new exports
  only): `openrar_archive_list_file_ex` and `openrar_archive_list_ex` take the
  existing `openrar_progress_cb` / `openrar_cancel_cb` convention. Progress is
  byte-based — `done` = archive bytes consumed vs. `total` = archive size,
  polled between header blocks and during the SFX scan, with one final
  `(total, total)` on success — because RAR has no central directory and an
  entry-count denominator cannot work. Cancel is polled between header
  blocks; a non-zero return yields `RAR_ERR_ABORTED` with all outputs left
  untouched and nothing partial allocated.
- **Streaming file listing**: `openrar_archive_list_file_ex` walks the archive
  on disk instead of slurping it into memory, so listing multi-GB archives no
  longer materialises them in RAM and aborts stay responsive while the walk
  seeks across slow (network) storage. The header-walk state machine is now
  shared with the in-memory `BufferArchive::list` so both listing paths
  cannot drift apart.
- **Early password signal**: a header-encrypted archive (`HEAD_CRYPT`) now
  returns the new `RAR_ERR_ENCRYPTED` (-12) from the `_ex` listing exports as
  soon as the block is reached, letting hosts prompt for a password
  immediately. The historical surfaces keep `RAR_ERR_UNSUPPORTED_FEATURE` for
  the same condition.
- **Capability negotiation**: `openrar_abi_features()` returns a feature
  bitmask (`OPENRAR_ABI_FEATURE_LIST_PROGRESS`). `OPENRAR_DLL_API_VERSION`
  stays at 1 by policy — hosts negotiate additive exports via the feature bit
  or `GetProcAddress`, never via the version probe (`docs/versioning.md`).
- C++ wrapper overloads `list_archive(rar, progress, cancel, user)` and
  `list_archive_file(path, progress, cancel, user)`.

## [1.0.126] - 2026-09-13

First release on the public repository (github.com/geometric-dev/openrar).
Rolls up the initial CI bring-up: the codebase had never run under hosted CI,
and the first runs surfaced five platform correctness bugs alongside the
expected workflow fixes.

### Fixed

- **ARM64 CRC-32 produced wrong checksums on Apple silicon** — the ARMv8 CRC
  instructions chain the accumulator in the same raw (pre-inversion)
  convention as the scalar table step; an erroneous double inversion
  corrupted every CRC when the hardware path dispatched (`arm_crc32=1`).
  This path only compiles on macOS, where it had never been exercised.
- **SFX modules built with the bundled `Default.SFX` stub failed in
  UnRAR** ("Main archive header is corrupt"): the stub binary embedded a
  literal RAR5 signature constant, and UnRAR locates the archive behind an
  SFX prefix with a naive first-match scan, stopping inside the module.
  The signature is now assembled at runtime from XOR-masked bytes; the
  literal no longer appears in any binary. WinRAR's own stubs avoid
  embedding it for the same reason.
- **SFX structural test parser**: the JS `findSig` helper locked onto the
  first signature match — inside a module that legitimately embeds one —
  and parsed garbage. Candidates are now validated (header CRC plus a
  main/crypt block type must follow), mirroring the C++ reader.
- POSIX portability: `<sys/stat.h>` include in `archive_mutator.cpp`;
  MSVC ARM64 include guards (`<arm_acle.h>` / `<immintrin.h>` are not
  available there).
- aarch64 GNU/Clang builds now request `-march=armv8-a+crypto`; the AES-256
  and SHA-256 kernels use the crypto intrinsics unconditionally on
  `__aarch64__` and generic cross toolchains don't enable the feature by
  default (Apple clang does).

### Changed

- CI: Windows legs pinned to `windows-2022` (`windows-latest` now ships
  only VS 2026, so the VS 2022 generator cannot configure); the
  `msvc-arm64` leg is cross-compile-only (x64 hosts cannot execute ARM64
  test binaries — aarch64 runtime coverage stays with the QEMU job); all
  third-party actions pinned to commit SHAs; dead `numa08/setup-ninja`
  replaced with `seanmiddleditch/gha-setup-ninja`; formatting gate pinned
  to `clang-format-18` and the tree reformatted with it; Linux test
  runners accept Ninja-style binary paths.
- Repository: `LICENSE` renamed from `license.txt` (acknowledgement footer
  moved out so GitHub detects MIT), trademark/non-affiliation notice added
  to the README, `.gitattributes` added for cross-platform line endings.

## [1.0.121] - 2026-09-10

First tagged release, cut 121 commits past the v1.0.0 baseline. Entries cover
everything since the 2026-08-31 baseline where WinRAR interoperability was
restored and verified and compression speed parity was regained; earlier
development history is not itemized here.

### Added

- **Shared library**: stable C ABI (`openrar_dll.h`) with a C++ wrapper,
  architecture-suffixed binaries (`openrar_x64` / `openrar_arm64`), and an
  integration spec with C#, Python, and Rust examples.
- **WebAssembly build**: in-memory RAR5 create/list/extract with a handle-based
  JS API and a canonical interface contract.
- **Streaming**: incremental block encoder, streaming decompression with
  extraction flush callbacks, and buffered in-memory archive read/write.
- **Multi-volume archives**: streamed multi-volume create, plus `.rev` recovery
  volumes with generation and repair matching WinRAR's shard layout.
- **SFX creation** (`-sfx[name]`): SFX module prepended on create.
- **Header encryption** (`-hp`): supported on write and read.
- **Extended metadata**: nanosecond high-precision times plus OWNER and
  VERSION header extras.
- **Interop gate** (`tools/interop_gate.py`): self-roundtrip, WinRAR
  cross-decode, and a full ctest run, enforced as a pre-commit hook and in CI;
  covers multi-volume and SFX archives.
- **Performance**: compression runs ~2-4x faster than WinRAR on the 50 MB
  benchmark suite; ~2x faster PBKDF2 via cached HMAC midstates; ~8x filter
  speedup on non-SIMD paths; faster crc64 and header reads; leaner per-literal
  token storage.
- **Test suite**: property-based tests, a deterministic shape-aware roundtrip
  fuzzer, a libFuzzer-ready decoder harness, decompression cross-validation,
  WinRAR conformance scripts, golden files, and known-answer tests.
- **Release process**: SemVer versioning with the patch component as a
  monotonic commit counter (`docs/versioning.md`), this changelog, and
  `project(VERSION)` as the single version source of truth.

### Fixed

Safety and robustness:

- Zip-Slip path traversal on extraction; hardlink and FILECOPY sources
  confined to the extraction root; archive-controlled names hardened for
  Windows filesystems; planted-symlink truncation blocked via unique temporary
  names; recovery-record directory conversion only touches links the reader
  itself created.
- Malformed-archive hardening: vint field validation, attacker-controlled size
  clamping, 64-bit recovery-record geometry, decompressor filter/window
  accounting, buffer-extract output budgets, and AVX2 dispatch gated on
  OS-level AVX state (OSXSAVE/XCR0).
- Crypto: constant-time password-check comparison, PBKDF2 iteration-count
  guard, SHA-NI digest self-check, and password-derived material wiped on
  teardown.
- C ABI: escaping C++ exceptions caught at every `extern "C"` boundary, struct
  packing hygiene, and stream callbacks dispatched outside the map mutex.

Correctness:

- WinRAR interoperability restored (absolute Huffman table lengths, BC max
  bits, window-size sync) and locked in by the interop gate.
- Copy-match overlap undefined behavior; encoder corruption past the window
  size on external-buffer sources; large-file compression (empty blocks,
  memory streaming, RLE overflow); compressor state reuse across instances;
  solid-entry decoder state persistence; stored-entry CRC32/BLAKE2sp
  verification.
- Data-loss safety: archive replacement made data-loss-safe; EINTR-safe POSIX
  I/O; 32-bit MSVC intrinsic fixes.

### Changed

- CI builds and publishes the shared library across the matrix and runs the
  hardened interop gate (strict multi-volume cross-check, no weak fallback).
- Volume-chain cap raised to 65535 volumes; SFX size bound raised to 64 MiB.

## [1.0.0] - 2026-08-31

Baseline (development state, not distributed): clean-room RAR5 archiver with
WinRAR interoperability verified by cross-decode and compression running 2-3x
faster than WinRAR on the 50 MB benchmark suite.
