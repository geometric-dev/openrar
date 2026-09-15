# OpenRAR Architecture & Engineering Standard

This document establishes the architecture, design principles, naming conventions, and clean-room provenance standards for **OpenRAR**. It serves as the durable baseline for all current code and future refactoring.

---

## 1. Architectural Mission & Clean-Room Mandate

OpenRAR is an independent, clean-room C++17 implementation of the RAR 5.0 archive format specification (`RAR5-FORMAT.md`). 

### Core Clean-Room Principles
1. **Specification-Driven**: Implementation must derive strictly from published technical specifications, RFCs, and external behavior documentation—never from proprietary or reverse-engineered source trees.
2. **Zero Code Carryover**: No source code, header definitions, preprocessor macros, comment threads, or internal identifier schemes from UnRAR or WinRAR may be copied into OpenRAR.
3. **Decoupled Modern Design**: Modern C++17 paradigms (RAII, standard library containers, `<filesystem>`, smart pointers, value semantics, `std::string_view`, explicit error propagation) replace legacy C++98 / DOS idioms (custom dynamic strings, 8.3 filenames, global singletons, raw pointers, direct `#include "*.cpp"` compilation units).

---

## 2. System Architecture & Layering

The codebase is organized into eight decoupled subsystems under the root namespace `openrar::`. Dependencies flow strictly downward: higher layers may depend on lower layers, but lower layers must never depend on higher layers.

Beside the layer stack sit the **boundary surfaces**: `src/api/abi_contract.hpp` (the types shared by both ABIs), `src/dll/` (C ABI, `openrar::api`), `src/wasm/` (Emscripten ABI, `openrar::wasm`), and the SFX extractor (`openrar::sfx` in `src/cli/sfx_main.cpp`). They wrap the top of the stack and follow the same downward-only rule.

```
┌──────────────────────────────────────────────────────────────────┐
│                           openrar::cli                           │
│  CLI Command Dispatcher, Argument Parser, UI Reporting, Banners  │
└────────────────────────────────┬─────────────────────────────────┘
                                 │
┌────────────────────────────────▼─────────────────────────────────┐
│                         openrar::archive                         │
│      ArchiveReader, ArchiveMutator, ArchiveEntry, Volumes        │
└──────────────┬─────────────────┬─────────────────┬───────────────┘
               │                 │                 │
┌──────────────▼──────┐   ┌──────▼──────────┐   ┌──────▼───────────────┐
│  openrar::compress  │   │ openrar::format │   │  openrar::recovery   │
│  LZ/Huffman Encoder │   │HeaderReader │   │  Reed-Solomon GF(2¹⁶)│
│  & Filter Pipeline  │   │HeaderWriter │   │  Cauchy Parity & Rec │
└──────────────┬──────┘   └──────┬──────┘   └──────┬───────────────┘
               │                 │                 │
┌──────────────▼─────────────────▼─────────────────▼───────────────┐
│                         openrar::crypto                          │
│          Aes256 (AES-NI / NEON), Blake2sp, Crc32, Pbkdf2         │
└────────────────────────────────┬─────────────────────────────────┘
                                 │
┌────────────────────────────────▼─────────────────────────────────┐
│                            openrar::io                           │
│     FileStream, MemoryStream, PathUtil, Win32Metadata / POSIX    │
└────────────────────────────────┬─────────────────────────────────┘
                                 │
┌────────────────────────────────▼─────────────────────────────────┐
│                           openrar::core                          │
│   Fixed-width Types, Endian Utilities, ExitCode, Varint/Vint     │
└──────────────────────────────────────────────────────────────────┘
```

### Subsystem Responsibilities

