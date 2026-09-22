# Retained fuzz crash artifacts

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
