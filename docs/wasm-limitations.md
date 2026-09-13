# WASM Interface Limitations

**Status:** Catalogued 2026-09 against `src/wasm/wasm_api.{hpp,cpp}` (C ABI)
+ `wasm/js/openrar.{js,d.ts}` (JS surface) + `cmake/wasm.cmake` (build
configuration). Each entry: **what's missing / wrong**, **workaround**,
**effort to fix**. Items marked **FIXED** were addressed in `e13c5f3` /
subsequent commits.

---

## 1. Functional gaps

### 1.1 No streaming (block codec is all-or-nothing) — **HALF FIXED (encoder)**

**Status (2026-09, API v2):** `OpenRAR.compressStream()` is shipped and
byte-identical to one-shot `compress()` (Path A `Compressor50::feed()`,
gated by `tests/unit/stream_encoder_tests.cpp`). **Decoder-side streaming
(`decompressStream`) is still open** — it needs a resumable decode loop in
`Decompressor50` (same Path-A pattern: internal pump method, never external
state patching).

`openrar_compress2(src, src_len, ...)` materialises the entire input in
memory before the compressor sees a byte.

- **Workaround:** Use the `wasm-cli` build for archive-level work and
  stream via MEMFS. Or chunk the input in JS (`compress(chunk1);
  compress(chunk2); concat`) — but this loses cross-chunk LZ matches and
  produces invalid block framing because `compress()` already emits a
  `last_block=true` per call.
- **Effort to fix:** ~650 LOC (C++ `StreamEncoder` + WASM bindings + JS
  driver).

### 1.2 No archive-level API in the block codec build — **DEFERRED**

**Status:** The bespoke `openrar-wasm-v1` JS-side container has been
removed. The default WASM build (`openrar_wasm`) only exposes RAR5 block
compression/decompression, **no full `.rar` archive I/O**.

**Recommended path forward**: add
`openrar_archive_*` exports to `wasm_api.cpp` that wrap `ArchiveReader`
and `ArchiveMutator` against a new `BufferStream` adapter. Lets JS
produce/read real RAR5 archives in-memory without depending on MEMFS.

- **Workaround:** Use the `wasm-cli` build (`-DOPENRAR_WASM_CLI=ON`) and
  drive via `FS.writeFile` + `callMain(['a', '/out.rar', '/input.bin'])`.
- **Effort to fix:** ~300 LOC (BufferStream + reader/writer buffer
  overloads + new exports). Blocks feature parity with the native CLI.

### 1.3 No encryption / password support in WASM

`src/crypto/aes256.cpp` and `src/crypto/pbkdf2.cpp` build into the WASM
core but are not exposed via the WASM API. The `wasm_api.cpp` surface has
no `openrar_decrypt_*` exports.

- **Workaround:** None from JS. For encrypted RAR5 archives, use the
  `wasm-cli` build and `callMain(['x', '-psecret', '/out.rar', '/dir/'])`.
- **Effort to fix:** ~100 LOC in `wasm_api.cpp`. Crypto paths already
  work on WASM (scalar fallback for AES-NI / SHA-NI).

### 1.4 No recovery-record / Reed-Solomon support in WASM

`src/recovery/rs16.cpp` and `recovery_writer.cpp` build into the WASM
core but are not exposed.

- **Workaround:** Same as 1.3 — use `wasm-cli`.
- **Effort to fix:** ~80 LOC.

### 1.5 No multi-volume / `-v<size>` support

The compressor is single-volume only. Multi-volume slicing is in the
native CLI's `ArchiveMutator` path but not in WASM.

- **Workaround:** `wasm-cli`.
- **Effort to fix:** ~200 LOC once the archive-level API exists (1.2).

---

## 2. Build / distribution

### 2.1 Default build uses scalar AES/SHA — **FIXED §doc**

`__EMSCRIPTEN__` early-outs in `src/crypto/aes256.cpp` and
`src/crypto/sha256.cpp` skip AES-NI / SHA-NI / NEON intrinsics. Pure
scalar fallback runs ~10× slower than native on large payloads.

- **Status:** Documented in `src/crypto/{aes256,sha256}.cpp` and
  `docs/wasm-limitations.md`. Until 1.3 lands (encryption), this only
  matters for synthetic benchmarks.
- **Effort to fix:** Add `wasm_simd128` AES / SHA intrinsics via
  Emscripten's `<wasm_simd128.h>`. ~400 LOC + testing against scalar
  reference. Defer until encryption (1.3) is exposed.

