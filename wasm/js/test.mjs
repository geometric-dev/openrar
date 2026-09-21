// wasm/js/test.mjs — CI smoke test for the block codec.
// Runs only if wasm/dist/openrar.js is built; CI runs this after a fresh
// `cmake --build --preset wasm`.
import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const distJs = path.join(here, '../dist/openrar.js');

if (!existsSync(distJs)) {
  test('wasm/dist/openrar.js missing — run `emcmake cmake --preset wasm && cmake --build --preset wasm`', { skip: true }, () => {});
} else {
  const { OpenRAR, WASM_API_VERSION, getModule } = await import('./openrar.js');

  test('module boots with matching ABI version', async () => {
    const m = await getModule();
    assert.equal(m._openrar_version(), WASM_API_VERSION);
  });

  test('wrapper round-trip (string + Uint8Array input)', async () => {
    const r = new OpenRAR();
    await r.ready;
    const text = 'The quick brown fox jumps over the lazy dog. '.repeat(50);
    const c = await r.compress(text, 3);
    assert.ok(c instanceof Uint8Array && c.length > 0);
    assert.equal(new TextDecoder().decode(await r.decompress(c)), text);
    r.destroy();
  });

  test('C ABI smoke: openrar_compress2 returns 1 and out_len is set', async () => {
    const m = await getModule();
    const src = new Uint8Array(4096);
    for (let i = 0; i < src.length; i++) src[i] = (i * 31) & 0xFF;
    const ptr = m._openrar_alloc(src.length);
    m.HEAPU8.set(src, ptr);
    const outPtr = m._malloc(4), outLen = m._malloc(4);
    const rc = m.ccall('openrar_compress2', 'number',
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [ptr, src.length, outPtr, outLen, 3, 2 * 1024 * 1024]);
    assert.equal(rc, 1);
    const len = m.HEAPU32[outLen >>> 2];
    assert.ok(len > 0, 'out_len must be set on success');
    // Failure path must zero the out params, never leave garbage. A window
    // above MAX_WIN_SIZE cannot be expressed through this 32-bit ABI
    // (size_t truncates 5 GiB to 1 GiB, which is legal), so the exercised
    // failure is a null source with a non-zero length.
    const rc2 = m.ccall('openrar_compress2', 'number',
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [0, src.length, outPtr, outLen, 3, 2 * 1024 * 1024]);
    assert.equal(rc2, 0);
    assert.equal(m.HEAPU32[outLen >>> 2], 0, 'out_len must be zeroed on failure');
    m._openrar_free(ptr);
    m._free(outPtr);
    m._free(outLen);
  });
}
