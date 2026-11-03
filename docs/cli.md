# 📖 OpenRAR Command-Line Interface Reference Manual

**Tool:** `openrar` (Version 2.0.x LTS)  
**Specification:** Conforms to `ARCHITECTURE.md`, `docs/EXTRACTION_CONTRACT.md`, and `docs/SECURITY_ARCHITECTURE.md`.

---

## 1. Synopsis & Syntax

```bash
openrar <command> [switches] <archive> [files...] [@listfile] [path_to_extract]
openrar b [switches] [--submit] [--system=<desc>]
openrar inspect [switches] <archive>
openrar diff [switches] <archive1> <archive2>
openrar help [<command>]
```

* **Command**: Exactly one primary action character or keyword (e.g. `a`, `x`, `b`, `inspect`, `diff`).
* **Switches**: Zero or more options prefixed by `-` or `--`. Switches may appear in any order.
* **Archive**: Target `.rar` archive path (or foreign archive for `cv`).
* **Files / Wildcards**: Optional file paths or patterns to add, extract, or test.
* **Listfiles (`@list`)**: Plaintext list file containing UTF-8 filenames, one per line (lines starting with `#`, `;`, or `//` are treated as comments).

---

## 2. Primary Commands

| Command | Name | Description |
|---|---|---|
| `a` | **Add** | Adds files and directories to the archive. Creates a new archive if it does not exist. Recurses subdirectories by default. |
| `x` | **Extract (Full)** | Extracts archived files preserving their full directory tree structure. Subject to path containment rules. |
| `e` | **Extract (Flat)** | Extracts archived files directly into the destination directory, discarding all directory hierarchies. |
| `l` | **List** | Displays standard tabular listing of archive members (size, ratio, date, attributes, name). |
| `v` | **Verbose List** | Displays verbose listing including volume numbers, host OS, and service blocks. |
| `lt` | **Technical List** | Displays comprehensive technical parameters per entry (method, dictionary, CRC32/BLAKE2sp, attributes). |
| `lb` | **Bare List** | Prints only relative archived paths, one per line (ideal for shell scripts and piping). |
| `t` | **Test** | Decodes and verifies integrity of all archived members against CRC32/BLAKE2sp hashes without disk writes. |
| `b` / `bench` | **Benchmark** | In-memory CPU compression/decompression benchmark measuring throughput, scaling, and RARMark score. |
| `inspect` | **Inspect** | Zero-extraction forensic x-ray dissecting physical blocks, service headers, encryption workbounds, and RS16 geometry. |
| `diff` / `cmp` | **Diff** | In-archive structural comparison between two archives without disk extraction. |
| `d` | **Delete** | Removes specified files or wildcard matches from an existing archive. |
| `u` | **Update** | Freshens existing archive files and adds files present on disk but absent in the archive. |
| `f` | **Freshen** | Updates existing archive files that have newer disk timestamps; does not add new files. |
| `m` | **Move** | Archives files and deletes successfully archived sources from disk. |
| `k` | **Lock** | Locks archive against future additions, modifications, or deletions (`MHFL_LOCK`). |
| `r` | **Repair** | Repairs damaged archives using Cauchy RS16 recovery records or reconstructs missing volumes via `.rev` files. |
| `rr[N]` | **Add Recovery** | Adds Reed-Solomon GF(2¹⁶) recovery record (`-rr[N%]`) to an existing archive. |
| `s` | **Convert to SFX** | Converts an existing archive into a self-extracting executable (`.exe`). |
| `p` | **Print** | Decodes and prints contents of archived files directly to stdout. |
| `cv` | **Convert / Transcode** | One-way migration of foreign archives (ZIP, TAR, GZIP) into a clean-room RAR 5.0 archive. |

---

## 3. Comprehensive Switch Matrix

### 3.1 Compression & Dictionary Configuration

| Switch | Syntax & Parameters | Description |
|---|---|---|
| `-m<0..5>` | `-m0` (store) … `-m3` (default) … `-m5` (maximal) | Sets compression level. Stored in header `method` field. Level 0 is store-only (no LZ/Huffman). |
| `-md<size>` | `-md128k`, `-md4m`, `-md1g`, `-md=auto` | Sets compression dictionary window size (128 KB to 1 TB). `-md=auto` dynamically chooses the optimal window based on input file size and compression method. |
| `-s` | `-s` (enable), `-s-` (disable) | Creates a solid archive (`MHFL_SOLID`). Shares the compression sliding window across files for higher ratio. |
| `-cdc` | `-cdc` | Content-Defined Chunking solid chain packing: uses FastCDC gear-hash affinity to cluster redundant files in solid chains. Implies `-s` and `-oi1`. |
| `-mc<params>` | `-mc-`, `-mcE+`, `-mcD+` | Compression pre-processing filter control: `E` = x86 E8/E9 CALL/JMP translation; `D` = Delta byte stride; `-mc-` disables filters. |

