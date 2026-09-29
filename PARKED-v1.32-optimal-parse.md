# PARKED — v1.32.0 optimal parse (cost-model token selection)

**Not for merge.** This branch is an inspection artifact for an arc that was
built, measured, failed its gates, and was removed from the main line. The
v1.32.0 version number was re-purposed for MT encoder scaling.

Read the "Descoped" section of `docs/ROADMAP.md` for the full outcome record
and the structural finding. Do not treat anything here as v1.32.0 scope.

## What this is

A rolling forward dynamic program over the token vocabulary
`{literal, match, rep0..3, 257}` (Design P-A in the preserved Gate 0 docs):
price tables carried from the previous block, a forward relax over window
positions, packed back-pointer edges, and a back-walk emission through the
existing token emitters with live re-verification of every state-dependent
token.

The DP is gated to m3 by `dp_should_enable()` — one table value, consumed by
both `begin_archive` (sizing) and `init_match_params` (the flag).

## State: fixes applied, correctness partly unverified

The defects listed in the earlier parked commit have been **fixed** in this
branch's history:

- window reopen moved inside the scan loop (a mid-loop close, or a close
  followed by a streaming-defer return, previously never reopened);
- the DP scan now inserts positions into the match hash (this was the entire
  ratio failure);
- `dp_sym_bits()` prices a zero-length Huffman code as the table maximum —
  a 0-length code means *symbol unused*, not *free*;
- `dp_len_ladder()` replaces the O(n · MAX_LZ_MATCH) every-length relax
  (8 MiB of zeros: 22 min → ~30 s), strictly increasing so it cannot cycle;
- the LDD distance symbol is gated on `raw_bits >= 4`, not `dslot >= 4`;
- a DP window is no longer a RAR block (this doubled the block count, and the
  Huffman tables, on run-heavy input);
- `dp_win_cap_` is clamped to `min(DP_MAX_WIN, win_size_)`;
- `dp_win_open_` is cleared in `reset_state`;
- `dp_state_` is `u32` (20 B/entry, not 40) with a `max_dist_ <= 2^32-1`
  enablement guard.

**Verified:** the four gate corpora roundtrip byte-exact and reproduce the
measured numbers (code -1.95%, text -1.39%, random tie, runs +28%). The
filter multi-block roundtrip (`test_filter_multi_token_multiblock_roundtrip`
reproduction, 1228800 B → 1153 B) passes with the DP on.

**NOT verified:** the full `ctest 45/45`. `compress_tests` reported a failure
that is *not* the filter case above, and it was not isolated — the host was
under load and the suite is ~40 min. **Anyone reviving this must isolate that
failure first.** It is the one open question about this branch's correctness.

## Why it still ships nowhere

Release build, m3, against greedy:

| | DP | greedy | gate |
|---|---:|---:|---|
| code | -1.95% | — | T5 passes |
| text | -1.39% | — | T5 passes |
| runs/zeros | +28% | — | **T1 fails** |
| time | ~3.9x | — | **T6 fails** (budget 1.42x) |

A chain-depth sweep (4/8/16/32) clears neither failing gate: the ratio win
shrinks toward zero as the chain shrinks, and the runs regression is
depth-independent.

## The finding worth keeping

A cost-model parse over this match finder must relax an edge out of **every**
position, so it spends ~4x the finder budget that greedy/lazy parsing spends.
Greedy is not merely a cheaper approximation of an optimal parse — it is
*finder-frugal*, searching only at token starts because a match skips its own
body. That frugality is a large part of why it performs well on a fixed chain
budget, and the tax is inherent rather than a bug.

So **the time gate, not the ratio gate, is the binding constraint** for any
future parse work — the reverse of what this arc assumed when it was scoped.
This is the second independent measurement (with v1.31-M2's binary tree)
that the m1/m3 ratio gap is structural in the **finder**, not the parse. If
this is ever revisited, attack the candidate source first (a match cache
shared with the greedy scan, or restricting the relax to token-start
candidates) and measure time before ratio.

## Latent encoder bugs this work surfaced (not DP-specific)

Real hazards for any future token-selection work:

1. `add_match` clamps `base = len - inc` into `[2, MAX_LZ_MATCH]` and the
   decoder rebuilds `base + inc`, so an out-of-range `(len, dist)` pair
   silently decodes to a **different length** than recorded — a one-byte
   desync, not a rejection. The greedy loop guards it
   (`len < inc + 2 → reject`); no other caller did.
2. The LDD low-bits distance symbol is on the wire only when the distance has
   **≥ 4 raw extra bits**. Pricing that gates on `dslot >= 4` charges a
   phantom symbol to mid-range distances.
3. A rep-state used for pricing must be **captured at write time**. Reading
   it back from the edge arrays is wrong: a longer match from a later position
   overwrites the entry after the earlier position relaxed, so the state
   advanced is not the state the back-walk replays.
