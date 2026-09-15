# OpenRAR Native DLL — Integration Spec

**Status:** v1.0 — shipped with `openrar.dll` / `libopenrar.so` / `libopenrar.dylib` Hybrid D
**Artifact:** `openrar.dll` (Windows), `libopenrar.so` (Linux), `libopenrar.dylib` (macOS)
**API version:** `OPENRAR_DLL_API_VERSION = 1` (`src/dll/openrar_dll.h:44`), archive ABI `openrar_archive_version() == 1`
**Language:** C ABI + header-only C++17 wrapper (`include/openrar/openrar.hpp`)
**Source:** `src/dll/openrar_dll.h`, `src/dll/dll_api.cpp`, `include/openrar/openrar.hpp`
**Build:** `CMakeLists.txt` `openrar_dll` SHARED target, C++17, `RUNTIME_OUTPUT_DIRECTORY` = `build/`

---

## 1. Design Tenets (why Hybrid D)

| Tenet | How |
|---|---|
| Single stable ABI | All exports `extern "C"` `__cdecl` (`openrar_dll.h:36`), no C++ mangling, no STL across boundary. Survives MSVC / Clang / GCC upgrades. |
| One binary | `openrar.dll` contains block codec + BufferArchive + StreamEncoder. No split (`openrar_archive.dll`) — desktop has no 500 KiB WASM budget. |
| No fork | DLL forwards to `src/archive/buffer_archive.hpp:53`, `src/compress/compressor50.hpp:142` / `decompressor50.hpp:79`, `src/compress/stream_encoder.hpp:17`. WASM `src/wasm/wasm_api.hpp` / `archive_api.hpp` contracts reused, not duplicated. |
| Two ergonomics | C for FFI (`C#`/`Python`/`Rust`), inline C++ for `std::filesystem`/`std::vector` consumers — header-only, no second ABI. |

---

## 2. Package Layout

```
build/
  openrar.dll            # Windows default (also build/openrar.lib import lib)
  openrar_x64.dll        # Windows x64  (-DOPENRAR_DLL_SUFFIX=_x64)
  openrar_arm64.dll      # Windows ARM64 (-DOPENRAR_DLL_SUFFIX=_arm64)
  libopenrar.so          # Linux x64
  libopenrar_x64.so      # Linux x64 (suffixed)
  libopenrar_arm64.so    # Linux ARM64
  libopenrar.dylib       # macOS
include/
  openrar/
    openrar_dll.h        # C ABI — sole public FFI header
    openrar.hpp          # C++17 wrapper (includes openrar_dll.h)
    openrar_dll.h        # shim -> src/dll/openrar_dll.h (for old include path)
src/dll/
  openrar_dll.h          # canonical C ABI
  dll_api.cpp            # implementation
```

**Arch note:** One binary per ISA (x64 vs arm64 vs armv7) — OS cannot load mismatched arch. Within one ISA the DLL already runtime-dispatches (`src/core/cpu.cpp:30` detects SSE2/AES-NI/NEON at startup), so no `_sse2`/`_avx2` variants needed. Pass `-DOPENRAR_DLL_SUFFIX=_x64` (or `_arm64`) to `cmake` to produce side-by-side files; default (no suffix) stays `openrar.dll` for single-arch local use. CI matrix (`build.yml`) publishes `win-x64` + `win-arm64` artifacts.

Consumers copy `include/openrar/` + the built library. No runtime deps beyond CRT / `libstdc++` / `libc++` and `Threads`.

**CMake integration (recommended):**

```cmake
add_subdirectory(openrar) # provides openrar_dll target
target_link_libraries(myapp PRIVATE openrar_dll)
target_include_directories(myapp PRIVATE openrar/include)
```

Or link prebuilt:

```cmake
find_library(OPENRAR_LIB openrar PATHS ${OPENRAR_DIR}/build)
target_include_directories(myapp PRIVATE ${OPENRAR_DIR}/include)
target_link_libraries(myapp PRIVATE ${OPENRAR_LIB})
```

---

## 3. ABI Stability

* `OPENRAR_DLL_API` is `__declspec(dllexport)` / `__declspec(dllimport)` on Windows, `visibility("default")` elsewhere (`openrar_dll.h:19`).
* `OPENRAR_DLL_CALL` is `__cdecl` on Windows (`openrar_dll.h:36`), default elsewhere. **Do not use `__stdcall` / COM.**
* `openrar_archive_entry_t` is **64 bytes, `#pragma pack(push,1)`** (`openrar_dll.h:93`), `static_assert` in `archive_api.hpp:69`. Fields are little-endian (native on `x86`/`wasm32`).
* Version probe at startup:

