# OpenRAR — Modern RAR 5.0 Archiver

> **OpenRAR** is a modern C++17 RAR 5.0 archiver featuring complete archive creation (store + LZ/Huffman compression), decompression, mutation, recovery, and inspection capabilities.

---

## Status

| Capability | State |
|---|---|
| **Extract / Test / List** (`x`, `e`, `t`, `l`, `v`, `p`) | fully supported — all RAR 5.0 filters, multi-volume, solid streams, AES-256 decryption |
| **Create — store** (`-m0`) | stable, deterministic, WinRAR-verified |
| **Create — compressed** (`-m1` … `-m5`, RAR 5.0 v0) | stable, deterministic, WinRAR-verified |
| **High-precision times** | `FHEXTRA_HTIME` (FILETIME) via `-tsm/-tsc/-tsa` |
| **Solid archive** (`-s`) | `MHFL_SOLID` + per-file `FCI_SOLID`, shared sliding dictionary window |
| **Archive comment** (`-z<file>`) | `CMT` service header, UTF-8 |
| **Archive metadata** (`-ams`) | `MHEXTRA_METADATA` (name + FILETIME) |
| **Quick-open + locator** | `QO` service caching file headers + `MHEXTRA_LOCATOR` patched in main header |
| **Encryption** (`-p`/`-hp`) | AES-256 CBC + PBKDF2; file-level and header-level encryption |
| **Recovery record** (`-rr[N%]`) | Reed-Solomon GF(2¹⁶) Cauchy parity via `rs16`; `MHEXTRA_LOCATOR` RROffset; repair command `r` |
| **Recovery volumes** (`-rv`) | External `.partNN.rev` parity volumes for multi-volume sets; reconstructs missing/corrupt data volumes via `r` |
| **Unix owner/group** (`-og`) | `FHEXTRA_OWNER` UID/GID + names, restore opt-in (`-ow`/`-og`) |
| **NTFS ACLs** (`-ow`) | Save and restore NTFS access control lists |
| **NTFS Alternate Data Streams** (`-os`) | Save and restore alternate streams (`STM` service blocks) |
| **Redirections / symlinks** (`-ol`) | `FHEXTRA_REDIR`; file/dir symlinks + junctions |
| **Multi-volume** (`-v<size>`) | `.partNN.rar` naming; per-slice CRC/BLAKE2 MACs; encrypted multi-volume |
| **SFX scripting** | v1.23: directive engine with consent framework, TempMode hardening, runtime process policy |
| **Extraction containment** | v1.24: syscall-level path containment (write-through-handle), atomic extraction with journal manifests, collision rejection, `--json-summary` |
| **Mapped read engine** | v1.25: memory-mapped listing/header-scan (`--no-mmap` forces buffered; extraction always buffered) |
| **Archive mutation** (`d`/`u`/`f`/`m`/`k`) | delete, update, freshen, move, lock — QO/locator stripped on mutation |
| **CDC packing** (`-cdc`) | v1.26: content-defined chunking drives solid-chain ordering (window-bounded reduction, measured against a plain-solid baseline) — ordinary RAR5 solid streams, WinRAR/UnRAR-verified |
| **Filter switches** (`-mc`) | `-mcE`/`-mcD`/`-mcL`/`-mcX` parsed and forwarded; DELTA/E8 applied by decoder |
| **File versioning** (`-ver[n]`) | Versioned adds keep N versions of the same name (`FHEXTRA_VERSION`); extraction filter `-ver<idx>` |
| **POSIX/macOS extended attributes** (`-ox`) | v1.27: `FHEXTRA_XATTR` (0x08) records — `user.*`/`security.*`/`trusted.*` + macOS Finder-tag namespaces; restore allow-listed namespaces (`--xattr-security` opts in to `security.*`/`trusted.*`); stock WinRAR/UnRAR skip the records (Track 10) |
| **Mark of the Web / quarantine** (`-oz`) | v1.27: propagation keyed on the archive file's own Zone.Identifier ADS (Windows) / `com.apple.quarantine` (macOS); mark content generated locally — never parsed from archive-provided streams; `-oz-` disables |
| **Interactive TUI** | v1.28: dual-progress renderer (overall + current-file bars) on TTYs — terminal-injection hardening is a release gate (every rendered string passes the §7.1 sanitizer, including the archive comment, owner names and the JSON path); ESC/q/^C cancel cooperatively (exit 255, temps swept); width-clamped lines, UTF-8-safe truncation; non-TTY (pipe/file/CI) renders plain per-file lines with zero escape bytes and identical exit codes |
| **Benchmark engine** (`openrar_bench`) | v1.28: warm-up + median-of-7 protocol with spread reporting, hardware disclosure, `--json` (schema v1) for CI trends, `--strict` opt-in variance gate; CDC three-number reduction suite (rollover closed) |
| **Sandboxed worker** | v1.30: the parse/decode engine runs in a sandboxed worker process — Windows AppContainer (empty capability set) / Linux seccomp-BPF allowlist — with the broker holding every file handle and enforcing every cap; the worker cannot open, write, or execute anything. `--in-proc` / `OPENRAR_IN_PROC=1` forces in-process; a worker that cannot be engaged falls back in-process with a loud `W:` notice (once per run). Encrypted and multi-volume archives parse in-process (worker scope: single-volume, unencrypted) |
| **Python & C# SDKs** | v1.30: `openrar` (PyPI, ctypes over the frozen C ABI — no compiled extension) and `OpenRAR.NET` (NuGet, P/Invoke) — both gated by the normative cross-binding conformance suite (`python_conformance` ctest + the xUnit port); loader performs the ABI-version + feature-mask probes |
| **ABI freeze** | v1.30: the C ABI surface is frozen and machine-enforced — layout goldens, export-list parity, feature-mask parity, JS error-code mirror (`docs/abi-freeze.md`); `OPENRAR_DLL_API_VERSION` stays 1 (additive-only) |
| **Supply chain** | v1.30: release artifacts carry SHA256SUMS, CycloneDX SBOM, and Sigstore keyless build-provenance attestations; verification instructions in `SECURITY.md` |
| **Archive migration (`cv`)** | v1.29: transcode ZIP / TAR (ustar + documented GNU/pax subset) / GZIP (multi-member = one concatenated entry) into RAR 5.0 through the shipped writer pipeline; ZIP CD-vs-LFH mismatch aborts before any output (spec 11); names normalized + percent-encoded losslessly (the escaped name IS the name); collision classes refused pre-output; unconditional per-entry BLAKE2sp roundtrip verify; `-df` deletes the source only on a 100% verified migration; `--json-summary` schema v2 fields (`format`/`verified`/`source_deleted`) |
| **FILECOPY materialization** | v1.29: `-oi` FILECOPY references (FHEXTRA_REDIR type 5, an in-archive copy directive) materialize by default at extraction — decoupled from the `-ol` links opt-in; symlinks/hardlinks/junctions stay default-deny; the caps debit (v1.27) applies |
| **Not yet** | Multi-erasure RR repair and scale>1 record reassembly (inline repair currently restores a single damaged unit in single-shape records; the MDS parity supports more — `docs/v1.35.0-rr-probe.md` §8), resource forks & FinderInfo (2.1), RAR 5.0 compression v1 streams (write-side), foreign-format WRITE side (ZIP/TAR/GZIP emission), legacy RAR (1.5–4.0) migration incl. header parsing, foreign-format decryption (ZipCrypto/AES-ZIP), `cv` `-s/-v/-ts` output shaping |

