# PERFORMANCE.md — OpenRAR vs WinRAR benchmark summary (v1.31.0)

Machine-collected numbers from the protocol below. Every config ran on
deterministic corpora, was verified by cross-extraction (both engines'
extractions must hash-identically match the source tree), and reports the
MEDIAN of timed runs. Raw data: `tools/perf/results.json` (corpus
fingerprints included — the canonical corpus embeds the repo `src/` tree,
so sizes are only comparable within one corpus era); harness:
`tools/perf_vs_winrar.py`; micro-benchmarks (kernels, listing):
`openrar_bench`.

## Host & tools

| | |
|---|---|
| CPU | Intel Core i7-7500U @ 2.70 GHz (2C/4T, laptop) |
| RAM / OS | 15.9 GB / Windows 11 |
| OpenRAR | 1.31.0, MSVC Release (`build/openrar64/Release`) |
| WinRAR / UnRAR | 7.20 x64 / 7.20 x64 freeware |
| Date | 2026-09-29 |

**Protocol:** deterministic seeded corpora (fingerprints in
`results.json`; this era's canonical corpus hashes `3d7434a82d82e644` and
is NOT size-comparable to pre-v1.31 eras); per config 1 untimed warm-up
run + 3 timed runs (fresh output files each run, pre-deleted outside the
timed window); MEDIAN wall-clock reported; archive verified by extracting
with both engines and comparing SHA-256 against the source tree. WinRAR
invoked with `-inul -y`, OpenRAR with `-q`; identical switch names map
1:1 (`-mN`, `-mtN`, `-s`, `-v32m`). Single disk, quiescent host.

## Compression — method sweep (canonical: 135.5 MB = text + code + binary thirds, single-thread)

| Config | Median | Throughput | Archive | Ratio |
|---|---:|---:|---:|---:|
| OpenRAR `-m0 -mt1` | **0.11 s** | **1232 MB/s** | 135.55 MB | 100.0% |
| WinRAR `-m0 -mt1` | 0.22 s | 616 MB/s | 135.55 MB | 100.0% |
| OpenRAR `-m1 -mt1` | 3.16 s | 42.9 MB/s | 65.38 MB | 48.2% |
| WinRAR `-m1 -mt1` | **2.99 s** | **45.3 MB/s** | **55.60 MB** | **41.0%** |
| OpenRAR `-m3 -mt1` | **8.43 s** | **16.1 MB/s** | 54.14 MB | 40.0% |
| WinRAR `-m3 -mt1` | 16.47 s | 8.2 MB/s | **52.68 MB** | **38.9%** |
| OpenRAR `-m5 -mt1` | **27.63 s** | **4.9 MB/s** | 52.93 MB | 39.1% |
| WinRAR `-m5 -mt1` | 38.06 s | 3.6 MB/s | **51.40 MB** | **37.9%** |

- **m3 (default): OpenRAR is ~1.95x faster ST**; WinRAR's ratio is 2.7%
  tighter.
- **m5: OpenRAR is now 1.38x FASTER** (v1.30 was 2.3x slower — the M2/M3
  match-finder decision); WinRAR's ratio is 2.9% tighter.
- **m1: WinRAR wins on both axes** — slightly faster and 17.6% tighter
  (structural; see the gap notes below).

## Multi-threaded (m3)

**v1.32.0 re-measured this row, and the previous one was wrong.** The
v1.31.0 figures (5.72 s / 1.48x) were recorded while the MT path was
effectively disabled — an inverted enablement condition meant every
filter-free input, which is all of it, fell through to the sequential
encoder. The numbers below are from the fixed build.

### Canonical corpus, 2 physical cores / 4 logical

i7-7500U, Windows 11, min of 5 runs. This corpus's code third is deliberately
repetitive ("repeated to exercise long-range matches"), so it is the case that
used to break MT.

| Config | Best | Archive | Note |
|---|---:|---:|---|
| OpenRAR `-m3 -mt1` | 3.24 s | 20,794,925 | sequential |
| OpenRAR `-m3 -mt4` | 2.19 s | 20,794,925 | **1.48x**, byte-identical to `-mt1` |
| WinRAR `-m3 -mt1` | 4.90 s | 20,272,021 | |
| WinRAR `-m3 -mt4` | 2.43 s | 20,272,039 | 2.02x |

