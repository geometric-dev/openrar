# Changelog

All notable changes to OpenRAR are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.121] - 2026-09-10

First tagged release, cut 121 commits past the v1.0.0 baseline. Entries cover
everything since the 2026-08-31 baseline where WinRAR interoperability was
restored and verified and compression speed parity was regained; earlier
development history is not itemized here.

### Added

- **Shared library**: stable C ABI (`openrar_dll.h`) with a C++ wrapper,
  architecture-suffixed binaries (`openrar_x64` / `openrar_arm64`), and an
  integration spec with C#, Python, and Rust examples.
- **WebAssembly build**: in-memory RAR5 create/list/extract with a handle-based
  JS API and a canonical interface contract.
- **Streaming**: incremental block encoder, streaming decompression with
  extraction flush callbacks, and buffered in-memory archive read/write.
- **Multi-volume archives**: streamed multi-volume create, plus `.rev` recovery
  volumes with generation and repair matching WinRAR's shard layout.
- **SFX creation** (`-sfx[name]`): SFX module prepended on create.
- **Header encryption** (`-hp`): supported on write and read.
- **Extended metadata**: nanosecond high-precision times plus OWNER and
  VERSION header extras.
- **Interop gate** (`tools/interop_gate.py`): self-roundtrip, WinRAR
  cross-decode, and a full ctest run, enforced as a pre-commit hook and in CI;
  covers multi-volume and SFX archives.
- **Performance**: compression runs ~2-4x faster than WinRAR on the 50 MB
  benchmark suite; ~2x faster PBKDF2 via cached HMAC midstates; ~8x filter
  speedup on non-SIMD paths; faster crc64 and header reads; leaner per-literal
  token storage.
- **Test suite**: property-based tests, a deterministic shape-aware roundtrip
  fuzzer, a libFuzzer-ready decoder harness, decompression cross-validation,
  WinRAR conformance scripts, golden files, and known-answer tests.
- **Release process**: SemVer versioning with the patch component as a
  monotonic commit counter (`docs/versioning.md`), this changelog, and
  `project(VERSION)` as the single version source of truth.

### Fixed

Safety and robustness:

- Zip-Slip path traversal on extraction; hardlink and FILECOPY sources
  confined to the extraction root; archive-controlled names hardened for
  Windows filesystems; planted-symlink truncation blocked via unique temporary
  names; recovery-record directory conversion only touches links the reader
  itself created.
- Malformed-archive hardening: vint field validation, attacker-controlled size
  clamping, 64-bit recovery-record geometry, decompressor filter/window
  accounting, buffer-extract output budgets, and AVX2 dispatch gated on
  OS-level AVX state (OSXSAVE/XCR0).
- Crypto: constant-time password-check comparison, PBKDF2 iteration-count
  guard, SHA-NI digest self-check, and password-derived material wiped on
  teardown.
- C ABI: escaping C++ exceptions caught at every `extern "C"` boundary, struct
  packing hygiene, and stream callbacks dispatched outside the map mutex.

Correctness:

- WinRAR interoperability restored (absolute Huffman table lengths, BC max
  bits, window-size sync) and locked in by the interop gate.
- Copy-match overlap undefined behavior; encoder corruption past the window
  size on external-buffer sources; large-file compression (empty blocks,
  memory streaming, RLE overflow); compressor state reuse across instances;
  solid-entry decoder state persistence; stored-entry CRC32/BLAKE2sp
  verification.
- Data-loss safety: archive replacement made data-loss-safe; EINTR-safe POSIX
  I/O; 32-bit MSVC intrinsic fixes.

### Changed

- CI builds and publishes the shared library across the matrix and runs the
  hardened interop gate (strict multi-volume cross-check, no weak fallback).
- Volume-chain cap raised to 65535 volumes; SFX size bound raised to 64 MiB.

## [1.0.0] - 2026-08-31

Baseline (development state, not distributed): clean-room RAR5 archiver with
WinRAR interoperability verified by cross-decode and compression running 2-3x
faster than WinRAR on the 50 MB benchmark suite.