---

## How to create a RAR from the command line

### Build

#### Standard CMake (Windows, Linux, macOS with GCC, Clang, or MSVC)

```bash
# Configure & build Release binaries
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

# Run all unit tests
ctest --test-dir build -C Release --output-on-failure
# → OpenRAR binary at build/openrar64/Release/openrar.exe (Windows) or build/openrar (Linux/macOS)
```

#### POSIX Makefile (Linux / macOS)

```bash
make -j$(nproc)
```

#### Visual Studio 2022 / VS Code
Open the repository folder directly — CMake and `CMakePresets.json` are automatically detected.

### Basic create

```bash
# Archive a folder (recurse is default for `a`)
openrar a -r archive.rar ./myFolder

# Archive explicit files
openrar a photos.rar ./pics/*.jpg

# Store without compression (fastest, largest)
openrar a -m0 -r store.rar ./src

# Best compression (slowest, smallest) — default is -m3
openrar a -m5 -r best.rar ./src
```

### Switches you can use

| Switch | Effect |
|---|---|
| `-m0` … `-m5` | 0 store, 1 fast … 5 best (`-m3` default). Persisted as `method` bits in every file header. |
| `-md<n>` | Dictionary hint (e.g. `-md1m`, `-md4m`, `-md1g`). Quantized to 128 KB–4 GB, informs `WinSize` in `FCI` bits. RAR 7.0 fractional/non-power-of-two sizes accepted (nearest discrete step). |
| `-ver[n]` | File version control: versioned adds keep `n` versions per name; `-ver<idx>` extracts that version. |
| `-s` | Solid archive (`MHFL_SOLID` + `FCI_SOLID`). Shares one `Compressor50` window across files; stored/empty files don't break the chain. |
| `-r` | Recurse subdirectories. Default for `a` is already recursive; `-r0` limits to wildcards. |
| `-ed` | Omit directory records — files still archived with their paths; empty-dir info is lost (rar.exe semantics). |
| `-ep` | Strip all paths — store bare filenames. |
| `-df` / `-dr` / `-dw` | Delete successfully archived sources: plain / to Recycle Bin / wipe (zero overwrite → truncate → temp-name → delete). `m` implies plain delete; explicit switch overrides. |
| `-oi[0-4][:<size>]` | Identical files as references via `FILECOPY` (0 off, 1 silent, 2 list, 3 list+exit, 4 dup-list+exit; default 64 KB comparison threshold; stored as `FHEXTRA_REDIR` type 5 with no data area — the first stored copy is referenced; materializing references on extraction follows the `-ol` links opt-in). In solid archives the run breaks around reference entries. |
| `-cdc` | CDC-driven solid-chain packing (v1.26): chunk-hash affinity ordering of solid runs + identical-file references (implies `-s` and `-oi1`; explicit `-oi0` wins). Pack-time report `cdc: logical/packed/window/flag`; the fingerprint index caps at 2M entries with an original-order fallback for the tail (reported). `-ver` and `-v` refused. |
| `-ep1` | Strip the common base (as typed). |
| `-ep2` | Save full path minus drive letter. |
| `-ep3` | Full path with drive. |
| `-z<file>` | Store archive comment from UTF-8/ANSI file (`CMT` service). |
| `-ams` | Store archive metadata (original name + creation FILETIME) as `MHEXTRA_METADATA`. |
| `-ts[m][c][a][4]` | High-precision times: `-tsm4 -tsc4 -tsa4` adds `FHEXTRA_HTIME` with `ctime`/`atime`. Default stores `mtime` only. |
| `-qo-` | Disable quick-open. Default writes `QO` + locator. |
| `-htb` / `-htc` | File checksum: BLAKE2sp or CRC32 (`FHEXTRA_HASH`). Default CRC32. |
| `-log[fmt][=name]` | Write archive (`A`) and processed file/dir (`F`) names to `name` (default `rarinfo.log`); `P` appends, `U` writes UTF-16LE. Create path. |
| `-tk[<date>]` / `-tl` | Archive mtime: keep original on update, set `YYYYMMDDHHMMSS`, or set to newest stored file. Applied by `a` on close. |
| `-p[<pwd>]` / `-hp[<pwd>]` | Encryption: AES-256 CBC with PBKDF2 (`crypt5`). `-p` encrypts file data; `-hp` encrypts headers and file data. |
| `-rr[N[%]]` | In-archive recovery record: Reed-Solomon GF(2¹⁶) parity (`rs16`) protecting up to RR header; repaired via `openrar r`. |
| `-rv` | External `.rev` recovery volumes for multi-volume sets (`-v` required); rebuilt data volumes via `openrar r`. |
| `-ow` | Save and restore NTFS file security and access control lists (ACLs) via `win32acl.cpp`. |
| `-os` | Save and restore NTFS Alternate Data Streams (ADS) as `STM` service blocks via `win32stm.cpp`. Zone.Identifier is excluded: it is transport provenance, not content — propagation is the `-oz` policy. |
| `-ol` | Save symbolic links and junctions as `FHEXTRA_REDIR` records. Links are default-deny on extraction; `-ol` opts in (v1.24 §6.1). |
| `-ox` | Save POSIX/macOS extended attributes into the header's `FHEXTRA_XATTR` (0x08) record: `user.*`, `security.*`, `trusted.*`, `com.apple.metadata.*` (Finder tags). `system.*` (ACL side door), quarantine/provenance namespaces and resource forks are never stored. Over-cap attributes (name > 255 B, value > 64 KiB, > 1 MiB per file) are skipped, never truncated. Symlinks/hardlinks carry no records. Restore is automatic for `user.*`/`com.apple.metadata.*`; `--xattr-security` additionally restores `security.*`/`trusted.*` (admin opt-in, the `--preserve-suid` model). Stock WinRAR/UnRAR skip the record without error. |
| `-oz` / `-oz-` | Mark-of-the-Web propagation at extraction (default ON; `-oz-` disables). When the archive file itself carries a Zone.Identifier ADS (Windows) or `com.apple.quarantine` (macOS), each extracted file receives a freshly generated mark — content is generated locally (`[ZoneTransfer]
ZoneId=N
`); the archive's HostUrl/ReferrerUrl never travel; an existing stronger mark is never removed or downgraded. Zone streams stored inside the archive (WinRAR `-os` shape) are never restored. Extraction-side only: nothing is emitted into the archive. |
| `--json-summary[=path]` | Machine-readable per-entry extraction report (v1.24); without `path`, stdout carries only JSON. |
| `--no-mmap` | Force the buffered scan engine (v1.25; the mapped engine is default-on for listing). |
| `-v<size>` | Create multi-volume split archive (`.part01.rar`, etc.) with per-slice checksums and MACs. |
| `-mc[params]` | Compression filter control: `-mcE` (x86 E8/E9), `-mcD` (Delta), `-mcL` (Long range), etc. |
| `-y` | Assume Yes (no prompts), `-o+` overwrite. |

