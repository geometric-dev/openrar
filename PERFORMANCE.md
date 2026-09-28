# PERFORMANCE.md — OpenRAR vs WinRAR benchmark summary (v1.30.0)

Machine-collected numbers from the protocol below. Every config ran on
deterministic corpora, was verified by cross-extraction (both engines'
extractions must hash-identically match the source tree), and reports the
MEDIAN of timed runs. Raw data: `tools/perf/results.json`; harness:
`tools/perf_vs_winrar.py`; micro-benchmarks (kernels, listing): `openrar_bench`.

## Host & tools

| | |
|---|---|
| CPU | Intel Core i7-7500U @ 2.70 GHz (2C/4T, laptop) |
| RAM / OS | 15.9 GB / Windows 11 |
| OpenRAR | 1.30.0, MSVC Release (`build/openrar64/Release`) |
| WinRAR / UnRAR | 7.20 x64 / 7.20 x64 freeware |
| Date | 2026-09-28 |

**Protocol:** deterministic seeded corpora; per config 1 untimed warm-up run
+ 3 timed runs (fresh output files each run, pre-deleted outside the timed
window); MEDIAN wall-clock reported; archive verified by extracting with
both engines and comparing SHA-256 against the source tree. WinRAR invoked
with `-inul -y`, OpenRAR with `-q`; identical switch names map 1:1
(`-mN`, `-mtN`, `-s`, `-v32m`). Single disk, quiescent host.

## Compression — method sweep (canonical: 128.9 MB = text + code + binary thirds, single-thread)

| Config | Median | Throughput | Archive | Ratio |
|---|---:|---:|---:|---:|
| OpenRAR `-m0 -mt1` | **0.13 s** | **992 MB/s** | 128.94 MB | 100.0% |
| WinRAR `-m0 -mt1` | 0.23 s | 560 MB/s | 128.94 MB | 100.0% |
| OpenRAR `-m1 -mt1` | 3.43 s | 37.6 MB/s | 62.26 MB | 48.3% |
| WinRAR `-m1 -mt1` | **3.20 s** | **40.3 MB/s** | **53.01 MB** | **41.1%** |
| OpenRAR `-m3 -mt1` | **8.78 s** | **14.7 MB/s** | 51.64 MB | 40.1% |
| WinRAR `-m3 -mt1` | 16.45 s | 7.8 MB/s | **50.24 MB** | **39.0%** |
| OpenRAR `-m5 -mt1` | 85.61 s | 1.5 MB/s | 50.32 MB | 39.0% |
| WinRAR `-m5 -mt1` | **37.60 s** | **3.4 MB/s** | **49.02 MB** | **38.0%** |

- **m3 (default): OpenRAR is ~1.9x faster ST**; WinRAR's ratio is 2.7% tighter.
- **m1: WinRAR wins on both axes** — slightly faster and 14% smaller.
- **m5: WinRAR is ~2.3x faster with a slightly tighter ratio.** OpenRAR's m5
  path is the weakest config in this matrix.

## Multi-threaded (m3, `-mt4` = all 4 logical cores)

| Config | Median | Throughput | Archive |
|---|---:|---:|---:|
| OpenRAR `-m3 -mt4` | **5.75 s** | **22.4 MB/s** | 51.64 MB |
| WinRAR `-m3 -mt4` | 8.22 s | 15.7 MB/s | 50.24 MB |

OpenRAR scales 1.53x from ST→MT here; WinRAR scales 2.0x but from a slower
base. **OpenRAR is ~1.4x faster MT.**

## Solid, multivolume (canonical, m3 ST)

| Config | Median | Archive |
|---|---:|---:|
| OpenRAR `-m3 -s -mt1` | **9.33 s** | 51.66 MB |
| WinRAR `-m3 -s -mt1` | 17.39 s | 50.32 MB |
| OpenRAR `-m3 -v32m -mt1` | **6.91 s** | 51.64 MB (4 parts) |
| WinRAR `-m3 -v32m -mt1` | 16.53 s | 50.32 MB (4 parts) |

Solid costs OpenRAR ~6% over plain m3 and WinRAR ~6% — parity in overhead.
Multivolume adds ~nothing for either engine. OpenRAR is ~1.9x (solid) and
~2.4x (volumes) faster.

## Mixed multi-file corpus (153 MB, 37 files, `-m3 -mt4`)

| Config | Median | Archive | Ratio |
|---|---:|---:|---:|
| OpenRAR `-m3 -mt4` | **3.90 s** | 59.16 MB | 38.7% |
| WinRAR `-m3 -mt4` | 7.27 s | 57.40 MB | 37.5% |
| OpenRAR `-cdc -m3 -mt4` | 6.47 s | **39.56 MB** | **25.9%** |

The mixed corpus contains a duplicate pair (dedup target). OpenRAR's
`-cdc` (content-defined chunking + in-archive dedup) produces a **33%
smaller archive than plain `-m3 -mt4`** on this corpus — at ~66% higher
compression time. WinRAR has no in-archive dedup equivalent.

## Highly compressible data (zeros: 512 MB, `-m1 -mt1`)