### 2.2 No `-pthread` support unless opted in

WASM threads require `SharedArrayBuffer` which requires `COOP`/`COEP`
HTTP headers. The default build is single-threaded.

- **Status:** Working as designed. CMake flag
  `-DOPENRAR_WASM_THREADS=ON` documented in `cmake/wasm.cmake`.
- **Effort to fix:** None required.

### 2.3 No prebuilt `.wasm` artifact in the repo

Consumers must build from source via `emcmake cmake --preset wasm` or
`make wasm`. Adds ~30 s to setup, requires `emsdk` install.

- **Workaround:** Pin a known-good build via the CI artifact
  (`actions/upload-artifact@v4` already in `.github/workflows/build.yml`).
- **Effort to fix:** Automate GitHub Release with the artifact attached.

### 2.4 No bundler interop (webpack/rollup/vite) — FIXED docs

The Emscripten MODULARIZE output is CommonJS-compatible but uses
`import.meta.url`. The wrapper at `wasm/js/openrar.js` ships as ESM.

- **Status:** Documented in `docs/wasm-examples.md` §11 (added in
  `e13c5f3`); users can `npm --prefix wasm/js install` and import.
- **Effort to fix:** None — accepted.

### 2.5 No `wasm32-wasi` (non-Emscripten) build

Only Emscripten is wired in `cmake/wasm.cmake`. WASI SDK hosts (Wasmtime,
Wasmer) need a different toolchain file.

- **Effort to fix:** Add a `wasm-wasi` preset using `wasi-sdk`'s
  `wasi-sdk.cmake`. ~50 LOC. Deferred — no demand yet.

---

## 3. Correctness / safety

### 3.1 `openrar_alloc` vs `_openrar_alloc` allocator pairing — **FIXED docs**

The C ABI exports `openrar_alloc`/`openrar_free` as a thin `malloc`/`free`
wrapper. The JS-side `OpenRAR.compressHeap()` uses `m._malloc` for its
output-pointer temp vars and `_openrar_free` for the result buffer.
**These are different allocators** — results must be freed with
`_openrar_free`, not `_free`.

- **Status:** Documented in `openrar.js` (`compressHeap` docstring) and
  `wasm/README.md` §2.6.
- **Effort to fix:** Documentation only — done.

### 3.2 No input validation on `openrar_compress*` — **FIXED**

`method < 0`, `method > 5`, `win_size > 4 GiB` were not rejected by the
C ABI. The C++ helper clamped `method` and defaulted `win_size`, but the
C ABI entry points passed straight through to `std::vector` growth.

- **Status:** `wasm_api.cpp` now checks `win_size > MAX_WIN_SIZE` and
  returns 0. New tests `test_oversize_win_size_rejected` cover the path.
  Bumped `WASM_API_VERSION` to 2.

### 3.3 Decoding untrusted bytes leaks partial output — **FIXED**

`decompress_to_vector()` wrote partial output to the destination before
returning false on bit-stream corruption. JS-side callers received
garbage.

- **Status:** `wasm_api.cpp::decompress_buffer_raw` now saves `prior =
  out.size()` and clears on failure. New tests
  `test_decompress_corrupt_returns_empty` and
  `test_decompress_raw_clears_on_failure` cover it.

### 3.4 WASM heap aliasing with `HEAPU8` — **FIXED docs**

`m._openrar_alloc(n)` returns a pointer into the Emscripten-managed heap.
If the host JS code triggers a memory growth (e.g., another allocation
pushes the heap past its current bound), the heap is reallocated and
**all live pointers are invalidated**. The JS wrapper previously did not
warn.

- **Status:** `openrar.js::compressHeap` docstring now documents the
  aliasing rule. Added `OpenRAR.safeRead(ptr, len)` helper that copies
  out before any further WASM call. Demo in
  `docs/wasm-examples.md` §6.

### 3.5 QuickJS host fallback silent

`quickjs-host.mjs::isQuickJS()` is a soft feature check. The fallback is
silent when it triggers on Bun/Deno/etc.

- **Effort to fix:** Add a one-time warning when fallback is taken inside
  a quickjs-emscripten context. ~5 LOC. Cosmetic.

---

## 4. JS surface gaps

### 4.1 No `OpenRAR.destroy()` — **FIXED**