**MT output being byte-identical to `-mt1` is the intended result, not a
fallback.** With a member-scoped dictionary each chunk reproduces sequential's
blocks exactly, because chunk boundaries (4 MiB) are a multiple of the 512 KiB
block flush. So MT costs nothing in ratio here. The earlier "1.5x, not
parallel" reading was the redundancy probe declining MT, which the dictionary
fix made unnecessary.

### Where MT scales, and where it does not

| Input | `-mt1` | `-mt4` | Speedup | WinRAR `-mt4` | WinRAR speedup |
|---|---:|---:|---:|---:|---:|
| text third, 17 MB, no redundancy | 1.06 s | 0.62 s | 1.71x | 0.85 s | 1.99x |
| code third, 18 MB, highly compressible | 0.24 s | **0.35 s** | **0.69x** | 0.56 s | 1.54x |

We lead WinRAR on wall clock in every configuration (1.35x ST, 1.11x MT on
the corpus), but scale less well than it (1.48x vs 2.02x), and on highly
compressible input MT is a net loss — seeding costs more than the compression
it replaces there.

An optimisation to carry the match index across a worker's consecutive chunks
was built and measured, and recovered no measurable time. `docs/v1.32-pre-analysis.md`
section 8.8 records the numbers, the two reasons it did not pay, and the open
question of whether seeding is even the scaling limiter. Nothing is scheduled
for it.

### The known gap

On redundant input our MT loses matches that WinRAR's keeps — 428,242 B vs
3,855,663 B on the code third compressed alone, a 9x regression, against
WinRAR's +0.005%. This is a defect in our chunking, not an inherent RAR5
property. Bounded for corpus-shaped input by a redundancy probe, but not
completely; `test_parallel_probe_known_gap` pins the residual with its
measurement.

Root cause, measured by sweeping chunk size and nothing else (code third,
ST 428,242 B, WinRAR `-mt4` 396,331 B):

| our chunk size | our `-mt4` | vs our ST |
|---|---:|---:|
| 2 MiB (current cap) | 3,855,663 | +800% |
| 4 MiB | 2,139,969 | +400% |
| 8 MiB | 1,284,227 | +200% |
| 16 MiB | 856,975 | +100% |
| 24 MiB (one chunk) | 428,228 | 0% |

The loss was exactly proportional to the number of chunk boundaries, and is
now fixed: each MT context is seeded with the member bytes preceding its chunk
(`min(dictionary_size, member_offset) - 1`), indexed into the hash but never
emitted. On the code third, `-mt4` went from 3,855,663 B (+800% vs `-mt1`)
to 428,447 B (+0.05%); on the canonical corpus MT output is now
byte-identical to `-mt1`, because chunk boundaries (4 MiB) are a multiple of
the 512 KiB block flush, so each chunk reproduces sequential's blocks exactly.

The scaling cost is real: re-indexing the seed is O(seed) per chunk, so
clean-data scaling is 1.71x and on highly compressible input MT is a net loss
(0.69x on the code third) where WinRAR holds 1.54x. Two attempts to remove
that cost both failed to move the number, so whether seeding is even the
limiter is an open question — see `docs/v1.32-pre-analysis.md` section 8.8.
We still lead WinRAR on wall clock in every configuration (1.35x ST, 1.11x
MT on the corpus).

## Solid, multivolume (canonical, m3 ST)

| Config | Median | Archive |
|---|---:|---:|
| OpenRAR `-m3 -s -mt1` | **9.27 s** | 54.18 MB |
| WinRAR `-m3 -s -mt1` | 18.38 s | 52.79 MB |
| OpenRAR `-m3 -v32m -mt1` | **9.22 s** | 54.17 MB (2 parts) |
| WinRAR `-m3 -v32m -mt1` | 17.16 s | 52.79 MB (2 parts) |

Solid costs OpenRAR ~4% over plain m3 and WinRAR ~8% — parity in overhead.
**OpenRAR is ~2.0x (solid) and ~1.9x (volumes) faster.**

