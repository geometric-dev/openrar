# 🏛️ Principal Architect Challenge & Review — v1.25.0 Memory-Mapped Read Engine

**Plan Reviewed:** docs/v1.25-implementation-plan.md — Memory-Mapped Read
Engine (Re-scoped)
**Architect Verdict:** 🟡 CONDITIONAL APPROVAL (REVISIONS REQUIRED) —
directives 1–7 applied to the plan in the same commit; implementation
proceeds on the revised plan.

---

## 1. Executive Summary & Core Posture

The re-scope is sound and, importantly, *honest about its own limits*: mapped
reads for listing/scan/random-read only, buffered extraction untouched
(SECURITY_ARCHITECTURE §5.2), no POSIX signal handlers, fail-open to the
buffered path. The single-code-path decision — widening
`read_block_raw(FileStream&)` to a minimal `io::ReadSource` interface rather
than duplicating the scanner — is the strongest architectural choice in the
plan. Verified against the code: `read_block_raw` uses only `src.read`
(header_reader.cpp:97–182), the scan region uses exactly the narrow
`read/seek/tell/size` set (~32 call sites), and an existing memory-mode
reader (`read_block_raw_mem`) already exists as precedent. The interface
refactor is real and low-risk.

But the plan had one **unexamined platform conflict with shipped v1.24
behavior**, one **fault-policy asymmetry it under-specified**, and one
**binding-layer wording error**. On Windows, a mapped view *pins the file
size*: any concurrent `SetEndOfFile` fails with `ERROR_USER_MAPPED_FILE` —
proved empirically in mapped_file_tests during this arc's M1. That directly
collides with a deliberate v1.24 contract: `FileStream::open` shares reads
with writers so *scans proceed while another process appends to a volume
set* (the INTENDED comment at file_stream.cpp). A mapped scan holding a view
for the handle's lifetime would freeze every appending producer.

The Failure-Mode Matrix and Negative-Test Plan are both present (Pillar-7
gate check passes structurally), and the seven named tests are deterministic.

## 2. 🚨 Critical Red Flags & Fatal Flaws

None fatal. The design keeps extraction on the buffered path (normative),
never installs POSIX signal handlers, and bounds every mapped access. No ABI
break: bit 16 is the next free feature bit, and the new export is additive
behind the feature query (verified: `openrar_abi_features` at dll_api.cpp:919
currently ORs bits 0–15).

## 3. ⚠️ Weak Assumptions & Fragile Invariants

- **"Windows SEH covers truncation" is moot — Windows prevents truncation
  of mapped files.** The OS refuses `SetEndOfFile` on a mapped file
  (`ERROR_USER_MAPPED_FILE`, verified in mapped_file_tests). Consequences:
  (a) the POSIX truncation race is structurally absent on Windows, so the
  Windows fault-guard leaf defends against in-page errors, not truncation;
  (b) more importantly, **the mapped view blocks concurrent appenders**.
- **"WASM surface unchanged except feature advertisement" was factually
  wrong.** There is no `openrar_abi_features` export on the WASM surface
  (verified: no hits in src/wasm). The plan now says: the feature bit is
  advertised on the native DLL only; the WASM surface gains nothing and
  loses nothing.
- **Mapped `size()` is pinned at open; a live `FileStream::size()` grows
  with the file.** Mixed semantics across engines is a Pillar-7.9
  coordinate-frame hazard. Both engines now treat the volume size as the
  open-time size for the duration of the scan; stated in the plan.

## 4. 🧱 Happy-Path Bias Screen (Pillar 7)

- **Failure-Mode Matrix:** CLEAN (plus a new appender-pin row).
- **Negative-Test Plan:** CLEAN — seven named, deterministic tests.
- **Parallel/Streaming Divergence (7.2):** HIT (minor) → the plan now names
  `select_volume_source()` as the single decision function and commits to no
  `if (mapped)` branches inside `process_volume` beyond source selection.
- **Fail-Open Verification (7.4):** HIT (minor) → the fallback is diagnosable
  (`--no-mmap` / `OPENRAR_NO_MMAP=1` + env-gated debug line).
- **Binding-Layer Blindness (7.7):** HIT (minor) → the read-region export's
  conformance test is named (DLL C++ suite + feature-bit test); WASM pinned
  to no-change.
- **Destructive Commit (7.3) / Determinism (7.8):** CLEAN.

## 5. ⚡ Mechanical Sympathy Gaps

- **`fstat` per `read_at`** halves the syscall win on POSIX. Resolution: the
  size re-check rides the scan chunk granularity, not per header copy.
- **Zero-copy parse** (parsing header bodies in place from the view via
  `read_block_raw_mem`) is deliberately deferred — it would fork validation
  logic. Recorded as a measured follow-up.
- **Benchmark honesty:** the 50 GB gate uses a sparse archive AND a
  header-dense 100k-entry archive; both timings reported.

## 6. 🔍 Omissions, Edge Cases & Recovery Gaps

- **Runtime kill switch:** `--no-mmap` / `OPENRAR_NO_MMAP=1` (v1.23
  `-sfxnoexec` pattern).
- **Mapped view lifetime:** held only for the volume scan; released before
  payload/extraction reads; random-read maps per region.
- **Empty files:** `MappedFile::open` refuses `st_size == 0` — callers fall
  back to buffered reads (no zero-length mappings).

## 7. 🛠️ Actionable Revision Directives (all applied)

1. Mapping lifetime: scan-scoped views; appender-block window = the scan.
2. Kill switch: `--no-mmap` / `OPENRAR_NO_MMAP=1`.
3. WASM wording corrected: no feature advertisement there, none added.
4. Size-frame rule stated: open-time size for the whole scan, both engines.
5. Decision function named: `select_volume_source()`, single mapped/buffered
   branch.
6. Benchmark: sparse 50 GB + header-dense 100k-entry archives, both timings.
7. Conformance test named for the read-region export (DLL C++ suite +
   feature-bit test).