```c
if (openrar_version() != OPENRAR_DLL_API_VERSION) abort(); // host / DLL mismatch
if (openrar_archive_version() != 1) abort();
```

* Adding new exports is minor; breaking `openrar_archive_entry_t` or removing an export is major (bump `SOVERSION`).
* **Negotiating additive exports.** `OPENRAR_DLL_API_VERSION` does *not* change when exports are added (`docs/versioning.md`) — strict-equality probes keep working across versions. Detect a capability at runtime instead:
  * `openrar_abi_features()` returns a `uint64_t` bitmask — `OPENRAR_ABI_FEATURE_LIST_PROGRESS` (0x1), `OPENRAR_ABI_FEATURE_LIST_PASSWORD` (0x2), `OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS` (0x4) — once the DLL is loaded;
  * or resolve the symbol directly: `GetProcAddress(hmod, "openrar_archive_list_file_ex")` / `dlsym`.
  * **Do not reference new exports through the import lib if you must keep running against older DLLs** — the process fails to *load* before any version check runs. Either resolve dynamically as above or link with `/DELAYLOAD:openrar.dll`.

---

## 4. Memory & Ownership

* **Single heap:** `openrar_alloc` / `openrar_archive_alloc` are aliases (`dll_api.cpp:12`) — both are `malloc`. Free with **either** `openrar_free` or `openrar_archive_free`. Never `::free` / `delete`.
* Every `uint8_t** out_ptr` / `void** entries_out` / `uint64_t** offsets` returned by the DLL **must** be freed by the caller with `openrar_free`. On empty output (`size==0`) the DLL sets `*ptr = nullptr` — `free(nullptr)` is safe.
* `HEAPU8` aliasing note from WASM **does not apply** — DLL heap does not relocate on growth. No `safeRead` dance.
* Peak memory per call (same as `docs/wasm-archive-spec.md:6`):

| Op | Peak | Notes |
|---|---|---:|
| `list` / `list_ex` | `~rar.byteLength` | buffer must be resident; scan only |
| `list_file` | `~rar.byteLength` | slurps the file into memory, then scans |
| `list_file_ex` | `~Σ(entries + paths)` | **streams from disk** — header walk reads only headers, no whole-file buffer |
| `extractFile` | `~rar + per-file` | per-file |
| `extractAll` | `~rar + Σ(unp)` | single alloc, re-used |
| `create` | `Σ(data_len) + archive` | per-file compressed freed before next |
| `stream_finish` | `Σ(input) + Σ(compressed)` | materialised on `finish` |

---

## 5. Errors

```c
enum RarError {
    RAR_OK = 0,
    RAR_ERR_PARTIAL_OK = 1,
    RAR_ERR_NOT_RAR = -1,
    RAR_ERR_UNSUPPORTED_FEATURE = -2,
    RAR_ERR_TRUNCATED = -3,
    RAR_ERR_CRC_MISMATCH = -4,
    RAR_ERR_NOMEM = -5,
    RAR_ERR_IO = -6,
    RAR_ERR_BAD_PASSWORD = -7,
    RAR_ERR_INVALID_ARG = -9,
    RAR_ERR_ABORTED = -11,
    RAR_ERR_ENCRYPTED = -12
};
```

* Numeric return; detail string via `openrar_archive_get_error(buf, len)` / `openrar_last_error` (`dll_api.cpp:20` — `thread_local g_last_error`). Valid until next archive call on same thread.
* `block` codec returns `1` on success, `0` on failure (kept for `wasm_api.hpp:42` compat). Archive API returns `RAR_*`.

