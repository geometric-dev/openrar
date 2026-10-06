# Changelog

All notable changes to OpenRAR are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.37.0] - 2026-10-06

Two-phase intra-entry parallel decode — the revised v1.37.0 arc per the
design-wall record (`docs/v1.37.0-design-wall.md` SS4 lever 1). Gate 0:
`docs/v1.37.0-two-phase-pre-analysis.md`; plan with the shipped-state M4
record: `docs/v1.37.0-two-phase-implementation-plan.md`. MINOR bump: the
patch counter resets at this tag.

### Added

- **Two-phase parallel decode for single members** (`-mtN`, N >= 2):
  a header-only pre-scan builds the block/table timeline; span workers
  decode symbol spans into operation records (the parse is stateless
  given the tables — Gate 0 SS0, verified structurally per token class,
  empirically by the parked scout's 141/141 block-state validation, and
  against the black-box scaling record); one applier consumes spans in
  order through the SAME apply engine the sequential token loop drives —
  sequential/parallel divergence is structurally impossible, and emission
  is byte- AND chunk-identical (`src/compress/parallel_decode.{hpp,cpp}`
  `should_use_parallel_decode`/`decode_entry`,
  `src/compress/decompressor50.{hpp,cpp}` `ApplyEngine`, `prescan_member`,
  `decode_span`, `OpRecord`, `apply_span_records`,
  `src/archive/archive_reader.cpp` `decode_compressed`).
- Decision-function gating: solid chains, multivolume members, encrypted
  entries, v1-flavor (>4 GiB window) members, and sub-floor packed sizes
  stay sequential (bit-identical path); `OPENRAR_NO_PARALLEL_DECODE=1`
  kill switch; `OPENRAR_PARALLEL_DECODE_THREADS` override; WASM/ILP32
  compile-out (`should_use_parallel_decode`).
- MT-vs-ST differential sweep in the roundtrip fuzzer: both engines on
  every generated archive (byte + chunk identity) plus hostile packed-
  stream mutations (fail-closed or fallback-owned verdicts)
  (`tests/fuzz/fuzz_roundtrip.cpp` `two_phase_differential`).
- `two_phase_tests`: pre-scan agreement + hostile rows (F1), R2 block-cap
  row, span-records identity across splits 1/2/3/4/8 with 257-at-span-
  start and dest-crossing rows [R8], driver gate matrix, MT/ST byte+chunk
  identity at threads 2/3/4/8, filter-bearing member identity
  (`tests/unit/two_phase_tests.cpp`).
- `tools/tokencensus.cpp` (Gate 0 probe, master path): token census +
  alpha measurement through the real phase-1 path; `tools/perf_two_phase.py`
  (paired-delta claim harness, v1.34 SS3 protocol).

### Changed

- `Decompressor50::ApplyEngine` extraction: the apply path (ring window,
  `copy_match`, filter queue, out-of-place transforms, flush cadence) is
  shared by the sequential token loop and the record applier — the M0
  refactor is behavior-neutral and frozen by the full gate
  (`src/compress/decompressor50.{hpp,cpp}`).
- ROADMAP/PERFORMANCE/README updated with the measured claims: 1.21-1.36x
  at `-mt4` on decode-bound 64 MiB text (2C/4T host), no-regression
  0.96-1.09x; the Gate 0 prediction (1.5-2.2x) missed for structural
  reasons recorded in the plan's M4 section (the end-to-end serial stage
  is apply+flush+CRC+write at 0.55-0.62 of sequential, contended on 2
  cores — reference parity within session noise).

## [1.36.16] - 2026-10-06

Solid-chain E8/ARM transform-base fix, found by the v1.37.0 (revised)
Gate 0 while probing the open questions parked in
`docs/v1.37.0-two-phase-pre-analysis.md` SS9 against the oracle.

### Fixed

- **E8/E8E9/ARM transform base carried the accumulated solid position**
  (`Decompressor50::decompress_internal`): `fe.file_offset` — the address
  fixup base the executable/ARM filters apply — was seeded from
  `base_at_entry` (the carried-window chain position), while the wire
  encodes filter offsets relative to the current member; the reference
  resets the transform base per member even on solid chains
  (oracle-verified: a Rar 7.20-made solid four-member archive with real
  DLL content engaging 78 E8 + 28 E8E9 filter tokens extracts byte-exact
  with the per-member base and CRC-fails on members 3-4 without it —
  never silent, always rejected by the CRC32 check). Never triggered by
  OpenRAR-produced archives (the encoder scopes filters to chain heads)
  and invisible to self-roundtrip tests by construction; spec 04's
  `solid_base + ...` wording corrected alongside
  (`src/compress/decompressor50.hpp` `member_start_`,
  `src/compress/decompressor50.cpp` filter-token site,
  `docs/spec/04-filters.md` Filter Signalling).
- Regression `solid-chain E8 transform base resets per member`
  (`tests/unit/solid_packer_tests.cpp`): a carried-window chain whose
  non-first members are PE-like (E8-filtered via `compress_buffer`)
  roundtrips byte-exact; verified to fail on the pre-fix decoder.
- Interop Track 18 `solid chain, filtered non-first member`
  (`tools/interop_gate.py`): Rar-made solid archive with an E8/E9-dense
  non-first member extracts byte-exact (OpenRAR + reference control);
  gate grows 24 -> 25 stages.

## [1.36.13] - 2026-10-05

