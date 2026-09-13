// wasm/js/helpers.test.mjs — exercises the JS wrapper API contract.
// Skipped if wasm/dist/openrar.js is missing (run emcmake first).
import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const distJs = path.join(here, '../dist/openrar.js');

if (!existsSync(distJs)) {
  test('wasm/dist missing — run `emcmake cmake --preset wasm` first', { skip: true }, () => {});
} else {
  const {
    OpenRAR, CompressionMethod, WASM_API_VERSION, DEFAULT_WIN_SIZE,
    hex, base64Encode, base64Decode, toUint8, blobToBytes, getModule,
  } = await import('./openrar.js');

  test('hex round-trip', () => {
    const u = new Uint8Array([0, 1, 15, 16, 255]);
    assert.equal(hex(u), '00010f10ff');
  });

  test('base64 round-trip', () => {
    const u = new Uint8Array([72, 101, 108, 108, 111]); // 'Hello'
    assert.equal(base64Encode(u), 'SGVsbG8=');
    assert.deepEqual(base64Decode('SGVsbG8='), u);
  });

  test('toUint8 accepts string, ArrayBuffer, Uint8Array, View', () => {
    assert.deepEqual(toUint8('hi'), new Uint8Array([104, 105]));
    assert.deepEqual(toUint8(new Uint8Array([1, 2])), new Uint8Array([1, 2]));
    const ab = new Uint8Array([3, 4]).buffer;
    assert.deepEqual(toUint8(ab), new Uint8Array([3, 4]));
    const view = new Uint16Array([5, 6]);
    // View bytes in platform byte order (little-endian everywhere we ship).
    assert.deepEqual(toUint8(view), new Uint8Array([5, 0, 6, 0]));
  });

  test('toUint8 rejects unsupported types', () => {
    assert.throws(() => toUint8(123));
    assert.throws(() => toUint8({}));
    assert.throws(() => toUint8(null));
  });

  test('blobToBytes converts Blob', async () => {
    const blob = new Blob([new Uint8Array([7, 8, 9])]);
    const u = await blobToBytes(blob);
    assert.deepEqual(u, new Uint8Array([7, 8, 9]));
  });

  test('getModule is a singleton (same handle)', async () => {
    const a = await getModule();
    const b = await getModule();
    assert.equal(a, b);
  });

  test('WASM_API_VERSION matches module', async () => {
    const m = await getModule();
    assert.equal(WASM_API_VERSION, m._openrar_version());
  });

  test('OpenRAR.compress accepts string and Uint8Array', async () => {
    const r = new OpenRAR();
    await r.ready;
    const text = 'The quick brown fox jumps over the lazy dog. '.repeat(50);
    const c = await r.compress(text);
    assert.ok(c instanceof Uint8Array);
    assert.ok(c.length > 0);
    const restored = await r.decompress(c);
    assert.equal(new TextDecoder().decode(restored), text);
  });

  test('OpenRAR.compress with all methods 1..5', async () => {
    const r = new OpenRAR();
    await r.ready;
    const src = new Uint8Array(4096);
    for (let i = 0; i < src.length; i++) src[i] = (i * 31) & 0xFF;
    for (const m of [1, 2, 3, 4, 5]) {
      const c = await r.compress(src, m);
      const d = await r.decompress(c);
      assert.deepEqual(d, src, `method ${m} round-trip failed`);
    }
  });

  test('OpenRAR.compress method 0 (STORE) is a passthrough identity', async () => {
    const r = new OpenRAR();
    await r.ready;
    const src = new Uint8Array([1, 2, 3, 4, 5]);
    const c = await r.compress(src, CompressionMethod.STORE);
    // STORE is not block framing — the output is the input unchanged and
    // decompress() must reject it rather than mis-parse.
    assert.deepEqual(c, src);
    await assert.rejects(r.decompress(c));
  });

  test('OpenRAR.compress rejects bad method and winSize', async () => {
    const r = new OpenRAR();
    await r.ready;
    const src = new Uint8Array([1, 2, 3]);
    await assert.rejects(r.compress(src, 99), RangeError);
    await assert.rejects(r.compress(src, 3, 0), RangeError);
    await assert.rejects(r.compress(src, 3, -1), RangeError);
    await assert.rejects(r.compress(src, 3, 4 * 1024 * 1024 * 1024), RangeError);
    await assert.rejects(r.decompress(src, 4 * 1024 * 1024 * 1024), RangeError);
  });

  test('OpenRAR.compress round-trips highly repetitive single-byte input', async () => {
    // Pins the RAR5 encoder on degenerate input (8 KiB of one byte).
    const r = new OpenRAR();
    await r.ready;
    const src = new Uint8Array(8192).fill(0x41);
    const c = await r.compress(src, 3);
    assert.ok(c.length > 0);
    const d = await r.decompress(c);
    assert.deepEqual(d, src);
  });

  test('OpenRAR.destroy() prevents further use; module re-instantiates', async () => {
    const r = new OpenRAR();
    await r.ready;
    r.destroy();
    assert.throws(() => r.module);
    await assert.rejects(r.compress(new Uint8Array([1])));
    // The singleton is gone — a fresh handle must boot a fresh module.
    const r2 = new OpenRAR();
    await r2.ready;
    const c = await r2.compress(new Uint8Array([5, 4, 3, 2, 1]), 3);
    const d = await r2.decompress(c);
    assert.deepEqual(d, new Uint8Array([5, 4, 3, 2, 1]));
    r2.destroy();
  });

  test('OpenRAR.compressHeap + safeRead round-trip', async () => {
    const r = new OpenRAR();
    await r.ready;
    const src = new Uint8Array(4096);
    for (let i = 0; i < src.length; i++) src[i] = (i * 31 + 7) & 0xFF;
    const heap = await r.compressHeap(src);
    assert.ok(heap);
    assert.ok(heap.len > 0);
    assert.equal(typeof heap.ptr, 'number');
    // safeRead copies out — safe across subsequent allocations.
    const copy = r.safeRead(heap.ptr, heap.len);
    heap.free();
    const restored = await r.decompress(copy);
    assert.deepEqual(restored, src);
  });

  test('compress rejects corrupt input', async () => {
    const r = new OpenRAR();
    await r.ready;
    await assert.rejects(r.decompress(new Uint8Array([0x52, 0x61, 0x72])));
  });

  test('DEFAULT_WIN_SIZE matches the C ABI default', () => {
    assert.equal(DEFAULT_WIN_SIZE, 2 * 1024 * 1024);
  });

  test('compressStream is byte-identical to compress and round-trips', async () => {
    const r = new OpenRAR();
    await r.ready;
    const src = new Uint8Array(700 * 1024); // spans the 512 KiB look-ahead quantum
    for (let i = 0; i < src.length; i++) src[i] = (i * 13 + 5) & 0xFF;
    const stream = new ReadableStream({
      start(controller) {
        for (let off = 0; off < src.length; off += 100 * 1024) {
          controller.enqueue(src.subarray(off, Math.min(off + 100 * 1024, src.length)));
        }
        controller.close();
      },
    });
    const streamed = await r.compressStream(stream, { method: 3 });
    const oneshot = await r.compress(src, 3);
    assert.deepEqual(streamed, oneshot);
    assert.deepEqual(await r.decompress(streamed), src);
  });

  test('safeRead + isPointerValid + heapBase', async () => {
    const r = new OpenRAR();
    await r.ready;
    const m = r.module;
    const ptr = m._openrar_alloc(64);
    m.HEAPU8.fill(0xAB, ptr, ptr + 64);
    // isPointerValid checks HEAP bounds (not allocation bounds) — anything
    // inside the heap passes, addresses beyond the heap fail.
    assert.equal(r.isPointerValid(ptr, 64), true);
    assert.equal(r.isPointerValid(ptr + 64, 1), true);   // past the alloc, inside the heap
    assert.equal(r.isPointerValid(0xfffff000, 4096), false); // beyond the heap
    // safeRead copies bytes out.
    const copy = r.safeRead(ptr, 64);
    assert.deepEqual(copy, new Uint8Array(64).fill(0xAB));
    copy[0] = 0x00;
    assert.equal(m.HEAPU8[ptr], 0xAB);  // proves the copy is independent
    // heapBase is normally 0.
    assert.equal(typeof r.heapBase, 'number');
    // edge: empty
    assert.deepEqual(r.safeRead(ptr, 0), new Uint8Array(0));
    assert.deepEqual(r.safeRead(0, 64), new Uint8Array(0));
    // invalid args
    assert.throws(() => r.safeRead(-1, 1));
    m._openrar_free(ptr);
  });
}
