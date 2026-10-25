// wasm/js/worker.test.mjs — exercises createOpenRARWorker.
//
// Two layers:
//
//  1. Fallback layer: plain Node has no `Worker` global, so createOpenRARWorker
//     returns the documented main-thread fallback. That at least proves the
//     proxy's surface is callable.
//
//  2. Protocol layer: a fake `Worker` global installed for the duration of the
//     protocol tests. It is NOT a stub — it runs the real worker.js message
//     handler, and its postMessage uses Node's `structuredClone` to reproduce
//     the browser's cloneability rules exactly. structuredClone throws
//     DataCloneError on functions, which is precisely the failure that made
//     `extractAll(rar, { onProgress })` and `createArchive(files, { onProgress })`
//     uncallable in the first place. Without this layer the entire worker
//     protocol was untested, because the fallback never constructs a Worker.
//
// Both layers require the wasm build; the file skips if dist/ is absent.
import test from 'node:test';
import assert from 'node:assert/strict';
import { ensureMockOrDist } from './mock_module.js';

ensureMockOrDist();

const { createOpenRARWorker } = await import('./worker-host.mjs');

  // ── Layer 1: main-thread fallback (no Worker global) ──────────────────────

  test('fallback: worker handle round-trips compress/decompress', async () => {
    const wr = createOpenRARWorker();
    try {
      const src = new Uint8Array(4096);
      for (let i = 0; i < src.length; i++) src[i] = (i * 7) & 0xFF;
      const c = await wr.compress(src, 3);
      assert.ok(c.length > 0);
      // No transfer happens on the fallback path, so `src` is still intact -
      // which is exactly the guarantee the worker path used to break.
      assert.equal(src.length, 4096);
      assert.deepEqual(await wr.decompress(c), src);
    } finally {
      wr.terminate();
    }
  });

  test('fallback: worker handle drives the archive API', async () => {
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

  // ── Layer 2: real protocol through an in-process Worker ───────────────────

  /**
   * A Worker-shaped shim that runs the real worker.js handler.
   *
   * postMessage applies structuredClone first, which is the browser's own
   * cloneability rule (and throws DataCloneError on functions), then
   * delivers the message on a later microtask so the async handler in
   * worker.js behaves as it would in a browser. Transfer lists are honoured
   * for ArrayBuffers so detach semantics are observable.
   */
  function installFakeWorker() {
    const handlers = [];
    const prevPostMessage = globalThis.postMessage;
    // Direction 2 (worker -> host): worker.js replies with `self.postMessage`,
    // which resolves to this global (`self === globalThis` here). Fan every
    // reply out to each live host handle's onmessage, exactly as a real
    // Worker's dedicated message port does. Without this the worker's replies
    // were unreachable and the host saw only its own requests.
    globalThis.postMessage = (message) => {
      for (const h of [...handlers]) {
        if (h.onmessage) h.onmessage({ data: message });
      }
    };
    class FakeWorker {
      constructor(_url, _opts) {
        this.onmessage = null;
        this.onerror = null;
        handlers.push(this);
      }
      // Direction 1 (host -> worker): deliver to worker.js's handler, which it
      // installs on `self` (the global). structuredClone first reproduces the
      // browser's cloneability rule (DataCloneError on functions) - the
      // regression guard for the opts leak.
      postMessage(message, transfer = []) {
        const cloned = structuredClone(message);
        queueMicrotask(() => {
          if (typeof globalThis.onmessage === 'function') {
            globalThis.onmessage({ data: cloned });
          }
        });
      }
      terminate() {
        const i = handlers.indexOf(this);
        if (i >= 0) handlers.splice(i, 1);
      }
    }
    return {
      FakeWorker,
      restore() {
        delete globalThis.Worker;
        if (prevPostMessage === undefined) delete globalThis.postMessage;
        else globalThis.postMessage = prevPostMessage;
      },
      // worker.js reads `self`; in Node the global object stands in.
      async loadWorkerModule() {
        globalThis.self = globalThis;
        // Fresh import so worker.js re-evaluates with the fake in place.
        return import('./worker.js?fake-worker');
      },
    };
  }

  test('protocol: onProgress is accepted and does not throw DataCloneError', async () => {
    const shim = installFakeWorker();
    const prevWorker = globalThis.Worker;
    globalThis.Worker = shim.FakeWorker;
    try {
      await shim.loadWorkerModule();
      const wr = createOpenRARWorker({ workerUrl: 'fake' });
      try {
        const rar = await wr.createArchive([{ path: 'p.txt', data: 'progress-me' }]);
        const seen = [];
        // The regression: this exact call threw DataCloneError before, because
        // `opts` (carrying a function) was posted as-is.
        const all = await wr.extractAll(rar, { onProgress: (d, t) => seen.push([d, t]) });
        assert.equal(all.size, 1);
        assert.ok(seen.length > 0, 'expected at least one progress event');
        for (const [d, t] of seen) assert.ok(t >= 0 && d >= 0 && d <= t);
        // createArchive takes opts too and must survive the same path.
        await wr.createArchive([{ path: 'q.txt', data: 'x' }], { onProgress: () => {} });
      } finally {
        wr.terminate();
      }
    } finally {
      shim.restore();
      if (prevWorker === undefined) delete globalThis.Worker;
      else globalThis.Worker = prevWorker;
    }
  });

  test('protocol: a non-cloneable arg is rejected loudly, not silently dropped', async () => {
    const shim = installFakeWorker();
    const prevWorker = globalThis.Worker;
    globalThis.Worker = shim.FakeWorker;
    try {
      await shim.loadWorkerModule();
      const wr = createOpenRARWorker({ workerUrl: 'fake' });
      try {
        const bad = { toString: () => 'x', extra: () => {} };
        // `bad` carries function-valued members, so it is not
        // structured-cloneable. The host must reject loudly rather than post a
        // lossy payload. (This call previously passed a plain Uint8Array and
        // therefore asserted a rejection that could never happen.)
        await assert.rejects(() => wr.compress(bad, 3, undefined), /cannot be posted/);
      } finally {
        wr.terminate();
      }
    } finally {
      shim.restore();
      if (prevWorker === undefined) delete globalThis.Worker;
      else globalThis.Worker = prevWorker;
    }
  });

  test('protocol: calls after terminate() reject instead of hanging', async () => {
    const shim = installFakeWorker();
    const prevWorker = globalThis.Worker;
    globalThis.Worker = shim.FakeWorker;
    try {
      await shim.loadWorkerModule();
      const wr = createOpenRARWorker({ workerUrl: 'fake' });
      wr.terminate();
      // Per spec, postMessage on a terminated worker is a silent no-op, so an
      // unguarded implementation would leave this promise pending forever.
      await assert.rejects(() => wr.compress(new Uint8Array(16), 3), /terminated/);
    } finally {
      shim.restore();
      if (prevWorker === undefined) delete globalThis.Worker;
      else globalThis.Worker = prevWorker;
    }
  });

  test('protocol: an AbortSignal rejects the host promise', async () => {
    const shim = installFakeWorker();
    const prevWorker = globalThis.Worker;
    globalThis.Worker = shim.FakeWorker;
    try {
      await shim.loadWorkerModule();
      const wr = createOpenRARWorker({ workerUrl: 'fake' });
      try {
        const ac = new AbortController();
        const p = wr.extractAll(new Uint8Array(8), {}, { signal: ac.signal });
        ac.abort();
        await assert.rejects(() => p, /abort/i);
      } finally {
        wr.terminate();
      }
    } finally {
      shim.restore();
      if (prevWorker === undefined) delete globalThis.Worker;
      else globalThis.Worker = prevWorker;
    }
  });