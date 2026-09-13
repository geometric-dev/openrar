# OpenRAR WASM

Browser / Node bindings built with Emscripten (`emcc`, **pinned: 3.1.50** —
the same version CI uses). Two artifacts:

| Artifact | Emscripten preset | Contents | Size (wasm) |
|---|---|---|---|
| `wasm/dist/openrar.{js,wasm}` | `wasm` | RAR5 v0 block codec (`compress` / `decompress`), no filesystem | ~70 KB |
| `wasm/dist/openrar_archive.{js,wasm}` | `wasm-archive` | Block codec + in-memory RAR5 archive reader/writer (`createArchive`, `listArchive`, `extract*`, handles, progress/cancel) | ~125 KB |

Both are `MODULARIZE` + `EXPORT_ES6` + `ALLOW_MEMORY_GROWTH` + `FILESYSTEM=0`.

## Layout

```
src/wasm/wasm_api.{hpp,cpp}     Block codec C ABI (WASM_API_VERSION = 2)
src/wasm/archive_api.{hpp,cpp}  Archive C ABI (ARCHIVE_WASM_API_VERSION = 2)
cmake/wasm.cmake                Emscripten targets (included only under emcmake)
wasm/js/openrar.js              Block codec wrapper — default surface
wasm/js/openrar.d.ts            Block codec TS contract
wasm/js/openrar-archive.js      Archive wrapper (lazy-loads the archive module)
wasm/js/openrar-archive.d.ts    Archive TS contract
wasm/js/heap_helpers.js         safeRead / requireExports shared utilities
wasm/js/check-exports.mjs       Contract gate: required exports present on built modules
wasm/js/{test,helpers.test,archive.test}.mjs   node --test suites
wasm/dist/                      Build artifacts (gitignored — rebuild with make/npm)
```

## Stability

| Surface | Stability |
|---|---|
| `src/wasm/wasm_api.cpp` C ABI | Pinned at `WASM_API_VERSION` (v2: embind removed). Bump on any breaking change. |
| `src/wasm/archive_api.cpp` C ABI | `ARCHIVE_WASM_API_VERSION` (v2: create2/extract_all2/hooks/last_error_code). v1 entry points kept as shims. |
| `wasm/js/*.d.ts` | Semver. Source of truth for JS consumers. |
| `wasm/js/*.js` | Must match the `.d.ts`. Bug fixes without version bump. |
| Export set | `wasm/js/check-exports.mjs` runs in CI — additive/removed exports fail the build even when the version pin matches. |

The JS wrappers verify the numeric ABI version AND the required export set
at module-load time; a stale `dist/` fails loudly with a rebuild hint.

## Build

Prerequisites: emsdk (pinned **3.1.50**): `source emsdk/emsdk_env.sh`.

```bash
make wasm wasm-archive          # or:
emcmake cmake --preset wasm && cmake --build --preset wasm
emcmake cmake --preset wasm-archive && cmake --build --preset wasm-archive

# debug build
emcmake cmake --preset wasm-debug && cmake --build --preset wasm-debug

# full CLI with MEMFS (for archive-level a/x/t via callMain)
emcmake cmake --preset wasm-cli -DOPENRAR_WASM_CLI=ON && cmake --build --preset wasm-cli
```

```bash
npm --prefix wasm/js run build   # both artifacts via the presets
npm --prefix wasm/js test        # node --test (auto-skips if dist missing)
npm --prefix wasm/js run check-exports
```

## Block codec

```js
import { OpenRAR, CompressionMethod } from './openrar.js';

const rar = new OpenRAR();
await rar.ready;
const compressed = await rar.compress(bytes, CompressionMethod.NORMAL);   // method 3
const original   = await rar.decompress(compressed);
rar.destroy();                  // frees the wasm heap (all handles share one module)
```

- Inputs: `Uint8Array`, string, `ArrayBuffer`, any `ArrayBufferView`
  (Blob/Response via `blobToBytes` first).