| Code | When |
|---|---|
| `NOT_RAR` | No `R  a  r  ! 0x1A 0x07 0x01 0x00` at `0` and SFX scan (4 MiB) failed |
| `TRUNCATED` | `data_size` past buffer, mid-vint, or `extract` reads past EOF |
| `CRC_MISMATCH` | `FHEXTRA_HASH` BLAKE2 / `FHFL_CRC32` mismatch on extract |
| `UNSUPPORTED_FEATURE` | encrypted entry, multivolume `MHFL_VOLUME`, solid `FCI_SOLID`, method ∉ {0,3,5} |
| `INVALID_ARG` | `window_log2 ∉ [1,4]`, `method ∉ {0,3,5}`, path empty / >2048 B / contains `\` or `..`, `nullptr` |
| `BAD_PASSWORD` | `list_file_pw` with a password that does not decrypt the headers (PswCheck mismatch / header CRC) |
| `ABORTED` | `openrar_cancel_cb` returned non-zero during `feed` / `extract` / `_ex` listing / `open_ex` |
| `ENCRYPTED` | header-encrypted archive (`HEAD_CRYPT`) reached with no password — `_ex` and `_pw` listing; the non-`_ex` listing calls keep `UNSUPPORTED_FEATURE` for this condition |

---

## 6. C ABI Reference

### 6.1 Lifecycle

```c
int openrar_version(void);                // == OPENRAR_DLL_API_VERSION
int openrar_archive_version(void);        // == 1
void* openrar_alloc(size_t n); void openrar_free(void* p);
void* openrar_archive_alloc(size_t n); void openrar_archive_free(void* p); // alias
int openrar_last_error(char* buf, int len);
int openrar_archive_get_error(char* buf, int len); // same
```

### 6.2 Block codec

```c
// method 0..5 (0=store, 3=default, 5=max), win_size 0 => 2 MiB default, max 4 GiB.
int openrar_compress(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len, int method);
int openrar_compress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len, int method, size_t win_size);
int openrar_decompress(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len);
int openrar_decompress2(const uint8_t* src, size_t src_len, uint8_t** out_ptr, size_t* out_len, size_t win_size);
```

### 6.3 Archive entry

```c
#pragma pack(push,1)
typedef struct {
    uint32_t path_offset;  // offset into paths blob
    uint32_t path_len;     // UTF-8, '/' sep, '/' suffix iff dir
    uint32_t is_dir;
    uint32_t method;       // 0,3,5
    uint32_t is_encrypted;
    uint32_t crc32;        // 0 == unknown
    uint64_t size;         // uncompressed
    uint64_t packed_size;  // 0 for dirs
    uint64_t mtime;        // UNIX seconds (DOS time converted)
    uint64_t _pad[2];
} openrar_archive_entry_t; // 64 B
#pragma pack(pop)
```

### 6.4 Buffer archive — one-shot

```c
int openrar_archive_list(const uint8_t* data, size_t size,
                         uint32_t* count, void** entries_out, void** paths_out, size_t* paths_size_out);
void openrar_archive_list_free(void* entries, void* paths, size_t paths_size);

int openrar_archive_extract(const uint8_t* data, size_t size, uint32_t entry_index,
                            uint8_t** out_ptr, size_t* out_len);

int openrar_archive_extract_all(const uint8_t* data, size_t size,
                                uint8_t** buf_out_ptr, size_t* buf_size_out,
                                uint64_t** offsets_out_ptr, uint32_t* offsets_count_out);
// offsets is [offset,size, offset,size, ...] length = count*2

int openrar_archive_create(const uint8_t* const* paths_arr,
                           const uint8_t* const* data_arr,
                           const size_t* sizes_arr,
                           uint32_t file_count,
                           int method, uint32_t window_log2,
                           uint8_t** out_ptr, size_t* out_len);
// method ∈ {0,3,5}, window_log2 ∈ [1,4] (128K,256K,512K,1M). 5 => 4M for stream compat.
```

### 6.5 Handle API (scan-once)

```c
uint32_t openrar_archive_open(const uint8_t* data, size_t size);
uint32_t openrar_archive_open_ex(const uint8_t* data, size_t size,
                                 openrar_progress_cb progress, openrar_cancel_cb cancel, void* user);
void     openrar_archive_close(uint32_t handle);
int      openrar_archive_handle_list(uint32_t handle, uint32_t* count, void** entries_out, void** paths_out, size_t* paths_size_out);
int      openrar_archive_handle_extract(uint32_t handle, uint32_t entry_index, uint8_t** out_ptr, size_t* out_len);
int      openrar_archive_handle_extract_all(uint32_t handle, uint8_t** buf_out_ptr, size_t* buf_size_out, uint64_t** offsets_out_ptr, uint32_t* offsets_count_out);
```

`open_ex` runs the scan that happens at open time with byte progress and a
cancel poll between header blocks (same semantics as the `_ex` listing
exports). Failure returns 0 with the detail in `openrar_archive_get_error`; a
cancelled scan returns 0 with an "open aborted" detail. Entry semantics are
those of `openrar_archive_list` (rejects encrypted/solid/multi-volume/
recovery archives).

### 6.6 File helpers (buffer + file flexibility)

```c
int openrar_archive_list_file(const char* arc_path, uint32_t* count, void** entries_out, void** paths_out, size_t* paths_size_out);
int openrar_archive_extract_file(const char* arc_path, uint32_t entry_index, uint8_t** out_ptr, size_t* out_len);
int openrar_archive_extract_file_to_path(const char* arc_path, uint32_t entry_index, const char* dest_path);
int openrar_archive_create_from_paths(const char* const* src_paths, const char* const* arc_names,
                                      uint32_t file_count, int method, uint32_t window_log2,
                                      uint8_t** out_ptr, size_t* out_len);