### More examples

```bash
# Solid archive of a source tree, keep directory structure
openrar a -s -m5 -r src-solid.rar ./src

# Archive with comment and high-precision timestamps for all three times
openrar a -m3 -r -tsm4 -tsc4 -tsa4 -z notes.txt timed.rar ./docs

# Archive name/time embedded (non-deterministic: embeds creation time)
openrar a -ams -r meta.rar ./data

# List, test, extract what you just created
openrar l archive.rar        # short list
openrar v archive.rar        # verbose
openrar t archive.rar        # test (hash-verified)
openrar x archive.rar ./out/
```

### Migrate a foreign archive into RAR 5 (`cv`)

```bash
openrar cv source.zip                  # -> source.rar
openrar cv source.tar migrated.rar     # explicit destination
openrar cv -m5 -rr3 backup.tar.gz b.rar # output compression + recovery record
openrar cv -df old.zip old.rar         # delete source after a verified migration
```

Sources: ZIP (store/deflate, ZIP64, UTF-8+cp437 names), TAR (ustar + GNU long
names + a documented pax subset; sparse refused per entry), GZIP (multi-member
files migrate as one concatenated entry). Switches: `-m0..-m5`, `-md<n>`,
`-p`/`-hp` (output encryption only), `-rr[N%]`, `-o+`/`-y`, `-df`,
`--json-summary[=path]`. Output-shaping switches (`-s`, `-ts*`, `-v`, `-oi`,
`-ep*`, `-z`, ...) are refused with exit 7 rather than silently ignored. The
emitted RAR 5.0 archive is readable by WinRAR/UnRAR/7-Zip (interop Tracks
11-15); every entry is roundtrip-verified (BLAKE2sp) before `cv` reports
success.

