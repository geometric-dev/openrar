// wasm/js/check-exports.mjs — contract gate for the built wasm artifacts.
//
// Every export the JS wrappers require must exist on the built module.
// The numeric ABI version pin cannot catch additive drift (exports added
// or removed without a version bump) — this check can. Run in CI after a
// fresh build; exits non-zero listing exactly what is missing.
//
// Usage: node wasm/js/check-exports.mjs
import { existsSync } from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const dist = path.join(here, '../dist');

const REQUIRED = {
  // Block codec (src/wasm/wasm_api.cpp) — consumed by wasm/js/openrar.js
  'openrar.js': [
    '_openrar_version', '_openrar_alloc', '_openrar_free',
    '_openrar_compress2', '_openrar_decompress2',
    '_openrar_stream_create', '_openrar_stream_feed', '_openrar_stream_finish', '_openrar_stream_free',
    '_malloc', '_free', 'ccall', 'cwrap', 'UTF8ToString',
  ],
  // Archive API (src/wasm/archive_api.cpp) — consumed by wasm/js/openrar-archive.js
  'openrar_archive.js': [
    '_openrar_archive_version', '_openrar_archive_open', '_openrar_archive_close',
    '_openrar_archive_handle_list', '_openrar_archive_handle_extract',
    '_openrar_archive_handle_extract_all', '_openrar_archive_handle_extract_all2',
    '_openrar_archive_handle_set_limits',
    '_openrar_archive_list', '_openrar_archive_list_free',
    '_openrar_archive_extract', '_openrar_archive_extract_all',
    '_openrar_archive_extract_all2', '_openrar_archive_create',
    '_openrar_archive_create2', '_openrar_archive_get_error',
    '_openrar_archive_last_error_code',
    '_openrar_archive_alloc', '_openrar_archive_free',
    '_malloc', '_free', 'ccall', 'cwrap', 'UTF8ToString',
    'addFunction', 'removeFunction',
  ],
};

let failures = 0;
for (const [file, required] of Object.entries(REQUIRED)) {
  const p = path.join(dist, file);
  if (!existsSync(p)) {
    console.error(`MISSING: ${p} (build it first: emcmake cmake --preset wasm / wasm-archive)`);
    failures++;
    continue;
  }
  try {
    const mod = await import(url.pathToFileURL(p).href);
    const factory = mod.default;
    if (typeof factory !== 'function') {
      console.error(`FAIL ${file}: no MODULARIZE factory default export`);
      failures++;
      continue;
    }
    const m = await factory();
    const missing = required.filter((n) => !(n in m));
    if (missing.length > 0) {
      console.error(`FAIL ${file}: missing exports [${missing.join(', ')}] — rebuild the module`);
      failures++;
    } else {
      console.log(`OK ${file}: all ${required.length} required exports present`);
    }
    if (typeof m.delete === 'function') { try { m.delete(); } catch { /* freed */ } }
  } catch (e) {
    console.error(`FAIL ${file}: module failed to instantiate: ${e.message}`);
    failures++;
  }
}
process.exit(failures > 0 ? 1 : 0);
