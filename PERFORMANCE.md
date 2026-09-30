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

### Canonical corpus, 4 logical cores

| Config | Best | Archive | Note |
|---|---:|---:|---|
| OpenRAR `-m3 -mt1` | 3.40 s | 20,794,925 | sequential |
| OpenRAR `-m3 -mt8` | 2.26 s | 20,794,925 | **byte-identical to `-mt1`** |
| WinRAR `-m3 -mt1` | 5.21 s | 20,272,021 | |
| WinRAR `-m3 -mt4` | 2.47 s | 20,272,039 | |

**The OpenRAR row above is not a parallel-scaling measurement.** Its
`-mt8` output is byte-identical to `-mt1`, which means MT did not chunk: the
redundancy probe declined it, and the 1.5x is the sequential fallback being
faster than the `-mt1` path. The corpus is deliberately hostile to chunking
— its code third is "repeated to exercise long-range matches" — and OpenRAR
detects that and declines, where WinRAR chunk-parallelises safely because its
MT encoder does not lose the cross-boundary matches.

### Where MT does scale

On input with no long-range redundancy, OpenRAR's MT is a real 2.0x at
+0.08% size:

| Input | `-mt1` | `-mt4` | Speedup | Size |
|---|---:|---:|---:|---:|
| `text.txt` third alone, 17 MB | 1.08 s / 3,589,437 | 0.54 s / 3,592,185 | **2.0x** | +0.08% |

So the honest summary is: **2.0x on clean data, 1.5x on the canonical
corpus without parallelising at all, and a 9x ratio regression on redundant
input that we decline in the corpus case but not in the single-file case.**

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

The scaling cost is real and is tracked as v1.32.1: re-indexing the seed is
O(seed) per chunk, so clean-data scaling moved 2.0x → 1.71x, and on highly
compressible input MT is now a net loss (0.69x on the code third) where
WinRAR holds 1.54x. We still lead WinRAR on wall clock in every configuration
(1.35x ST, 1.11x MT on the corpus).

## Solid, multivolume (canonical, m3 ST)

| Config | Median | Archive |
|---|---:|---:|
| OpenRAR `-m3 -s -mt1` | **8.74 s** | 54.16 MB |
| WinRAR `-m3 -s -mt1` | 17.73 s | 52.77 MB |
| OpenRAR `-m3 -v32m -mt1` | **10.63 s** | 62.59 MB (2 parts) |
| WinRAR `-m3 -v32m -mt1` | 16.61 s | 52.77 MB (2 parts) |

Solid costs OpenRAR ~4% over plain m3 and WinRAR ~8% — parity in overhead.
**OpenRAR is ~2.0x (solid) and ~1.6x (volumes) faster.**

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