Worker verify fix for members >= 64 MiB unpacked (the en-route P2 of the
v1.37.0 record; root cause and isolation notes in
`docs/v1.37.0-design-wall.md` SS5). Version note: the 1.36 line is the
highest MINOR released; the patch counter counts every commit on master
since `v1.36.0`, including the interleaved 1.35.0 release line, which
restores the strictly increasing version order this policy mandates.

### Fixed

- **Streaming BitReader mid-stream short refill** (`BitReader::refill()`):
  the next input chunk was fetched only when the 64 KiB staging buffer
  was completely drained, so an accumulator down to its last bits
  coupled with a buffer down to its last bytes returned a zero-padded
  peek MID-STREAM (zero padding is end-of-stream semantics). Whether
  that corrupts decoded output depends on the resolved Huffman code
  length, which is why the sandboxed-worker verify path (`t`) reported
  false FAILED with scattered single-byte corruption only on some large
  members while contiguous readers over the same stream were immune.
  `refill()` now loops - tops the accumulator to >= 57 bits, fetching
  successive chunks, and serves short only at the true end of stream.
  Regression: `bitreader_stream_tests` (streaming vs contiguous
  differential over an identical op sequence; fails on the pre-fix
  reader).

## [1.35.0] - 2026-10-04

RAR 7.x recovery-record parity + resource forks. Gate 0:
`docs/v1.35.0-pre-analysis.md` (conditional approval, directives D1–D16).
Research and black-box probe record: `docs/v1.35.0-rr-probe.md` (all claims
measured against Rar.exe 7.20 / UnRAR 7.20; independently reviewed against
outside format analysis with corrections re-verified on the wire).

### Added

- Recovery records now emit the shard-header state blob the reference
  repairer consumes as its erasure locator: per-data-chunk raw CRC-64
  entries (init 0, no final XOR, unpadded tail chunk) and
  `chunk_data_extent`, with a fixed zero seed — the record stays
  bit-for-bit reproducible and the reference validator and repairer accept
  and fully use it (`src/recovery/recovery_writer.cpp` `build_shard`,
  `add_recovery_record`; probe record §5 expC, T-ORACLE-1/2).
- `r` multi-erasure repair: a new localization tier reads the per-chunk
  entries and rebuilds any set of up to NR damaged chunks byte-exactly,
  where v1.33.5 repaired exactly one damaged unit; capacity is refused
  fail-closed (D6) and zero-entry legacy records keep the v1.33.5
  syndrome tiers unchanged (F7 pin)
  (`src/recovery/recovery_writer.cpp` `RecoveryWriter::repair`).
- Reference multi-physical record support: `r` reassembles the scale>1
  packaging (64 KiB physical chunk-shards, unscaled headers, per-slice
  entries; piece-count bounded, all-or-nothing validation) — damaged
  RAR 7.20 16 MiB-class archives repair byte-exact (T-ORACLE-4).
- Resource forks & FinderInfo: `-ox` captures `com.apple.ResourceFork` /
  `com.apple.FinderInfo` under the existing FHEXTRA_XATTR caps (over-cap
  values skip, never truncate); restore is default-on and Apple-hosts
  only (`src/archive/archive_mutator.cpp` `xattr_capturable`;
  `src/archive/archive_reader.cpp` `xattr_restorable`).
- `tools/scan_rr.py`: CRC32-validated RR-section probe — shard analyzer
  with CRC-64/XZ verification and packaging-shape classification, plus
  `--matrix` (writer-equivalence proof against the Cauchy/GF(2^16)
  convention).

### Changed

- Premise revision recorded: "WinRAR 7.0 changed the RR format" is not
  reproducible through the console oracle in any probed configuration;
  RAR 7.20's inline RR is the classic `{RB}` scheme, RS-core bit-equal to
  ours, and 0x11D is the RAR4-era GF(256) polynomial absent from RAR5
  containers (`docs/spec/05-recovery.md` corrected accordingly).

### Verification

- Wiped-build 45/45 test suites, ALL 24 interop gate stages, golden
  fixtures unchanged (non-RR archives byte-identical), oracle sweep
  T-ORACLE-1..5 green (Rar 7.20 `t`/`r`, UnRAR 7.20 `t`, both directions,
  scale 1 and 16 MiB). Verification record:
  `docs/v1.35.0-implementation-plan.md`.

## [1.36.0] - 2026-10-04

cv (foreign-format migration) now migrates symlink and hardlink entries to
RAR5 FHEXTRA_REDIR records instead of skipping them. Targets migrate
verbatim; extraction-time safety (absolute/escaping target refusal,
symlink-parent scan, spec-07 skip semantics) is unchanged and enforced by
the extractor. Hardlinks whose master did not migrate are skipped with a
`link_target_missing` report rather than emitted dangling.

Also carries the v1.34.0 extraction-throughput record (docs/): the measured
baseline (extraction ~2x behind UnRAR; decode is 84% of it), the negative
decode-micro-optimization result with the paired-delta/min-statistic
protocols, and the scoped levers (v1.37.0 MT decode driver, v1.38.0 decode
kernel rework, durability batching candidate in the pool).

### Added

- cv: symlink/hardlink migration with per-entry report rows and a new
  `migrated_links` result counter and summary line.
- Mutator: `prepare_add_symlink_from_memory` /
  `prepare_add_hardlink_from_memory` staging variants (times from
  caller-supplied foreign metadata; the unix HTIME fields are 32-bit).
- Gate 0 documents: `docs/v1.36.0-pre-analysis.md`,
  `docs/v1.34.0-pre-analysis.md`, `docs/v1.34.0-implementation-plan.md`.

## [1.33.5] - 2026-10-03