Multivolume sizes re-measured after the v1.37.4 window-default fix
(question-log Entry 24): the volume add path used to default the compressor
window to a flat 2 MiB for every method, which inflated this set to
62.90 MB (+19.15% vs WinRAR's 52.79 MB) — the corpus's generated-code
member packed 8.39 MB through `-v32m` where the non-volume path packs
0.45 MB. With the method-tuned defaults shared with the non-volume path,
the set lands at 54.17 MB (+2.61% vs WinRAR; slicing overhead only) and
code.cpp returns to 0.45 MB / 8 MiB dictionary. The same-session
non-volume rows are byte-identical to the pre-fix run, so the size change
is attributable to the volume-path fix alone; the window's match-search
cost shows within-run as volumes-vs-nonvolume median +2.3% -> +5.9%.

## Mixed multi-file corpus (153 MB, 37 files, `-m3 -mt4`)

| Config | Median | Archive | Ratio |
|---|---:|---:|---:|
| OpenRAR `-m3 -mt4` | **3.71 s** | 62.03 MB | 40.6% |
| WinRAR `-m3 -mt4` | 6.70 s | 60.19 MB | 39.4% |
| OpenRAR `-cdc -m3 -mt4` | 6.20 s | **41.90 MB** | **27.4%** |

The mixed corpus contains a duplicate pair (dedup target). OpenRAR's
`-cdc` (content-defined chunking + in-archive dedup) produces a **32%
smaller archive than plain `-m3 -mt4`** on this corpus — at ~67% higher
compression time. WinRAR has no in-archive dedup equivalent.

## Highly compressible data (zeros: 512 MB, `-m1 -mt1`)

| Config | Median | Throughput | Archive | Ratio |
|---|---:|---:|---:|---:|
| OpenRAR `-m1 -mt1` | **1.59 s** | **322 MB/s** | 0.026 MB | 0.005% |
| WinRAR `-m1 -mt1` | 5.51 s | 92.9 MB/s | 0.021 MB | 0.004% |

OpenRAR is ~3.5x faster; the v1.30 ratio gap (8x) is now **1.23x** — the
slot-257 run-collapse tokens (v1.31 M1) match the reference encoder's
mechanism.

## 1 GB canonical (`-m3 -mt4`)

| Config | Median | Throughput | Archive | Ratio |
|---|---:|---:|---:|---:|
| OpenRAR | **47.75 s** | **22.0 MB/s** | 437.64 MB | 40.7% |
| WinRAR | 69.32 s | 15.2 MB/s | 426.34 MB | 39.8% |

**OpenRAR is ~1.45x faster** on the full-gigabyte run.

## Extraction (canonical m3 archive, 135.5 MB → disk)

| Engine | Median | Throughput |
|---|---:|---:|
| UnRAR 7.20 | **0.31 s** | **437 MB/s** |
| WinRAR 7.20 (`rar x`) | 0.31 s | 437 MB/s |
| OpenRAR 1.31 (`x`) | 0.70 s | 194 MB/s |

Extraction is the one axis where OpenRAR trails the reference engines
(~2.3x slower on this workload). Cross-extraction parity is nonetheless
byte-exact in every direction (verified per config above and by the
24-stage interop gate).

### Decode-kernel arc (v1.38.0): flat tables and SIMD dispatch measured and declined

The roadmap's v1.38 premise — single-lookup 15-bit flat Huffman tables with
lazy second-level pages, then SIMD-assisted symbol dispatch — was tested in
Gate 0 (`tools/decode_kernel_probe.cpp`, hash-verified token-stream A/B
against the real kernel) and **measured false on this host**: the shipped
10-bit quick table + canonical slow path beats every flat variant on the
symbol-decode hot path (full-flat 17–45% slower; the lazy-page sweep
degrades monotonically with primary size — L1 working set, not branch
count, decides). The SIMD-dispatch family (gather, dual-speculative, GFNI)
sits on the same tables and the same serial chain and is declined with the
same evidence. The recorded v1.34 build-cost trap was also corrected by
measurement: a full 4×32768 span fill costs 24.8 µs/rebuild against the
shipped build's 10.1 µs (+5.6 ms per 64 MiB member), ~30x smaller than
projected — the lookup, not the build, was the blocker. Full record:
`docs/v1.38.0-pre-analysis.md`.

What shipped instead, measured (probe + paired-delta protocol, same
session, no-record replica row as the session-drift control):

| row (16 MiB members) | v1.37.4 | v1.38.0 | delta |
|---|---:|---:|---|
| phase-1 span decode, text m3 | 148.0 ms | 135.8 ms | **−8%** |
| phase-1 span decode, exelike m3 | 216.1 ms | 180.6 ms | **−17%** |
| phase-1 span decode, random m3 (stored) | 195.3 ms | 146.1 ms | **−25%** |
| sequential decode, text m3 (no records — control surface) | 148.5 ms | 147.0–149.0 ms | neutral |

(phase-1 record-emission slimming: pre-reserve from the span's packed size,
bulk 255-byte literal runs, hoisted record-cap counter —
`decode_span`, `src/compress/decompressor50.cpp`; the G5 cap fails on the
same token as before. The filter-queue head cursor —
`ApplyEngine::flush_pending` — replaces the per-region erase shift; measured
neutral at the tested scale, kept as the principled discipline. A
wrap-split memcpy filter-region read — 24x on its isolated micro-bench —
measured **+16% end-to-end** on exelike sequential and was declined; the
integrated-behavior reversal is recorded in the pre-analysis §6.)

Two-phase ratios re-measured with the slimmed workers (paired-delta,
min-of-15, alternating, 2C/4T; v1.37.2 record in parentheses):

| member | true `-mt4` | `-mt8` |
|---|---:|---:|
| Rar 7.20-made m3 -md2m | **1.36x** (1.18x) | 1.25x (1.17x) |
| Rar 7.20-made m3 -md128m | 1.27x (1.24x) | — |
| OpenRAR-made m3 -md2m | **1.45x** (1.25x) | — |

No-regression rows: stored 1.06x; `-mt1` parity 0.98x; small members
(4-block 0.91x, zeros 0.88x) read slightly below the v1.37.2 band with
absolute deltas of 6–34 ms on sub-300 ms operations. The serial
apply+flush+CRC+write stage remains the structural floor on this host; the
phase-1 win grows with core count.

Extraction rows, full-matrix re-record on the v1.38.0 release binary
(`tools/perf_vs_winrar.py` complete A–H run, 28 configs, cross-extraction
verification 28/28): OpenRAR ST 0.94 s / `-mt4` 0.76 s on our archive,
1.03 s / 0.66 s on the WinRAR-made archive; UnRAR 0.35 s ST; WinRAR
`-mt4` 0.31 s. Session-drift disclosure: the reference engines also read
~20% faster than the prior session in this run (UnRAR ST 0.43 → 0.35 s),
so cross-session deltas are not the claim — the durable claims are the
same-session paired measurements above. What the full matrix adds is the
byte-stability contract: **all 21 compression rows byte-identical to the
v1.37.4 run** (the encoder is untouched; archive sizes match to the byte),
and OpenRAR-vs-UnRAR relative position on ST extraction moved from ~3.4x
to ~2.7x behind in like-for-like sessions.

### Two-phase parallel decode (v1.37.0; plumbing corrected in v1.37.2)

Single-member extraction engages the two-phase driver when `-mtN` (N >= 2)
is requested — or by default with 2 workers when `-mt` is unset — and the
member clears the gating rules: symbol spans decode on worker threads
while one applier applies records to the window through the same engine
as the sequential decoder (byte- AND chunk-identical output). Paired-delta
protocol (min-of-15, alternating order, 2C/4T laptop), 64 MiB text
members:

| member | sequential | parallel (2 workers) | ratio |
|---|---:|---:|---:|
| Rar 7.20-made m3 -md2m | 786-919 ms | 625-760 ms | **1.21-1.26x** |
| Rar 7.20-made m3 -md128m | 976-1008 ms | 723-747 ms | **1.35x** |
| OpenRAR-made m3 -md2m | 819-1169 ms | 660-928 ms | **1.14-1.26x** |

v1.37.2 correction: the shipped v1.37.0 never actually received the CLI's
`-mt` on decode — the requested count was re-resolved to the default
inside the driver, and the measurement env override could not raise the
count either — so the table above, measured with the v1.37.0 build, is
the **2-worker (default) configuration**, now relabeled. The same protocol
re-run on the fixed build, at the counts the CLI actually requested:

| member | true `-mt4` | true `-mt8` |
|---|---:|---:|
| Rar 7.20-made m3 -md2m | 1.18x | 1.17x |
| Rar 7.20-made m3 -md128m | 1.24x | — |
| OpenRAR-made m3 -md2m | 1.25x | — |

**The default of 2 workers is the best configuration on this 2C/4T
host**: symbol workers beyond the second hyperthread deepen contention
with the serial applier (D9 stands as measured).

No-regression rows (4-block member, stored, zeros, `-mt1`): 0.96-1.09x.
The serial apply+flush+CRC+write stage (0.55-0.62 of sequential on this
host) is the structural floor; the win grows with core count. Full record:
`docs/v1.37.0-two-phase-implementation-plan.md` M4 + the v1.37.2
correction addendum.

## Highlights

- **Compression at the default `-m3`: OpenRAR is 1.3–2.0x faster than
  WinRAR 7.20** across single-file, multi-file, solid, multivolume, and
  1 GB workloads, with ratios within ~3%.
- **`-m5`: now FASTER than WinRAR (1.38x)** — v1.30 was 2.3x slower. The
  fix was the match-finder walk depth (512 → 128), found by measurement
  after a full binary-tree finder iteration missed its gate
  (docs/v1.31-implementation-plan.md M2 records the decision).
- **Zero-run / RLE-heavy data**: 3.5x faster than WinRAR, ratio within
  1.23x (v1.30 was 8x looser) — slot-257 repeat-last-length emission.
- **`-m0` store: 1.9x faster.**
- **OpenRAR's `-cdc` dedup cuts a duplicate-heavy corpus by 32% vs its
  own `-m3` archive — no WinRAR equivalent.**
- **Honest losses:** WinRAR's `-m1` is slightly faster and 17.6% tighter
  (structural — swept the full chain/nice/lazy grid in v1.31 M3; the gap
  is their m1 parse strategy, not effort — see
  `docs/v1.31-implementation-plan.md` M3); our m5 ratio is 2.9% looser;
  extraction trails UnRAR/WinRAR by ~2.3x on this workload (the v1.24
  crash-safety contract, below).
- Every archive in every config was verified by BOTH engines' extractors
  producing hash-identical trees — speed differences never trade away
  interop.

## Where the remaining gaps live (root-caused)

Four losses were investigated; one is deliberate/architectural, one is
structural, two were FIXED in v1.31:

- **Extraction (~2.3x vs UnRAR): the crash-safety contract.** Every
  extracted file is written to a temp file, `FlushFileBuffers`-ed,
  journaled (the journal record itself fsynced before the temp exists),
  and atomically renamed — the v1.24 containment/durability architecture.
  UnRAR and WinRAR do none of that (write + close). We keep the
  durability guarantee; an opt-in fast path would be a
  security-architecture decision, not a perf tweak.
  **[v1.39.0] Shipped as opt-in:** `--durability=batch` / `-db` defers
  the journal record sync to a boundary (every 32 appends, LRU eviction,
  session teardown) and elides the temp-data flush — data durability is
  the OS writeback's (reference-tool class). Measured (v1.34 §3 protocol,
  min-of-4, one session): stored sequential 1.99x, stored parallel 1.64x,
  text sequential 1.18x, text parallel 1.05x. Default-off; kill switch
  `OPENRAR_NO_BATCH_DURABILITY=1`. The contract revision is normative in
  SECURITY_ARCHITECTURE §3.3.
