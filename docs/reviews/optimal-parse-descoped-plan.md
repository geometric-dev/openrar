<!-- DESCOPED. This is the v1.32.0-optimal-parse Gate 0 record, preserved
     because its analysis is still partly valid and because the arc's
     negative result must not be re-derived from scratch. The arc was
     BUILT, MEASURED, FAILED T5/T6/T1 and REMOVED; the v1.32.0 version
     number was re-purposed for MT encoder scaling. See the "Descoped" 
     section of docs/ROADMAP.md for the outcome, the measured gate
     numbers, and the structural finding, and branch
     arc/v1.32.0-optimal-parse-parked for the parked engine (not for
     merge). Do not treat anything below as the v1.32.0 scope. -->

# v1.32.0 Implementation Plan — Ratio Parity: Optimal Token Parse (Gate 0 Cleared)

> Status: conditional approval (docs/optimal-parse-descoped-analysis.md §8, six
> directives applied). Base: v1.31.0 @ 5f6b717. Binding scope; the greedy
> path is retained verbatim as the byte-identical release valve.

## 1. Design

**Rolling forward DP per block, m3 first-class, m1 experimental, greedy
valve.** All parse work lives inside `process_available` (the shared
loop): when the per-method DP flag is on (table value in
`init_match_params()` — the one decision function), the block's token
choices come from the DP; when off, the code path is byte-for-byte
v1.31.0's (the greedy branch is untouched — T8 pins it).

- **Window:** the block quantum (flush at 32768 tokens / 512 KiB input,
  `need_flush()`). Forward relax over positions [block_start, block_end)
  with `price[i]` (u32, bits × 256) and `edge[i]` (u32 packed: type 3 b /
  len 12 b / dist-or-rep 17 b ≈ 4 MB per 512 KiB window; arrays sized
  once per session). Block end is input-derived — identical in one-shot
  and streaming (the 512 KiB look-ahead invariant already aligns them).