Close-out of the v1.33.1 threads: the length-3 admission floor is restored at
every method after the v1.33 Design B gate failed to reproduce, and the two
deferred investigation threads are resolved at the documentation level.

### Fixed

- **The length-2 admission floor is reverted.** The v1.33 gate claimed m1
  -1.49% / m2 -1.64% for admitting 2-byte matches at the shallow-finder
  methods; the re-gate measured +/-0.02% between floors on every corpus
  available (canonical payload, two 64 MiB text constructions, code-like
  text, random, zeros - byte-identical on random and zeros), and the floor
  measurably suppressed table reuse at zeros-to-random boundaries: floor 3
  recovers 8.3 KB and 167 reuse blocks on a 32 MiB heterogeneous member.
  `MIN_MATCH` stays 2 as the wire floor (slot 0 is legal); T10 pins the
  boundary admissibility and T12 now pins that no method emits the class.
  Emitted bytes change only on text-like inputs (bounded by the measured
  +/-0.02%) and improve on mixed-content members.

### Documented

- **Trailing-bits rule refined** (spec 06, question-log Entry 23): padding
  bits corrupt a block only when they complete a code in that block's table.
  Reference producers emit non-byte-aligned tails routinely (838 of 954
  blocks in a WinRAR sweep); our decoder stopping at the declared bit count
  is the conservative reader.
- **MT seed inheritance scoped** (question-log Entry 23): sharing table
  state across chunks is blocked by parallel chunk dispatch - inheritance
  needs serialization or speculative table commit - for a bounded payoff of
  ~361 B on zeros-format MT. Recorded as a scoped arc.
- **MT memory budget measured**: peak RSS on 256 MiB zeros at m3 is ~54 MB
  single-threaded and scales near-linearly with the 16 MiB chunk floor
  (195 MB at `-mt2`, 382 MB at `-mt4`, 502 MB at `-mt8`).
- `tools/scan_table_reuse.py` now reports the final-byte bit-count
  histogram.

## [1.33.0] - 2026-10-03

Table reuse: the encoder now omits a compression block's Huffman table
description when the decoder already holds exactly those tables, gated by
the decoder-slot seed rule that makes it interoperable with multithreaded
extractors. Emitted bytes change on run-heavy input (64 MiB of zeros:
3,616 -> 2,748 B single-threaded, 3,612 -> 3,109 B at `-mt4`). The wire
format is untouched: 30/30 oracle checks across UnRAR `-mt1/2/4/8` and
WinRAR at every size that previously failed, including members that force
a mid-member table change.

### Added

- Block-header table reuse with decoder-slot seeding: descriptions on the
  first 16 blocks of a member and again after every table-set change;
  reuse is unbounded between changes.
- Length-2 match emission at the shallow-finder methods (m1-m3): m1
  -1.49%, m2 -1.64%, m3 -0.04% on text; m4/m5 byte-identical.
- MT chunk floor of 16 MiB (2 x the seed length in block quanta) so a
  chunk amortises its re-seed: at `-mt4` zeros are ~14% smaller at 1.27x
  single-thread-equivalent throughput (peak RSS 382 MB on 256 MiB zeros
  vs 54 MB single-threaded).

### Fixed

- Match emitters reject unrepresentable (length, distance) pairs instead
  of silently clamping the base length - the clamp decoded to a different
  length than verified, a one-byte desync class.
- The parallel pipeline's sequential fallback read the whole chunk in one
  buffer; with the larger floor a small file was a single read and
  cancellation could never be observed mid-run. Reads are bounded at one
  block quantum.

### Changed

- Emitted bytes change for single-threaded run-heavy input and for all MT
  output. Decoded output is byte-exact; the wire format is untouched.

## [1.32.0] - 2026-09-30

Throughput scaling: the MT encoder is switched back on, and the reason it was
off is documented. Emitted bytes for `-mt>1` **change** (the wire format is
untouched; every stream still decodes under UnRAR and WinRAR, 24/24 interop
stages). Single-threaded output is unchanged.

The version number was re-purposed: v1.32.0 was first scoped as an
optimal-parse (cost-model token selection) arc. That was built, measured,
failed its gates and was removed; see ROADMAP.md "Descoped".

### Fixed

- **MT never ran for filter-free input.** `prepare_add_file` selected the
  parallel path with an inverted condition, and since the default filter mode
  is `Auto` the result was a double inversion: no filter detected meant
  *sequential*, and a filter detected meant *MT with the filter discarded*.
  Text, source and most data therefore got no multi-threading at all, while
  `-mc` parity was silently lost on the data that did. The comment above the
  code stated the opposite intent. The decision is now a named, directly
  testable function, and `test_parallel_path_selection` pins both branches —
  every previous test drove the pipeline directly, which is why this shipped.
- **The sequential fallback was not sequential.** The spool path built its
  fallback encoder with the *chunk* window instead of the full window, so
  "fall back" still lost every match spanning a chunk boundary.
- **Unbounded MT ratio loss, now bounded where it is detectable.** MT
  compresses chunks independently, so cross-chunk redundancy is lost and the
  loss is unbounded: a 4 MiB region repeated 8x costs **+698%** and runs 3x
  slower. MT is now declined on input that would pay it, which leaves the
  emitted bytes equivalent to sequential.
- **`tools/make_bench_payload.py` could not run on Windows.** It read sources
  with `errors="replace"` and wrote them back through the platform codec, so
  the canonical payload failed to generate outside a UTF-8 locale.