int openrar_archive_create_to_file(const char* const* src_paths, const char* const* arc_names,
                                   uint32_t file_count, int method, uint32_t window_log2,
                                   const char* out_path);
// arc_path / src_paths / out_path are UTF-8, forwarded to std::filesystem::u8path. Create parent dirs.
```

### 6.7 Streaming block codec

```c
uint32_t openrar_stream_create(int method, uint32_t window_log2); // 1..5, 5=>4M. 0 on invalid.
int      openrar_stream_feed(uint32_t handle, const uint8_t* src, size_t n);
int      openrar_stream_finish(uint32_t handle, uint8_t** out_ptr, size_t* out_len);
void     openrar_stream_free(uint32_t handle);
```

### 6.8 Progress / cancel

```c
typedef void (OPENRAR_DLL_CALL *openrar_progress_cb)(void* user, uint64_t done, uint64_t total);
typedef int  (OPENRAR_DLL_CALL *openrar_cancel_cb)(void* user); // non-zero => abort
int openrar_stream_set_progress(uint32_t handle, openrar_progress_cb cb, void* user);
int openrar_stream_set_cancel(uint32_t handle, openrar_cancel_cb cb, void* user);
```

Polled between blocks (`feed` / `finish` / `extract_all`). `done` is monotonic. (`openrar_set_progress` / `openrar_set_cancel` were removed — they were silent no-ops; callbacks are wired per-call or per-handle as above.)

### 6.9 Listing with progress / cancel (_ex, additive)

```c
#define OPENRAR_ABI_FEATURE_LIST_PROGRESS (1ull << 0)
uint64_t openrar_abi_features(void); // bit set <=> these exports exist

int openrar_archive_list_file_ex(const char* arc_path,
                                 uint32_t* count, void** entries_out,
                                 void** paths_out, size_t* paths_size_out,
                                 openrar_progress_cb progress,
                                 openrar_cancel_cb cancel, void* user);
int openrar_archive_list_ex(const uint8_t* data, size_t size,
                            uint32_t* count, void** entries_out,
                            void** paths_out, size_t* paths_size_out,
                            openrar_progress_cb progress,
                            openrar_cancel_cb cancel, void* user);
```

* **Byte-based progress.** RAR has no central directory, so the entry count is unknown until the walk completes — there is no entry-index denominator. `done` = archive bytes consumed (includes any SFX prefix), `total` = archive size; cumulative, monotonic, and exactly one `(total, total)` callback fires on success. Callbacks may fire before a failing return (e.g. mid-SFX-scan). The file variant walks the archive **on disk** — it does not slurp the file — so it also avoids materialising multi-GB archives in RAM.
* **Cancel** is polled between header blocks (and per 64 KiB chunk during the SFX scan). A non-zero return aborts with `RAR_ERR_ABORTED`; all outputs stay null/zero and nothing partial is ever allocated (packing happens only after a successful walk).
* Both callbacks run on the calling thread with **no DLL-internal lock held** and receive the same `user` pointer. Either may be `NULL`; with both `NULL` the calls are equivalent to `openrar_archive_list_file` / `openrar_archive_list`.
* **Header-encrypted archives** return `RAR_ERR_ENCRYPTED` (-12) as soon as the `HEAD_CRYPT` block is reached — prompt the user immediately instead of waiting out a walk that could never succeed. Listing itself never needs a password; header-encrypted archives cannot be listed without one.
* All other semantics (entry layout, error mapping, `UNSUPPORTED_FEATURE` for encrypted/solid/multi-volume/recovery) are identical to the non-`_ex` calls.

### 6.10 Listing with a password (_pw; additive)

```c
#define OPENRAR_ABI_FEATURE_LIST_PASSWORD (1ull << 1)
int openrar_archive_list_file_pw(const char* arc_path, const char* password,
                                 uint32_t* count, void** entries_out,
                                 void** paths_out, size_t* paths_size_out,
                                 openrar_progress_cb progress,
                                 openrar_cancel_cb cancel, void* user);