Magic bytes: every archive starts `52 61 72 21 1A 07 01 00`.

---

## Verification

### Automated tests (Node 24 `node:test`)

```bash
node tools/run-tests.cjs      # 102 assertions across 22 suites (100% pass)
```

Runs full regression test suite covering:
- Format verification (headers, locator, quick-open, extra records, timestamps)
- Dual-oracle extraction round-trips against WinRAR
- Archive mutation (`d`, `u`, `f`, `m`, `k`)
- Reed-Solomon GF(2¹⁶) Cauchy recovery records (`-rr`) and repair (`r`)
- NTFS Alternate Data Streams (`-os`) and symlinks (`-ol`)
- Multi-volume archive creation and multi-volume extraction (`-v`)
- AES-256 CBC data and header encryption (`-p`, `-hp`)

---

## Performance

Fresh protocol-backed comparison vs WinRAR 7.20 (full matrix, methodology,
and the honest losses in **[PERFORMANCE.md](PERFORMANCE.md)**):
deterministic seeded corpora, warm-up + median-of-3, every archive verified
by both engines' extractors hash-identically. Host: i7-7500U laptop,
Windows 11, WinRAR 7.20 x64.

| Workload (135.5 MB canonical = text + code + binary) | OpenRAR | WinRAR 7.20 |
|---|---:|---:|
| `-m3` single-thread | **8.4 s** (16.1 MB/s) | 16.5 s (8.2 MB/s) |
| `-m3` `-mt4` | **5.7 s** (23.7 MB/s) | 7.4 s (18.2 MB/s) |
| `-m0` store | **0.11 s** | 0.22 s |
| `-m5` single-thread | **27.6 s** (4.9 MB/s) | 38.1 s (3.6 MB/s) |
| solid `-m3` | **8.7 s** | 17.7 s |
| multivolume `-m3 -v32m` | **10.6 s** | 16.6 s |
| 1 GB `-m3 -mt4` | **47.8 s** | 69.3 s |