- `compress(data, method?, winSize?)` — `winSize` in bytes, ≤ 2 GiB (i32
  boundary; C caps at 4 GiB). Throws on failure.
- `method: CompressionMethod.STORE (0)` is a passthrough — the output is
  the input unchanged and is **not** accepted by `decompress`.
- Peak memory ≈ 3N + 7 MiB. For >100 MiB inputs, run in a Worker.
- Raw C ABI (no embind since v2): `createOpenRAR()` → `_openrar_*` exports
  via `ccall`/`cwrap`; `compressHeap` returns a live `{ptr, len, free}` —
  copy out with `safeRead` before any further wasm call.

## Archive API

```js
import { createArchive, listArchive, extractAll, openArchive } from './openrar-archive.js';

const rar = await createArchive(
  [
    { path: 'hello.txt', data: 'Hello, world!', mtime: 1600000000 },
    { path: 'data.bin', data: new Uint8Array([1, 2, 3]) },
    { path: 'dir/' },                        // directory: trailing '/'
  ],
  { method: 3, windowLog2: 4 },              // 1 MiB dictionary
);

const entries = await listArchive(rar);      // RarEntry[]

const files = await extractAll(rar, {
  onProgress: (done, total) => console.log(`${done}/${total}`),
  signal: controller.signal,                 // abort between entries
  // maxOutputBytes: 512 MiB default bomb guard; bulk: true for one-shot
});
files.get('hello.txt');                      // Uint8Array

// Handle API — no re-scan for list → extract flows:
const h = await openArchive(rar);
const one = await h.extract('hello.txt');
h.close();
```

- `windowLog2`: 1=128 KiB, 2=256 KiB, 3=512 KiB, 4=1 MiB.
- Error model: `RarError { code, detail, numericCode }`; codes mirror the
  C `RarError` enum (`NOT_RAR`, `TRUNCATED`, `CRC_MISMATCH`, `NOMEM`,
  `ABORTED`, …). `openArchive` reports the real failure code, not a guess.
- `mtime` is UNIX seconds (DOS-encoded on disk: 2-second granularity,
  1980–2107); omitted/0 ⇒ now.
- Progress callbacks fire once per entry (cumulative, monotonic bytes).
- Encrypted / solid / multi-volume / recovery archives are rejected with
  `UNSUPPORTED_FEATURE` (see `docs/wasm-limitations.md`).

## Memory contract

| Operation | Peak |
|---|---|
| `compress(N)` | ~3N + 7 MiB (input copy, output, scratchpad) |
| `extractAll` per-entry (default) | Σ results + max entry (no concat buffer) |
| `extractAll` bulk | Σ results + Σ payload (concatenated buffer) |
| `createArchive` | Σ inputs + archive bytes |

`ALLOW_MEMORY_GROWTH` is on; the wasm32 ceiling is ~2 GiB practical. Any
allocation can relocate the heap — read outputs with `safeRead` only.

## Hosting (browser)

- Serve `.wasm` as `Content-Type: application/wasm`, brotli-compressed
  (~70 KB → ~25 KB), `Cache-Control: immutable` on hashed filenames.
- CSP: `script-src 'wasm-unsafe-eval'` is required for wasm instantiation.
- Cross-origin: the wasm fetch must be CORS-enabled when hosted on a CDN.
- See `docs/wasm-deployment.md` for the full recipe (SRI, locateFile,
  COOP/COEP notes).

## Testing / CI

- `node --test wasm/js/*.test.mjs` — wrapper + archive suites (34 tests).
- `node wasm/js/check-exports.mjs` — required-export contract gate.
- Native ctest: `wasm_api_tests` / `archive_api_tests` exercise both C ABIs
  with a regular compiler on every CI matrix (no emsdk needed).
- CI builds both artifacts with the pinned emsdk, runs all suites, and
  asserts size budgets (block codec ≤ 500 KiB, archive ≤ 1.5 MiB).
