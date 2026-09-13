# OpenRAR WASM — Browser Deployment Guide

How to ship `wasm/dist/*.js|*.wasm` so it loads fast and doesn't get blocked
by browser security policy. Versions of everything below are pinned to the
emsdk build described in `wasm/README.md`.

## 1. MIME type

WebAssembly streams fail without the correct type:

```
application/wasm    .wasm
text/javascript     .js     (application/javascript also accepted)
```

Without `application/wasm`, browsers fall back to the slow text-decode
instantiation path — `WebAssembly.instantiateStreaming` rejects outright.

## 2. Compression + caching

The artifacts compress extremely well; serve brotli where available:

| File | Raw | Brotli -11 |
|---|---|---|
| `openrar.wasm` (block codec) | ~70 KB | ~25 KB |
| `openrar.js` (block codec glue) | ~15 KB | ~5 KB |
| `openrar_archive.wasm` | ~125 KB | ~45 KB |
| `openrar_archive.js` | ~21 KB | ~7 KB |

nginx: `brotli on; brotli_types application/wasm text/javascript;`
(Or precompress: `brotli -f wasm/dist/*.wasm` + serve `.wasm.br` with
`Content-Encoding: br`.)

Cache hard, content-hash filenames: the artifacts are immutable for a given
build — `Cache-Control: public, max-age=31536000, immutable` behind
`openrar.<hash>.wasm`. The JS glue references its `.wasm` via `import.meta.url`
relative resolution, so keep the pair together in the same directory.

## 3. CDN / non-default locations

Emscripten locates `openrar.wasm` next to the JS glue. When the two must be
split (JS bundled by webpack, wasm on a CDN), pass `locateFile`:

```js
import createOpenRAR from 'openrar-wasm/openrar.js';

const m = await createOpenRAR({
  locateFile: (path) => `https://cdn.example.com/openrar/${path}`,
});
```

Note the wrapper module (`wasm/js/openrar.js`) owns instantiation; to inject
`locateFile` through it, import the raw factory
(`import { createOpenRAR } from './openrar.js'` → pass opts) or host the pair
adjacent to your own bundle.

## 4. Content Security Policy

Instantiating wasm requires `wasm-unsafe-eval` in the `script-src` (or
`default-src`) directive:

```
Content-Security-Policy: script-src 'self' 'wasm-unsafe-eval'; connect-src 'self' https://cdn.example.com
```

Without it: `CompileError: WebAssembly.instantiate() is disallowed by the
script-src Content Security Policy`. No `unsafe-eval` needed — the build does
not use dynamic code generation beyond wasm itself.

## 5. Integrity

`integrity` attributes do not apply to Emscripten's internal wasm fetch, so
verify one of two ways:

1. **Release checksums.** Every release publishes `checksums.txt`
   (`sha256sum wasm/dist/*`). Verify at deploy time:

   ```bash
   sha256sum -c checksums.txt
   ```

2. **In-app verification** (self-hosted wasm without SRI): fetch the bytes,
   verify, then hand them to Emscripten:

   ```js
   const wasmBinary = new Uint8Array(await (await fetch(cdnUrl + '/openrar.wasm')).arrayBuffer());
   const digest = await crypto.subtle.digest('SHA-256', wasmBinary);
   if (hex(new Uint8Array(digest)) !== EXPECTED_SHA256) throw new Error('wasm integrity check failed');
   const m = await createOpenRAR({ wasmBinary });
   ```

## 6. Cross-origin isolation (only if you enable threads later)

The shipped artifacts are single-threaded and need **no** COOP/COEP headers.
A future `-pthread` build requires:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

which makes every embedded cross-origin resource require `crossorigin` /
CORP headers. Do not enable these preemptively — they break third-party
embeds. See `docs/wasm-limitations.md` §2.2.

## 7. Threading model (off the main thread)

All compression work blocks. Rules of thumb:

- Inputs ≤ ~10 MiB: main-thread `rar.compress()` is fine (a few ms).
- Anything larger: use the Worker handle — same API, off-thread, transferable
  buffers both directions:

  ```js
  import { createOpenRARWorker } from 'openrar-wasm/worker-host.mjs'; // or ./wasm/js/worker-host.mjs
  const wr = createOpenRARWorker();          // spawns wasm/js/worker.js
  const c = await wr.compress(bigBytes);     // main thread stays interactive
  wr.terminate();
  ```

  With `{ transfer: true }`-style semantics: inputs passed with
  `transferIn` detach the caller's buffer (zero copy) — pass a copy when the
  caller still needs it. Outputs are always transferred (zero copy).
- Archives: `wr.createArchive(...)` / `wr.extractAll(rar, { onProgress })`
  keep both extraction and wasm-heap pressure inside the worker.
- Bundlers: the worker is created with `new Worker(new URL('./worker.js',
  import.meta.url), { type: 'module' })` — webpack 5 / Vite / Rollup handle
  this natively; pass `{ workerUrl }` if your bundler relocates chunks.

## 8. Memory ceilings

| Constraint | Value |
|---|---|
| wasm32 linear memory | 4 GiB addressable; ~2 GiB practical ceiling with `ALLOW_MEMORY_GROWTH` |
| `compress(N)` peak | ~3N + 7 MiB |
| `extractAll` per-entry (default) | Σ results + largest entry |
| `extractAll` bulk | Σ results + Σ payload |
| `maxOutputBytes` bomb guard | default 512 MiB (raise per call as needed) |

For archives larger than ~1 GiB, stream them (see
`docs/streaming-considerations.md`) or extract server-side; never
`arrayBuffer()` a 2 GiB download.

## 9. Preload checklist

- [ ] `.wasm` served as `application/wasm` (check DevTools → Network → Type)
- [ ] brotli/gzip actually compressing (compare transfer size)
- [ ] CSP includes `wasm-unsafe-eval`
- [ ] checksums verified at deploy
- [ ] `check-exports.mjs` run against the exact artifacts being shipped
- [ ] Worker path exercised in the target browsers (module workers:
      Chrome/Edge 80+, Firefox 114+, Safari 15+)