### 3.2 Concurrency & Performance

| Switch | Syntax & Parameters | Description |
|---|---|---|
| `-mt<n>` | `-mt0` (auto), `-mt1` (sequential), `-mtN` | Sets worker thread count. On `a`: parallel multi-file batch add; on `x`/`t`: single-member two-phase decode workers; on `b`: benchmark worker threads. |
| `--seconds=<sec>` | `--seconds=5` (default: 5, range: 1..60) | Duration per timed phase in benchmark command `b`. |
| `--durability=batch` | `--durability=batch` / `-db` | Opt-in batch durability: defers journal syncs to boundaries (every 32 appends or LRU eviction). Measured 1.2x–2.0x faster extraction. Default is entry durability. |
| `--no-mmap` | `--no-mmap` | Forces buffered file reading instead of memory-mapped reads (`MappedFile`) for header scanning and listing. |

### 3.3 Benchmark & Leaderboard Submissions

| Switch | Syntax & Parameters | Description |
|---|---|---|
| `--leaderboard` / `--top` | `--leaderboard`, `--top` | Queries and displays the ranked global cloud hardware leaderboard in the terminal. |
| `--submit` | `--submit`, `-submit` | Automatically submits verified benchmark results to the live OpenRAR Cloudflare Workers edge API (`https://benchmark.openrar.workers.dev/submit`). |
| `--system=<desc>` | `--system="Ryzen 9 7950X / 64GB DDR5"` | Custom hardware / rig description attached to the benchmark result for the community leaderboard in `BENCHMARKS.md`. |
| `--submit-endpoint=<url>` | `--submit-endpoint=https://...` | Overrides the default submission API endpoint (for private testing or custom mirrors). |

### 3.4 Encryption & Security

| Switch | Syntax & Parameters | Description |
|---|---|---|
| `-p[<pwd>]` | `-pSecret`, `-p` (prompt) | Encrypts or decrypts archive member data using AES-256 CBC and PBKDF2 HMAC-SHA256. Headers remain plaintext. |
| `-hp[<pwd>]` | `-hpSecret`, `-hp` (prompt) | Encrypts headers and file data (`MHFL_ENCRYPTVER`). The archive cannot be listed without the password. |
| `--in-proc` | `--in-proc` | Forces in-process decompression, bypassing the sandboxed worker process (Windows AppContainer or Linux seccomp-BPF). |
| `--xattr-security` | `--xattr-security` | Opt-in administrative restore for privileged extended attributes (`security.*` and `trusted.*`). Default restores user attributes only. |

### 3.5 Integrity & Recovery Records

| Switch | Syntax & Parameters | Description |
|---|---|---|
| `-rr[N[%]]` | `-rr3%`, `-rr5` (default: 3%) | Appends Cauchy Reed-Solomon GF(2¹⁶) recovery record (`rs16`) protecting against physical corruption. Can repair up to N% damaged sectors. |
| `-rv[N]` | `-rv`, `-rv3` | Generates external `.partNN.rev` recovery volumes for multi-volume archives, allowing reconstruction of missing volumes. |

### 3.6 Filesystem Metadata & Attributes

| Switch | Syntax & Parameters | Description |
|---|---|---|
| `-ts[m][c][a][4]` | `-tsm4 -tsc4 -tsa4` | Stores high-precision timestamps (FILETIME, 100ns resolution) for modified (`m`), created (`c`), and accessed (`a`) times via `FHEXTRA_HTIME`. |
| `-ow` | `-ow` | Saves and restores Windows NTFS Access Control Lists (ACLs) or POSIX owner/group UID/GID. |
| `-os` | `-os` | Saves and restores Windows NTFS Alternate Data Streams (ADS) as `STM` service blocks. |
| `-ox` | `-ox` | Saves and restores POSIX and macOS extended attributes (`user.*`, Finder tags, Resource Forks). |
| `-oz` / `-oz-` | `-oz` (default ON), `-oz-` | Propagates Mark-of-the-Web (Zone.Identifier on Windows, `com.apple.quarantine` on macOS) onto extracted files based on archive transport provenance. |
| `-ol` | `-ol` | Saves symbolic links and junctions as `FHEXTRA_REDIR` records. Link creation on extraction is default-deny; `-ol` opts in. |
| `-oi[0-4]` | `-oi1` (default 64 KB threshold) | Identifies duplicate files and stores them as `FILECOPY` references (`FHEXTRA_REDIR` type 5), eliminating duplicate storage. |