- **`${CMAKE_SOURCE_DIR}` broke FetchContent/add_subdirectory consumers**
  (issue #2). It resolves to the *consumer's* source tree: binaries landed in
  their checkout, two Python gates failed with `[Errno 2]`, and
  `js_error_mirror_parity` was silently never registered. Source paths now use
  `${PROJECT_SOURCE_DIR}`; the output root is a cache variable that is
  `${CMAKE_BINARY_DIR}/openrar64` under a subproject and unchanged
  standalone, because ~12 tools and CI hardcode `build/openrar64` and a bare
  swap would have moved the binary out from under preset builds.

### Known issues

- **Our MT loses matches that WinRAR's keeps.** On redundant input, 428,242 B
  vs 3,855,663 B on the benchmark payload's code third — a 9x regression
  against WinRAR's +0.005%. Bounded for corpus-shaped input by a redundancy
  probe, but not completely: `test_parallel_probe_known_gap` pins the residual
  with its measurement.
Root cause, measured by sweeping chunk size and nothing else: the loss was
  exactly proportional to the number of chunk boundaries (2 MiB chunks +800%,
  8 MiB +200%, a single chunk 0%). **Fixed in this release.** Each MT context
  is now seeded with the member bytes preceding its chunk
  (`min(dictionary_size, member_offset) - 1`), indexed into the hash but
  never emitted, with the rep state unset so no block opens with a
  rep-distance or 257. `-mt4` on the benchmark's code third went from
  3,855,663 B to 428,447 B (+0.05% over `-mt1`), and MT output on the
  canonical corpus is now byte-identical to `-mt1`. Our decoder was verified
  correct throughout (a WinRAR `-mt4` archive extracts byte-exact).
- Known follow-up: seeding is O(seed) per chunk, which costs clean-data
    scaling (2.0x → 1.71x) and makes MT a net loss on highly compressible
    input (0.69x on the code third, where WinRAR holds 1.54x). An optimisation
    to carry the match index across a worker's consecutive chunks was built
    and measured, and recovered no measurable time — see
    docs/v1.32-pre-analysis.md §8.8. Nothing is scheduled for it.
- The canonical benchmark corpus now DOES measure MT scaling (the redundancy
  probe that used to decline it was removed with the dictionary fix), and MT
  output on it is byte-identical to `-mt1`. See PERFORMANCE.md.

## [1.31.0] - 2026-09-29

Encoder Match Engine: the compression-quality arc. Two long-documented
performance losses are fixed, one is honestly characterized. Emitted
bytes change for `-m1`–`-m5` (wire format untouched; every stream
cross-verified by UnRAR 7.20 + WinRAR 7.20 — interop gate extended to 24
stages including a permanent slot-257 long-match legality track).
Gate 0 + decision records: docs/v1.31-pre-analysis.md,
docs/v1.31-implementation-plan.md.

### Added

- **Slot-257 run-collapse emission** (`src/compress/compressor50.cpp`):
  the encoder now emits LD symbol 257 (repeat-last-length) on exact
  continuations of the `(OldDist[0], LastLength)` thread — one Huffman
  symbol, zero extra bits. Zero-run/RLE-heavy data: **64 MiB of zeros
  packs 23,989 -> 3,514 bytes (-85.4%)**; the 8x zeros-ratio gap vs
  WinRAR collapses to 1.23x. Guarded by byte verification, exact-
  continuation (strict-equality) selection inside the existing rep-win
  branch, and the filter-region emission bound (T1-T5, T9 in
  tests/unit/compress_tests.cpp; streaming identity in
  tests/unit/stream_encoder_tests.cpp).
- **`compress_m5_matchfinder` bench suite** (tests/bench/openrar_bench.cpp):
  the m5 match finder's end-to-end kernel number, median-of-7 protocol.
- **Corpus fingerprints in perf results** (tools/perf_vs_winrar.py): the
  canonical corpus embeds the repo `src/` tree, so results.json now
  records per-corpus hashes — cross-era size comparisons are detectable
  instead of misleading.

### Changed

- **`-m5` match-finder walk depth 512 -> 128: 3.2x faster** (canonical
  85.2 -> 26.9 s) at +0.3% archive size. The full binary-tree finder was
  built, measured, and descoped per its pre-agreed gate (best variant:
  -8.9% time at 2.9x the cost of the depth cut) — the decision record,
  including the two measured finder corrections (body-insertion capping,
  best-length-capped probes), is docs/v1.31-implementation-plan.md M2.
  **`-m5` is now 1.38x faster than WinRAR 7.20** (v1.30: 2.3x slower);
  the m3 -> m5 effort slope drops from 9.7x to 3.3x.
- **`-m4` match-finder walk depth 128 -> 64: 1.6x faster** at +0.89%
  size, restoring the `-m3` -> `-m4` -> `-m5` effort ladder (8.4 s/54.14
  MB -> 16.2 s/53.41 MB -> 27.6 s/52.93 MB canonical) that the `-m5`
  change had collapsed.
- spec 03's encoder-heuristics table now notes that MaxChain/NiceLen/
  Lazy are reference-encoder values, not format; OpenRAR's shipped table
  lives in `Compressor50::init_match_params()`.

### Unchanged (honestly characterized)

- **`-m1` ratio (~17.6% behind WinRAR): structural.** The full sweep
  (chains 4/8/16 x nice 256/512 x lazy 0/1) found no candidate meeting
  "ratio gain at <= current runtime" — best: -1.63% size for +25% time.
  WinRAR's m1 advantage is its parse strategy, not effort; future-arc
  candidates recorded. `-m1`/`-m2`/`-m3` parameters and emitted bytes are
  unchanged from v1.30.4.

## [1.30.4] - 2026-09-28

### Fixed

- **WinRAR-created archives could list and extract incompletely**: the
  streaming scanner's QuickOpen fast-path (v1.8.0) trusted the archive's
  QO locator chain for entry enumeration; WinRAR 7.20's QO record for
  `-mt4` archives omits middle entries (all six zero-compressed files
  vanished from a 37-entry corpus). The chain is now validated for
  contiguity — each cached header must start exactly where the previous
  one ended — and any gap falls back to the authoritative linear scan
  (`qo_gap_fallback_tests` pins the fallback deterministically).
  Found by the new cross-extraction performance harness.

### Changed

- Sandboxed-worker hardening follow-ups: the Linux seccomp allowlist now
  permits `close` (FileStream dtors were getting EPERM on dup'd fds), and
  the Windows AppContainer worker runs under a kill-on-close Job Object
  with a 4 GiB memory cap (the v1.23 SFX containment pattern).

## [1.30.0] - 2026-09-27

OpenRAR 2.0 LTS: Enterprise Stability, Sandboxing & Universal SDKs — the
ABI-freeze arc. NO emitted RAR5 bytes change (Gate 0, docs/v1.30-pre-analysis.md
§0/§8: conditional approval + twelve directives; the 23-stage interop gate
and golden bytes ran unchanged through every milestone). Freeze
prerequisites 1–7 all landed with file:line or artifact evidence
(docs/v1.30-implementation-plan.md).

### Added

- **Sandboxed worker (SECURITY_ARCHITECTURE §5.1)** (`src/sandbox/`): the
  parse/decode engine runs in a sandboxed worker process — Windows
  AppContainer (per-run profile, empty capability set, CFG mitigation
  policy) / Linux seccomp-BPF allowlist (fail-closed install, memory
  family included, mmap/mprotect PROT_EXEC argument-filtered, RET_ERRNO
  denials) — while the broker holds every file handle and enforces every
  cap. The worker runs the SAME parser engine (no divergent parser);
  entry names never reach it (open_read_handle) and its only I/O is the
  channel pair + pread on the inherited volume. Proof-of-denial e2e per
  model (`sandbox_sandboxed_e2e_tests`: AppContainer write denial err=5;
  seccomp open/socket EPERM; volume grant usable; extract parity through
  the sandboxed worker; unsandboxed control). v1.29 directive met: TWO
  tested OS mechanisms.
- **Sandboxed `t`**: the test command runs through the worker where a
  model ships — byte-identical output and exit codes (pinned vs
  `--in-proc`); multi-volume sets and encrypted-with-password archives
  fall back in-process loudly (the interop gate's multivolume stage
  caught the missing scan-time refusal).
- **`--in-proc` switch + `OPENRAR_IN_PROC` env**: the in-process
  kill-switch (the OPENRAR_NO_MMAP pattern); the ONE decision function
  (`sandbox_mode_for`) consumes both.
- **Python & C# SDKs** (`bindings/`): `openrar` (PyPI layout, ctypes over
  the frozen C ABI — loader performs the ABI-version + feature-mask
  probes) and `OpenRAR.NET` (NuGet layout, P/Invoke); the normative
  cross-binding conformance suite runs as the `python_conformance` ctest
  gate against the built library (freeze prereq 3; the WASM v2 defects
  are the cautionary tale).
- **Attack regression corpus** (`attack_corpus_tests`): CVE-2025-8088
  traversal contained, CVE-2023-38831 spoofing exact-name extraction,
  CVE-2023-40477 hostile RR geometry refused, decompression-bomb caps
  fire mid-decode, hostile vint refused (freeze prereq 4).
- **Assurance legs**: ASan+UBSan and TSan PRIMARY CI legs; MSan
  best-effort (Linux/clang, non-gating, §7.2 revised); fuzz crash dedup
  (`tools/dedup_crashes.py`, signature buckets) + corpus persistence.
- **Supply chain** (freeze prereq 5): release SHA256SUMS, CycloneDX SBOM
  (syft), Sigstore keyless build-provenance attestations; release
  verification instructions in `SECURITY.md`.
- **ABI freeze enforcement** (freeze prereq 1; `docs/abi-freeze.md`):
  absolute-offset/alignof static_asserts for every public struct,
  `abi_layout_tests` (golden JSON + C-mode compile proof +
  feature-mask parity), `abi_export_parity` (58 exports ==
  canonical list), `js_error_mirror_parity` (JS CODE_MAP/union == C
  enum). The asserts caught a real frozen fact during the arc:
  `openrar_archive_info_t.recovery_size` is UNALIGNED at offset 12
  (pack(1), frozen since v1.6.0 — now documented).

### Changed

- **Library-mode containment made precise and testable** (§5.1, M2): the
  non-disableable floors (MAX_STREAM_OUTPUT, KDF lg2≤24, window caps,
  path containment) are pinned by named tests as unreachable by any
  embedder call; `set_limits`/ExtractionLimits documented as the
  CALLER's own budget layer (§7.3 embedder advisory normative in
  `docs/dll-integration-spec.md` §13 and `SECURITY.md`).