```

Completes the flow `list_file_ex` starts: list → `RAR_ERR_ENCRYPTED` → prompt → **re-list with `openrar_archive_list_file_pw`**. It streams from disk like `list_file_ex`, and when the archive carries a clear `HEAD_CRYPT` block it derives keys from `password` (PBKDF2) and decrypts every following header (AES-256-CBC). Empty or `NULL` password counts as none; a password on an archive without header encryption is ignored.

| Condition | Result |
|---|---|
| Header-encrypted, no password | `RAR_ERR_ENCRYPTED` (same early signal as `_ex`) |
| Header-encrypted, wrong password | `RAR_ERR_BAD_PASSWORD` |
| Header-encrypted, correct password | `RAR_OK`, headers decrypted |
| Unknown `HEAD_CRYPT` crypto version | `RAR_ERR_UNSUPPORTED_FEATURE` |

Unlike the frozen list/list_ex/list_file_ex semantics, file entries whose payload is encrypted are **reported** (`is_encrypted = 1`) and the walk continues: `-hp` implies encrypted file data for every entry, so rejecting them would make password listing useless. Solid, multi-volume and recovery archives stay rejected. Extraction of encrypted entries is still not supported by this DLL, and there is deliberately no password variant of the in-memory `openrar_archive_list` (see `openrar_dll.h`).

---

## 7. C++ Wrapper (`include/openrar/openrar.hpp`)

Header-only, C++17, no exported symbols. All inline forward to C ABI via `check()`. Requires linking `openrar.dll`.

```cpp
#include "openrar/openrar.hpp"

using namespace openrar;

// Block
std::vector<uint8_t> compress_block(const std::vector<uint8_t>& src, int method=3, size_t win=2<<20);
std::vector<uint8_t> compress_block(const uint8_t* data, size_t size, int method, size_t win);
std::vector<uint8_t> decompress_block(const std::vector<uint8_t>& src, size_t win=1<<20);

// Archive
struct Entry { std::string path; uint64_t size, packed_size, mtime; uint32_t crc32, method; bool is_dir, is_encrypted; uint32_t index; };
struct InputFile { std::string path; std::vector<uint8_t> data; uint64_t mtime=0; };
struct CreateOptions { int method=3; uint32_t window_log2=4; };

std::vector<Entry> list_archive(const std::vector<uint8_t>& rar);
std::vector<Entry> list_archive(const uint8_t* data, size_t size);
std::vector<Entry> list_archive_file(const std::filesystem::path& arc);
// _ex overloads: byte progress + cancel (either callback may be nullptr).
std::vector<Entry> list_archive(const std::vector<uint8_t>& rar, openrar_progress_cb progress,
                                openrar_cancel_cb cancel = nullptr, void* user = nullptr);
std::vector<Entry> list_archive_file(const std::filesystem::path& arc, openrar_progress_cb progress,
                                     openrar_cancel_cb cancel = nullptr, void* user = nullptr);
// _pw overload: header-encrypted archives list with `password`; wrong
// password throws with RAR_ERR_BAD_PASSWORD.
std::vector<Entry> list_archive_file(const std::filesystem::path& arc, const char* password,
                                     openrar_progress_cb progress = nullptr,
                                     openrar_cancel_cb cancel = nullptr, void* user = nullptr);
std::vector<uint8_t> extract_file(const std::vector<uint8_t>& rar, uint32_t index);
std::map<std::string, std::vector<uint8_t>> extract_all(const std::vector<uint8_t>& rar);

std::vector<uint8_t> create_archive(const std::vector<InputFile>& files, CreateOptions opts={});

class ArchiveHandle { // RAII, move-only
  ArchiveHandle(const std::vector<uint8_t>& rar);
  ArchiveHandle(const uint8_t* data, size_t size);
  // open_ex overloads: progress/cancel over the open-time scan.
  ArchiveHandle(const std::vector<uint8_t>& rar, openrar_progress_cb progress,
                openrar_cancel_cb cancel = nullptr, void* user = nullptr);
  ArchiveHandle(const uint8_t* data, size_t size, openrar_progress_cb progress,
                openrar_cancel_cb cancel, void* user = nullptr);
  std::vector<Entry> list() const;
  std::vector<uint8_t> extract(uint32_t idx) const;
};