| Config | Median | Throughput | Archive | Ratio |
|---|---:|---:|---:|---:|
| OpenRAR `-m1 -mt1` | **1.69 s** | **303 MB/s** | 0.18 MB | 0.035% |
| WinRAR `-m1 -mt1` | 5.59 s | 91.6 MB/s | **0.02 MB** | **0.004%** |

OpenRAR is ~3.3x faster; WinRAR's m1 encoder collapses zero runs ~8x smaller
(different repeat-run encoding).

## 1 GB canonical (`-m3 -mt4`)

| Config | Median | Throughput | Archive | Ratio |
|---|---:|---:|---:|---:|
| OpenRAR | **47.78 s** | **21.5 MB/s** | 417.47 MB | 40.7% |
| WinRAR | 70.55 s | 14.5 MB/s | 406.59 MB | 39.7% |

**OpenRAR is ~1.5x faster** on the full-gigabyte run (the earlier README
claim of 4x at 1 GB was measured single-threaded vs WinRAR single-threaded
with a min-of-2 protocol — the honest MT number is 1.5x).

## Extraction (canonical m3 archive, 128.9 MB → disk)

| Engine | Median | Throughput |
|---|---:|---:|
| UnRAR 7.20 | **0.31 s** | **416 MB/s** |
| WinRAR 7.20 (`rar x`) | 0.32 s | 403 MB/s |
| OpenRAR 1.30 (`x`) | 0.50 s | 258 MB/s |

Extraction is the one axis where OpenRAR trails the reference engines
(~1.6x slower on this workload). Cross-extraction parity is nonetheless
byte-exact in every direction (verified per config above and by the 23-stage
interop gate).

## Highlights

- **Compression m3 (the default): OpenRAR is 1.4–1.9x faster than WinRAR 7.20**
  across single-file, multi-file, solid, multivolume, and 1 GB workloads,
  with ratios within 3% of WinRAR's.
- **Store (m0): 1.8x faster.**
- **OpenRAR's `-cdc` dedup cuts a duplicate-heavy corpus by 33% vs its own
  m3 archive — no WinRAR equivalent.**
- **Honest losses:** WinRAR's m1 is slightly faster and 14% tighter;
  WinRAR's m5 is ~2.3x faster; WinRAR's zero-run encoding is ~8x tighter
  at m1; extraction trails UnRAR/WinRAR by ~1.6x on this workload.
- Every archive in every config was verified by BOTH engines' extractors
  producing hash-identical trees — speed differences never trade away
  interop.

## Where the remaining gaps live (root-caused)

Each loss above was investigated; three are deliberate or architectural,
one is a tuning candidate:

- **Extraction (~1.6x vs UnRAR): the crash-safety contract.** Every
  extracted file is written to a temp file, `FlushFileBuffers`-ed, journaled
  (the journal record itself fsynced before the temp exists), and atomically
  renamed — the v1.24 containment/durability architecture. UnRAR and WinRAR
  do none of that (write + close). Attribution measured: extracting 3×43 MB
  takes 0.55 s with the contract and ~0.07 s for 37 smaller files whose
  flushes stay in the NVMe cache; UnRAR does the same 129 MB in 0.035 s by
  never syncing. We keep the durability guarantee; an opt-in fast path
  would be a security-architecture decision, not a perf tweak.
- **m5 (~2.3x vs WinRAR): match-finder architecture.** OpenRAR uses hash
  chains with per-method depth (m5 = 512-deep + lazy evaluation). WinRAR's
  high-effort encoder keeps more candidate structure per position. The
  9.7x m3→m5 effort slope (vs WinRAR's 2.3x) says the chain walk cost
  dominates at high depth — the known fix is a binary-tree/suffix-structure
  match finder, a self-contained codec project (decoder unaffected).
- **m1/zeros ratio: match-length encoding cap.** OpenRAR's encoder caps
  match tokens at 4097 bytes; RAR5's length field allows ~64 KB, which is
  how WinRAR collapses a 512 MB zero file to 0.02 MB (≈7,800 long-match
  tokens vs our ≈125,000). Extending the encoder to long-length encoding
  changes every emitted archive's bitstream and needs a format-legality
  review (Gate 0) — the decoder already handles such streams (WinRAR's
  zeros archive extracts correctly). Candidate for a future compression arc.
- **m1 speed/ratio tuning:** `-m1` parameters (chain 4, nice 256) are
  greedier than WinRAR's fastest preset. Re-tuning is safe (no format
  change) but needs a full ratio/speed matrix re-run; deferred as minor.

## Reproducing

```
python tools/perf_vs_winrar.py          # full matrix (~26 min on the host above)
python tools/perf_vs_winrar.py --quick  # 1 warm-up + 1 timed run (smoke)
```

Requires WinRAR 7.x (`C:\Program Files\WinRAR\rar.exe`) and a Release build.
Results land in `tools/perf/results.json`. `openrar_bench` carries the
kernel-level suites (SIMD folds, match length, listing, CDC reduction) with
the median-of-7 protocol and hardware disclosure.