| Subsystem | Namespace | Responsibilities |
|---|---|---|
| **Core** | `openrar::core` | Fixed-width integer aliases, endian conversion (`read_le32`, etc.), variable-length integer (`vint`) encoding/decoding, global error definitions (`ExitCode`), thread pool. |
| **I/O** | `openrar::io` | RAII stream abstractions (`FileStream`), platform filesystem utilities (`PathUtil`), OS attribute serialization (Windows security descriptors, alternate streams, POSIX permissions). |
| **Crypto** | `openrar::crypto` | Hardware-accelerated cryptographic primitives: AES-256 CBC, BLAKE2sp, CRC32, PBKDF2-HMAC-SHA256, SHA-256. |
| **Format** | `openrar::format` | RAR 5.0 block serialization and deserialization (`HeaderReader`, `HeaderWriter`, `FileBlock`, `ServiceBlock`, `MainHeaderBlock`). |
| **Compress** | `openrar::compress` | RAR5 LZ77 parsing, Huffman entropy coding, sliding dictionary window management, byte-transform filters (Delta, x86 E8/E9). |
| **Recovery** | `openrar::recovery` | Reed-Solomon erasure coding over GF(2¹⁶) with Cauchy matrices (`ReedSolomon16`), in-archive recovery record generation and repair (`RecoveryRecord`). |
| **Archive** | `openrar::archive` | High-level archive lifecycle management: scanning, creating, modifying, verifying, and extracting multi-file archives. |
| **CLI** | `openrar::cli` | Console application entry point, switch parsing, exit code mapping, user interaction, and progress reporting. |

---

## 3. Naming & Style Conventions

### 3.1 File Naming
- All filenames must use **lower snake_case** with clear, descriptive words:
  - Good: `archive_reader.cpp`, `file_stream.hpp`, `recovery_record.cpp`, `header_writer.hpp`
  - Prohibited: 8.3 legacy abbreviations (`arcread.cpp`, `filcreat.cpp`, `rdwrfn.cpp`, `errhnd.cpp`, `isnt.cpp`, `smallfn.cpp`, `secpassword.cpp`, `uowners.cpp`, `ulinks.cpp`)
  - Prohibited: `cmd*` prefixes (`cmdadd.cpp`, `cmddata.cpp`, `cmdrepair.cpp`)
- C++ headers use `.hpp`; implementation files use `.cpp`.
- Every translation unit must compile independently. Never include `.cpp` files inside other source files (`#include "*.cpp"` is strictly forbidden).

### 3.2 Identifiers & Formatting

| Symbol Type | Convention | Examples | Anti-Patterns to Avoid |
|---|---|---|---|
| **Namespaces** | `lower_snake_case` | `openrar::archive`, `openrar::crypto` | Global scope symbols, `unrar` |
| **Types / Classes / Structs** | `PascalCase` | `ArchiveReader`, `FileBlock`, `FileStream` | `CommandData`, `ComprDataIO`, `BaseBlock` |
| **Enums** | `enum class` + `PascalCase` | `enum class CompressionLevel`, `enum class ExitCode` | C-style untyped enums, `RARX_CRC`, `NMDF_*` |
| **Functions / Methods** | `lower_snake_case` | `read_header()`, `write_block()`, `is_solid()` | `ReadHeader()`, `DoExtract()`, `PushVint()` |
| **Member Variables** | `lower_snake_case_` (trailing underscore) — *class* members | `file_stream_`, `header_offset_`, `is_encrypted_` | PascalCase (`CurBlockPos`, `NextBlockPos`, `ExitCode`) |
| **POD Struct Members** | `lower_snake_case`, no trailing underscore (wire-format blocks mirroring on-disk layout) | `fb.pack_size`, `header.redir_type` | `pack_size_` on a format block |
| **Local Variables** | `lower_snake_case` | `block_size`, `read_bytes`, `entry_it` | PascalCase (`ReadSize`, `CurPos`, `Item`) |
| **Constants / Macros** | `UPPER_SNAKE_CASE` (macros additionally `OPENRAR_`-prefixed; scope/class-scope constants UPPER, function-local `const` stays lower_snake) | `MAX_HEADER_SIZE`, `DEFAULT_DICT_SIZE` | Mixed case, Google-style `k`-prefix, un-namespaced macros |
| **Header Guards** | `OPENRAR_<SUBSYSTEM>_<FILENAME>_HPP` | `OPENRAR_ARCHIVE_ARCHIVE_READER_HPP` | `_RAR_ARCHIVE_`, `_RAR_ERRHANDLER_` |