class StreamEncoder { // RAII, move-only
  StreamEncoder(int method=3, uint32_t window_log2=5);
  void feed(const std::vector<uint8_t>& chunk);
  void feed(const uint8_t* data, size_t size);
  std::vector<uint8_t> finish();
};
```

Throws `std::runtime_error` with `code + detail` on `!= RAR_OK`.

---

## 8. Recipes

### 8.1 C — one-shot create / list / extract

```c
#include "openrar/openrar_dll.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    if (openrar_version() != OPENRAR_DLL_API_VERSION) return 1;

    const char* paths[] = {"hello.txt", "dir/nested.txt"};
    const uint8_t data1[] = "hello world";
    const uint8_t data2[] = "nested content here";
    const uint8_t* datas[] = {data1, data2};
    size_t sizes[] = {11, 19};
    const uint8_t* path_ptrs[] = {(const uint8_t*)paths[0], (const uint8_t*)paths[1]};

    uint8_t* rar = NULL; size_t rar_len = 0;
    int rc = openrar_archive_create(path_ptrs, datas, sizes, 2, 3, 4, &rar, &rar_len);
    if (rc != RAR_OK) { char b[256]; openrar_archive_get_error(b, sizeof(b)); fprintf(stderr, "%s\n", b); return 1; }

    uint32_t count = 0; void *ents = NULL, *pbuf = NULL; size_t psize = 0;
    rc = openrar_archive_list(rar, rar_len, &count, &ents, &pbuf, &psize);
    // ... read openrar_archive_entry_t + paths blob ...
    openrar_archive_list_free(ents, pbuf, psize);

    uint8_t* out = NULL; size_t out_len = 0;
    rc = openrar_archive_extract(rar, rar_len, 0, &out, &out_len);
    // ... use out ...
    openrar_free(out);
    openrar_free(rar);
}
```

### 8.2 C — handle reuse (list → extract without re-scan)

```c
uint32_t h = openrar_archive_open(rar, rar_len);
uint32_t n; void *e,*p; size_t ps;
openrar_archive_handle_list(h, &n, &e, &p, &ps);
// ... 
uint8_t *buf; size_t blen;
openrar_archive_handle_extract(h, 1, &buf, &blen);
openrar_free(buf);
openrar_archive_list_free(e,p,ps);
openrar_archive_close(h);
```

### 8.3 C — file helpers (no manual read)

```c
const char* srcs[] = {"C:/data/a.txt", "C:/data/b.bin"};
const char* arcs[] = {"a.txt", "b.bin"};
int rc = openrar_archive_create_to_file(srcs, arcs, 2, 3, 4, "out.rar");

uint32_t cnt; void *e,*p; size_t ps;
openrar_archive_list_file("out.rar", &cnt, &e, &p, &ps);
openrar_archive_extract_file_to_path("out.rar", 0, "C:/tmp/a.txt");
```

### 8.4 C — streaming large input (bounded memory)

```c
uint32_t h = openrar_stream_create(3, 5); // 4M window
for (each chunk) openrar_stream_feed(h, chunk, n);
uint8_t* out; size_t out_len;
openrar_stream_finish(h, &out, &out_len);
openrar_stream_free(h);
// out is block-codec payload — decompress with openrar_decompress
```

### 8.5 C — listing a large archive with progress + cancel

```c
struct Ctx { int aborted; uint64_t ui_total; };
static void OPENRAR_DLL_CALL my_progress(void* user, uint64_t done, uint64_t total) {
    // done/total are bytes of archive consumed — an honest percentage.
    ((Ctx*)user)->ui_total = total;
    set_percent((int)(100.0 * done / total));
}
static int OPENRAR_DLL_CALL my_cancel(void* user) {
    return ((Ctx*)user)->aborted; // polled between header blocks
}

// Header-encrypted archive => RAR_ERR_ENCRYPTED immediately; prompt, then
// report. Listing streams from disk; no whole-file buffer.
uint32_t n; void *e, *p; size_t ps;
Ctx ctx = {0, 0};
int rc = openrar_archive_list_file_ex("big.rar", &n, &e, &p, &ps, my_progress, my_cancel, &ctx);
if (rc == RAR_ERR_ENCRYPTED) {
    // Headers need a password: prompt, then re-list with it (wrong password
    // comes back as RAR_ERR_BAD_PASSWORD — loop until correct or cancelled).
    const char* pw = prompt_password();
    rc = openrar_archive_list_file_pw("big.rar", pw, &n, &e, &p, &ps,
                                      my_progress, my_cancel, &ctx);
}
else if (rc == RAR_ERR_ABORTED) { /* user cancelled; outputs untouched */ }
if (rc == RAR_OK) openrar_archive_list_free(e, p, ps);
```

### 8.6 C++ — RAII

```cpp
#include "openrar/openrar.hpp"
using namespace openrar;