- **CLI switch debt closed** (M2.5, prereq 7): `-df`/`-dr`/`-dw` now
  parse and delete sources only after a fully successful batch write
  (Plain / Windows Recycle Bin / the documented wipe sequence; POSIX
  `-dr` degrades with a `W:`); `-tl`/`-tk[<date>]` apply the archive
  mtime on close (newest stored file / keep-original / UTC date). The
  README-vs-parser audit's debt set is now EMPTY. cv keeps its own
  verified-migration `-df` (the transcode suite caught the shadowing).

### Fixed

- `docs/dll-integration-spec.md` §5 error table synced with the shipped
  enum (`RAR_ERR_LIMIT_EXCEEDED` −15, the reserved −8/−10 slots).
- The assert-modal-dialog trap in new test suites: CRT assert routing
  now also covers _CRT_ERROR and disables the abort() WER dialog
  (`test_support.hpp`).

## [1.29.0] - 2026-09-27

Archive Migration & Transcoder - the split-surface arc: a new PARSED-byte
surface (ZIP / TAR / GZIP readers) with ZERO new emission code (Gate 0,
docs/v1.29-pre-analysis.md section 0/9 - the cv output is produced entirely
by the shipped writer pipeline; the interop gate and golden bytes ran
unchanged on every milestone, plus the new cv-vs-a differential control).
The ZIP CD-vs-LFH hardening (SECURITY_ARCHITECTURE 2.1) landed as a
RELEASE GATE: any mismatch outside the documented tolerated set aborts
with the structural exit code BEFORE any output exists.