**Bitmask flag groups** are the one sanctioned alternative to `enum class`:
OR-able wire flags use a namespace of UPPER_SNAKE `inline constexpr`
constants instead — see `format::header_flags`, `format::main_flags`,
`format::file_flags` (`src/format/headers.hpp`) and `archive::time_flags`
(`src/archive/archive_mutator.hpp`). The namespace keeps the flags scoped
and typed like the field they mask, where an unscoped `enum` would leak
enumerators and `enum class` would force casts at every `|` and `&`.

---

## 4. Types, Standard Library & Modern C++ Guidelines

1. **Standard Integer Types**: Use standard fixed-width types from `<cstdint>` via `openrar::core`: `uint8`, `uint16`, `uint32`, `uint64`, `int8`, `int16`, `int32`, `int64`, `byte`. Never use legacy non-standard typedefs (`uint`, `ushort`, `int64ndf`).
2. **Paths & File System**: Use `std::filesystem::path` for all path representations. Do not manipulate raw wchar arrays with custom string functions (`UnixSlashToDos`, `ConvertPath`, `wcsncatz`).
3. **Strings**: Standard internal encoding is UTF-8 stored in `std::string` or `std::string_view`. Conversion to `std::wstring` is restricted to the lowest OS-interface boundary on Windows (via `PathUtil` or `FileStream`).
4. **Memory Management**: Enforce strict RAII. Raw `new`/`delete` and `malloc`/`free` are prohibited. Use `std::unique_ptr`, `std::shared_ptr`, or container types (`std::vector<byte>`).
5. **No Global Singletons**: Global objects (such as `ErrHandler`) are banned. State must be passed via context structures or class members.

---

## 5. Error Handling Architecture

Legacy error handling relied on a monolithic global object (`ErrorHandler ErrHandler`), C-style exit code enums (`RARX_*`), long-lived state flags (`ErrHandler.SetErrorCode(...)`), and process termination helpers.

OpenRAR replaces this with a modern, layered approach:

```cpp
namespace openrar::core {

enum class ExitCode : int {
    Success       = 0,
    Warning       = 1,
    FatalError    = 2,
    CrcError      = 3,
    LockedArchive = 4,
    WriteError    = 5,
    OpenError     = 6,
    UserError     = 7,
    OutOfMemory   = 8,
    CreateError   = 9,
    NoFilesFound  = 10,
    BadPassword   = 11,
    ReadError     = 12,
    CorruptArchive= 13,
    UserBreak     = 255
};

} // namespace openrar::core
```

### Rules for Error Propagation:
1. **Core / Format / I/O / Recovery Layers**: Return `bool` with clear out-parameters, `std::optional<T>`, or `std::error_code`. The format reader returns its tri-state `HeaderResult` (`Ok` / `Eof` / `Error` / `HeaderCrcMismatch`) because EOF is a normal scan outcome there, not a failure. Throw exceptions (`std::runtime_error`, `std::out_of_range`) only for unrecoverable structural invariant violations.
2. **Compress Layer**: codec calls return `bool`; the failing codec records a `DecompressErrorCode` (plus message) retrievable via `last_error()` on the instance. This is a deliberate, bounded exception to the no-mutable-error-state rule — codecs own a single decode at a time, so the slot cannot race, and call sites stay allocation-free on the hot path. Keep this pattern confined to the codecs; don't propagate it upward.
3. **Archive Layer**: operations return `bool`; outcome details that outlive the call are queryable members (e.g. `ArchiveReader::has_bad_password()`). Multi-file operations aggregate per-entry failures without aborting the batch.
4. **CLI Layer**: Maps operation outcomes directly to `openrar::core::ExitCode` for process termination. A local control-flow exception (`PrepareFailed`) unwinds the parallel add pipeline to its drain path; it is pipeline plumbing, not error reporting, and must not inspire new exception-based error channels.