- **Compression at the default `-m3`: 1.3–2.0x faster than WinRAR**, ratios
  within ~3%. **`-m5` is now faster than WinRAR too (1.4x)** — v1.30 was
  2.3x slower (match-finder walk depth, see PERFORMANCE.md).
- **Multi-threading (`-mtN`) was switched back on in v1.32.0** after an
  inverted enablement condition left it disabled for all filter-free input —
  so the `-mt4` figures above predate the fix and the published 1.48x
  scaling was not real. Re-measured: **2.0x on data without long-range
  redundancy, at +0.08% size.** On redundant input our chunking loses
  cross-boundary matches that WinRAR's keeps (9x on the benchmark's code
  third), which a redundancy probe bounds for corpus-shaped input but does
  not eliminate. See PERFORMANCE.md and CHANGELOG "Known issues".
- `-cdc` dedup produced a **32% smaller archive** on a duplicate-heavy
  corpus (no WinRAR equivalent).
- **Honest losses:** WinRAR's `-m1` is slightly faster and ~18% tighter
  (structural — swept the full effort grid in v1.31); our `-m5` ratio is
  ~3% looser; extraction trails UnRAR/WinRAR by ~2.3x on this workload
  (the crash-safety contract, below).

**Measurement protocol (v1.28).** Machine-collected numbers come from the
`openrar_bench` tool: every suite runs 1 untimed warm-up pass plus 7 timed
passes (3 with `--quick`), reports the MEDIAN and the spread
(`(max−min)/median`), and discloses the host (CPU brand, cores, OS,
compiler) in its output and `--json` document. The <5% variance claim is
scoped to compute suites on a quiescent host (no concurrent load, fixed
power plan); I/O-bound suites (listing, extract/add throughput) are labeled
`kind:"io"` and excluded — disk cache state dominates them. `--json`
(schema version 1) is the CI-trend artifact; `--strict` turns a compute
suite spread above `--spread-threshold-pct` (default 5) into exit 1 for
hosts that want the gate. The tool is informational by default (exit 0).
The CLI-vs-CLI matrix above uses the sibling protocol from
`tools/perf_vs_winrar.py` (warm-up + median-of-3, cross-extraction
verification); raw data in `tools/perf/results.json`.

