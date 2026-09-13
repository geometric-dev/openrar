# WASM Examples — openrar-wasm

Copy-pasteable recipes for common openrar-wasm tasks. All examples assume
the `openrar.js` wrapper is available at `./openrar.js` (adjust the import
path to match your build).

For the underlying API contract and stability guarantees see `wasm/README.md`.
For known limitations see `docs/wasm-limitations.md`.

---

## 1. Round-trip a buffer

```js
import { OpenRAR, CompressionMethod } from './openrar.js';

const rar = new OpenRAR();
await rar.ready;

const original = new TextEncoder().encode('Hello, OpenRAR!');
const compressed = await rar.compress(original, CompressionMethod.NORMAL);
const restored = await rar.decompress(compressed);

console.assert(
  new TextDecoder().decode(restored) === 'Hello, OpenRAR!',
  'round-trip failed',
);
```

## 2. Compress a string and store as base64

```js
import { OpenRAR, base64Encode } from './openrar.js';

const rar = new OpenRAR();
const compressed = await rar.compress('hello', /*method*/ 3);

// Persist anywhere (localStorage, IndexedDB, JSON.stringify).
const wireFormat = base64Encode(compressed);
localStorage.setItem('cache', wireFormat);
```

## 3. Decompress a fetched asset (browser)

```js
import { OpenRAR } from './openrar.js';

async function decompressAsset(url) {
  const rar = new OpenRAR();
  await rar.ready;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`fetch failed: ${res.status}`);
  // Pass the Response directly — blobToBytes() handles .arrayBuffer().
  const compressed = new Uint8Array(await res.arrayBuffer());
  return rar.decompress(compressed);
}
```

## 4. Stream-decompress a large HTTP response (no full-buffer copy)

```js
import { OpenRAR, CompressionMethod } from './openrar.js';

async function decompressStream(url) {
  const rar = new OpenRAR();
  await rar.ready;
  const res = await fetch(url);
  const reader = res.body.getReader();
  const chunks = [];
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
  }
  // Reassemble (the block codec is one-shot, so we materialise).
  // For multi-MiB inputs, prefer a Worker + chunked compress.
  const blob = new Blob(chunks);
  return rar.decompress(blob);
}
```

For *true* streaming (per-chunk LZ window) see `docs/streaming-considerations.md`.

## 5. Worker offload (avoid blocking the main thread)

```js
// main.js
const worker = new Worker(new URL('./rar-worker.js', import.meta.url), { type: 'module' });

const compressed = await new Promise((resolve, reject) => {
  worker.onmessage = (e) => e.data.ok ? resolve(e.data.bytes) : reject(new Error(e.data.err));
  worker.postMessage({ cmd: 'compress', data: largeBuffer, method: 3 }, [largeBuffer.buffer]);
});

// rar-worker.js
import { OpenRAR, CompressionMethod } from './openrar.js';
const rar = new OpenRAR();
await rar.ready;

self.onmessage = async (e) => {
  try {
    if (e.data.cmd === 'compress') {
      const bytes = await rar.compress(e.data.data, e.data.method ?? CompressionMethod.NORMAL);
      self.postMessage({ ok: true, bytes }, [bytes.buffer]);   // transferable
    } else if (e.data.cmd === 'decompress') {
      const bytes = await rar.decompress(e.data.data);
      self.postMessage({ ok: true, bytes }, [bytes.buffer]);
    }
  } catch (err) {
    self.postMessage({ ok: false, err: String(err) });
  }
};
```

The `[bytes.buffer]` transferable avoids copying the result back to the
main thread.

## 6. Use the raw module (ccall / HEAPU8) — FFI escape hatch

```js
import { OpenRAR } from './openrar.js';

const rar = new OpenRAR();
await rar.ready;
const m = rar.module;

// Compress without the JS wrapper — full heap control.
const src = new Uint8Array(64 * 1024);
for (let i = 0; i < src.length; i++) src[i] = (i * 31) & 0xFF;

const inPtr = m._openrar_alloc(src.length);
m.HEAPU8.set(src, inPtr);

const outPtr = m._malloc(4);
const outLen = m._malloc(4);

const ok = m.ccall('openrar_compress2', 'number',
  ['number', 'number', 'number', 'number', 'number', 'number'],
  [inPtr, src.length, outPtr, outLen, 3 /*method*/, 2 * 1024 * 1024 /*winSize*/]);

if (ok === 1) {
  const len = m.HEAP32[outLen >>> 2];
  const ptr = m.HEAP32[outPtr >>> 2];
  // CRITICAL: copy out before any other allocation. HEAPU8 may grow
  // and invalidate the pointer. Use safeRead() for safety:
  const result = rar.safeRead(ptr, len);
  // ...use result...
  m._openrar_free(ptr);     // free the compress output
}

m._openrar_free(inPtr);     // free the input buffer
m._free(outPtr);
m._free(outLen);
```