### Added

- **RFC 1951 inflate** (src/compress/inflate.{hpp,cpp}): clean-room DEFLATE
  decoder serving ZIP method 8 and GZIP; strict RFC semantics (oversubscribed
  Huffman sets are hard errors, incomplete sets only for the documented
  distance special cases, invalid bit patterns never coerce to symbols);
  mandatory in-flight output cap wired to the caller's LimitState - no
  declared size is ever a bound (plan D5). Known-answer vectors from raw
  zlib streams (tests/unit/deflate_kats.inc) plus hand-crafted negatives;
  fuzz_inflate target for the nightly harness.
- **Foreign-format readers** (src/archive/foreign_*.cpp): one dispatch
  function (signatures first, TAR checksum probe last - plan D8); ZIP with
  EOCD/EOCD64/ZIP64, UTF-8+cp437 names, UT/NTFS timestamps (stream sub-blocks
  never restored, 4.3) and the CD-vs-LFH pre-flight comparison table (spec 11
  2.3); TAR with ustar + GNU long records + a documented pax subset (sparse
  refused per entry); GZIP with per-member CRC32+ISIZE and multi-member files
  migrating as ONE concatenated entry. Names flow through ONE composition
  pipeline (decode, slash normalization, percent-encode, sanitize_archive_path
  - plan D14); traversal shapes never reach an emitted archive.
- **`cv` - migrate a foreign archive into RAR 5.0**: staging pipeline reusing
  prepare_add_*/write_batch_add verbatim (CV-A1); archive-internal collision
  pre-check over the translated names (3.2); unconditional roundtrip verify
  (per-entry BLAKE2sp equality + count parity, D9); -df deletes the source
  only after a 100% verified migration with zero skips; -rr via the shipped
  RecoveryWriter pass; exit mapping per D1 (0 / 11 encrypted-refused /
  13 unparseable / 2 structural+io / 3 source CRC-truncation / 7 usage /
  10 nothing migrated, no output written); --json-summary schema v2 with the
  additive format/verified/source_deleted fields. Output-shaping switches
  (-s, -ts*, -v, -oi, -ep*, -z, ...) are refused with exit 7 (D7).
- **Interop gate Tracks 11-15**: cv ZIP/TAR/GZIP roundtrips verified against
  the UnRAR oracle byte-for-byte, the hostile-ZIP structural refusal (exit 2,
  no output), and 7z readability where installed (availability-gated).
- **New suites**: inflate_tests, foreign_format_tests, transcode_tests
  (34 ctest suites total).

### Changed

- **FILECOPY materialization is default-on** (rollover settlement, pre-analysis
  0.1): FHEXTRA_REDIR type 5 is an in-archive copy directive, not a filesystem
  link, so extraction no longer requires -ol (which keeps gating actual links:
  symlink/hardlink/junction types 1/2/4 stay default-deny). The v1.27 caps
  debit applies unchanged (archive_reader.cpp FILECOPY branch). A FILECOPY
  whose target cannot be materialized is now reported as a skipped entry with
  the filecopy_skipped flag instead of a silent fake success.
- **--json-summary schema v2** (plan D3): schema_version 2; x/e documents are
  field-identical to v1 (the transcode fields appear only on cv runs).

### Fixed

- **A failing MSVC test no longer opens modal dialogs**: the shared assert
  routing (tests/unit/test_support.hpp) also disables _CALL_REPORTFAULT and
  routes _CRT_ERROR to stderr - assert failures print and die under ctest.
- Deferred dir-meta mode assignment narrowed explicitly
  (archive_reader.cpp static_cast; MSVC /W4 C4244).

### Not implemented (truthful boundary)

- Foreign-format WRITE side (ZIP/TAR/GZIP emission) - deferred (CV-B).
- Legacy RAR (1.5-4.0) migration - DROPPED by user scope decision mid-arc;
  cv refuses the signature explicitly (the 5.3 VM boundary is provable by
  total absence).
- Foreign decryption (ZipCrypto refused by policy; AES-ZIP needs a SHA-1
  primitive) and cv -s/-v/-ts output shaping - see README Not yet.
## [1.28.0] - 2026-09-27

