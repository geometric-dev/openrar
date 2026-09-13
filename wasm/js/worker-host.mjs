// ─────────────────────────────────────────────────────────────────────────────
//  worker-host.mjs — main-thread handle to an OpenRAR worker.
//
//  Same async surface as OpenRAR / the archive functions, executed inside a
//  module worker so the main thread never blocks on compression.
//
//    const wr = createOpenRARWorker();
//    const c = await wr.compress(bigBytes);       // main thread stays free
//    const d = await wr.decompress(c);
//    const entries = await wr.listArchive(rarBytes);
//    wr.terminate();
//
//  Input transfer semantics: `transfer: true` moves the caller's ArrayBuffer
//  into the worker (zero copy — the caller's view becomes detached and
//  unusable). Default is structured-clone (copy), which is always safe.
//  Outputs are always transferred back (zero copy).
//
//  In environments without Worker support (plain Node tests) the factory
//  falls back to running the same operations on the current thread.
// ─────────────────────────────────────────────────────────────────────────────

import { OpenRAR } from './openrar.js';
import {
  RarError, createArchive, listArchive, extractFile,
  extractFileByIndex, extractAll as archiveExtractAll,
} from './openrar-archive.js';

const HAS_WORKER = typeof Worker !== 'undefined' && typeof import.meta.url === 'string';

class WorkerCall {
  constructor(worker, id) {
    this.worker = worker;
    this.id = id;
    this.promise = new Promise((resolve, reject) => {
      this.resolve = resolve;
      this.reject = reject;
    });
  }
}

function reconstructError(err) {
  if (err && err.code) return new RarError(err.code, err.message, err.numericCode);
  const e = new Error(err ? err.message : 'openrar-worker: unknown error');
  if (err && err.name) e.name = err.name;
  return e;
}

class OpenRARWorkerImpl {
  constructor(url) {
    this.#worker = new Worker(url, { type: 'module' });
    this.#worker.onmessage = (ev) => this.#onMessage(ev.data);
    this.#worker.onerror = (ev) => {
      // Fail every in-flight call — the worker script itself is broken.
      const err = new Error(`openrar-worker: worker error: ${ev.message}`);
      for (const call of this.#pending.values()) call.reject(err);
      this.#pending.clear();
    };
  }

  #worker;
  #nextId = 1;
  #pending = new Map();

  #onMessage(msg) {
    const call = msg && this.#pending.get(msg.id);
    if (!call) return; // progress events are routed separately
    if (msg.type === 'progress') {
      if (call.onProgress) call.onProgress(msg.done, msg.total);
      return;
    }
    this.#pending.delete(msg.id);
    if (msg.ok) call.resolve(msg.result);
    else call.reject(reconstructError(msg.error));
  }

  #call(op, args, { transferOut = [], transferIn = false, onProgress } = {}) {
    if (!HAS_WORKER) throw new Error('unreachable');
    const id = this.#nextId++;
    const call = new WorkerCall(this.#worker, id);
    call.onProgress = onProgress;
    this.#pending.set(id, call);
    const transfer = transferIn ? collectTransferables(args) : [];
    this.#worker.postMessage({ id, op, args }, transfer);
    return call.promise;
  }

  compress(data, method, winSize) {
    return this.#call('compress', { data, method, winSize }, { transferIn: true });
  }

  decompress(data, winSize) {
    return this.#call('decompress', { data, winSize }, { transferIn: true });
  }

  createArchive(files, opts) {
    return this.#call('createArchive', { files, opts }, { transferIn: true });
  }

  listArchive(rar) {
    return this.#call('listArchive', { rar }, { transferIn: true });
  }

  extractFile(rar, path) {
    return this.#call('extractFile', { rar, path }, { transferIn: true });
  }

  extractFileByIndex(rar, index) {
    return this.#call('extractFileByIndex', { rar, index }, { transferIn: true });
  }

  extractAll(rar, opts) {
    return this.#call('extractAll', { rar, opts }, {
      transferIn: true,
      onProgress: opts && opts.onProgress,
    }).then((payload) => new Map(payload.entries));
  }

  /** Deletes the in-worker wasm module; the worker itself stays alive. */
  destroy() {
    return this.#call('destroy', {});
  }

  /** Hard-stops the worker. All in-flight calls are dropped (never resolve). */
  terminate() {
    this.#worker.terminate();
    const err = new Error('openrar-worker: terminated');
    for (const call of this.#pending.values()) call.reject(err);
    this.#pending.clear();
  }
}

// Recursively collect ArrayBuffers from an args object so postMessage can
// transfer them. Only used with transferIn:true (documented detach semantics).
function collectTransferables(value, found = new Set()) {
  if (!value || typeof value !== 'object') return found;
  if (value instanceof ArrayBuffer) found.add(value);
  else if (ArrayBuffer.isView(value)) found.add(value.buffer);
  else if (Array.isArray(value)) for (const v of value) collectTransferables(v, found);
  else for (const v of Object.values(value)) collectTransferables(v, found);
  return found;
}

// ── Main-thread fallback (no Worker support) ────────────────────────────────

class OpenRARProxyBlock {
  #handle = null;
  async #block() {
    if (!this.#handle) {
      this.#handle = new OpenRAR();
      await this.#handle.ready;
    }
    return this.#handle;
  }
  async compress(data, method, winSize) { return (await this.#block()).compress(data, method, winSize); }
  async decompress(data, winSize) { return (await this.#block()).decompress(data, winSize); }
}

class OpenRARProxyFallback {
  #blockProxy = new OpenRARProxyBlock();
  compress(data, method, winSize) { return this.#blockProxy.compress(data, method, winSize); }
  decompress(data, winSize) { return this.#blockProxy.decompress(data, winSize); }
  createArchive(files, opts) { return createArchive(files, opts); }
  listArchive(rar) { return listArchive(rar); }
  extractFile(rar, path) { return extractFile(rar, path); }
  extractFileByIndex(rar, index) { return extractFileByIndex(rar, index); }
  extractAll(rar, opts) { return archiveExtractAll(rar, opts); }
  destroy() { return Promise.resolve(); }
  terminate() {}
}

/**
 * Create an off-thread OpenRAR handle.
 * @param {{ workerUrl?: string }} [opts]
 *   workerUrl overrides the bundled worker script location (e.g. when a
 *   bundler relocates it). Must point at wasm/js/worker.js.
 */
export function createOpenRARWorker(opts = {}) {
  if (!HAS_WORKER) return new OpenRARProxyFallback();
  const url = opts.workerUrl || new URL('./worker.js', import.meta.url).href;
  return new OpenRARWorkerImpl(url);
}
