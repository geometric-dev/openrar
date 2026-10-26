import test from 'node:test';
import assert from 'node:assert/strict';
import { ensureMockOrDist } from './mock_module.js';

ensureMockOrDist();

const {
  createArchive, createArchiveStream, listArchive, extractFile, extractAll,
  openArchive, RarError, validateArchivePath,
} = await import('./openrar-archive.js');

  test('validateArchivePath rejects bad paths', () => {
    assert.equal(validateArchivePath('').ok, false);
    assert.equal(validateArchivePath('/abs').ok, false);
    assert.equal(validateArchivePath('a\\b').ok, false);
    assert.equal(validateArchivePath('a/../b').ok, false);
    assert.equal(validateArchivePath('ok/file.txt').ok, true);
    assert.equal(validateArchivePath('dir/').ok, true);
  });

  test('createArchive + listArchive round-trip', async () => {
    const files = [
      { path: 'hello.txt', data: new TextEncoder().encode('Hello') },
      { path: 'dir/', data: new Uint8Array(0) },
      { path: 'dir/file.bin', data: new Uint8Array([1,2,3]) },
    ];
    const rar = await createArchive(files, { method: 3 });
    assert.ok(rar instanceof Uint8Array);
    const entries = await listArchive(rar);
    assert.equal(entries.length, 3);
    assert.equal(entries[0].path, 'hello.txt');
    assert.equal(entries[1].path, 'dir/');
    assert.equal(entries[1].isDir, true);
    assert.equal(entries[2].path, 'dir/file.bin');
  });

  test('extractFile round-trip', async () => {
    const data = new TextEncoder().encode('payload');
    const rar = await createArchive([{ path: 'a.txt', data }]);
    const out = await extractFile(rar, 'a.txt');
    assert.deepEqual(out, data);
  });

  test('extractAll returns Map', async () => {
    const files = [
      { path: 'a.txt', data: new TextEncoder().encode('aaa') },
      { path: 'b.txt', data: new TextEncoder().encode('bbb') },
    ];
    const rar = await createArchive(files);
    const map = await extractAll(rar);
    assert.equal(map.size, 2);
    assert.deepEqual(map.get('a.txt'), new TextEncoder().encode('aaa'));
    assert.deepEqual(map.get('b.txt'), new TextEncoder().encode('bbb'));
  });

  test('openArchive handle avoids double scan', async () => {
    const rar = await createArchive([{ path: 'x', data: new Uint8Array([9,8,7]) }]);
    const h = await openArchive(rar);
    assert.equal(h.pathCount, 1);
    assert.equal(h.list().length, 1);
    const out = await h.extract('x');
    assert.deepEqual(out, new Uint8Array([9,8,7]));
    const all = await h.extractAll();
    assert.equal(all.size, 1);
    h.close();
    assert.throws(() => h.list(), /closed/);
  });

  test('RarError on bad archive', async () => {
    await assert.rejects(async () => await listArchive(new Uint8Array([1,2,3])), (e) => e.code === 'NOT_RAR');
  });

  test('createArchive rejects invalid method', async () => {
    await assert.rejects(createArchive([{ path: 'x', data: new Uint8Array([1]) }], { method: 1 }), (e) => e.code === 'INVALID_ARG');
  });

  test('createArchive failure does not corrupt the heap (double-free regression)', async () => {
    // Regression: the error path used to free its allocations twice, which
    // corrupted the shared singleton wasm heap for every later call.
    await assert.rejects(createArchive([{ path: '/abs', data: new Uint8Array([1]) }]), (e) => e.code === 'INVALID_ARG');
    await assert.rejects(createArchive([{ path: 'a/../b', data: new Uint8Array([1]) }]), (e) => e.code === 'INVALID_ARG');
    // The module must still work perfectly after two failed creates.
    const rar = await createArchive([{ path: 'ok.txt', data: new TextEncoder().encode('still alive') }]);
    const entries = await listArchive(rar);
    assert.equal(entries.length, 1);
    assert.equal(entries[0].path, 'ok.txt');
  });

  test('createArchive with onProgress fires the C-ABI callback and keeps the heap healthy (hooks conformance)', async () => {
    // v1.21.1 regression pair: (a) the hooks pointer was freed twice on every
    // hooks-using createArchive call (freelist corruption), and (b) the
    // progress function pointer was registered with the wrong wasm signature
    // ('vijj' instead of 'vjji'), trapping the module on the first callback.
    // The existing double-free regression above passed hooksPtr=0 and never
    // exercised this path. This test drives the REAL C ABI.
    let calls = 0;
    let lastDone = -1;
    let lastTotal = -1;
    const rar = await createArchive(
      [{ path: 'hook.txt', data: new TextEncoder().encode('hooks conformance payload') }],
      {
        onProgress: (done, total) => {
          calls++;
          lastDone = done;
          lastTotal = total;
        },
      },
    );
    assert.ok(calls >= 1, 'progress callback never fired');
    assert.ok(lastDone >= 0 && lastTotal >= 0, 'progress reported non-negative counts');
    const entries = await listArchive(rar);
    assert.equal(entries.length, 1);
    // The module must still work after the hooks-using call: a heap corruption
    // or a trapped callback would fail the calls above or break this one.
    const out = await extractFile(rar, 'hook.txt');
    assert.equal(new TextDecoder().decode(out), 'hooks conformance payload');
  });

  test('createArchive with an AbortSignal wires cancellation through the C ABI', async () => {
    // Healthy signal: the operation completes.
    const healthy = new AbortController();
    const rar = await createArchive(
      [{ path: 'sig.txt', data: new TextEncoder().encode('signal payload') }],
      { signal: healthy.signal },
    );
    assert.equal((await listArchive(rar)).length, 1);
    // Pre-aborted signal: rejected with ABORTED before any work.
    const aborted = new AbortController();
    aborted.abort();
    await assert.rejects(
      createArchive([{ path: 'x.txt', data: new Uint8Array([1]) }], { signal: aborted.signal }),
      (e) => e.code === 'ABORTED',
    );
  });

  test('createArchive + extractAll round-trips repetitive single-byte payloads', async () => {
    const rar = await createArchive([
      { path: 'rep8k.bin', data: new Uint8Array(8192).fill(0x41) },
      { path: 'empty.bin', data: new Uint8Array(0) },
    ]);
    const map = await extractAll(rar);
    assert.equal(map.size, 2);
    assert.deepEqual(map.get('rep8k.bin'), new Uint8Array(8192).fill(0x41));
    assert.equal(map.get('empty.bin').length, 0);
  });

  test('createArchive honours mtime; listArchive reports UNIX seconds', async () => {
    const mtime = 1600000000; // 2020-09-13 12:26:40 UTC (2-second DOS granularity)
    const rar = await createArchive([{ path: 'timed.txt', data: 'x', mtime }]);
    const entries = await listArchive(rar);
    assert.equal(entries.length, 1);
    assert.ok(Math.abs(entries[0].mtime - mtime) <= 2, `mtime ${entries[0].mtime} vs ${mtime}`);
  });

  test('onProgress fires and is monotonic', async () => {
    const rar = await createArchive([
      { path: 'a.bin', data: new Uint8Array(2048).fill(7) },
      { path: 'b.bin', data: new Uint8Array(2048).fill(9) },
    ]);
    const extracts = [];
    await extractAll(rar, { onProgress: (done, total) => extracts.push([done, total]) });
    assert.ok(extracts.length >= 2, 'progress must fire');
    assert.equal(extracts[0][1], 4096);
    for (let i = 1; i < extracts.length; i++) {
      assert.ok(extracts[i][0] >= extracts[i - 1][0], 'done must be monotonic');
    }
    assert.equal(extracts[extracts.length - 1][0], 4096);
  });

  test('AbortSignal stops extractAll between entries', async () => {
    const rar = await createArchive([
      { path: 'a.bin', data: new Uint8Array(1024).fill(1) },
      { path: 'b.bin', data: new Uint8Array(1024).fill(2) },
    ]);
    const controller = new AbortController();
    const h = await openArchive(rar);
    try {
      // Abort from the progress callback once the first entry is done —
      // the next per-entry poll must reject with ABORTED.
      const promise = h.extractAll({
        signal: controller.signal,
        onProgress: (done) => { if (done >= 1024) controller.abort(); },
      });
      await assert.rejects(promise, (e) => e.code === 'ABORTED');
    } finally {
      h.close();
    }
  });

  test('extractAll bomb guard rejects lying archives', async () => {
    const rar = await createArchive([{ path: 'small.bin', data: new Uint8Array(64).fill(3) }]);
    await assert.rejects(
      extractAll(rar, { maxOutputBytes: 8 }),
      (e) => e.code === 'NOMEM',
    );
    // With the guard effectively off, extraction succeeds.
    const map = await extractAll(rar, { maxOutputBytes: 0 });
    assert.equal(map.get('small.bin').length, 64);
  });

  test('bulk extractAll returns the same content as per-entry', async () => {
    const rar = await createArchive([
      { path: 'x.bin', data: new Uint8Array(4096).fill(5) },
      { path: 'y.bin', data: 'plain text' },
    ]);
    const perEntry = await extractAll(rar);
    const bulk = await extractAll(rar, { bulk: true });
    assert.equal(bulk.size, perEntry.size);
    assert.deepEqual(bulk.get('x.bin'), perEntry.get('x.bin'));
    assert.deepEqual(bulk.get('y.bin'), perEntry.get('y.bin'));
  });

  test('extractAll with ExtractionLimits enforces maxMemberBytes and maxTotalBytes', async () => {
    const rar = await createArchive([
      { path: 'file1.bin', data: new Uint8Array(64).fill(1) },
      { path: 'file2.bin', data: new Uint8Array(64).fill(2) },
    ]);
    // Member limit exceeded
    await assert.rejects(
      extractAll(rar, { limits: { maxMemberBytes: 32 } }),
      (e) => e.code === 'LIMIT_EXCEEDED' && e.numericCode === -15,
    );
    // Total limit exceeded
    await assert.rejects(
      extractAll(rar, { limits: { maxTotalBytes: 100 } }),
      (e) => e.code === 'LIMIT_EXCEEDED' && e.numericCode === -15,
    );
    // OpenArchive handle-based member limit exceeded
    const h = await openArchive(rar, { limits: { maxMemberBytes: 32 } });
    try {
      await assert.rejects(
        h.extract('file1.bin'),
        (e) => e.code === 'LIMIT_EXCEEDED' && e.numericCode === -15,
      );
    } finally {
      h.close();
    }
  });

  // ── Streaming Archive Encode (v1.49 §7.7) ───────────────────────────────

  test('createArchiveStream yields byte-identical chunks to createArchive (parity I-3)', async () => {
    const files = [
      { path: 'doc.txt', data: new TextEncoder().encode('OpenRAR Streaming Parity Test Payload'), mtime: 1700000000 },
      { path: 'data.bin', data: new Uint8Array([10, 20, 30, 40, 50, 60, 70, 80]), mtime: 1700000000 },
      { path: 'folder/', data: new Uint8Array(0), mtime: 1700000000 },
    ];
    const oneshot = await createArchive(files, { method: 3 });

    // Stream with 16-byte chunks to exercise multi-chunk reassembly
    const stream = createArchiveStream(files, { method: 3, chunkSize: 16 });
    const chunks = [];
    let totalLen = 0;
    for await (const chunk of stream) {
      assert.ok(chunk instanceof Uint8Array);
      chunks.push(chunk);
      totalLen += chunk.length;
    }
    const streamed = new Uint8Array(totalLen);
    let off = 0;
    for (const c of chunks) {
      streamed.set(c, off);
      off += c.length;
    }

    assert.deepEqual(streamed, oneshot);

    // Verify the streamed archive is completely readable by openArchive
    const h = await openArchive(streamed);
    assert.equal(h.pathCount, 3);
    const extracted = await h.extract('doc.txt');
    assert.deepEqual(extracted, new TextEncoder().encode('OpenRAR Streaming Parity Test Payload'));
    h.close();
  });

  test('createArchiveStream supports AsyncIterable file inputs', async () => {
    async function* fileGenerator() {
      yield { path: 'gen1.txt', data: new TextEncoder().encode('file 1') };
      yield { path: 'gen2.txt', data: new TextEncoder().encode('file 2') };
    }
    const stream = createArchiveStream(fileGenerator(), { method: 3 });
    const chunks = [];
    for await (const chunk of stream) {
      chunks.push(chunk);
    }
    assert.ok(chunks.length > 0);
    const total = chunks.reduce((acc, c) => acc + c.length, 0);
    const combined = new Uint8Array(total);
    let cur = 0;
    for (const c of chunks) { combined.set(c, cur); cur += c.length; }
    const entries = await listArchive(combined);
    assert.equal(entries.length, 2);
    assert.equal(entries[0].path, 'gen1.txt');
    assert.equal(entries[1].path, 'gen2.txt');
  });

  test('createArchiveStream fail-closed: aborted signal errors controller and never clean-closes', async () => {
    const ac = new AbortController();
    ac.abort(); // already aborted

    const stream = createArchiveStream([{ path: 'test.bin', data: new Uint8Array(100) }], { signal: ac.signal });
    const reader = stream.getReader();

    await assert.rejects(
      async () => {
        await reader.read();
      },
      (e) => e.code === 'ABORTED' && e.numericCode === -11,
    );
  });

  test('createArchiveStream fail-closed: invalid method errors controller', async () => {
    assert.throws(
      () => createArchiveStream([{ path: 'test.bin', data: new Uint8Array(10) }], { method: 99 }),
      (e) => e.code === 'INVALID_ARG',
    );
  });

  test('createArchiveStream chunk ownership: yielded chunks are independent JS-owned copies', async () => {
    const files = [{ path: 'own.txt', data: new TextEncoder().encode('ownership test') }];
    const stream = createArchiveStream(files);
    const chunks = [];
    for await (const chunk of stream) {
      chunks.push(chunk);
      chunk[0] ^= 0xFF; // mutate the chunk copy to prove independence
    }
    assert.ok(chunks.length > 0);
  });