### Boundary exemptions (C ABI / WASM)

The C ABI surface (`src/dll/openrar_dll.h`, `src/dll/dll_api.cpp`) and the
WASM API (`src/wasm/`) are intentionally exempt from some rules above — these
are frozen ABI surfaces:

- Raw `malloc`/`new` is allowed where memory crosses the C boundary, but must
  pair with `openrar_free` ("single heap" contract).
- The plain `enum RarError` with `RAR_*` enumerator names is part of the
  frozen ABI — do not convert it to `enum class` or rename it.
- Every non-trivial export body catches exceptions and routes the message to
  the thread-local error string (`openrar_archive_get_error`).

**Shared ABI contract.** The types both surfaces must agree on live once, in
`src/api/abi_contract.hpp`: the `RarError` enum (canonical in
`src/archive/buffer_archive.hpp`), the stable 64-byte `ArchiveEntryOut`
layout, `pack_entries`, `heap_dup`, thread-local `ThreadError`, and a
`HandleTable` with shared_ptr pinning (host callbacks never run under a
table lock). `src/dll/openrar_dll.h` re-declares the enum and entry struct
in C-compatible form for C hosts; `dll_api.cpp` pins those declarations to
the canonical ones with compile-time equivalence checks (`static_assert` on
enum values and per-field offsets), so the C header cannot drift. The
in-memory archive layer (`src/archive/buffer_archive.cpp`) that both
surfaces sit on is gated by the `OPENRAR_INMEM_ARCHIVE` CMake option — it is
excluded from the CLI binary and the lean block-codec wasm build; the DLL
always compiles it directly.

**Listing walk.** The per-block header classification lives once, in
`buffer_archive.cpp` (`walk_headers`), behind two source adapters: the
in-memory `BufferArchive::list` and the file-streaming `list_file_stream`.
The boundaries map the shared internal status to public codes and
deliberately differ on `CryptHeader`: the historical surfaces keep
`RAR_ERR_UNSUPPORTED_FEATURE`, while the DLL's `_ex`/`_pw` listing exports
return the dedicated `RAR_ERR_ENCRYPTED` (-12) as an early password signal.
Only the streaming path accepts a password (the CBC/size-recovery crypto
lives in the stream `read_block_raw`); its password mode also reports
encrypted file entries (`is_encrypted = 1`) instead of stopping, because
`-hp` implies encrypted file data for every entry. The additive exports
negotiate via `openrar_abi_features()`; `OPENRAR_DLL_API_VERSION` does not
move for new exports (see `docs/versioning.md`).

These exemptions are also encoded in `.clang-tidy` (`RAR_` enum-constant
ignore rule) and documented in CONTRIBUTING.md.

---

## 6. Subsystem Architecture Map

Every module under `src/` is written from the ground up as clean-room `openrar::*` code adhering to C++17 standards:

| Concern | Subsystem Module |
|---|---|
| Error codes / exit codes | `src/core/error.hpp` (`ExitCode`) |
| Fixed-width types, varint | `src/core/types.hpp`, `src/core/vint.*` |
| Streams, paths, platform metadata | `src/io/file_stream.*`, `path_util.*`, `win32_meta.*` |
| AES-256, BLAKE2sp, CRC32/64, PBKDF2 | `src/crypto/*` |
| Header read/write | `src/format/header_reader.*`, `header_writer.*` |
| Archive scan / extract | `src/archive/archive_reader.*` |
| Archive create / append / delete | `src/archive/archive_mutator.*` |
| Decompression | `src/compress/decompressor50.*` |
| Compression | `src/compress/compressor50.*`, `stream_encoder.*` |
| Volumes, recovery records | `src/archive/volume.*`, `src/recovery/*` |
| CLI | `src/cli/main.cpp`; SFX extractor `src/cli/sfx_main.cpp` |

Residual legacy vocabulary that is intentionally retained:

- The `RAR_*` enumerator names in the frozen C ABI (`src/dll/openrar_dll.h`,
  canonical as `archive::BufferArchiveError`) — part of the published surface.