## 7. Direct heap pointer handoff (QuickJS / FFI hosts)

```js
import { OpenRAR } from './openrar.js';
import { compressZeroCopy } from './openrar/quickjs-host.mjs';

const rar = new OpenRAR();
await rar.ready;

const result = await compressZeroCopy(rar, someUint8);
if (result.fallback) {
  // Not a QuickJS host — use the bytes directly.
  console.log(result.bytes);
} else {
  // Host C code:
  //   JSValue ab = JS_NewArrayBuffer(ctx, (uint8_t*)result.ptr,
  //                                   result.len, openrar_free_thunk,
  //                                   NULL, /*is_shared*/ false);
  // Then call result.free() once JS releases its reference.
}
```

## 8. Cross-validate WASM output against native `openrar`

```bash
# Build both:
make                                  # → ./openrar
make wasm                             # → wasm/dist/openrar.{js,wasm}

# Compress a file with the native CLI:
./openrar a -m3 fixture.bin fixture.rar
hexdump -C fixture.rar | head

# Decompress with WASM in Node:
node -e '
import("./wasm/dist/openrar.js").then(async ({ default: OpenRAR }) => {
  const rar = new OpenRAR();
  await rar.ready;
  const compressed = await import("node:fs/promises").then(f => f.readFile("fixture.rar"));
  const restored = await rar.decompress(compressed);
  console.log("ok,", restored.length, "bytes");
});
'
```

The restored byte length should match `wc -c < fixture.bin`. Bytewise
equality is **not** expected at this layer — the WASM build emits block
codec output (not a full RAR5 archive). For end-to-end archive
round-trip use the `wasm-cli` build:

```bash
make wasm-cli
node -e '
import("./wasm/dist/openrar_cli.js").then(async (m) => {
  const FS = m.FS;
  FS.writeFile("/fixture.bin", await import("node:fs").then(f => f.readFileSync("fixture.bin")));
  m.callMain(["a", "-m3", "/fixture.rar", "/fixture.bin"]);
  const out = FS.readFile("/fixture.rar");
  console.log("archive size:", out.length);
});
'
```

## 9. Profile memory usage in Chrome

```js
// In the browser DevTools console:
performance.measureUserAgentSpecificMemory?.().then((m) => {
  console.log(`JS heap: ${(m.bytes / 1e6).toFixed(1)} MiB`);
});

// WASM heap:
// (Chrome DevTools → Memory → "Heap snapshot" → filter for "wasm")
// For live tracking, run `OpenRAR.compressHeap()` and observe HEAPU8
// buffer growth in the heap snapshot diff.
```

For >200 MiB inputs, **always** offload to a Worker (example §5) so the
main thread stays responsive.

## 10. Detect ABI mismatch at startup

```js
import { WASM_API_VERSION, createOpenRAR } from './openrar.js';

try {
  const m = await createOpenRAR();
  if (m.version() !== WASM_API_VERSION) {
    throw new Error('openrar-wasm: ABI mismatch — rebuild with `make wasm`');
  }
} catch (err) {
  console.error('Failed to load openrar-wasm:', err);
  // Fall back to a different RAR library, or show a user-facing error.
}
```

---

## Cookbook summary

| Task | Surface | Notes |
|---|---|---|
| Compress/decompress a buffer | `OpenRAR.compress/decompress` | Most common case. |
| Multiple entries (JS-side container) | `wasm-cli` build, `callMain(['a', ...])` | RAR5 output. |
| Read a `.rar` from a `File` | `wasm-cli` + `FS.writeFile` + `callMain(['x', ...])` | RAR5 read. |
| Persistent identity across modules | Reuse the singleton returned by `getModule()` | |
| FFI / ccall | `m._openrar_compress2`, `m.HEAPU8` | Use `safeRead()` for heap safety. |
| QuickJS host | `compressZeroCopy` | Returns `{ ptr, len, free }` for `JS_NewArrayBuffer`. |
| Multi-MiB inputs | Worker + transferable | Off main thread; avoid large array copies. |