### 3.7 Path Formatting & Extraction Control

| Switch | Syntax & Parameters | Description |
|---|---|---|
| `-r` | `-r`, `-r0` | Recurses subdirectories. `a` recurses by default; `-r0` restricts matching to explicitly named directories or wildcards. |
| `-ed` | `-ed` | Omits directory records from the archive (files are stored with paths, but empty directories are omitted). |
| `-ep` | `-ep` | Strips all directory paths, storing only bare filenames. |
| `-ep1` | `-ep1` | Strips the common base directory path entered on the command line. |
| `-ep2` | `-ep2` | Stores full path minus drive letter. |
| `-ep3` | `-ep3` | Stores full path including drive letter. |
| `-df` | `-df` | Deletes source files upon successful, verified archive creation or foreign format migration (`cv`). |
| `-dr` | `-dr` | Moves source files to the Recycle Bin / Trash after archiving. |
| `-dw` | `-dw` | Securely wipes source files (zero overwrite, truncate, delete) after archiving. |
| `-o+` / `-o-` | `-o+` (overwrite), `-o-` (skip) | Overwrite policy: `-o+` overwrites existing files without asking; `-o-` skips existing files. |
| `-y` | `-y` | Assumes "Yes" to all interactive console confirmation prompts. |

### 3.8 Output Formatting & Logging

| Switch | Syntax & Parameters | Description |
|---|---|---|
| `--json` | `--json` | Universal structured RFC 8259 JSON output across `l`, `v`, `lt`, `t`, `b`, `inspect`, and `diff`. Clean JSON to stdout, operational logs to stderr. |
| `--json-summary[=p]` | `--json-summary=report.json` | Emits a structured machine-readable JSON extraction report including per-entry status, byte counts, and session durability. |
| `-plain` / `--plain` | `-plain`, `--plain` | Disables ANSI terminal animations and TUI progress bars (automatic when redirected to a pipe, file, or CI runner). |
| `-q` / `-quiet` | `-q`, `-quiet`, `--quiet` | Quiet mode: suppresses informational banners, headers, and per-file progress lines. |
| `-v` / `--verbose` | `-v`, `--verbose` | Verbose mode: in `diff`, shows identical matching files; in `l`, lists technical details. |
| `--version` | `--version` | Displays OpenRAR version, build architecture, compiler, and hardware SIMD acceleration features. |

---

## 4. Exit Codes

| Code | Status | Meaning |
|:---:|---|---|
| `0` | **Success** | Operation completed successfully with 100% verified integrity. In `diff`: archives are identical. |
| `1` | **Warning / Diff** | Non-fatal warnings encountered (some files locked/skipped). In `diff`: structural differences detected between archives. |
| `2` | **Fatal Error** | Critical error occurred: missing file, corrupted archive header, CRC32 mismatch, out of memory, or bad password. |
| `7` | **Incompatible Switches**| Command invoked with mutually exclusive or inadmissible switch combinations (e.g. output-shaping switches in `cv`). |
| `255` | **User Abort** | Operation cancelled by user via interactive prompt, ESC key, or `SIGINT` (Ctrl+C). Partial extractions are safely unlinked. |

---

## 5. Environment Variables

| Variable | Values | Purpose |
|---|---|---|
| `OPENRAR_IN_PROC` | `1` / `0` | When set to `1`, forces in-process decompression, bypassing the AppContainer/seccomp sandboxed worker broker. |
| `OPENRAR_NO_BATCH_DURABILITY` | `1` / `0` | Global kill-switch disabling `--durability=batch` and forcing strict per-entry sync across all extractions. |
| `OPENRAR_SUBMIT_ENDPOINT` | URL | Overrides the default Cloudflare Workers endpoint for benchmark submissions (`openrar b --submit`). |
