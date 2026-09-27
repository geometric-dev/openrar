# Security Policy

OpenRAR parses untrusted archives: the extractor, the DLL, the WASM module,
and the SFX runtime all process attacker-controllable bytes. Reports about
memory safety, path escape, resource exhaustion, or crypto correctness in
those surfaces are security reports, and this file says how to send them.

## Supported versions

Security fixes land on `master` and ship in the next tagged release. Only the
most recent tagged version is supported — there are no backport branches
(PATCH is a monotonic commit counter, see `docs/versioning.md`, so "upgrade to
latest" is always the fix path).

## How to report

Use **GitHub private vulnerability reporting**:
[geometric-dev/openrar → Security → Report a vulnerability](https://github.com/geometric-dev/openrar/security/advisories/new).

**Do not open a public issue for a suspected vulnerability.** In particular,
do not attach a potentially malicious archive to a public issue or PR — the
bug-report template's guidance (describe the structure, don't attach the
binary) applies doubly here.

Please include:

- Version or commit (the CLI prints `--version`; PATCH identifies the exact
  commit).
- Build preset and platform (`windows-msvc`, `linux-gcc`, `wasm`, …).
- A minimal reproducer: the smallest archive or input that triggers it. A
  description of the archive structure (header types, flags, sizes) is often
  enough and is preferred when the sample is hostile.
- Expected vs. actual behavior; crash output or sanitizer log if you have one
  (the nightly fuzz workflow's ASan/UBSan configuration is a useful reference
  for a clean repro).

## What's in scope

- The RAR 5.0 / RAR 7 parser and decompressors (`src/format/`,
  `src/compress/`) on malformed or hostile input.
- Extraction containment: path traversal, link handling, collision and
  metadata behavior (`docs/EXTRACTION_CONTRACT.md`,
  `docs/SECURITY_ARCHITECTURE.md` describe the intended guarantees — a
  divergence from those documents is a bug in either place).
- Resource limits: the RAM ceiling invariants, extraction caps, the WASM
  memory and 500 KiB size budgets.
- Crypto correctness: AES-256-CBC, BLAKE2sp, PBKDF2, CRC32, Reed-Solomon
  recovery (`src/crypto/`, `src/recovery/`).
- The C ABI / WASM ABI boundaries and the SFX runtime policy engine.
- **Library embedders:** the library keeps its containment floors
  (stream-output cap, KDF ceiling, path containment) strictly
  non-disableable — `docs/abi-freeze.md` §8 and
  `docs/dll-integration-spec.md` §13 describe exactly what is a floor and
  what is a caller-tunable budget. A divergence is a security bug. Note the
  documented residual risk: in-process embedding inherits parser risk in the
  host process — embedders processing untrusted archives should wrap their
  host in their own OS-level sandbox.

## What's out of scope

- Social engineering (the SFX consent prompts exist precisely because a
  scripted "yes" is not a security boundary the code can enforce).
- Denial of service by feeding gigabyte-scale inputs to the interactive CLI
  where documented behavior already bounds memory independently of input
  size.
- Reports that require modifying the host OS or the archive-testing tool
  (WinRAR/UnRAR) itself.

## Hardening posture, for context

All crypto and math is in-tree (no third-party dependencies), the tree builds
warning-clean with `-Wall -Wextra` / MSVC `/W4`, and a nightly workflow fuzzes
the parsers with libFuzzer under ASan/UBSan
(`.github/workflows/fuzz.yml`). Fuzz findings that reproduce are treated as
security-relevant by default and triaged as P1s. Local fuzzing against your
own copies is welcome; there is no hosted infrastructure to test against.
