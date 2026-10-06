# Question log — block table reuse (Design A) interoperability failure

Append-only. Newest entries at the bottom of each section.

**Status: RESOLVED (Entry 22).** Root cause identified (Entry 8: the reference
decoder's multithreaded block-slot seeding) and the fix shipped and validated
32/32. The summary and open questions below are preserved as they stood when the
log was opened; §7 supersedes them. Everything below is measured, not inferred.

Artifacts live in `%TEMP%\opencode\` (scanner in `blockscan\`, test archives in
`final\`) — ephemeral local storage, not in version control. The instrumented
build was a git worktree of this repo at `%TEMP%\opencode\wt-reuse`. Entries 19
and 20 were appended out of chronological order and have been re-sequenced.

---

## 1. Summary

A RAR5 encoder optimisation ("Design A") omits a compression block's table
description and clears block-header flag bit 7 when the block's freshly built
Huffman code lengths are bit-identical to the ones the decoder already holds.

The optimisation is legal per the format description. The decoder's semantics are
unambiguous: bit 7 clear means "consume no description, keep the tables in force".
The only hard rule is that the first block of a non-solid member must carry a
description.

It round-trips through our own decoder. Independent decoders reject it above a
size threshold. The failure is a clean early stop, not corruption.

**The question we cannot answer from the outside: what does an independent
decoder do differently once a member contains many consecutive reuse blocks?**

---

## 2. Established facts (measured)

### 2.1 Reproduction

Restored the implementation from stash `7f89b469` ("On
arc/v1.33.0-candidate-source: item2-wip", 155 insertions across
`compressor50.{cpp,hpp}`, `compress_tests.cpp`, `docs/spec/03-compression-m1-m5.md`).
Built clean. Failure reproduces.

| input | 1 thread | 2 threads | 4 threads |
|---|---|---|---|
| 16 MiB zeros | PASS | PASS | — |
| 20 MiB | PASS | PASS | PASS |
| 21 MiB | — | PASS | PASS |
| 21.25 MiB | — | FAIL | — |
| 22 MiB | — | FAIL | FAIL |
| 24 MiB | PASS | FAIL | — |
| 25–27 MiB | PASS | — | — |
| 27.25 / 27.5 MiB | PASS | — | — |
| 27.75 MiB | FAIL | — | — |
| 28–32 MiB | FAIL | — | — |
| 48 / 64 MiB | FAIL | FAIL | — |

- Single-thread threshold: **27.5 → 27.75 MiB** (55 → 56 blocks).
- Multi-thread threshold: **21 → 21.25 MiB** (42 → 43 blocks).
- **2 threads and 4 threads produce byte-identical archives.** The failure is
  deterministic, not a race.
- **Reuse disabled: 12/12 PASS** at 16–64 MiB, 1 and 2 threads. The failure is
  strictly gated on the reuse path.

### 2.2 Block geometry is not the discriminator

| input | blocks | archive bytes | verdict |
|---|---|---|---|
| ST 27.0 MiB | 54 | 1224 | PASS |
| ST 27.25 MiB | 55 | 1235 | PASS |
| ST 27.5 MiB | 55 | 1243 | PASS |
| ST 27.75 MiB | 56 | 1254 | FAIL |
| ST 28.0 MiB | 56 | 1262 | FAIL |
| MT 21.0 MiB | 42 | 1222 | PASS |
| MT 21.25 MiB | 43 | 1240 | FAIL |

Neither block count (55 pass / 56 fail vs 42 pass / 43 fail) nor archive size
(1243 pass / 1254 fail vs 1222 pass / 1240 fail) is the discriminator.

### 2.3 The reuse predicate and table serialisation are correct

Instrumented dump of every `write_block` call (block index, reuse decision, table
size, FNV hash of the four freshly built length vectors, hash of the saved
reference, bit count):

- No block ever has `reuse=1` with a fresh-vector hash differing from the saved
  reference hash.
- The saved reference is advanced only on the wire-success path, after the
  empty-block early returns.
- On 27 MiB: 52 of 53 blocks share one identical table set (hash
  `5b8ed33862f198c8`); the final block genuinely differs and correctly emits a
  description.

Table vectors dumped at block 1:

```
BC(20)          : 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1
LD length hist  : L0=305  L1=1  L2..L15=0
LD[248..265]    : 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0     (index 257 -> length 1)
DD[0..7]        : all 0
LDD[0..7]       : all 0
RD[0..7]        : all 0
```

This is a coherent description: a single used LD symbol (257, "repeat
OldDist[0] / LastLength") at length 1, every other symbol unused, DD/LDD/RD
entirely unused. The token model documents symbol 257 as needing no extra bits,
which is why RD being empty is consistent.

### 2.4 Framing is exact

For both a passing and a failing archive: every block header's 1-byte checksum
validates, and the walk closes exactly (`sum(BlockSize) + headers == packed`).

### 2.5 Failure shape: clean stop, correct output

Decompressing with an independent decoder:

- All output bytes up to the stop are **zero** (input is zeros, so a correct
  decode). No corruption signature.
- The stop is a hard stop, not a completed member. Error is reported as a file
  checksum mismatch, i.e. a consequence of the truncation, not the cause.
- The decoder consumes a long correct prefix, then stops partway through a block
  roughly 84–96% of the way through the member.

Representative: the 27.75 MiB single-threaded archive yields 28,573,577 bytes of
29,097,984 — 54 complete blocks plus ~64 tokens' worth of block 54.

---

## 3. Ruled out

1. **Dictionary size / algorithm selector.** Independent decoders never select the
   80-slot distance alphabet; selector is a 6-bit field in the file header where
   only 0 (64 slots) and 1 (80 slots) are meaningful, and normal producers always
   emit 0 with a power-of-two dictionary. Our failing archives all declare the
   same 8 MiB dictionary and selector 0 at every size, so it cannot discriminate.
2. **Block size ceiling.** The 24-bit block size field is nowhere near its limit
   (largest observed payload ≈ 36 KB).
3. **Table-description format.** Independent format documentation describes
   delta-against-previous-description and multi-channel table readers, reached
   from an in-band symbol rather than the block-header flag. Nothing in our
   streams takes that path, and our own decoder round-trips.
4. **Multi-volume boundaries cutting blocks.** Confirmed they do cut blocks, and
   confirmed every false-positive reuse observation we had came from scanning
   without the header checksum. Not related to this failure.
5. **Reference producers emitting reuse blocks.** A sweep of 54,279 block headers
   across every RAR5 archive on this machine found bit 7 clear only in our own
   archives and in deliberately corrupt fixtures. No external validation of the
   reuse path exists.
6. **MT-specific cause.** Single-threaded fails too, at a larger size. MT only
   reaches the threshold sooner because chunking reduces how often the tables
   repeat.
7. **A data race.** 2 and 4 threads are byte-identical.

---

## 4. Open questions as they stood

**Q1 (primary).** An independent decoder stops after decoding ~64 of a reuse
block's 128 tokens. The block's payload is 16 bytes = 128 bits = 128
one-bit-coded tokens. 64 tokens is exactly half. A clean stop with all-correct
output and exact framing is what you get when the decoder runs out of bits
mid-block. Is there a rule limiting how many consecutive bit-7-clear blocks a
decoder will honour, or a limit on total input consumed between table
descriptions, that we have not found?

**Q2.** Our reuse blocks always have a declared final-byte bit count of 8 (the
token stream lands byte-aligned). Our description-carrying blocks have varied
values (1, 5, 6). Is a final-byte bit count of 8 handled identically by every
decoder, or is there an interaction between "fully used final byte" and the
bit-7-clear path?

**Q3.** Is there any per-block constraint on the *token alphabet* when tables are
inherited? Specifically: our reuse blocks use exactly one LD symbol (257) at code
length 1 and 305 symbols at code length 0. Is a description whose alphabets are
almost entirely "unused" (length 0) legal to inherit repeatedly, or is there a
constraint the reference reader enforces that our decoder does not?

**Q4.** Our MT path gives each chunk a fresh encoder instance with its own cold
tables and its own first-block description, then concatenates the chunk block
streams into one member. The member therefore contains several distinct table
sets. Is that legal, or does the format expect a single table sequence per
member even though it is not enforced at block granularity?

**Q5 (process).** We would like an independent check on one thing we cannot
verify: for a member whose blocks 0..53 are byte-identical to those of a passing
archive, and whose block 54 onward differ, is there any decoder state that a
*correct* encoder could get wrong which is not visible in the block framing? We
have verified the reuse predicate, the table serialisation, the framing
arithmetic and the reference slot discipline. We are out of hypotheses that are
consistent with all of those being correct.

---

## 5. Experiments already run (so they need not be repeated)

- Full-size sweep 16–64 MiB × 1/2/4 threads, with and without reuse.
- Fine-grained sweep at 0.25 MiB granularity across both thresholds.
- Instrumented encoder dump: per-block reuse decision + table hashes + bit counts
  + raw length vectors.
- Header-checksum-validating block walk over the whole member.
- Independent extraction with byte-level output verification (zero/non-zero scan).
- Archive-to-archive structural and byte-prefix comparison between a passing and
  a failing archive in the same series (identical through block 38; first
  difference is block 39's last-block flag only).
- Multi-thread archive architecture read from source (per-chunk encoder instance,
  chunk size `max(1 MiB, min(4 MiB, ceil(size/threads)))`, outputs concatenated
  in order).

---

## 6. Next experiments queued

1. Synthesise a bit-7-clear archive by hand from a known-good archive (decode the
   description, splice out a description, re-emit the bitstream) and test it
   against independent decoders. This gives us an independent oracle on our own
   bitstream and turns "our encoder is self-consistent" into "our encoder is
   self-consistent *and* independently decodable".
2. Vary only `block_bit_size` on reuse blocks (force the token stream to end
   non-byte-aligned) and see whether the threshold moves. Isolates Q2.
3. Vary block size independently of member length (e.g. a much smaller block
   quantum on a large member) to see whether the threshold tracks block count,
   member length, or total bytes between descriptions.

---

## 7. Appended findings

*(newest last)*

### Entry 1 — 2026-10-02, initial log

Established §2, ruled out §3, open §4. Reproduced from stash `7f89b469`.

### Entry 2 — Instrumented encoder: predicate and serialisation verified correct

Added an env-gated dump to `write_block` (block index, reuse decision, table size,
FNV hash of the four freshly built length vectors, hash of the saved reference,
bit count, and the raw length vectors).

Result across the full 27 MiB pass case and the 28 MiB fail case:

- **No block ever has `reuse = 1` with a fresh-vector hash differing from the saved
  reference hash.** The predicate is doing what it claims.
- The saved reference advances only on the wire-success path, after the
  empty-block early returns, as the comment intends.
- 52 of 53 blocks share one identical table set; the final block genuinely differs
  and correctly emits a description.

Table vectors at block 1:

```
BC(20)       : 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1
LD histogram : L0=305  L1=1  L2..L15=0
LD[248..265] : 0 0 0 0 0 0 0 0 0 1 0 ...      (index 257 -> length 1)
DD, LDD, RD  : all zero
```

Coherent: one used LD symbol (257, "repeat OldDist[0] / LastLength") at length 1,
everything else unused. RD being empty is consistent because symbol 257 needs no
length code.

**Conclusion: the encoder's reuse decision and its table serialisation are both
correct. Neither is the bug.**

### Entry 3 — REFRAME: our own decoder accepts the failing archive

`openrar t` on the 27.75 MiB failing archive:

```
Testing     z.bin... OK
Done. 1 files tested
```

**Our decoder decodes the member fully and the CRC validates.** The stream is
correct.

An independent decoder stops after producing 28,573,577 of 29,097,984 bytes —
short by 524,407, i.e. almost exactly one 512 KiB block — with byte-perfect
output up to the stop.

**This is therefore not an encoder bug. It is a decoder-conformance gap in our
decoder.** We emit something our decoder accepts and a conforming reader refuses
to finish. Every earlier hypothesis that assumed the encoder was wrong is now
dead.

### Entry 4 — `block_bit_size == 8` is not the problem (Q2 closed)

Diagnostic patch: for reuse blocks only, append one zero bit so the token stream no
longer ends byte-aligned (declared final-byte bit count becomes 1 instead of 8).

Result: **every** size fails, including 27.5 MiB which previously passed.

- `block_bit_size = 8` is fine. Q2 closed — no decoder objection to a fully used
  final byte.
- New constraint discovered: **trailing bits in a block's final byte corrupt the
  decode.** A conforming decoder does not use the declared final-byte bit count to
  stop before it; it consumes the whole final byte as tokens. So an encoder must
  emit byte-aligned block tails and must not rely on the bit count to truncate.

This also means the aligned case is the strictly better one, and our encoder
already does the right thing.

### Entry 5 — Dictionary, method and selector identical across every pass/fail pair

| archive | COMP_INFO | sel | method | dict |
|---|---|---|---|---|
| q27 (pass) | 0x1980 | 0 | 3 | 8388608 |
| q28 (fail) | 0x1980 | 0 | 3 | 8388608 |
| h27.5 (pass) | 0x1980 | 0 | 3 | 8388608 |
| h27.75 (fail) | 0x1980 | 0 | 3 | 8388608 |
| b21_t2 (pass) | 0x1980 | 0 | 3 | 8388608 |
| b22_t2 (fail) | 0x1980 | 0 | 3 | 8388608 |
| s24 (pass) | 0x1980 | 0 | 3 | 8388608 |
| s32 (fail) | 0x1980 | 0 | 3 | 8388608 |

Dictionary size cannot discriminate. Also ruled out.

### Entry 6 — Revised open questions

The reframe in Entry 3 changes what we are asking. We no longer ask "what is our
encoder doing wrong". We ask:

**Q1 (new, primary).** Our decoder accepts a member that a conforming decoder
terminates early. Where is our decoder more permissive than the format requires,
specifically around a block that inherits its tables? Entry 4 shows we already had
one such gap (final-byte bit count). What is the next one?

**Q2 (new).** Our reuse blocks have payloads that are entirely zero bytes, with
exactly one LD symbol (257) at code length 1 and every other symbol unused. Is a
block whose entire payload decodes to a single repeated symbol, under an inherited
all-but-one-unused table, something a conforming decoder is entitled to treat as
degenerate?

**Q3 (new).** Does a conforming decoder impose any limit on how long a member may
run without a table description — by block count, by bytes, or by total decoded
output? Our single-threaded member runs 53 consecutive inherited-table blocks and
fails somewhere past block 54; the multi-threaded member fails past block 42. If
such a limit exists it is the most likely explanation, and it would mean reuse is
bounded but not forbidden.

**Q4 (carried).** Is a member containing several distinct table sets (our MT path:
one encoder instance per chunk, each with its own first-block description, block
streams concatenated in order) legal?

**Q5 (carried, process).** We have now verified the reuse predicate, the table
serialisation, the framing arithmetic, the reference-slot discipline, the
dictionary, and the block size ceiling. The remaining suspect is our decoder's
permissiveness, not the encoder.

### Entry 8 — RESOLVED: table-slot seeding, and the proposed cap is wrong

The mechanism, settled by experiment: the sequential decoder path has no bound of any kind on
inherited-table blocks; the failure lives in the reference decoder's
**multithreaded driver**, which pre-scans block headers, gives each block a work
item with its **own copy of table state initialised empty**, and slots blocks by
index so that a reuse block landing in a never-seeded slot decodes against empty
tables and degenerates. The model is behavioral: every element of it is
confirmed by the black-box experiments below, and no reference implementation
source informs it.

**Tested and confirmed.**

1. *The decisive test.* Forced single-thread versus default:

   | archive | default | `-mt1` | `-mt2` |
   |---|---|---|---|
   | h27.5 (pass) | PASS | PASS | PASS |
   | h27.75 (fail) | PASS | PASS | **FAIL** |
   | q28 (fail) | PASS | PASS | **FAIL** |
   | b22_t2 (fail) | PASS | PASS | **FAIL** |
   | s32 (fail) | PASS | PASS | **FAIL** |

   **`-mt1` passes everything at every size. `-mt2` fails above the thresholds.**
   The failure is exclusively in the multithreaded path. Confirmed.

   *Caveat found in passing:* with no `-mt` the default thread count is
   load-dependent and the same archive passed on one run and failed on another.
   **Never gate on `unrar t` without an explicit `-mt`.**

2. *The slot model, tested by seeding.* Diagnostic patch forcing a description on
   the first N blocks of a member, reuse free thereafter:

   | seed | 64 MiB: mt1/mt2/mt4 | 128 MiB: mt1/mt2/mt4 |
   |---|---|---|
   | 0, 1, 2, 3 | P/**F**/F | P/**F**/F |
   | **4** | P/**P**/F | P/**P**/F |
   | **8** | P/P/**P** | P/P/**P** |
   | 16 | P/P/P | P/P/P |

   The period is exactly `2 × decoder threads`: seed 4 fixes a 2-thread decoder,
   seed 8 fixes 4, seed 16 fixes 8. **Model confirmed precisely.**

3. *The proposed cap does not work.* Diagnostic patch forcing a description at
   least once every K blocks:

   | K | 2 | 3 | 4 | 8 | 16 | 32 | 64 | 1000 | 1 |
   |---|---|---|---|---|---|---|---|---|---|
   | result at `-mt2` | F | F | F | F | F | F | F | F | **P** |

   Every cap from 2 to 1000 fails. Only "never reuse" passes.

   **Why the cap cannot work, given the model is otherwise right:** a description
   every K blocks only ever lands on indices ≡ r0 (mod K), which populates a
   single slot residue r0 (mod P). Only the **first P consecutive blocks** cover
   all P residues. A periodic cap seeds one slot and starves P−1 of them.

### The fix

**Emit a table description on the first `2 × max_decoder_threads` blocks of every
member. After that, reuse is unbounded.**

- The slot count follows the decoder's requested thread count; the worker pool is
  capped at eight, so **seed 16** is the portable value.
- Cost is a fixed ~16 descriptions per member (~400 bytes), **independent of
  member size**.
- Savings are essentially the full win. Confirmed at 64 / 128 / 256 MiB:

  | input | archive | blocks | reuse | mt1 | mt2 | mt4 | mt8 | default |
  |---|---|---|---|---|---|---|---|---|
  | 64 MiB | 2,725 B | 128 | 111 | P | P | P | P | PASS |
  | 128 MiB | 5,157 B | 256 | 239 | P | P | P | P | PASS |
  | 256 MiB | 10,022 B | 512 | 495 | P | P | P | P | PASS |

- The seed counter resets at each member start, at the same place
  `prev_tables_valid_` is reset.
- The sequential path is indifferent to the extra descriptions.

### Entry 9 — Retired hypotheses

- **Q2 (degenerate table legality): retired.** Accepted, and consistent with
  consistent with our measurements — 305 unused plus one length-1 symbol is legal
  and an inheriting block owes nothing else. Not a cause.
- **Entry 4 side finding: consistent.** The final-byte bit count is used exactly
  as inferred; a count of 8 makes the whole final byte decodable, so byte-aligned
  tails are correct. Matches our padding experiment (which made every size fail).
- **The decoder-conformance gap (Entry 3) is now explained.** Our decoder is
  sequential-only and therefore never exercised the slot model. We were not
  lenient about block boundaries; we simply never implemented the multithreaded
  path at all.

### Entry 11 — Refinement 1 CONFIRMED by experiment: once-per-member seed is not enough

Built a member with a **genuine mid-member table change**: 16 MiB of zeros
followed by 16 MiB of high-entropy bytes (33,554,432 bytes uncompressed). The
table set necessarily changes at the boundary.

| build | blocks | carry | reuse | mt1 | mt2 | mt4 | mt8 |
|---|---:|---:|---:|---|---|---|---|
| seed 0 (no seed) | 544 | 79 | 465 | P | **F** | **F** | **F** |
| seed 16 | 544 | **93** | 451 | P | **F** | **F** | **F** |

The seed demonstrably applied (carry count rose 79 → 93; the delta is 14 rather
than 16 because some of blocks 0–15 already carried descriptions, so the seed was
a no-op there).

**Result: seeding the first 16 blocks is NOT sufficient once the table set changes
mid-member.** The member fails at every multithread setting.

This validates the general rule rather than the bare seed: a table-set change is a
re-seed event, because a change refreshes exactly one slot and leaves the other
15 holding a stale set. **The re-seed-on-change rule is mandatory.**

The conservative form — 16 consecutive descriptions on the first 16 blocks and
on the 16 blocks following every table-set change — is correct and sufficient.
Sixteen consecutive descriptions cover every residue class mod any period from 4
to 16, so it holds regardless of the extractor's thread count.

The alternative (model all seven possible periods inside the reuse predicate)
permits sparser re-seeding after a change but buys ~400 bytes per table change on
run-heavy data. Not worth the code. Take the conservative rule.

### Entry 12 — Refinement 2: carry positions resolved, and it partially refutes the strict model

Direct walk of the actual wire layout (not the table-hash dump, not the earlier
scanner counts):

| archive | blocks | carry indices | reuse run |
|---|---:|---|---|
| h27.5 (passes `-mt2`) | 55 | **0, 1, 54** | 2…53 (52 consecutive) |
| h27.75 (fails `-mt2`) | 56 | **0, 1, 55** | 2…54 (53 consecutive) |
| d27 (passes) | 55 | 0, 1, 53, 54 | 2…52 |
| cf64, seed 16 (passes) | 129 | 0–15, then 127, 128 | 16…126 |
| c16_64, cap 16 (fails) | 129 | 0, 1, 15, 31, 47, … every 16 | — |

Two corrections and one open item:

1. **Correction to the earlier reading.** The early carries are **not** at 0, 1, 2.
   They are at **0, 1 and the final block**. Our instrumented dump logged the
   `reuse` decision per block, so carry *positions* were always available; the
   "3 carry" figures came from the block scan and are correct as counts. Both
   sources agree on the count (3) and the dump gives the positions.

2. **Partial refutation of strict same-slot inheritance.** h27.5 runs **52
   consecutive reuse blocks (2…53) seeded by only two early descriptions**, and
   still passes a period-4 decoder. Under strict per-slot accumulation, blocks
   landing in slots 2 and 3 would never be seeded and every fourth block would
   fail. It does not. **So table state propagates further than strict same-slot
   history** — the model is incomplete on the propagation question.

3. **This does not weaken the fix.** The conservative rule is safe whether
   propagation is strict or not, and Entry 11 shows it is *necessary* in the
   changing-table case. The propagation question only affects how much the rule
   could be optimised, not whether it is correct.

### Entry 13 — Answer to the open questions, as now settled

- **Q3 (limit on running without a description):** no count-based limit of any
  kind. The binding constraint is slot coverage — cover all `2 × threads` slots
  before the first reuse, and re-cover after every table-set change. The earlier
  "at most 2×threads−1 consecutive reuse blocks" phrasing was the wrong shape and
  is retired.
- **Q4 (multiple distinct table sets per member):** legal. The sequential path
  accepts arbitrary description sequences. Under the multithreaded driver each
  set change behaves as a re-seed event.
- **Q5 (state invisible in the framing):** confirmed. Slot assignment and each
  slot's accumulated table history are invisible in the byte stream and
  deterministic given the extractor's thread count. Our encoder was correct
  against sequential semantics; the missing model was decoder-side.

### Entry 15 — Fix implemented and validated

Implemented as specified, two files, +46/−5. All four env-gated diagnostics
removed.

```cpp
// header (private)
static constexpr unsigned MAX_DECODER_THREADS = 8;
static constexpr unsigned REUSE_SEED_BLOCKS = 2 * MAX_DECODER_THREADS;
unsigned seed_remaining_{REUSE_SEED_BLOCKS};

// begin_archive() and start_file(), alongside prev_tables_valid_ = false
seed_remaining_ = REUSE_SEED_BLOCKS;

// write_block()
const bool tables_identical = prev_tables_valid_ && prev_table_size_ == cur_table_size_ &&
                              memcmp x4;
if (!tables_identical) seed_remaining_ = REUSE_SEED_BLOCKS;   // change => re-seed
const bool tables_reusable = tables_identical && seed_remaining_ == 0;
if (seed_remaining_ > 0) --seed_remaining_;
```

Validation — 11 archives, 4 decoder thread settings each, **40/40 pass**,
`badChecksum = 0` everywhere:

| input | enc | archive | blocks | carry | reuse | mt1 | mt2 | mt4 | mt8 |
|---|---|---|---|---|---|---|---|---|---|
| zeros 16 MiB | ST | 887 B | 32 | 17 | 15 | P | P | P | P |
| zeros 27.75 MiB † | ST | 1,361 B | 56 | 18 | 38 | P | P | P | P |
| zeros 28 MiB | ST | 1,369 B | 56 | 18 | 38 | P | P | P | P |
| zeros 64 MiB | ST | 2,734 B | 128 | 18 | 110 | P | P | P | P |
| zeros 22 MiB † | MT | 1,346 B | 44 | 44 | 0 | P | P | P | P |
| zeros 64 MiB | MT | 3,614 B | 128 | 128 | 0 | P | P | P | P |
| heterogeneous 32 MiB | ST | 16.78 MB | 544 | 369 | 175 | P | P | P | P |
| heterogeneous 32 MiB | MT | 16.78 MB | 544 | 408 | 136 | P | P | P | P |

† previously failed. "heterogeneous" is 16 MiB of zeros followed by 16 MiB of
high-entropy data, which forces a genuine mid-member table change — the Entry 11
case that the bare seed did not survive.

### Entry 16 — Finding: the MT path currently gets no reuse on run-heavy data

`chunk_size = max(1 MiB, min(4 MiB, ceil(size / threads)))`, and blocks are
512 KiB, so a chunk is **8 blocks**. The seed is 16. Every block of every chunk
therefore emits a description:

- zeros 22 MiB, MT: 44 blocks, **44 carry, 0 reuse**
- zeros 64 MiB, MT: 128 blocks, **128 carry, 0 reuse**
- zeros 64 MiB, ST: 128 blocks, 18 carry, **110 reuse**

So the fix is correct but currently only pays on the single-threaded path. MT
does benefit when a chunk contains more than 16 blocks — the heterogeneous MT
case got 136 reuse blocks out of 544, because incompressible data trips the
token-count flush and packs far more blocks into each chunk.

Options, in the order I would try them:

1. **Raise the MT chunk floor** so a chunk holds at least `REUSE_SEED_BLOCKS`
   blocks. The floor exists to bound per-worker hash allocation, so this trades
   memory for ratio and needs a look at the allocation bound first.
2. **Raise the block quantum** so fewer, larger blocks mean more blocks per
   chunk. Hurts the block-overhead ratio on short members.
3. **Accept it.** MT's win is throughput; reuse is a ratio feature. The
   single-threaded path already captures the run-heavy case, which is where the
   measured saving lives.

Not a correctness issue — the MT path is simply not benefiting yet.

### Entry 19 — Second build-tree incident, same root cause

Deleting `*.obj` from `build/rel` and rebuilding also produced a silently broken
binary: 152-byte archives containing no member data, exit 0. The only reliable
procedure on this machine is to **delete `build/rel` entirely and re-run the
CMake configure**, then build. Object-file-level surgery is not safe here.

Two consequences for how the numbers above should be read:

- Every "identical output regardless of the constant" result I saw during the
  chunk-floor experiment was a stale binary, not a real null result. The first
  floor measurement (3,305 B / 30 reuse) was taken after a verified recompile
  and is the one reading I trust; the follow-up "no effect at any floor" runs
  were not.
- The final shipped state was re-verified from a **wiped and reconfigured**
  build, and reproduces the original validation numbers exactly.

### Entry 20 — Shipped state

Diff against the stash commit: **2 files, +60/−6**, all of it the slot re-seed.
No diagnostics, no tuning, no env gates.

```cpp
// compressor50.hpp (private)
static constexpr unsigned MAX_DECODER_THREADS = 8;
static constexpr unsigned REUSE_SEED_BLOCKS = 2 * MAX_DECODER_THREADS;
unsigned seed_remaining_{REUSE_SEED_BLOCKS};

// begin_archive() and start_file(), beside the existing prev_tables_valid_ reset
seed_remaining_ = REUSE_SEED_BLOCKS;

// write_block()
const bool tables_identical = <the original predicate>;
if (!tables_identical) seed_remaining_ = REUSE_SEED_BLOCKS;   // change => re-seed
const bool tables_reusable = tables_identical && seed_remaining_ == 0;
if (seed_remaining_ > 0) --seed_remaining_;
```

Verified from a wiped-and-reconfigured build, 24/24 decoder checks, `badck = 0`
everywhere:

| input | enc | archive | blocks | carry | reuse | mt1 | mt2 | mt4 | mt8 |
|---|---|---|---|---|---|---|---|---|---|
| zeros 27.75 MiB † | ST | 1,359 B | 56 | 18 | 38 | P | P | P | P |
| zeros 27.75 MiB † | MT | 1,660 B | 56 | 56 | 0 | P | P | P | P |
| zeros 64 MiB | ST | 2,732 B | 128 | 18 | 110 | P | P | P | P |
| zeros 64 MiB | MT | 3,612 B | 128 | 128 | 0 | P | P | P | P |
| heterogeneous 32 MiB | ST | 16.78 MB | 544 | 279 | 265 | P | P | P | P |
| heterogeneous 32 MiB | MT | 16.78 MB | 544 | 398 | 146 | P | P | P | P |

† sizes that failed before the fix.

**Case closed.** Table reuse is interoperable on the single-threaded path and
safe everywhere. The residual MT ratio gap on run-heavy data is a separate,
### Entry 21 — Chunk floor: CLOSED, and the framing was wrong

Redone with a procedure that cannot produce a stale binary: patch floor → **wipe
`build/rel`** → configure → build → **sanity gate** (8 MiB of zeros must compress
under 100 kB; otherwise the reading is discarded) → measure. Five floors, each
built and gated independently.

**64 MiB of zeros, `-mt4`:**

| chunk floor | archive | reuse | wall | vs `-mt1` |
|---|---:|---:|---:|---:|
| 4 MiB (previous) | 3,550 B | 0 | 413 ms | **0.56×** |
| 8 MiB | 3,526 B | 0 | 303 ms | — |
| 12 MiB | 3,305 B | 30 | 306 ms | — |
| **16 MiB** | **3,095 B** | **57** | **202 ms** | **1.27×** |
| 32 MiB | 2,846 B | 92 | 215 ms | 1.17× |

**The premise I gave in Entry 16 was wrong.** I wrote that the floor "trades
parallelism for ratio". It does not. At the old 4 MiB ceiling the multithreaded
path was **slower than the sequential one** (0.49–0.56×) — a chunk held 8 blocks,
spent all of them on its re-seed, and reused nothing. Chunk granularity was not
the bottleneck; the per-chunk re-seed was. Raising the floor makes MT both
**smaller and faster**, because it amortises the re-seed.

Text is unaffected: no reuse is available either way (tables keep changing), and
the spread across floors is ±0.2%, i.e. noise.

**Adopted: floor = 2 × seed length in blocks = 32 blocks × 512 KiB = 16 MiB.**
Chosen over 32 MiB because 16 MiB gives full thread occupancy at the sizes that
matter (64 MiB / 16 MiB = 4 chunks = 4 threads) and the best throughput reading.
The value is derived from the seed length rather than tuned to this machine, so
it holds as the seed or the block quantum changes.

Final effect versus the previous behaviour, all decoder thread settings passing,
`badck = 0` everywhere:

| input | enc | before | after | change |
|---|---|---:|---:|---|
| zeros 27.75 MiB | MT | 1,660 B | **1,464 B** | −11.8% |
| zeros 64 MiB | MT | 3,612 B | **3,093 B** | −14.4% |
| zeros 64 MiB | MT throughput | 0.56× ST | **1.27× ST** | — |
| zeros, any size | ST | unchanged | unchanged | — |
| text, any size | either | unchanged | unchanged | — |
| heterogeneous 32 MiB | MT | 16,784,187 B | **16,782,921 B** | slightly better |

### Entry 22 — Final shipped state

Three files, +74/−7, against the stash commit:

- `compressor50.hpp` — `MAX_DECODER_THREADS`, `REUSE_SEED_BLOCKS`, `seed_remaining_`,
  and the reasoning.
- `compressor50.cpp` — the re-seed predicate in `write_block`, the two resets, and
  a note tying this entry point's chunk clamp to the pipeline's.
- `parallel_compressor.cpp` — the chunk floor, sized from the seed length.

Verified from a wiped-and-reconfigured build with the sanity gate passing:
**32/32 oracle checks** across zeros at 27.75 and 64 MiB, heterogeneous 32 MiB,
encoded single- and multi-threaded, decoded at `-mt1/2/4/8`. `badck = 0` on every
archive.

Everything above is measured. The two things left on the table, both recorded and
neither blocking:

1. **Member-scoped seed across chunks** (Entry 18 option 2) would close the
   remaining gap between MT (3,093 B) and ST (2,732 B) on runs. It needs the
   per-chunk `StreamEncoder` instances to share table state, which is a design
   change rather than a tuning change.
2. **A memory measurement for the larger chunk.** The floor doubles the per-chunk
   span the worker hash allocation has to cover. Ratio and throughput both improved
   on this corpus set, so the size cost has not shown up — but peak RSS was not
   instrumented, and it should be before the floor is raised further.


### Entry 23 — v1.33.1 close-out: the two deferred threads, pulled

*(appended after the v1.33.0 release; both threads were opened by Entries 16
and 4 and are now resolved at the documentation level.)*

**Member-scoped seed across chunks (Entry 18 option 2, Entry 22 leftover 1):
blocked by parallel dispatch, payoff bounded — deferred.** The idea: chunk
encoders after the first inherit the member's last-emitted table state with
`seed_remaining_ = 0`, so zeros-format MT stops paying a 16-description seed
per chunk (the entire measured MT↔ST gap on 64 MiB zeros is ~361 B, i.e.
almost exactly the per-chunk re-seed). The blocker is structural, not
tuning: `parallel_compressor.cpp` keeps `max_in_flight = max(2, 2 x threads)`
chunks in flight, so chunk *N+1*'s first table build happens before chunk *N*
has produced its final table vectors. Inheriting therefore requires either
serializing chunk starts (destroys the MT throughput win) or a speculative
table commit with fixup (new failure modes). The design change is recorded as
a scoped arc, not a tuning change — matching Entry 22's original assessment.

**`BlockBitSize` / trailing bits (Entry 4): model refined, decoder change
deferred.** Entry 4 concluded that a reference decoder "consumes the whole
final byte as tokens". The v1.33.1 sweep refines this to a condition, not a
universal rule: padding bits corrupt only when they complete a code in the
block's table. Two measurements, one model:

- Reference producers emit non-byte-aligned tails routinely: 838 of 954
  compression blocks in a WinRAR m1/m3/m5 sweep over text/random/runs declare
  `BlockBitSize` 1-7, and such archives decode everywhere.
- Our own v1.33.0 archives ship 491 non-aligned of 672 blocks (every
  description-carrying block ends mid-byte; only reuse blocks are
  byte-aligned) — 24/24 interop gate stages and 36 oracle checks pass.

So non-alignment alone is harmless; the corruption in Entry 4's experiment
came from a sparse all-257 table in which every padding bit completed the
1-bit code for symbol 257. Consequences as shipped: the encoder-side
byte-aligned-tail rule for reuse blocks stands (that is exactly the sparse-
table configuration where it matters); our decoder stops at the declared bit
count (`decompressor50.cpp` `block_end_bit`) — the conservative reader, which
differs from reference behavior only on foreign archives whose padding
completes a code. `scan_table_reuse.py` now reports the final-byte bit-count
histogram so this stays measurable.

### Entry 24 — The multivolume default window is 2 MiB regardless of method (perf-check finding, RESOLVED in v1.37.4)

Found by the v1.37.2 perf-check benchmark's first full multivolume size
row: `openrar a -m3 -v32m` produced a 62.9 MB volume set where the same
corpus packs to 54.2 MB non-volume (+16%), while WinRAR's set (52.8 MB)
matches its own non-volume archive. Root cause, isolated by a minimal
repro (46 MB generated code + 44 MB random, `-m3 -v4m`):

`ArchiveMutator::add_file_to_archive_vol` sizes the compressor window
with `win_size = (dict_size > 0) ? dict_size : 0x200000ULL` — a flat
2 MiB default for every method — where the non-volume path
(`prepare_add_file`) uses the method-tuned defaults (8 MiB for m3, 16 MiB
for m4, 64 MiB for m5). The perf corpus's generated code has a ~2 MiB
redundancy period, so a 2 MiB window kills nearly all cross-period
matches: code.cpp packs 0.45 MB non-volume, 8.39 MB through the volume
path (46,452,882 -> 452,174 vs 4,194,201+4,194,200 across two 4 MiB
volumes). Text (short match distances) is unaffected; stored members are
unaffected; explicit `-md` IS honored (the drift is default-only, which
is also the user workaround). Solo-file volume sets do not inflate; the
overhead appears exactly when a member's redundancy period exceeds 2 MiB.

Not a v1.37 regression — the compressor is untouched this arc; the
benchmark simply recorded volume-set sizes for the first time. Fix
direction: share the method-tuned default table with the non-volume path
(one helper), keep the existing pow2 file-size clamp and the dict_size
override, then re-run the interop gate (the window change alters packed
streams) and re-record the volume rows. Scheduled as its own patch
release: the change alters archive bytes, so it ships measured, not
rushed.

#### Entry 24 close-out — fixed and measured (v1.37.4)

**Fix as shipped.** `add_file_to_archive_vol` resolves the window through
`compress::resolve_dict_window_size` (`src/compress/compress_plan.hpp`) —
one shared helper with `prepare_add_file` (1..15 legacy 128 KiB scale,
0 = method-tuned default, >15 exact bytes), then the existing FCI snap and
pow2 file-size clamp. The sibling sweep found and fixed two adjacent drifts
in the same lines: the volume path treated `-md 1..15` as exact bytes (a
1-byte window for `-md1 -v4m`), and it never set `unp_ver = 1`, so a
non-pow2 or above-v0-ceiling window would have been written with the v0
COMP_INFO encoding, which cannot carry the fraction — the header would
silently floor below the encoder's match horizon (the v1.21.2 corrupt
direction). It also gained a solid-run window clamp: a member continuing a
solid chain packs with the chain's prior compressed window when smaller
(shrink-or-equal is safe under every decoder model; run heads are free).
The raw-codec ABI defaults (`openrar_compress`/WASM 2 MiB pairs,
`StreamEncoder`/`StreamDecoder` 4 MiB pair) are self-consistent headerless
surfaces — documented as intentionally separate in `docs/invariants.md`
§7, not changed. The volume path's RAM profile stays O(window): one
StreamEncoder (~5x window + 5 MiB) with packed bytes streamed to the spool
file, i.e. the 64 MiB m5 default costs ~330 MiB transient, the same class
the non-volume path already declares per entry and strictly less than its
parallel path.

**After numbers (same host, full `perf_vs_winrar.py` re-run, corpora
fingerprint-identical; every archive cross-extracted by both engines).**

| metric | before | after | prediction | verdict |
|---|---|---|---|---|
| `o_volumes` size | 62,900,354 B | **54,168,121 B** (−13.88%) | ~54.5–55.5 MB | ✓ |
| `o_volumes` vs `w_volumes` (52,789,355 B) | +19.15% | **+2.61%** | ~+3% | ✓ |
| code.cpp via volume path | 8.39 MB | **451,867 B, method 3, 8 MiB dict** | ~0.45–0.55 MB | ✓ |
| non-volume `o_*` rows | — | all byte-identical (+0.00%) | byte-identical | ✓ |
| `w_*` rows | — | all byte-identical (+0.00%) | identical | ✓ |
| `o_volumes` median | 11.40 s | 9.22 s | "<2%" | ✗ partially |

The timing prediction needs the honest reading: absolute medians moved
with session-level machine variance (both engines' m3-ST rows ~21% faster
this session, MT rows flat), so the window's true cost is the within-run
volumes-vs-nonvolume ratio: **+2.3% → +5.9%** — the 8 MiB window's
match-search cost is ~3.6% relative, more than the <2% predicted. Sizes
were the contract; they landed. Extraction rows are within session noise
(decode path untouched).

**Tests.** `volume_tests` pins the shared table, per-method
volume/non-volume header `win_size` agreement (m0..m5, dict 0), the pow2
clamp agreement, the `-md` legacy/exact/non-pow2 (`unp_ver = 1`) rows, the
3 MiB-redundancy-period regression (volume set within 1.2x of non-volume;
fails against the 2 MiB default by construction), and the solid window
clamp in both directions. 47/47 ctest green; full gate green (Release
build + ctest + interop incl. multivolume cross-extraction). Entry closed.