Interactive TUI & Benchmark Engine — the zero-format arc: NO emitted
archive bytes change (Gate 0, docs/v1.28-pre-analysis.md §0/§7 — the
`FHEXTRA_XATTR`/0x08 record set shipped in 1.27.0 stays closed; the
interop gate and golden bytes ran unchanged on every milestone). The arc
ships the SECURITY_ARCHITECTURE §7.1 terminal-injection RELEASE GATE, the
dual-progress TUI on top of it, the non-TTY degradation contract, and the
benchmark engine's reproducibility protocol.

### Added

- **Dual-progress TUI** (`src/cli/tui.hpp`): on TTYs, `x`/`e`/`t`/add render
  a width-clamped 4-line region — overall bar plus CURRENT-FILE bar with
  live within-file byte progress from the new reader disk hooks
  (`ArchiveReader::set_disk_hooks`: stored/compressed/encrypted/hardlink
  write loops). The renderer is a pure function (`render_tui(state,
  width)`) with UTF-8-safe truncation — a long name can no longer wrap and
  garble the redraw region (the shipped fixed-BAR_WIDTH renderer's latent
  bug). Single-writer rule: workers mutate state under one mutex and never
  render; 30 ms coalescing.
- **Interactive cancel**: ESC or `q` cancels cooperatively (raw-stdin
  keyboard thread, only when both streams are TTYs); `^C` maps to the same
  path via a handler installed ONLY inside TUI-active scopes — outside
  them, `^C` keeps the terminate semantics. Cancel = exit 255 (pinned
  user-break), `"aborted":true` in the JSON summary, remaining entries
  `unprocessed`, mid-file cancels abandon the temp (journal sweep stays
  exact). Terminal modes are restored RAII-style on every exit.
- **Benchmark engine v2** (`tests/bench/openrar_bench`): measurement
  protocol — 1 untimed warm-up pass + 7 timed passes (3 with `--quick`),
  result = median, spread = `(max−min)/median`; hardware disclosure (CPU
  brand, cores, OS, compiler) in output and JSON; `--json` prints ONLY the
  schema_version-1 document (humans move to stderr); `--strict` exits 1
  when a compute suite's spread exceeds `--spread-threshold-pct` (default
  5). New suites: `extract_throughput_store`/`_m3`, `add_throughput_m3`,
  `cdc_fingerprint`, and `cdc_three_number_store`/`_plain_solid`/
  `_cdc_packed` — the three-number reduction gate as a bench suite,
  packed through the CLI's batch pipeline (`CompressPlan` →
  `prepare_add_file` → `write_batch_add`); on the engineered corpus:
  store 16.78 MB > plain-solid 10.49 MB > CDC-packed 9.84 MB at spread
  0.0%. The 50 GB listing suites keep the v1.25 sparse harness (corpus
  build = the warm-up).
- **PROMPT state**: overwrite queries wipe the TUI region, park the
  keyboard thread and restore cooked stdin; the region redraws after the
  answer (fixes a shipped garble where the prompt printed inside the
  drawn region).

### Changed

- **Terminal-injection release gate (§7.1)**: every rendered name-shaped
  string passes the idempotent sanitizer AT RENDER — and the coverage
  audit's shipped gaps are closed: the archive comment (`l`) and
  `FHEXTRA_UOWNER` owner names (`lt`) rendered byte-raw until now;
  local-path `W:` lines (`cannot stat`/`cannot read`); the `p` command's
  error lines. Clean archives render identically; hostile bytes become
  `?` per category. VT capability now follows the SINK stream
  (`--json-summary` progress renders to stderr and probes stderr, not
  hardcoded stdout).
- **`--json-summary` output is always valid UTF-8** (RFC 8259): bytes of
  invalid sequences are neutralized as the ASCII `\uFFFD` escape (never
  the raw character); valid non-ASCII names pass through. The JSON still
  reports the ON-DISK name (the displayed ≡ extracted contract) — the
  terminal layer owns rendering safety, the JSON owns on-disk truth.
- **Non-TTY degradation is a pinned contract**: piped/redirected runs
  carry ZERO escape bytes, keep the per-file lines (stdout) and warnings
  (stderr), and exit identically to rendered runs for the same archive,
  flags and stdin state; `-q` silences human output; `-plain` never
  emits VT bytes.

### Fixed

- **The overwrite prompt garbled the progress region** (pre-existing):
  `ask_overwrite` printed inside the drawn 4-line block, corrupting the
  cursor-up redraw math for the rest of the run — the PROMPT state clears
  and redraws around it.
- **VT detection could disagree with the render sink** (pre-existing):
  capability was probed on stdout while `--json-summary` progress renders
  to stderr.

## [1.27.0] - 2026-09-26

Extended Attributes, Quarantine & MotW — the arc the Gate 0 format-legality
review cleared (docs/v1.27-pre-analysis.md §7): one new extra-record type
(`FHEXTRA_XATTR` 0x08, opt-in via `-ox`) that every stock RAR5 reader skips
without error, and a Mark-of-the-Web policy (`-oz`) that emits nothing into
the archive at all — propagation keys on the archive file's own transport
metadata with content generated locally (SECURITY_ARCHITECTURE §4.3). The
shipped Zone.Identifier-via-`-os` residue was closed in the same arc.

### Added

- **`FHEXTRA_XATTR` (0x08) extra record** (`src/format/headers.hpp`
  `ExtraType::Xattr`, `header_reader.cpp` parse branch, `header_writer.cpp`
  emission): `Size/Type/Flags(0)/Count` + per-attribute
  `NameLen/Name/ValueLen/Value` with opaque byte values. The type number was
  verified free against unrar `headers5.hpp` and bitplane rar-research;
  record bounds (name 255 B, value 64 KiB, count 4096, 1 MiB per file) are
  shared constants (`FHEXTRA_XATTR_*`) used by reader validation, writer
  emission and capture. ANY malformed record (flags != 0, length violations,
  count overrun, trailing bytes, duplicate names) falls back to the v1.24
  unknown-extra VERBATIM capture — never a partial parse, never an abort.
  Canonical re-serialization keeps mutation roundtrips byte-identical.