std::vector<InputFile> files{{"a.txt", {'h','i'}, 0}, {"d/b.txt", {'b','y','e'}, 0}};
auto rar = create_archive(files, {3,4});
auto entries = list_archive(rar);
auto all = extract_all(rar); // map<string, vector<uint8_t>>

ArchiveHandle h(rar);
auto e = h.extract(0);

StreamEncoder enc(3,5);
enc.feed(std::vector<uint8_t>{'h','i'});
auto comp = enc.finish();
auto dec = decompress_block(comp);
```

### 8.7 C# — P/Invoke

```csharp
using System.Runtime.InteropServices;

class OpenRar {
  const string Dll = "openrar";
  [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)]
  static extern int openrar_version();
  [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)]
  static extern IntPtr openrar_alloc(nuint n);
  [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)]
  static extern void openrar_free(IntPtr p);
  [DllImport(Dll, CallingConvention=CallingConvention.Cdecl)]
  static extern int openrar_archive_create(
    IntPtr[] paths, IntPtr[] datas, nuint[] sizes,
    uint count, int method, uint windowLog2,
    out IntPtr outPtr, out nuint outLen);
  // ... similarly list/extract ...
}
```

Marshal `paths` as `byte*` via `Marshal.StringToHGlobalAnsi` + `openrar_alloc` for output.

### 8.8 Python — ctypes

```python
import ctypes, pathlib
lib = ctypes.CDLL(str(pathlib.Path("build/openrar.dll").resolve()))
lib.openrar_version.restype = ctypes.c_int
lib.openrar_archive_create.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
                                       ctypes.POINTER(ctypes.c_size_t), ctypes.c_uint32,
                                       ctypes.c_int, ctypes.c_uint32,
                                       ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
paths = [b"hello.txt"]
# ... allocate c_void_p arrays, call, then lib.openrar_free(out_ptr) ...
```

### 8.9 Rust — FFI

```rust
#[link(name="openrar")]
extern "C" {
    fn openrar_version() -> i32;
    fn openrar_archive_create(paths: *const *const u8, datas: *const *const u8,
        sizes: *const usize, count: u32, method: i32, window_log2: u32,
        out_ptr: *mut *mut u8, out_len: *mut usize) -> i32;
    fn openrar_free(p: *mut std::ffi::c_void);
}
```

---

## 9. Path & Validation

* Archive paths are UTF-8, `/` separators, `//` and `\` rejected (`src/archive/buffer_archive.cpp:260` `validate_archive_path`).
* `2048` byte max, no leading `/`, no `..` segment, no control `<0x20` / NUL.
* Dir entries **must** have trailing `/`, files **must not**.
* `arc_names` in `create_from_paths` are the on-disk names — pass `/`-separated. `src_paths` are OS paths (`C:\…` or `/…`).

---

## 10. Thread Safety & Progress

* `openrar_alloc` / block codec are thread-safe (no global).
* Archive **handle map** (`dll_api.cpp:18` `g_handles` + `mutex`) is locked per call. Same `handle` must not be used concurrently from two threads; distinct handles are safe.
* `openrar_stream_*` handles are same — one thread per handle.
* `thread_local g_last_error` — call `openrar_archive_get_error` on same thread as failing call.

**Cancel / progress:**

```c
int my_cancel(void* user){ return ((MyCtx*)user)->should_abort ? 1 : 0; }
openrar_stream_set_cancel(h, my_cancel, &ctx);
openrar_stream_set_progress(h, my_progress, &ctx); // void(*)(void*, uint64_t done, uint64_t total)
```

Polled between blocks; abort returns `RAR_ERR_ABORTED`. The `_ex` listing exports (§6.9) take the same callbacks per call, run them on the calling thread with no internal lock held, and are safe to call from any single thread — different threads may list different archives concurrently.

---

## 11. Build from Source

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
# → build/openrar.dll + build/openrar.lib + tests