- **m1 ratio (~17.6% vs WinRAR): structural.** The full v1.31 M3 grid
  (chains 4/8/16 × nice 256/512 × lazy 0/1) found no candidate meeting
  the "ratio gain at ≤ current runtime" constraint — the best bought
  −1.63% size for +25% time. WinRAR's m1 advantage comes from its parse
  strategy (aggressive rep-match reuse), not effort. Future-arc
  candidates: lazy rep-match parsing, 2-byte matches.
- **[FIXED in v1.31] m5 speed (was 2.3x slower, now 1.38x faster):** the
  512-deep chain walk was the entire gap. The binary-tree finder was
  built, measured, and descoped per its pre-agreed gate; walk depth 128
  delivered 3.2x on its own (M2 decision record:
  docs/v1.31-implementation-plan.md).
- **[FIXED in v1.31] zeros/RLE ratio (was 8x looser, now 1.23x):** the
  encoder never emitted LD slot 257 (repeat-last-length) — the 1-bit
  continuation token the reference encoder uses ~16,000 times in a 64 MiB
  zeros archive. Root cause, evidence, and the win: M0/M1 in
  docs/v1.31-pre-analysis.md.

## Reproducing

```
python tools/perf_vs_winrar.py          # full matrix (~22 min on the host above)
python tools/perf_vs_winrar.py --quick  # 1 warm-up + 1 timed run (smoke)
```

Requires WinRAR 7.x (`C:\Program Files\WinRAR\rar.exe`) and a Release
build. Results land in `tools/perf/results.json` with per-corpus
fingerprints (the canonical corpus embeds the repo `src/` tree — sizes
are only comparable within one corpus era). `openrar_bench` carries the
kernel-level suites (SIMD folds, match length, m5 match finder, listing,
CDC reduction) with the median-of-7 protocol and hardware disclosure.
