// wasm/js/worker.test.mjs — exercises createOpenRARWorker.
// Plain Node has no browser Worker global, so these run against the
// documented main-thread fallback; the module-worker path is exercised in
// browsers via docs/wasm-examples.md and check-exports.mjs.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createOpenRARWorker } from './worker-host.mjs';

test('worker handle round-trips compress/decompress', async () => {
  const wr = createOpenRARWorker();
  try {
    const src = new Uint8Array(4096);
    for (let i = 0; i < src.length; i++) src[i] = (i * 7) & 0xFF;
    const c = await wr.compress(src, 3);
    assert.ok(c.length > 0);
    assert.deepEqual(await wr.decompress(c), src);
  } finally {
    wr.terminate();
  }
});

test('worker handle drives the archive API', async () => {
  const wr = createOpenRARWorker();
  try {
    const rar = await wr.createArchive([
      { path: 'w.txt', data: 'from-worker' },
      { path: 'd/', data: '' },
    ]);
    const entries = await wr.listArchive(rar);
    assert.equal(entries.length, 2);
    const one = await wr.extractFile(rar, 'w.txt');
    assert.deepEqual(one, new TextEncoder().encode('from-worker'));
    const all = await wr.extractAll(rar);
    assert.equal(all.size, 2);
    assert.deepEqual(all.get('w.txt'), new TextEncoder().encode('from-worker'));
  } finally {
    wr.terminate();
  }
});
