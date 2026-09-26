# 🏛️ Principal Architect Walkthrough & Implementation Sign-Off — v1.25.0

**Work Reviewed:** Memory-Mapped Read Engine (Re-scoped) — io::MappedFile,
ReadSource seam, mapped scanner wiring, random-read region ABI, listing
benchmark
**Plan Reference:** docs/v1.25-implementation-plan.md (architect-reviewed:
docs/mmap-v1.25-design-review.md — directives 1–7 applied pre-implementation)
**Range Audited:** 91ea44e..v1.25.0 (9 commits; +1497/−44)
**Architect Verdict:** 🟢 SIGN-OFF COMPLETE (RELEASED AS v1.25.0)

---

## 1. Executive Summary & Audit Posture

The arc delivers exactly what the plan committed: a memory-mapped read
engine for listing/header-scanning/random-read, never extraction inputs
(§5.2 normative), with the fail-open contract honored everywhere — mapping
failure, FSCTL refusal (ReFS dev volumes), and the --no-mmap /
OPENRAR_NO_MMAP kill switch all fall back to the buffered engine with
identical results. The ReadSource seam is the arc's best decision: one scan
code path, zero duplication, and the existing FileStream call sites compile
unchanged.

The self-audit re-screened the diff for the Pillar-7 patterns: the mapped
view is scan-scoped (released before payload reads — verified: the volume
loop constructs fs/mv locally per iteration and process_volume takes
ReadSource&), extraction stays on the buffered FileStream (verified: no
mapped references in the payload/extraction paths), and the POSIX
truncation posture is the plan's documented no-signal-handler residual.

The one deviation (bench corpus shrink on FSCTL-refusing volumes) is a
legitimate course correction, discovered empirically on this machine
(ReFS dev volume: ERROR_INVALID_FUNCTION), and it makes the CI ubuntu leg
(ext4 sparse, native) the full-scale gate — exactly the fail-open rule
applied to the bench itself.

## 2. 📋 Plan Reconciliation, Claim Ledger & Deviation Analysis

| Claim (plan §1/§3, CHANGELOG) | Status | Evidence |
|---|---|---|
| io::MappedFile, pre-flight size pin | CONFIRMED | mapped_file.cpp:159–166 (fstat → mmap), :235 (read_at re-check) |
| Windows SEH fault-guard leaf | CONFIRMED | mapped_file.cpp guarded_copy (__try/__except, AV+in-page) |
| POSIX no-signal-handler posture | CONFIRMED | mapped_file.cpp — no handler installs; fstat clamp per read_at:235 |
| ReadSource seam, one scan path | CONFIRMED | file_stream.hpp (interface), header_reader.hpp:41 (widened read_block_raw), archive_reader.cpp:974 (single select_volume_source) |
| select_volume_source decision fn | CONFIRMED | archive_reader.cpp:974–985, call sites :1003/:1009 only |
| --no-mmap / OPENRAR_NO_MMAP kill switch | CONFIRMED | archive_reader.cpp:166–171 (env), cli/main.cpp (--no-mmap switch, dispatch-wired use_mmap) |
| Scan-scoped view, extraction buffered | CONFIRMED | volume loop constructs fs/mv locally per iteration; payload path untouched by mapped code |
| OPENRAR_ABI_FEATURE_MMAP bit 16 | CONFIRMED | openrar_dll.h:110, dll_api.cpp openrar_abi_features() ORs it |
| read_entry_region export + refusals | CONFIRMED | openrar_dll.h:466+, dll_api.cpp (base default + FileArchiveHandle impl + C export), archive_reader.cpp:284+ (read_payload_region: stored-only, unencrypted, range-must-fit, whole-range CRC/BLAKE2sp verify) |
| 50 GB listing benchmark | CONFIRMED | openrar_bench.cpp bench_listing_50gb (sparse corpus, FSCTL verdict, skip on refusal) |
| Tests: truncation, limits, identity, feature bit | CONFIRMED | mapped_file_tests (truncation gate, platform-split by the ERROR_USER_MAPPED_FILE proof), mapped_scan_tests (limits-not-bypassable, byte identity, mid-scan truncation), dll_cpp_tests (feature bit + full/partial/out-of-range region reads) |

- **Delivered as planned:** all four milestones, both gates, the ABI bit and
  export, the kill switch, the fail-open diagnostics.
- **Justified course corrections:** (1) Windows pins mapped-file sizes
  (ERROR_USER_MAPPED_FILE, proven in-test) — the truncation gate is
  platform-split accordingly; (2) ReFS dev volumes refuse FSCTL_SET_SPARSE —
  the bench skips rather than fills the disk; (3) M2+M3 landed as one commit
  (interleaved in archive_reader.cpp) with separate test suites.
- **Unjustified deviations:** none found.

## 3. 🚨 Implementation Defects & Architectural Invariants

- **None open.** Two defects were caught and fixed INSIDE the arc by the
  gate tests: (a) the FSCTL-on-write-handle failure (replaced with a
  dedicated pre-open handle + verdict-checked corpus selection); (b) the
  --no-mmap switch parsed but never forwarded to extract_archive (found by
  the WSL -Werror leg's unused-variable warning). Both fixes are in the
  shipped range (47b16f0, e1af04b lineage).

## 4. 🧱 Happy-Path Bias Screen (Pillar 7)

- **Commit atomicity:** CLEAN — no new commit paths; the bench's corpus
  write is scratch data.
- **Fail-open verification:** CLEAN — mapping/FSCTL failures fall back with
  visibility (OPENRAR_DEBUG_MMAP line, bench FSCTL verdict print).
- **Parallel/streaming divergence:** CLEAN — one select_volume_source; the
  parallel CLI path uses slot readers for payload only (no scanning).
- **Destructive ordering:** CLEAN — no destructive operations introduced.
- **Cleanup symmetry:** CLEAN — MappedFile unmaps in close()/RAII; the bench
  removes its corpus on every path.
- **Binding conformance:** CLEAN — dll_cpp_tests exercises the real C ABI
  path (openrar_archive_handle_read_entry_region) incl. feature-bit check.
- **Error fidelity:** CLEAN — region refusals map to distinct codes with
  cause-naming set_error calls (incl. the durable_write_to stale-error fix
  inherited from the arc's diagnostics pass).

## 5. 🔬 Evidence Gaps & Missing Verification

- **The 50 GB full-scale run is CI-only** (ext4 sparse): the local ReFS dev
  volume refuses FSCTL_SET_SPARSE, so the bench self-skips locally. This is
  the documented design (fail-open), and the CI ubuntu leg covers it — noted
  for completeness, not a gap in the shipped gate.
- **read_payload_region on compressed/encrypted entries** returns refusals
  (tested); a future arc could add CBC random-access via block-chain
  decrypt. Recorded as a follow-up, out of scope.

## 6. ⚡ Performance, Systems & Code Hygiene

- The mapped scan eliminates per-header seek+read syscalls; the POSIX
  fstat-per-read_at re-check is the documented truncation posture (kept out
  of the byte-copy leaf).
- Bench hygiene: the corpus is sparse-marked (Windows) / hole-based (ext4)
  and removed on every path; the disk-pressure guard reduces the corpus on
  runners without headroom.
- No debug prints, no TODO litter, no scratch files committed (verified:
  git status clean at tag).

## 7. 🛠️ Remediation Directives

None — merge/release clearance granted. The arc shipped as v1.25.0 with all
gates green on every CI leg (MSVC, gcc, clang, macOS ARM64, linux-arm64
QEMU, WASM).
