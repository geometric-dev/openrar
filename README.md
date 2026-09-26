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
| **Not yet** | RAR 7.0-style recovery-record vintage (0x11D), POSIX extended attributes & MotW (`-oz` — roadmap v1.27), RAR 5.0 compression v1 streams (write-side) |

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
| `-os` | Save and restore NTFS Alternate Data Streams (ADS) as `STM` service blocks via `win32stm.cpp`. |
| `-ol` | Save symbolic links and junctions as `FHEXTRA_REDIR` records. Links are default-deny on extraction; `-ol` opts in (v1.24 §6.1). |
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

OpenRAR is heavily optimized and often outperforms the official WinRAR engine in single-threaded workloads. The following benchmarks compare OpenRAR to WinRAR (`rar.exe -m3 -mt1`) on canonical compression workloads:

**50 MB Canonical Payload (Mixed Text, Code, Binary)**
- **OpenRAR (`-m3`)**: 2.32s (16.02 MB)
- **WinRAR (`-m3`)**: 5.22s (16.03 MB)
*(OpenRAR is ~2.25x faster)*

**1 GB Canonical Payload (1/3 Text, 1/3 Random, 1/3 Zeros)**
- **OpenRAR (`-m3`)**: 44.56s (341.74 MB)
- **WinRAR (`-m3`)**: 180.40s (342.01 MB)
*(OpenRAR is ~4x faster)*

Both archives achieve seamless cross-extraction interop, validating exact mathematical compression bounds.

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