- **`-ox` — extended-attribute capture** (`src/io/posix_xattr.{hpp,cpp}`,
  `src/archive/archive_mutator.cpp` `apply_xattrs`/`xattr_capturable`,
  `prepare_add_file`/`prepare_add_dir`/`prepare_add_filecopy`): Linux/macOS
  xattr access via the no-follow l-variants with bounded probe-then-read
  loops (Windows/WASM compile to stubs — the format layer stays
  cross-platform). Namespace allow-list: `user.*`, `security.*`,
  `trusted.*`, `com.apple.metadata.*` (macOS Finder tags ride here);
  `system.*` (the ACL side door), quarantine/provenance namespaces and
  resource-fork metadata are never stored. Over-cap attributes are skipped
  whole, never truncated; attributes sort by name for deterministic record
  bytes. Symlinks/hardlinks carry no records (an extracted hardlink shares
  the master's inode); FILECOPY references capture their own source's.
- **Extended-attribute restore** (`src/archive/archive_reader.cpp`
  `xattr_restorable` + post-commit apply + `PendingDirMeta` deferred path,
  `--xattr-security`): `user.*`/`com.apple.metadata.*` restore by default on
  the committed path (reader-level, like modes/mtimes — DLL extraction
  inherits it); `security.*`/`trusted.*` only with the explicit
  `--xattr-security` admin opt-in (the `--preserve-suid` trust-decision
  model); everything else never. Per-attribute failures (EPERM/ENOTSUP) are
  fail-soft — extraction status unchanged.
- **`-oz` / `-oz-` — Mark-of-the-Web propagation** (`src/io/motw.{hpp,cpp}`,
  `src/cli/main.cpp` `restore_children`): default ON (the fail-safe
  direction; WinRAR 6.23+ parity), `-oz-` disables. The archive file's OWN
  `Zone.Identifier` ADS (Windows) or `com.apple.quarantine` (macOS) is
  probed once per session — no path heuristics, no browser data; unparseable
  provenance fails safe to zone 3. Each extracted file receives a freshly
  generated `[ZoneTransfer]
ZoneId=N
` mark — HostUrl/ReferrerUrl
  never travel (pinned byte-exact) — and an existing stronger mark on the
  target is never removed or downgraded. Files only; redirs excluded.
  Surfaced as the `motw_propagated` JSON security flag.
- **Interop gate Track 10** (`tools/interop_gate.py`): an archive crafted
  from docs/spec/01-headers.md in pure python carries a well-formed 0x08
  record (three namespaces) plus a genuinely unknown 0x42 record; OpenRAR
  and the local UnRAR oracle both extract the payload byte-identically —
  the spec's "unknown record types must be skipped without error" contract
  verified against a stock reader.

### Changed

- **Zone-stream policy closure** (`src/archive/archive_mutator.cpp` `-os`
  capture, `src/cli/main.cpp` STM restore): `-os` no longer stores
  `:Zone.Identifier` (provenance is not content), and extraction skips any
  zone-named STM child BEFORE reading its payload — case-insensitive,
  `:$DATA`-normalized — regardless of producer, including WinRAR `-os`
  archives. Skips surface as a `W:` line + the `zone_stream_skipped` JSON
  flag. WinRAR's "restore archive-provided zone if more secure than host"
  algorithm is deliberately not implemented: attacker-chosen zone content
  never reaches disk in either direction (documented divergence,
  docs/spec/07-services.md).
- **`-oz` is a behavior change by design**: archives that themselves carry a
  transport mark now mark their extracted files by default (the safe
  direction). Unmarked archives extract exactly as before; `-oz-` restores
  the old behavior.

### Fixed

- **POSIX `-ow` owner capture silently dropped FHEXTRA_OWNER for regular
  files** (`src/archive/archive_mutator.cpp` `prepare_add_file`): the owner
  fields were applied to the MOVED-FROM local block after
  `out.fb = std::move(fb)` — only the dir/symlink/hardlink/filecopy paths
  were ordered correctly. Now targets the live prepared block; pinned by
  `test_owner_capture_survives_prepared_move`.
- **FILECOPY materialization bypassed the cumulative extraction byte caps**
  (`src/archive/archive_reader.cpp` rtype==5): the §5.4 consistency gap with
  the hardlink EXDEV fallback — an `-oi`-heavy archive could materialize
  unbounded bytes outside `LimitState` accounting. The copy now debits; the
  zero-cap refusal is pinned by `test_filecopy_debits_caps`.
- **`-oi3`/`-oi4` exit paths ran the post-add `-rr` dispatch against the
  nonexistent archive** (`src/cli/main.cpp`): the dispatch now checks the
  archive exists (`test_oi34_no_archive_no_dispatch`).
- **`test_dir_metadata_deferred` left its read-only directory behind**
  (tests/unit/extraction_fidelity_tests.cpp): unlink inside a write-protected
  directory fails for the owner, so the leftover tree poisoned every later
  run sharing the temp path (reproduced 14/15 on rapid iteration over a
  persistent /tmp; fresh CI environments masked it since v1.24). The test
  re-opens the directory before cleanup.

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
