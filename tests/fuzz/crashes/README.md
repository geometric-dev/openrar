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
  (build with -DOPENRAR_FUZZ=ON); pre-fix it aborts in the oracle, post-fix
  it exits 0.

## roundtrip_divergence_727_input.bin / _compressed.bin (2026-09-22)

Deterministic-roundtrip fuzzer (rng(42), iteration 727, 80519 bytes, no
filters — filter-free LZ path). Roundtrip diverges: decoded output differs
from input at offset 65535. Cross-validation mode additionally reports a
window/source mismatch at pos 12364 (dist 12114) — the decoded window
diverges from the source well before the final memcmp.

- Reproduces on v1.21.0 and master (pre-existing; NOT introduced by the
  v1.21.1-v1.22.0 work).
- Reproduce: build with -DOPENRAR_FUZZ=ON -DOPENRAR_CROSS_VALIDATE=ON and
  run fuzz_roundtrip.exe (sweep aborts via harness dump), or decompress
  _compressed.bin with Decompressor50(win=4 MiB) and diff against _input.bin.
- Status: OPEN P1 — core codec roundtrip divergence. Blocks the v1.22.0
  release (docs/ROADMAP.md stabilization gate).
