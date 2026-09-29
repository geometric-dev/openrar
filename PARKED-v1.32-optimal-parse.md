docs(v1.32.0-optimal-parse-parked): PARKED — engine does NOT meet the T5/T6 gates

This branch parks the v1.32 optimal-parse (rolling DP) engine. It is NOT
production code and must not be merged as-is. It is kept for inspection only.

Read docs/ROADMAP.md ("v1.32.0 — optimal parse, descoped") for the full
outcome record. Summary:

GATE RESULT: FAILED. The engine compresses correctly (roundtrip-verified on
every corpus) and beats the greedy parse on real data, but it costs ~3.9x the
match-finder time against a 1.42x budget (T6), and it regresses run-heavy
input by ~28% (T1). The v1.32.0 version number was re-purposed for MT
encoder scaling; the DP ships nowhere.

IMPORTANT — this snapshot is the KNOWN-BROKEN intermediate. It is the state
as of the first T5 measurement (+50% size) and it still contains the
following defects, which were diagnosed and fixed afterwards but whose fixes
were never committed:

  1. The DP scan never inserts positions into the match hash: `unhashed_pos_
     = cur_` at the end of the scan body makes both catch-up loops
     permanently false. Decodable, but find_match only ever sees warm-up-era
     candidates. THIS WAS THE ENTIRE RATIO FAILURE — fixing it turned +50%
     into -1.9%.
  2. A zero-length Huffman code length is read as a zero-bit price. A
     0-length code means the symbol was UNUSED in the previous block, not
     free; when the tables are rebuilt the symbol gets a near-maximum code.
     On 8 MiB of zeros (warm-up emits no literals, so len_ld_[0] == 0) every
     price collapsed to 0 and the artifact went 422 B -> 184,685 B.
  3. Relaxing EVERY match length is O(n * MAX_LZ_MATCH): 8 MiB of zeros took
     22 minutes. Needs a length ladder.
  4. Conflating the DP window with the RAR block doubled the block count on
     run-heavy data (extra Huffman tables per block): 8 MiB of zeros 716 B
     vs greedy's 422 B.

Also present in this snapshot and already understood: the window-reopen block
sat at function entry instead of inside the scan loop, so a mid-loop window
close never reopened (stale prices) and a span relaxed after a close but
before a streaming-defer return was dropped.

BUGS THIS WORK FOUND IN THE EXISTING ENCODER (these are real hazards for any
future token-selection work, and they are NOT DP-specific):

  A. add_match() clamps `base = len - inc` into [2, MAX_LZ_MATCH] and the
     decoder rebuilds `base + inc`, so an out-of-range (len, dist) pair
     silently decodes to a DIFFERENT length than recorded — a one-byte
     desync, not a rejection. The greedy loop guards it with
     `len < inc + 2 -> reject`; no other caller did. Centralise this.
  B. The LDD low-bits distance symbol is on the wire only when the distance
     has >= 4 raw extra bits (add_match gates freq_ldd_ on raw_bits >= 4;
     the decoder reads it under d_bits >= 4). Any pricing that gates on
     dslot >= 4 instead charges a phantom symbol to mid-range distances.
  C. The rep-state used for pricing must be CAPTURED AT WRITE TIME. Reading
     it back from the edge arrays is wrong: a longer match from a later
     position overwrites dp_edge_meta_[i] after position i relaxed, so the
     state advanced is not the state the back-walk replays.

THE STRUCTURAL FINDING, which is the arc's real result: a cost-model parse
over this match finder must evaluate an edge out of EVERY position, and so
spends ~4x the finder budget that greedy/lazy parsing spends (greedy only
searches at token starts, because a match skips its own body). That tax is
inherent, not a bug — and the ratio it buys is only ~2%. Any future attempt
should attack the candidate source (a shared match cache, or restricting the
relax to token-start candidates) BEFORE re-deriving the parse, and should
assume the time gate is the binding constraint, not the ratio gate.