- **Edges per position, fixed order:** literal → rep0..3 (priced from the
  current state's distances; rep i moves slot i to front) → the
  chain-found match (all lengths priced from the one (len, dist) via the
  length-slot curve) → 257 (only when the exact-continuation condition
  holds against the CURRENT state; target state == source state — 257
  mutates nothing). Relaxation order fixed for deterministic ties.
- **Prices:** from the previous block's Huffman code lengths
  (`len_ld_/len_dd_/len_ldd_/len_rd_`) + fixed extra-bit widths,
  integer bits × 256. First block: greedy warm-up while tables build
  (T8 scope includes it). Update rule: replace with the emitted block's
  tables; λ=0.5 blend fallback if T5 shows cross-corpus instability
  (pre-analysis §8 directive 3).
- **State:** single-track — the state at each position is the one the
  recorded best path produced; emission replays the recorded edges so
  every state re-derives identically (correctness wall, pre-analysis §8).
- **Emission:** back-pointer walk from the block end; each chosen edge
  goes through the EXISTING token emitters (add_literal / add_match /
  add_rep / the 257 branch with its byte-verification and filter-region
  bounds re-checked — a DP edge that fails re-verification degrades to
  the greedy decision at that position, never to an unverified token).

## 2. Constraints & interactions (engine trace)

| Engine | DP behavior |
|---|---|
| One-shot `compress()` | DP per block, windows input-derived |
| Streaming `feed`/`finish_stream` | same loop; the 512 KiB look-ahead gate aligns windows (T3) |
| Solid carry (`begin_stream(continue_window)`) | DP state seeded from the carried `(old_dist_, last_length_)`; price tables carried (T4) |
| `compress_buffer_parallel` | chunk-local sessions — DP per chunk, deterministic per chunk geometry (T10) |
| WASM block codec | DP code ships in the artifact; ≤ 500 KiB size gate re-runs in M1; material delta → wasm defaults greedy via compile-time flag (directive 1) |

m3 non-regression (standing): m3 ≤ 12 s canonical (the 2x headroom minus
margin) and size ≤ greedy's — else the m3 gate flips to greedy for the
release (the valve). v1.31-regression bounds: zeros artifact ≤ 3,514 B
(T1); m5 untouched (out of scope); m1 bounded by its own 3.5 s gate (M2).

## 3. Failure-Mode Matrix

| New path | Failure mode | Behavior |
|---|---|---|
| DP parse | mispriced edges → ratio regression | T5 per-corpus A/B gate (DP ≤ greedy, else flip); the valve |
| DP parse | price oscillation across blocks | determinism holds (same input → same bytes); λ=0.5 blend fallback pre-agreed |
| DP parse | 257 edge priced but unverifiable at emission | emitter re-checks M1 invariants; degrades to greedy decision at that position (T1) |
| DP parse | filter region clamps an edge mid-window | edges respect `cur_max_lz` exactly as greedy (T2) |
| DP parse | window shorter than expected (EOF mid-block) | back-walk terminates at the last relaxed position (final-block semantics, T3) |
| Fallback valve | gate-off output drifts from v1.31.0 | T8 byte-identity, all blocks incl. warm-up |
| m1 experiment | time blowup | 3.5 s hard gate; honest-negative branch ships the reason (pre-committed) |
| WASM | code-size budget | M1 gate; compile-time greedy default escape hatch |

## 4. CLI / tool surface

None. No switches, no exit-code changes, no ABI surface change (the DP
is private to `Compressor50`; enablement is a table value).

## 5. Milestones

- **M1 — rolling DP engine, m3 only:** price tables + forward relax +
  back-pointer emission behind the m3 table flag; greedy warm-up block;
  WASM size gate; tests T1-T4, T8-T10; the T5 ratio A/B and T6 speed
  bound as the milestone gate. Commit.
- **M2 — m1 experiment:** the lean m1 DP (chain-4 edge set); the 3.5 s
  gate with the PRE-COMMITTED honest-negative branch (ship greedy + the
  measured reason if missed). Commit.
- **M3 — matrix + finals:** DP knob sweep (if any: price quantization,
  m3 window variants) on the full matrix; per-method finals; m5
  re-opened ONLY if m3's ratio gain ≥ 2% (evidence-gated). Commit.
- **M4 — docs + release v1.32.0:** PERFORMANCE.md ratio rows, spec 03
  parser note (heuristics, wire-invisible), CHANGELOG, version
  consistency set, preflight, tag last.

## 6. Named Negative Tests

- **T1 `dp_zeros_no_regression`:** zeros 64 MiB artifact ≤ 3,514 B under
  the DP (the 257 chain must survive DP pricing).
- **T2 `dp_filter_region_boundary`:** the E8 boundary corpus (fuzz-727
  class) through the DP, byte-exact.
- **T3 `dp_streaming_identity`:** adversarial feed sizes byte-identical
  to one-shot, incl. final-block semantics.
- **T4 `dp_solid_carry_seed`:** solid chain with the DP seeded from the
  carried state; roundtrip + carry parity.
- **T5 `dp_vs_greedy_ratio_gate`:** per-corpus (canonical, mixed,
  text-heavy) DP ≤ greedy size; the m3 flip trigger.
- **T6 `dp_m3_speed_bound`:** m3 ≤ 12 s canonical.
- **T7 `dp_m1_speed_bound`:** m1 ≤ 3.5 s canonical (M2; honest-negative
  pre-committed).
- **T8 `greedy_fallback_byte_identity`:** gate off == v1.31.0 bytes at
  every method, all blocks (the release valve).
- **T9 `dp_cross_interop`:** DP-emitted streams decode under UnRAR +
  WinRAR (Track 17 extension).
- **T10 `dp_parallel_identity`:** parallel chunk path deterministic per
  chunk geometry with the DP on.

## 7. Definition of Done

m3 ratio ≤ greedy on {canonical, mixed, text-heavy} with m3 ≤ 12 s (else
the valve ships greedy and the arc records the negative result); zeros +
m5 + m1 bounds held (T1/T7, m5 out of scope); every DP stream
cross-engine decoded; gate-off byte-identity pinned; ctest 45/45 + new
suites on MSVC Debug, WSL fresh-mirror -Werror, clang-format; WASM size
≤ 500 KiB (or documented greedy default); docs (spec 03 note,
PERFORMANCE.md, CHANGELOG) in-arc; release v1.32.0 per the versioning
protocol.