The Emscripten module had no `Module.destroy()` call. Once
`createOpenRAR()` resolved, the WASM heap was held for the lifetime of
the JS realm.

- **Status:** Added `OpenRAR.destroy()` that nulls the singleton and
  best-effort releases the module. Test
  `OpenRAR.destroy() prevents further use` covers it. Caveat documented:
  no way to enumerate live user allocations in Emscripten, so the
  singleton reset is best-effort and pending GC.

### 4.2 `Archive.fromBytes` allocates a fresh `OpenRAR` per call — **FIXED (removed)**

The bespoke `Archive` class was dropped in `e13c5f3`. `Archive.fromBytes`
no longer exists. For archive-level work, see `docs/wasm-examples.md` §8.

### 4.3 No streaming for `Archive.toBytes` / `fromBytes`

Subsumed by 1.2 (real-RAR5 archive work is deferred).

### 4.4 No `Archive` deduplication / hardlinks / symlinks

Same as 4.3.

### 4.5 `hex()` and `base64Encode()` are synchronous and O(N)

They walk the entire byte array in JS. For multi-MB blobs this blocks
the event loop.

- **Effort to fix:** Add `hexStream(reader, writer)` /
  `base64Stream(reader, writer)` variants. ~50 LOC.

### 4.6 CompressionMethod.STORE in bespoke Archive — **FIXED (removed)**

The Archive class was removed; `STORE` is now handled directly by the
block compressor (passthrough). Documented in `openrar.js` docstring.

---

## 5. Testing / CI

### 5.1 `helpers.test.mjs` auto-skips if `wasm/dist/` is missing

This is intentional (no emsdk in normal CI), but a regression in the
JS wrapper isn't caught unless someone manually runs `emcmake`.

- **Workaround:** None in CI. Run `npm --prefix wasm/js test` after any
  `wasm/js/` change.
- **Effort to fix:** Add a Node-only mock of the WASM module. ~100 LOC.

### 5.2 No fuzz tests

The decompressor is exposed to attacker-controlled bytes. There's no
fuzz corpus for malformed block headers, truncated vint, etc.

- **Effort to fix:** libFuzzer harness on the native build, then port
  the corpus to WASM. ~1 day.

### 5.3 No `.wasm` size budget in CI — **FIXED**

A regression that bloats the binary from 250 KiB to 800 KiB (e.g.
accidentally enabling LZ4 fallback) wasn't caught.

- **Status:** Added CI step in `.github/workflows/build.yml` asserting
  `wasm/dist/openrar.wasm ≤ 512000` bytes. 2× headroom over the current
  250 KiB build.

---

## 6. Documentation

### 6.1 No machine-generated `openrar.d.ts` from `wasm-dist`

The `.d.ts` is hand-written and can drift.

- **Effort to fix:** Migrate to `wasm-bindgen-cli`. ~1 day, requires
  Rust toolchain.

### 6.2 No API examples — **FIXED**

`wasm/README.md` had the canonical flow but no recipes.

- **Status:** Added `docs/wasm-examples.md` with 10 copy-pasteable
  recipes covering: round-trip, base64 persistence, fetch, streaming
  fetch, Worker offload, raw ccall, QuickJS host handoff, cross-validate
  with native, Chrome memory profiling, ABI detection.

---

## 7. Summary (post-`e13c5f3`)

| Category | Open items | Closed in `e13c5f3` | Total effort remaining |
|---|---:|---:|---:|
| 1. Functional gaps | 5 | 0 (1.2 deferred) | ~1500 LOC |
| 2. Build / distribution | 5 | 1 (docs) | ~500 LOC + docs |
| 3. Correctness / safety | 5 | 4 | ~5 LOC |
| 4. JS surface | 6 | 3 | ~50 LOC |
| 5. Testing / CI | 3 | 1 | ~350 LOC + 1 day |
| 6. Documentation | 2 | 2 | ~1 day |

**Remaining high-impact items** (ranked):

1. **§1.2** — real-RAR5 archive API in WASM (~300 LOC). Highest user
   value.
2. **§5.2** — fuzz tests for the decompressor. Critical for any code
   that ingests untrusted bytes.
3. **§1.1** — streaming API (~650 LOC).
4. **§1.3** — encryption exports (~100 LOC). Needed for parity with the
   native CLI.
5. **§5.1** — Node-only mock of the WASM module so the JS tests run in
   CI without emsdk.
