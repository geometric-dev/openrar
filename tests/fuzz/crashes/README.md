# Retained fuzz crash artifacts

## parallel_fallback_dup_input.bin (2026-10-23)

`fuzz_target` bitflip-diff lane, 37-byte input, deterministic:
`ORACLE FAILURE [bitflip-diff]: decoder emitted past dest_size
(a=1081382 b=1052692)`. The parallel applier's span-0 emitted 557075 bytes
and then failed; `decode_entry` only advances `total` at span boundaries,
so the failure looked pre-emission (`total == 0`) and it fell back to
`run_sequential()`, which re-decoded the same member into the same consumer
(`557075 + 491539 + 32768 = 1081382`). Fixed: the fallback now keys off
bytes actually handed to the consumer (`emitted == 0`), never retrying
after emission.

- Reproduce: `fuzz_target tests/fuzz/crashes/parallel_fallback_dup_input.bin`
  (build with -DOPENRAR_FUZZ=ON); pre-fix it aborts in the validator, post-fix
  it exits 0.

## roundtrip_divergence_727_input.bin / _compressed.bin (2026-09-22)

Deterministic-roundtrip fuzzer (rng(42), iteration 727, 80519 bytes, no
filters — filter-free LZ path). Roundtrip diverges: decoded output differs
from input at offset 65535. Cross-validation mode additionally reports a
window/source mismatch at pos 12364 (dist 12114) — the decoded window
diverges from the source well before the final memcmp.

- Reproduces on v1.21.0 (pre-existing; NOT introduced by the
  v1.21.1-v1.22.0 work).
- Reproduce: build with -DOPENRAR_FUZZ=ON -DOPENRAR_CROSS_VALIDATE=ON and
  run fuzz_roundtrip.exe (sweep aborts via harness dump), or decompress
  _compressed.bin with Decompressor50(win=4 MiB) and diff against _input.bin.
- Status: **FIXED** (v1.21.25) — encoder/decoder filter chunk-length mismatch
  (`filter_max_chunk()`); see docs/ROADMAP.md "CLOSED P1". The fixtures are
  kept as replay inputs for the nightly gate.

## stream_filter_stale_window_input.bin (2026-10-27)

`fuzz_target` roundtrip lane, 55158-byte input, deterministic: lane 1,
method 1, 64 KiB window, delta filter (1 channel), 256 KiB source fed through
`StreamEncoder` in random chunks:
`ORACLE FAILURE [roundtrip]: StreamDecoder disagrees with one-shot decode
(a=0 b=0, input=55158 bytes)`. Root cause: a delta-filter region that spans a
block boundary completes inside the next block, but the apply engine only
flushed on the `flush_cadence()` schedule (32 KiB for a 64 KiB window). By the
next cadence tick `abs_pos() - region_start >= win_size_`, so
`flush_pending()` rejected the region as stale and `StreamDecoder` emitted
nothing. Fixed: flush as soon as the head filter region completes
(`has_completed_filter()`), not just on the cadence tick.

- Reproduce: `fuzz_target tests/fuzz/crashes/stream_filter_stale_window_input.bin`
  (build with -DOPENRAR_FUZZ=ON); pre-fix it prints the ORACLE FAILURE above,
  post-fix it exits 0.