---

## Building

OpenRAR adheres to modern C++17 standards and can be built cleanly on any platform using standard tools:

### CMake (Cross-platform standard: Linux, macOS, Windows)

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### GNU Make (Linux / macOS)

```bash
make -j$(nproc)
```

### MSBuild (Windows Developer Command Prompt)

```bash
msbuild openrar.vcxproj /p:Configuration=Release /p:Platform=x64
# → build/openrar64/Release/openrar.exe
```

### WebAssembly / Browser (Emscripten)

The block codec (compressor + decompressor) is also available as a `MODULARIZE` JavaScript module with embind + a stable C ABI (`src/wasm/wasm_api.{hpp,cpp}`). Native flags (`/O2`, MSVC `/GL`, `RAR_SMP`, `Threads::Threads`) are kept separate from the WASM configuration — `cmake --preset wasm` only kicks in under `emcmake`.

```bash
# One-time: install emsdk
git clone https://github.com/emscripten-core/emsdk.git
./emsdk/emsdk install latest && ./emsdk/emsdk activate latest
source ./emsdk/emsdk_env.sh

# Build
emcmake cmake --preset wasm
cmake --build --preset wasm          # → wasm/dist/openrar.{js,wasm}

# Debug
emcmake cmake --preset wasm-debug
cmake --build --preset wasm-debug

# Full CLI with MEMFS (for archive-level a/x/t from the JS host)
emcmake cmake --preset wasm-cli -DOPENRAR_WASM_CLI=ON
cmake --build --preset wasm-cli      # → wasm/dist/openrar_cli.{js,wasm}

# Makefile-only path (no CMake needed)
make wasm                            # same outputs as `--preset wasm`
make wasm-cli
```

Usage from Node:

```js
import createOpenRAR from '../wasm/dist/openrar.js';
const m = await createOpenRAR();
const compressed = m.compress(new TextEncoder().encode('hello'), 3);
const restored   = m.decompress(compressed);
```

The `wasm_api_tests` ctest target runs the C ABI surface (`openrar_compress2`/`openrar_decompress2`) on every CI matrix — emscripten is not required to regression-test the bindings.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the clean-room ground rules, code
style (enforced by `.clang-format` / `.clang-tidy`), the warning policy, and
commit-message conventions. Code style details live in
[ARCHITECTURE.md](ARCHITECTURE.md).

---

## Acknowledgements & Licence

**OpenRAR** is released under the **MIT Licence** — see [LICENSE](LICENSE).

We extend our deep gratitude to the open-source community for their efforts in researching and documenting the RAR 5.0 structure.
Special thanks to:
- [**rar-research**](https://github.com/bitplane/rar-research) by *bitplane* for their extensive format documentation and parsing research.

For notices regarding third-party cryptography software (including BLAKE2sp and fast PCLMULQDQ CRC32 algorithms), please refer to `THIRD_PARTY_NOTICES.md`.

**Trademark notice:** OpenRAR is an independent, community implementation of the
published RAR 5.0 archive format. It is not affiliated with, endorsed by, or
connected to RARLAB or win.rar GmbH. "RAR" and "WinRAR" are trademarks of their
respective owners; references to the RAR format are solely for interoperability
and identification purposes.
