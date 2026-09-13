# WASM Archive API — Design Spec (v3)

> **SHIPPED-REALITY NOTE (2026-09, ABI v2).** This document is the original
> design record. The shipped surface (`src/wasm/archive_api.{hpp,cpp}`,
> `wasm/js/openrar-archive.{js,d.ts}`) differs in these ways, by decision:
>
> - **Versioning:** the archive module is versioned independently
>   (`ARCHIVE_WASM_API_VERSION`, now 2) instead of sharing the block codec's
>   `WASM_API_VERSION` (now 2, embind removed). Export presence is enforced
>   at boot (`requireExports`) and in CI (`check-exports.mjs`).
> - **Error model:** the v1 numbering (TRUNCATED=-3, CRC=-4, NOMEM=-5, IO=-6,
>   BAD_PASSWORD=-7, INVALID_ARG=-9, ABORTED=-11) was kept; the error message
>   is a copy-out getter (`openrar_archive_get_error(char*, int)`) plus
>   `openrar_archive_last_error_code()` (v2) instead of
>   `openrar_last_error_message()`. BAD_SIGNATURE stays collapsed into
>   NOT_RAR; BUFFER_TOO_SMALL is not used (extract returns malloc'd buffers).
> - **Entry struct:** `ArchiveEntryOut` (64 bytes: u32 offsets/flags, u64
>   sizes/mtime, path in a separate contiguous buffer) shipped as-is.
> - **Create:** shipped as `openrar_archive_create2` (§1.4 structs, mtime_unix
>   with 0⇒now, DOS-encoded on disk) with the v1 parallel-array
>   `openrar_archive_create` kept as a shim. **window_log2 ∈ [1,4] →
>   128 KiB..1 MiB** (amendment G: the spec's [3,5]/1-4 MiB mapping was not
>   implemented; the JS option is `windowLog2`, no MiB/bytes overload).
> - **Progress/cancel:** shipped via `ArchiveHooks` (openrar_progress_cb /
>   openrar_cancel_cb, per-entry granularity, RAR_ERR_ABORTED) on
>   `create2` / `extract_all2` / `handle_extract_all2` — amendment E adopted.
>   The module is built with WASM_BIGINT=1 so u64 callback params arrive as
>   BigInt ('vijj').
> - **Streaming (§1.2, §1.5, §7):** not yet shipped; scheduled as the next
>   phase (Path A Compressor50::feed).
> - **Extract in two passes (§1.3 BUFFER_TOO_SMALL):** not shipped; the
>   caller-allocated variant lost out to malloc'd outputs + per-entry JS
>   extraction (bounded peak memory without the double round-trip).

**Status:** Target ship: separate build
artifact `wasm/dist/openrar_archive.js`. `WASM_API_VERSION` bumps 2 → 3.

The block codec (`wasm/dist/openrar.js`) is **not** broken — the new
module ships alongside, lazy-loaded so consumers who don't need RAR5 I/O
still pay ~250 KiB for block codec only.

---

## 1. Final C ABI surface

All exports `extern "C"` from `src/wasm/archive_api.{hpp,cpp}`. The
existing block-codec ABI (`src/wasm/wasm_api.cpp`) is unchanged.

### 1.1 Error model

```c
enum {                                // matches RarError['code'] in .d.ts
    RAR_OK                    = 0,
    RAR_ERR_PARTIAL_OK        = 1,    // some entries failed; see per-entry status
    RAR_ERR_NOT_RAR           = -1,
    RAR_ERR_BAD_SIGNATURE     = -2,    // collapsed with NOT_RAR in JS (see §5)
    RAR_ERR_UNSUPPORTED       = -3,
    RAR_ERR_TRUNCATED         = -4,
    RAR_ERR_CRC_MISMATCH      = -5,
    RAR_ERR_BAD_PASSWORD      = -6,
    RAR_ERR_BUFFER_TOO_SMALL  = -7,
    RAR_ERR_NOMEM             = -8,
    RAR_ERR_IO                = -9,
    RAR_ERR_INVALID_ARG       = -10,
    RAR_ERR_ABORTED           = -11,
};

// Static, thread-local, NUL-terminated UTF-8. Valid until the next
// archive call on the same thread. Zero allocation per call.
const char* openrar_last_error_message(void);
```

Numeric codes (not string returns) keep the FFI alloc-free per call;
the JS wrapper throws `RarError{ code, detail }` via lookup table +
last-error string.

### 1.2 Lifecycle

```c
// One-shot. Caller owns source buffer until openrar_archive_close().
uint32_t openrar_archive_open(const uint8_t* buf, size_t sz);

// Streaming open: feed chunks, then finalise.
uint32_t openrar_archive_open_reader(void);
int      openrar_archive_open_feed(uint32_t h, const uint8_t* chunk, size_t n);
int      openrar_archive_open_finalize(uint32_t h);
void     openrar_archive_close(uint32_t h);
```

### 1.3 Read paths

```c
typedef struct {
    uint32_t path_off;       // offset into archive's string table
    uint32_t path_len;       // UTF-8, '/' separators, '/' suffix iff dir
    uint64_t size;           // uncompressed
    uint64_t packed_size;
    uint32_t mtime;          // UNIX seconds (DOS-time converted)
    uint32_t crc32;          // 0 if unknown
    uint8_t  method;         // 0..5
    uint8_t  is_dir;         // 1 if FHFL_DIRECTORY
    uint8_t  is_encrypted;
    uint8_t  reserved;
    uint32_t index;          // 0..N-1, stable for archive lifetime
} openrar_archive_entry_t;

int  openrar_archive_list(uint32_t h,
                          openrar_archive_entry_t** out_entries,
                          size_t* out_count);
void openrar_archive_free_entries(openrar_archive_entry_t* entries);

// Two-pass: pass NULL/0 first → RAR_ERR_BUFFER_TOO_SMALL + writes required
// size to *out_cap; re-call with openrar_alloc()-ed buffer.
int openrar_archive_extract(uint32_t h, uint32_t index,
                            uint8_t* out_buf, size_t* out_cap);

typedef struct { uint64_t offset; uint64_t size; uint32_t index; }
    openrar_archive_extract_offsets_t;

int openrar_archive_extract_all(uint32_t h,
                                openrar_archive_extract_offsets_t* out_offsets,
                                size_t* out_count,
                                uint8_t* out_buf, size_t* out_cap);
```

### 1.4 Create / write

```c
typedef struct {
    const char* path;        // UTF-8, '/' separators, '/' suffix for dirs
    const uint8_t* data;     // NULL iff is_dir
    size_t data_len;
    uint64_t mtime_unix;     // 0 ⇒ now()
    uint8_t  is_dir;
    uint8_t  reserved[3];
} openrar_archive_input_file_t;

typedef struct {
    uint8_t  method;         // 0, 3, or 5 (others → INVALID_ARG)
    uint32_t window_log2;    // dict = 128 KiB << window_log2; clamped 3..5
                             // (1 MiB..4 MiB) — see §8
    uint8_t  reserved[3];
} openrar_archive_create_opts_t;

// *out_buf allocated with openrar_alloc → caller MUST openrar_free().
int openrar_archive_create(const openrar_archive_input_file_t* files,
                           size_t file_count,
                           const openrar_archive_create_opts_t* opts, // NULL ⇒ method=3, 4 MiB
                           uint8_t** out_buf, size_t* out_sz);
```

### 1.5 Streaming block codec

```c
uint32_t openrar_stream_create(int method, uint32_t window_log2);
int      openrar_stream_feed(uint32_t h, const uint8_t* src, size_t n);
int      openrar_stream_finish(uint32_t h, uint8_t** out_buf, size_t* out_sz);
void     openrar_stream_free(uint32_t h);
```

### 1.6 Cancel + progress

```c
typedef void (*openrar_progress_cb)(void* ud, uint64_t done, uint64_t total);
typedef int  (*openrar_cancel_cb)(void* ud);
int openrar_set_progress(uint32_t h, openrar_progress_cb cb, void* ud);
int openrar_set_cancel (uint32_t h, openrar_cancel_cb  cb, void* ud);
```

Cancel is polled between block boundaries (`openrar_archive_open_feed` /
`extract_all` / `stream_feed`).

---

## 2. Final JS/TS surface (`wasm/js/openrar_archive.js` + `.d.ts`)

### 2.1 Module entry

```ts
export const WASM_API_VERSION: 3;
export function createOpenRARArchive(opts?: {
  locateFile?: (path: string, scriptDirectory: string) => string;
}): Promise<RawArchiveModule>;

export interface RawArchiveModule {
  version(): number;
  _openrar_version(): number;
  _openrar_last_error_message(): string;
  _openrar_archive_open(buf: number, sz: number): number;
  _openrar_archive_open_reader(): number;
  _openrar_archive_open_feed(h: number, chunk: number, n: number): number;
  _openrar_archive_open_finalize(h: number): number;
  _openrar_archive_close(h: number): void;
  _openrar_archive_list(h: number): number;
  _openrar_archive_extract(h: number, index: number, out: number, capPtr: number): number;
  _openrar_archive_extract_all(h: number, offsets: number, countPtr: number, buf: number, capPtr: number): number;
  _openrar_archive_free_entries(ptr: number): void;
  _openrar_archive_create(files: number, n: number, opts: number, outBufPtr: number, outSzPtr: number): number;
  _openrar_stream_create(method: number, window_log2: number): number;
  _openrar_stream_feed(h: number, src: number, n: number): number;
  _openrar_stream_finish(h: number, outBufPtr: number, outSzPtr: number): number;
  _openrar_stream_free(h: number): void;
  _openrar_set_cancel(h: number, cb: number | null): number;
  _openrar_set_progress(h: number, cb: number | null): number;
  _openrar_alloc(n: number): number;
  _openrar_free(ptr: number): void;
  _malloc(n: number): number; _free(ptr: number): void;
  ccall<T>(ident: string, returnType: string | null, argTypes: string[], args: unknown[]): T;
  cwrap(ident: string, returnType: string | null, argTypes: string[]): (...a: unknown[]) => unknown;
  HEAPU8: Uint8Array; HEAPU8_BASE: number;
  HEAP32: Int32Array; HEAP64: BigInt64Array;
  addFunction(fn: Function, sig: string): number;
  removeFunction(ptr: number): void;
  UTF8ToString(ptr: number, max?: number): string;
  stringToUTF8(str: string, ptr: number, maxBytes: number): void;
  lengthBytesUTF8(str: string): number;
}
```

### 2.2 Public types

```ts
export interface RarEntry {
  path: string;             // forward slashes; '/' suffix iff isDir
  size: number;             // uncompressed
  packedSize: number;
  mtime: number;            // UNIX seconds (DOS-time converted when mtime_ns absent)
  crc32: number;            // 0 if unknown
  method: 0 | 3 | 5;        // narrowed from C's 0..5
  isDir: boolean;
  isEncrypted: boolean;
  index: number;            // 0..N-1, stable for archive lifetime
}

export type RarErrorCode =
  | 'NOT_RAR' | 'UNSUPPORTED_FEATURE' | 'TRUNCATED' | 'CRC_MISMATCH'
  | 'BAD_PASSWORD' | 'IO' | 'NOMEM' | 'ABORTED' | 'INVALID_ARG';

export class RarError extends Error {
  readonly code: RarErrorCode;
  readonly detail?: string;
  constructor(code: RarErrorCode, detail?: string);
  static fromCode(code: number, detail?: string): RarError;
}
```

### 2.3 Public API

```ts
export interface CreateArchiveInput {
  path: string;
  data: Uint8Array | string;
  mtime?: number;
}
export interface CreateArchiveOptions {
  method?: 0 | 3 | 5;
  windowSize?: 1 | 2 | 3 | 4;        // MiB; default 4
  onProgress?: (done: number, total: number) => void;
  signal?: AbortSignal;
}
export function createArchive(
  files: CreateArchiveInput[],
  opts?: CreateArchiveOptions,
): Promise<Uint8Array>;

export function listArchive(rar: Uint8Array): Promise<RarEntry[]>;
export function extractFile(rar: Uint8Array, path: string): Promise<Uint8Array>;
export function extractFileByIndex(rar: Uint8Array, index: number): Promise<Uint8Array>;

export interface ExtractAllOptions {
  onProgress?: (done: number, total: number) => void;
  signal?: AbortSignal;
}
export function extractAll(rar: Uint8Array, opts?: ExtractAllOptions)
  : Promise<Map<string, Uint8Array>>;

export interface StreamOptions {
  method?: 0 | 3 | 5;
  windowSize?: 1 | 2 | 3 | 4;
  signal?: AbortSignal;
}
export function compressStream(
  src: ReadableStream<Uint8Array> | AsyncIterable<Uint8Array>,
  opts?: StreamOptions,
): Promise<Uint8Array>;

export function destroy(): void;
```

### 2.4 `extractAll` shape (sketch)

```js
const m = await getModule();
const h = m._openrar_archive_open(bufPtr, bufLen);
if (!h) throw RarError.fromCode(-1, m._openrar_last_error_message());

// Pass 1: sizes.
let rc = m.ccall('openrar_archive_extract_all', 'number',
  ['number','number','number','number','number'],
  [h, offsetsPtr, countPtr, 0, capPtr]);
if (rc === -7) {
  const total = Number(m.HEAPU64[(m.HEAPU8_BASE + capPtr) >> 3]);
  const bufPtr = m._openrar_alloc(total);   // openrar_alloc, not _malloc
  rc = m.ccall('openrar_archive_extract_all', 'number',
    ['number','number','number','number','number'],
    [h, offsetsPtr, countPtr, bufPtr, capPtr]);
  // Read offsets from HEAP, copy each slice into the result Map BEFORE
  // any further WASM call (ALLOW_MEMORY_GROWTH=1 may reallocate).
}
```

### 2.5 `AbortSignal` wiring

```js
function wireSignal(m, h, signal) {
  if (!signal) return () => {};
  const cb = m.addFunction(() => Number(signal.aborted), 'i');
  m._openrar_set_cancel(h, cb);
  return () => m.removeFunction(cb);
}
```

---

## 3. C++ shape

### 3.1 `BufferStream` (`src/io/buffer_stream.hpp`, ~80 LOC)

Read-only adapter over `const uint8_t*` + `size_t`, mirroring
`io::FileStream`'s public API:

```cpp
namespace openrar::io {
class BufferStream {
public:
    BufferStream() = default;
    explicit BufferStream(const uint8_t* data, size_t size);
    void set(const uint8_t* data, size_t size);  // rebind for streaming finalise
    bool   is_open() const { return data_ != nullptr; }
    size_t size() const { return size_; }
    size_t read(void* dst, size_t n);
    bool   seek(int64_t off, SeekOrigin origin);
    uint64_t tell() const { return pos_; }
private:
    const uint8_t* data_{nullptr};
    size_t size_{0}, pos_{0};
};
}
```

Wired into root `CMakeLists.txt` `OPENRAR_CORE_SOURCES`.

### 3.2 `ArchiveReader::open_buffer` + `extract_inplace`

```cpp
class ArchiveReader {
public:
    bool open_buffer(const uint8_t* buf, size_t sz);   // NEW
    bool extract_entry_inplace(const ArchiveEntry& e,
                               std::vector<uint8_t>& out);  // NEW
    // ...existing file-based API unchanged
private:
    io::BufferStream buf_stream_;       // NEW, alternative to stream_
};
```

`open_buffer` reuses the same scan logic as `scan_archive` but reads
from `buf_stream_`. SFX scan uses the same 4 MiB window. Output entries
are populated with `in_memory = true` and `memory_data` is a **view**
(no copy). Multi-volume returns `RAR_ERR_UNSUPPORTED`. `HeaderReader::read_block_raw_mem`
**already exists** (`src/format/header_reader.hpp:23`) — use directly.

### 3.3 `ArchiveMutator::add_buffer`

New static in `src/archive/archive_mutator.hpp`. Lifts header write +
per-file compress + payload assembly from `add_or_move_file`
(`src/archive/archive_mutator.cpp:304`) into a buffer-output variant
writing to `std::vector<uint8_t>`. No `std::filesystem::path` in this
code path. ~250 LOC.

`format::HeaderWriter` currently takes `io::FileStream&`; add a sibling
that takes a `std::vector<uint8_t>&` sink (~30 LOC, mechanical).

### 3.4 `Compressor50::feed` (Path A)

```cpp
class Compressor50 {
public:
    bool feed(const uint8_t* src, size_t n);   // NEW: Path A wrapper
    bool finish(std::vector<uint8_t>& out);    // emits last_block=true
    void reset_for_file();                     // begin_archive equivalent
};
```

Per `docs/streaming-considerations.md` §3 — owns per-call state, leaves
existing internals untouched. Save/restore shape in §7.2.

---

## 4. Lazy-load mechanics

### 4.1 Build artifacts

| Artifact | Size budget | Contents |
|---|---|---|
| `wasm/dist/openrar.js` | ≤ 512 KiB (existing) | Block codec only — identical to v2 |
| `wasm/dist/openrar_archive.js` | ≤ 1.5 MiB | Block codec + `archive_api.cpp` + `stream_encoder.cpp` |
| `wasm/dist/openrar_cli.js` | optional, MEMFS | Conformance only (default OFF) |

`cmake/wasm.cmake` adds `openrar_wasm_archive` linking `openrar_wasm_core`
plus `src/wasm/archive_api.cpp`. Flags: `MODULARIZE=1`, `EXPORT_ES6=1`,
`EXPORT_NAME=createOpenRARArchive`, `FILESYSTEM=0`, `ALLOW_MEMORY_GROWTH=1`,
`ENVIRONMENT=web,node`, the `EXPORTED_FUNCTIONS` list in §1, and runtime
methods `['ccall','cwrap','UTF8ToString','UTF8ToCString','lengthBytesUTF8',
'stringToUTF8','HEAPU8','HEAPU8_BASE','HEAP32','HEAP64','addFunction',
'removeFunction']`.

### 4.2 JS-side `import()`

```js
// wasm/js/openrar_archive.js (~150 LOC)
import createOpenRARRaw from '../dist/openrar_archive.js';

let _bootPromise = null, _module = null;

export async function getModule() {
  if (_module) return _module;
  if (!_bootPromise) _bootPromise = createOpenRARRaw().then(verifyVersion);
  return _bootPromise;
}

function verifyVersion(m) {
  if (m.version() !== WASM_API_VERSION) {
    throw new Error(`openrar-archive: ABI mismatch (host v${WASM_API_VERSION} / module v${m.version()})`);
  }
  _module = m; return m;
}

export const WASM_API_VERSION = 3;
```

Consumers who don't need RAR5 I/O never import this file.

### 4.3 Crate-side integration

```ts
let archiveMod: typeof import('openrar-archive') | null = null;
try { archiveMod = await import('openrar-archive'); }
catch { archiveMod = null; }
```

If `null`, falls back to v2 block codec; if present, routes
`createArchive` / `extractAll` / `compressStream` to the new module.
Main bundle unaffected when unused.

---

## 5. Error model

### 5.1 Mapping

```js
const CODE_MAP = Object.freeze({
  [-1]: 'NOT_RAR', [-2]: 'NOT_RAR', [-3]: 'UNSUPPORTED_FEATURE',
  [-4]: 'TRUNCATED', [-5]: 'CRC_MISMATCH', [-6]: 'BAD_PASSWORD',
  [-7]: 'INVALID_ARG', [-8]: 'NOMEM', [-9]: 'IO', [-11]: 'ABORTED',
});
```

`NOT_RAR` and `INVALID_ARG` collapse intentionally — disambiguation
stays in `openrar_last_error_message()`.

### 5.2 When each fires

| Code | Fires when |
|---|---|
| `NOT_RAR` | First 8 bytes ≠ RAR5 magic and SFX scan didn't locate magic within 4 MiB. |
| `TRUNCATED` | Header parsing ends mid-vint, `data_size` past buffer, or `extract_*` reads past EOF. |
| `CRC_MISMATCH` | `extract_*` succeeds at block layer but `FileBlock.data_crc32` mismatches. Only on extract. |
| `BAD_PASSWORD` | `psw_check` mismatch (always `UNSUPPORTED_FEATURE` in MVP — passwords deferred). |
| `UNSUPPORTED_FEATURE` | Encrypted entry, multi-volume archive, recovery archive, method ∉ {0,3,5}. |
| `IO` | Reserved for future virtual-FS writer. |
| `NOMEM` | `openrar_alloc` returns null. |
| `ABORTED` | Cancel callback returned non-zero during `feed` / `extract_*`. |
| `INVALID_ARG` | `window_log2` ∉ [3,5], method ∉ {0,3,5}, file path empty / > 2048 B, `data_len` with `is_dir ≠ 0`. |

### 5.3 Partial success

`extract_all` may partially succeed (one entry encrypted). Returns
`RAR_ERR_PARTIAL_OK = 1`; per-entry status in a parallel `uint8_t` array
(0 = OK). JS wrapper attaches a `Map<index, RarError>` to a synthetic
`RarError{ code: 'UNSUPPORTED_FEATURE', detail }` so consumers can
inspect per-entry cause.

---

## 6. Memory contract

**Per-call peak:**

| Op | WASM heap | Notes |
|---|---:|---|
| `listArchive` | `~rar.byteLength` | One-pass scan; no per-file materialisation. |
| `extractFile` | `~rar + per-file` | Per-file read only. |
| `extractAll` | `~rar + Σ(unp)` | Single re-used alloc; peak ≈ total uncompressed. |
| `createArchive` | `Σ(data_len) + archive` | Per-file compressed payload freed before next file's alloc. |
| `compressStream` | `1 chunk + 7 MiB + final_out` | Output materialised in `finish()`. |

**Ownership:** `openrar_alloc` → `openrar_free` for **all** output
buffers (consumer follows `safeRead → set → _free` pattern);
`/_malloc`/`/_free` for internal tmp only; JS `new Uint8Array` (GC)
for public return values.

**`HEAPU8.set` + `ccall` for >4 MiB buffers:** brief points at
`src/wasm/wasm_api.cpp:105-117` (embind `compress_val` byte-by-byte
walk into `std::vector<byte>`). New code does **not** use embind for
archive I/O — every input crosses via `HEAPU8.set(src, ptr)` (one
`memcpy`-equivalent) and every output via `HEAPU8.slice` + `safeRead`
(from `wasm/js/openrar.js:291`).

**`ALLOW_MEMORY_GROWTH=1`:** `MAX_WIN_SIZE = 4 GiB`
(`wasm_api.hpp:18`); a 4 MiB-window single archive needs ≈
`Σ(unp) + 2× max(Σ(packed)) + 7 MiB + ~250 KiB`. 200 MiB uncompressed
with ~150 MiB packed ≈ 500 MiB peak — within the ~4 GiB ceiling.
`HEAPU8_BASE` is exported so the JS wrapper can detect pointers that
pre-date a grow-and-relocate; `safeRead(ptr, len)` is called after
every C call that returns a heap pointer.

**Streaming input for >200 MB:** caller passes
`ReadableStream<Uint8Array>` to `compressStream`. JS driver loops
`reader.read()`, writes each chunk into a freshly `openrar_alloc`-ed
buffer (≤ 1 MiB), calls `openrar_stream_feed`, then `openrar_free`.
WASM input buffer is bounded at chunk size; native input vector
stays empty.

---

## 7. Streaming design (Path A)

### 7.1 C exports

See §1.5.

### 7.2 `Compressor50::feed` save/restore

```cpp
bool Compressor50::feed(const uint8_t* src, size_t n) {
    const uint8_t* saved_ptr = mem_src_ptr_;
    size_t saved_pos  = mem_src_pos_;
    size_t saved_size = src_size_;
    uint64_t saved_loaded = src_loaded_;
    bool saved_last = last_block_emitted_;

    mem_src_ptr_ = src;
    mem_src_size_ = n;
    mem_src_pos_ = 0;
    src_size_ = n;
    src_loaded_ = 0;
    last_block_emitted_ = false;
    bool ok = run_inner_loop_until_eof();

    mem_src_ptr_   = saved_ptr;
    mem_src_size_  = src_size_;
    mem_src_pos_   = saved_pos;
    src_size_      = saved_size;
    src_loaded_    = saved_loaded;
    last_block_emitted_ = saved_last;
    return ok;
}
```

Sidesteps the iter-3 crash root cause (`streaming-considerations.md` §3)
by **isolating** per-call state from the packer's invariants.

### 7.3 `StreamEncoder` (~80 LOC)

```cpp
class StreamEncoder {
public:
    StreamEncoder(int method, size_t win_size);
    bool feed(const uint8_t* src, size_t n);   // → Compressor50::feed
    bool finish(std::vector<uint8_t>& out);    // last_block=true
private:
    Compressor50 packer_;
    std::vector<uint8_t> blocks_;
};
```

### 7.4 JS `compressStream`

```js
export async function compressStream(src, opts = {}) {
  const m = await getModule();
  const method = opts.method ?? 3;
  const winLog2 = log2MiB(opts.windowSize ?? 4);   // 4 MiB ⇒ log2=5
  const h = m._openrar_stream_create(method, winLog2);
  if (!h) throw new RarError('NOMEM');

  const cleanup = wireSignal(m, h, opts.signal);
  try {
    const reader = src.getReader ? src.getReader() : asyncIterToReader(src);
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      if (value.byteLength === 0) continue;
      const ptr = m._openrar_alloc(value.byteLength);
      m.HEAPU8.set(value, ptr);
      const rc = m.ccall('openrar_stream_feed', 'number',
        ['number','number','number'], [h, ptr, value.byteLength]);
      m._openrar_free(ptr);
      if (rc !== 0) throw RarError.fromCode(rc);
    }
    const outPtr = m._malloc(4), outSz = m._malloc(4);
    const rc = m.ccall('openrar_stream_finish', 'number',
      ['number','number','number'], [h, outPtr, outSz]);
    if (rc !== 0) { m._free(outPtr); m._free(outSz); throw RarError.fromCode(rc); }
    const sz = m.HEAPU32[outSz >>> 2];
    const out = safeRead(m.HEAPU32[outPtr >>> 2], sz);
    m._openrar_free(m.HEAPU32[outPtr >>> 2]);
    m._free(outPtr); m._free(outSz);
    return out;
  } finally {
    cleanup();
    m._openrar_stream_free(h);
  }
}
```

---

## 8. Required features (MVP)

| Feature | Spec | Reason |
|---|---|---|
| Methods `0`/`3`/`5` | `method ∈ {0,3,5}`; other → `INVALID_ARG` | Mirrors crate `types.ts:7` (`Store`/`Balanced`/`Max`) |
| Window 1–4 MiB fixed | `window_log2 ∈ [3,5]` (one-way clamp at C ABI); matches `FileBlock.win_size` written | Avoids compressor 2 MiB / decompressor 1 MiB interop bug (`archive_mutator.cpp:364`); writer-side clamp sufficient |
| DOS-time mtime | `FileBlock.utime_unix` set from caller; reader → UNIX seconds | Per `format/headers.hpp:148`; mtime_ns deferred |
| Directory entries | `is_dir` round-trips via `FHFL_DIRECTORY`; empty data area | Dir tree round-trips (`crate/src/main.ts:334`) |
| `RarError{ code, detail }` | §5 | Replaces `boolean 0` |
| `Uint8Array` in/out | `HEAPU8.set` + `ccall` / `safeRead`; no embind byte loop | Per brief |
| `onProgress` + `AbortSignal` | §1.6 + §2.5 | `downloadBlob` large packs |
| Worker-safe | Transferable buffers; no DOM | Browser Worker |
| `.d.ts` published | `wasm/js/openrar_archive.d.ts` checked in | Consumers depend on it |

---

## 9. Deferred features

| Feature | Why deferred | Cost to add |
|---|---|---|
| **Solid** (`solid: true`) | Requires compressor to keep window + hash tables alive across `feed()` boundaries — conflicts with per-entry extract semantics | ~150 LOC: `solid` flag in `Compressor50::feed`, skip `begin_archive()` between files. v4 |
| **Password** (`-p`/`-hp`) | Crypto builds into WASM but exposing it invites a separate security review (PBKDF2 = O(seconds)/file) | ~200 LOC: AES-CBC + PBKDF2 in `extract_inplace`; security sign-off |
| **Multi-volume** (`-v`) | Requires writing `N` split files; WASM has one input buffer | ~400 LOC: `add_buffer_vol` → `vector<vector<uint8_t>>`; JS returns `Uint8Array[]` |
| **Recovery** (`-rr`) | RS built but WASM surface needs RS encoding for creation | ~300 LOC: wire `src/recovery/rs16.cpp` + `recovery_writer.cpp` into buffer mutator |
| **mtime_ns** (`FHEXTRA_HTIME`) | Separate code path through `FileBlock` parser + writer | ~80 LOC: `FHEXTRA_HTIME` extra area; decode into separate JS field |
| **Symlinks** (`FHEXTRA_REDIR`) | Buffer mode has no FS to materialise into | Out of scope — extract only produces `Uint8Array` |
| **Streaming extract** | `decompressStream` (reverse of `compressStream`); decoder is internally streaming but needs the WASM surface | ~250 LOC; Path A on `Decompressor50`. v4 |

Each fires `UNSUPPORTED_FEATURE` (or rounds mtime_ns to seconds). The
code is preserved even when JS consumers pass `solid: true` etc. — never
silent fall-through.

---

## 10. Test contract

**Native** — `tests/unit/archive_api_tests.cpp` (new CTEST target, ~250 LOC):

1. `create_buffer` → `extract_file` (each entry, byte-equal).
2. `create_buffer` → `extract_all` → Map rebuild → byte-equal.
3. `create_buffer` → `list_archive` → count + paths + sizes match.
4. Dir entries (`FHFL_DIRECTORY` round-tripped, empty payload).
6. WindowSize 1 / 2 / 4 MiB each round-trip.
7. `method=2` → `INVALID_ARG`.
8. Truncate buffer by 1 byte → `TRUNCATED` on `listArchive`.
9. `StreamEncoder` 100 MiB in 64 KiB chunks → ratio within 0.5 % of one-shot.
10. Iter-3 repro: 256 KiB in 4 × 64 KiB chunks (regression for
    `streaming-considerations.md` §3).
11. Cancel callback returns 1 mid-extract → `ABORTED`, partial freed.
12. Progress callback fires monotonically.

**JS** — `wasm/js/archive.test.mjs` (~200 LOC): `node --test`, mirrors
1–6, 8, 11, 12. Auto-skips if `wasm/dist/openrar_archive.{js,wasm}`
missing (same pattern as `helpers.test.mjs`).

**Interop smoke** — extend `tools/interop_gate.py` with `[6/6] archive_api_buffer`:

1. Native CLI: `openrar a -m3 fixture.bin fixture.rar` → read bytes,
   `openrar_archive_open + list + extract_all` → byte-equal.
2. Reverse: `openrar_archive_create([fixture.bin])` → write `out.rar`
   → `openrar t out.rar` → ok.
3. Cross-RAR: feed WinRAR-produced fixture through `extract_all` →
   byte-equal.

CI fallback (no `WinRAR.exe`) handled by existing gate logic.

**CI matrix:** native targets on every matrix (linux/macos/windows,
Debug/Release). WASM-build smoke on the `wasm-archive` job (linux + emsdk);
`.github/workflows/build.yml` adds it and uploads the new artifact.

---

## 11. Build flags

### 11.1 Defaults

| Flag | Default | Notes |
|---|---|---|
| `OPENRAR_WASM_CLI` | **OFF** | Brief: drop to OFF. Conformance only. |
| `FILESYSTEM` | **0** (block + archive) | Pinned; MEMFS only in `openrar_cli.js`. |
| `ALLOW_MEMORY_GROWTH` | **1** | Required for >200 MiB heap. |
| `MODULARIZE` + `EXPORT_ES6` | **1** | Both targets. |
| `EXPORT_NAME` | `createOpenRAR` / `createOpenRARArchive` | Distinct factories → lazy-load. |

### 11.2 `wasm-cli` posture

Before: `OPENRAR_WASM_CLI=ON` produced `openrar_cli.js` with MEMFS for
archive a/x/t. New posture: **conformance-only** — kept for
`tools/interop_gate.py` and manual WinRAR round-trips, **not** a default
consumer target. Documented in `wasm/README.md`. Crate feature-detects
and stays off the main bundle.

### 11.3 New target

`cmake/wasm.cmake` adds `openrar_wasm_archive` linking `openrar_wasm_core`
+ `src/wasm/archive_api.cpp` + `src/compress/stream_encoder.cpp`.
`EXPORTED_FUNCTIONS` lists every name in §1 plus the runtime methods
in §4.1.

---

## 12. Work breakdown

Eight ordered, independently shippable steps.

| # | Step | LOC | Risk |
|---|---|---:|---|
| 1 | `BufferStream` + `ArchiveReader::open_buffer` (new `src/io/buffer_stream.{hpp,cpp}`; edit `src/archive/archive_reader.{hpp,cpp}`; `CMakeLists.txt`) | 150 + 80 tests | Low — `read_block_raw_mem` already exists |
| 2 | `ArchiveMutator::add_buffer` + `HeaderWriter` sink (edit mutator + writer) | 280 + 100 tests | Low — mechanical lift |
| 3 | `Compressor50::feed` + `StreamEncoder` + native tests | 250 + 100 tests | Medium — Path A save/restore must touch every inner-loop field |
| 4 | C ABI exports (new `src/wasm/archive_api.{hpp,cpp}`; bump `WASM_API_VERSION` to 3) | 405 + 250 tests | Low — mechanical |
| 5 | JS wrapper + `.d.ts` (new `wasm/js/openrar_archive.{js,d.ts}`) | 200 + 200 tests | Low — pattern matches `openrar.js` |
| 6 | CMake wiring + CI job (edit `cmake/wasm.cmake`; `.github/workflows/build.yml`) | 60 | Low |
| 7 | Documentation (`wasm/README.md`, `docs/wasm-limitations.md`, `docs/wasm-examples.md`) | 200 | None |
| 8 | Interop smoke (edit `tools/interop_gate.py`) | 60 | Low |

**Totals:** ~1605 LOC C++/JS + ~730 LOC tests = ~2335 LOC. Higher than
brief's "~650 LOC MVP" because streaming adds ~250 and `extract_all` +
offsets table adds ~150; budget is bounded.

---

## 13. Migration path

**Version bump 2 → 3.** Justification:

- Block-codec WASM ABI is **unchanged**. Same exports, same `.js` URL,
  same `.d.ts`. A v2 consumer continues to load `openrar.js` without
  modification.
- New functionality ships in a **separate file**
  (`wasm/dist/openrar_archive.js`), not a rebuilt `openrar.js`.
- Bumping `WASM_API_VERSION` to 3 lets the block-codec loader detect a
  v3 build and warn if `openrar_archive.js` is missing — a coercion
  signal for consumers trying archive APIs with only block codec loaded.
- The bespoke `Archive` class dropped in `e13c5f3` never shipped. No
  consumer depends on it.

So 2 → 3 is feature-additive, not contract-breaking.

| Consumer | Impact | Action |
|---|---|---|
| Block-codec (`openrar.compress`/`decompress`) | None | None |
| Reading `WASM_API_VERSION` constant | Value 2 → 3; `module.version()` returns 3 | Update constant; loader already validates |
| Past `e13c5f3` referencing dropped `Archive` class | Already broken | Migrate to `createArchive` / `extractAll` |
| `wasm-cli` users | None (target unchanged when enabled) | None |

**`.d.ts` placement:** `wasm/js/openrar_archive.d.ts` is a **separate
file** from `wasm/js/openrar.d.ts`; consumers
`import { createArchive } from 'openrar-archive'`. Two declaration
files are fine — TypeScript handles via the package's `types` field.

**Release artefact:** three artifacts in the GitHub Release —
`openrar.{js,wasm}` (block codec, ABI v3, content unchanged),
`openrar_archive.{js,wasm}` (archive API, new), `openrar_cli.{js,wasm}`
(full CLI, conformance only, optional).