# Side-by-side arch variants (optional):
cmake -B build-x64 -A x64 -DOPENRAR_DLL_SUFFIX=_x64
cmake -B build-arm64 -A ARM64 -DOPENRAR_DLL_SUFFIX=_arm64
cmake --build build-x64 --config Release
cmake --build build-arm64 --config Release
# → build-x64/openrar_x64.dll  +  build-arm64/openrar_arm64.dll
```

Options: `-DOPENRAR_INMEM_ARCHIVE=ON` also builds `buffer_archive_tests` but DLL is always built.

---

## 12. Deployment

* **Windows:** place `openrar.dll` (or suffixed `openrar_x64.dll`/`openrar_arm64.dll`) beside `.exe` or on `PATH`. Import lib `openrar.lib` (or `openrar_x64.lib`) for link. `dumpbin /EXPORTS` lists 38 exports (the declarations in `openrar_dll.h` are the source of truth). Do not mix x64 DLL into arm64 process — OS will reject.
* **Linux:** `libopenrar.so` (`SOVERSION 1`) or `libopenrar_x64.so`/`libopenrar_arm64.so`, `rpath $ORIGIN` or `LD_LIBRARY_PATH`.
* **macOS:** `libopenrar.dylib` (or universal2 via `lipo`), `install_name @rpath/libopenrar.dylib`.
* No `SharedArrayBuffer` / `COOP/COEP` requirement (WASM-only).

---

## 13. Security Notes

* Inputs are validated (`method`, `window_log2`, `path`, `win_size <= 4 GiB`). Oversize `win_size` returns `0` / `RAR_ERR_INVALID_ARG`.
* Decompress writes only after `prior = out.size()` guard — corrupt bitstream clears partial output (`src/wasm/wasm_api.cpp:43`).
* Extraction does not create files on its own except `*_to_path` / `*_to_file` which `create_directories(parent)` and overwrite existing. Caller must validate `dest_path` stays inside intended root if archive is untrusted (check `entry.path` for `..` already rejected, but do not trust `mtime`/`crc`).

---

## 14. Limitations (v1)

* `UNSUPPORTED_FEATURE` for: encrypted entries (`FHEXTRA_CRYPT`), multivolume `MHFL_VOLUME`, solid `FCI_SOLID`, recovery `MHFL_PROTECT`, `method ∉ {0,3,5}`. The `_ex` listing exports keep these codes (§6.9); only the header-encrypted case maps to `RAR_ERR_ENCRYPTED` there. `list_file_pw` (§6.10) additionally reports encrypted file entries (`is_encrypted = 1`).
* **Password support is listing-only**: header-encrypted archives can be listed with `openrar_archive_list_file_pw`, but extraction of encrypted entries (file data) is not supported by any DLL export. The in-memory listing exports take no password by design (§6.10).
* No NTFS ACL/STM, no `FHEXTRA_HTIME` ns, no symlinks — stored entries still list but extract skips with `OK`.
* `window_log2 5` (4M) only for stream; buffer `create` clamps `[1,4]`.

---

## 15. Migration from WASM

| WASM | DLL |
|---|---|
| `createOpenRAR()` + `HEAPU8.set` + `ccall('openrar_compress2',…)` | `openrar_compress2` direct, no heap copy |
| `openrar_alloc` / `_malloc` mix | single `openrar_alloc` ↔ `openrar_free` |
| `ArchiveEntryOut` 64B with `path_offset` | same `openrar_archive_entry_t` |
| `openrar_archive_list(data,size, …)` | same — drop `HEAPU8_BASE` / `safeRead` |
| `wasm/dist/openrar_archive.js` 1.5M | `openrar.dll` ~2M, no Emscripten |

---

## 16. Version History

* **1.0 (2026-09):** Initial Hybrid D — block + buffer archive + handle + file helpers + streaming + C++ wrapper. Exports 35.
* **1.0.x (2026-09):** Additive — `openrar_archive_list_file_ex` / `openrar_archive_list_ex` (byte progress + cancel; file variant streams from disk), `openrar_abi_features` (`OPENRAR_ABI_FEATURE_LIST_PROGRESS`), `RAR_ERR_ENCRYPTED` (-12). Exports 36. `OPENRAR_DLL_API_VERSION` unchanged (§3).
* **1.1.x (2026-09):** Additive — `openrar_archive_list_file_pw` (password listing of header-encrypted archives; encrypted file entries reported), `openrar_archive_open_ex` (progress/cancel over the open-time scan), feature bits `OPENRAR_ABI_FEATURE_LIST_PASSWORD` / `OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS`. Exports 38. `OPENRAR_DLL_API_VERSION` unchanged (§3).

