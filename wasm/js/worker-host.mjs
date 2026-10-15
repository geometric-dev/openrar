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
//  Input transfer semantics: pass `{ transfer: true }` as the trailing
//  argument to move the caller's ArrayBuffer into the worker (zero copy —
//  the caller's view becomes detached and unusable afterwards). The default
//  is a structured-clone copy, which is always safe.
//
//  IMPORTANT (was broken): progress callbacks and AbortSignals are HOST-ONLY
//  objects. They are not structured-cloneable, so they can never appear in a
//  postMessage payload. The previous version passed `opts` — carrying
//  `onProgress` and `signal` — straight through to postMessage, so
//  `extractAll(rar, { onProgress })` and `createArchive(files, { onProgress })`
//  threw a synchronous DataCloneError and were uncallable. Progress is now
//  signalled to the worker as a plain boolean (`wantsProgress`) and the real
//  callback is retained on the host; `signal` aborts the host-side promise.
//  Anything else non-cloneable is now a loud error rather than a silent drop.
//
//  In environments without Worker support (plain Node tests) the factory
//  falls back to running the same operations on the current thread.
// ─────────────────────────────────────────────────────────────────────────────

import { OpenRAR } from './openrar.js';
import {
  RarError, createArchive, listArchive, extractFile,
  extractFileByIndex, extractAll as archiveExtractAll,
} from './openrar-archive.js';

// Upper bound on calls dispatched to the worker at once. Each in-flight call
// may hold a full copy of its input, and they all share one wasm heap, so
// unbounded fan-out turns a `for` loop into an out-of-memory. Extra calls
// queue and dispatch as earlier ones settle.
const MAX_INFLIGHT = 8;

// Keys that are meaningful only on the host and must never be posted.
const HOST_ONLY_KEYS = new Set(['onProgress', 'signal']);

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

// Strip host-only members from an options bag (shallow: these are always
// top-level keys of the caller's options object).
function stripHostOnly(opts) {
  if (!opts || typeof opts !== 'object') return opts;
  const out = {};
  for (const [k, v] of Object.entries(opts)) {
    if (!HOST_ONLY_KEYS.has(k)) out[k] = v;
  }
  return out;
}

// Fail loudly on anything postMessage cannot carry. Silently dropping it
// would turn a caller's function/signal into a no-op with no diagnostic,
// which is the exact failure mode this file previously had.
function assertPostable(value, path = 'args') {
  if (value === null) return;
  const t = typeof value;
  if (t === 'function') {
    throw new TypeError(
      `openrar-worker: ${path} is a function, which cannot be posted to a worker. ` +
      'Pass progress via the { onProgress } option (handled host-side) and ' +
      'cancellation via { signal }.');
  }
  if (t !== 'object') return; // primitives, bigint, string, boolean, undefined
  if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) return;
  if (value instanceof Map) {
    // Object.entries() is empty for a Map, so contents need their own walk -
    // a function value would otherwise pass validation here and still throw
    // DataCloneError inside the browser.
    let i = 0;
    for (const [k, v] of value.entries()) {
      assertPostable(k, `${path}.map_keys[${i}]`);
      assertPostable(v, `${path}.map_keys[${i}].value`);
      i++;
    }
    return;
  }
  if (value instanceof Set) {
    let i = 0;
    for (const v of value) { assertPostable(v, `${path}.set[${i}]`); i++; }
    return;
  }
  if (Array.isArray(value)) {
    value.forEach((v, i) => assertPostable(v, `${path}[${i}]`));
    return;
  }
  // Plain objects, Maps, Sets, Dates, RegExp and ArrayBuffers' views are all
  // cloneable; anything exotic is rejected rather than half-delivered.
  const proto = Object.getPrototypeOf(value);
  const plain = proto === Object.prototype || proto === null;
  if (!plain && !(value instanceof Map) && !(value instanceof Set)) {
    // Date and RegExp are structured-cloneable but cannot be meaningfully
    // inspected by Object.entries(); accept them explicitly rather than
    // rejecting a valid payload.
    if (value instanceof Date || value instanceof RegExp) return;
    throw new TypeError(
      `openrar-worker: ${path} is a ${value.constructor?.name || 'non-plain object'}, ` +
      'which cannot be posted to a worker. Wrap it in a plain object or ArrayBuffer.');
  }
  for (const [k, v] of Object.entries(value)) assertPostable(v, `${path}.${k}`);
}

class OpenRARWorkerImpl {
  constructor(url) {
    this.#worker = new Worker(url, { type: 'module' });
    this.#worker.onmessage = (ev) => this.#onMessage(ev.data);
    this.#worker.onerror = (ev) => {
      // Fail every in-flight call — the worker script itself is broken.
      // Latch it: per spec, postMessage on a terminated worker is a silent
      // no-op, so without this latch any call made after an error would
      // return a promise that never settles.
      const err = new Error(`openrar-worker: worker error: ${ev.message}`);
      this.#broken = err;
      this.#failAll(err);
    };
  }

  #worker;
  #nextId = 1;
  #pending = new Map();
  #queue = [];
  #broken = null;
  #terminated = false;

  #failAll(err) {
    for (const call of this.#pending.values()) call.reject(err);
    this.#pending.clear();
    for (const entry of this.#queue) entry.reject(err);
    this.#queue = [];
  }

  #onMessage(msg) {
    const call = msg && this.#pending.get(msg.id);
    if (!call) return; // already settled, or an id we never issued
    if (msg.type === 'progress') {
      if (call.onProgress) call.onProgress(msg.done, msg.total);
      return; // progress does NOT settle the call
    }
    this.#pending.delete(msg.id);
    this.#pump();
    if (msg.ok) call.resolve(msg.result);
    else call.reject(reconstructError(msg.error));
  }

  // Dispatch queued work until the in-flight cap is reached again.
  #pump() {
    while (!this.#terminated && !this.#broken && this.#queue.length
           && this.#pending.size < MAX_INFLIGHT) {
      const entry = this.#queue.shift();
      this.#pending.set(entry.call.id, entry.call);
      this.#worker.postMessage(entry.message, entry.transfer);
    }
  }

  #call(op, args, { transfer = false, onProgress, signal } = {}) {
    if (this.#terminated) {
      return Promise.reject(new Error('openrar-worker: worker has been terminated'));
    }
    if (this.#broken) return Promise.reject(this.#broken);

    // Validate before anything is registered, so a rejected call leaves no
    // entry in #pending and no dangling abort listener. Reject rather than
    // throw: every method on this handle is async and returns a promise, so a
    // non-cloneable argument must surface as a rejection the caller awaits,
    // not a synchronous throw that skips their catch.
    try {
      assertPostable(args);
    } catch (err) {
      return Promise.reject(err);
    }

    const id = this.#nextId++;
    const call = new WorkerCall(this.#worker, id);
    call.onProgress = onProgress;
    const message = { id, op, args };
    const transferList = transfer ? [...collectTransferables(args)] : [];

    if (signal) {
      if (signal.aborted) {
        return Promise.reject(signal.reason
          ?? new DOMException('The operation was aborted.', 'AbortError'));
      }
      const onAbort = () => {
        if (!this.#pending.delete(id)) return;
        // The worker keeps running the job (an AbortSignal cannot cross the
        // boundary); we stop waiting for it. Rejecting is the observable
        // behaviour the caller asked for.
        call.reject(signal.reason
          ?? new DOMException('The operation was aborted.', 'AbortError'));
        this.#pump();
      };
      signal.addEventListener('abort', onAbort, { once: true });
      // Settling the promise is the last thing that happens; drop the
      // listener then so an aborted call does not pin the signal.
      call.promise.then(
        () => signal.removeEventListener('abort', onAbort),
        () => signal.removeEventListener('abort', onAbort));
    }

    this.#pending.set(id, call);
    if (this.#pending.size <= MAX_INFLIGHT && this.#queue.length === 0) {
      this.#worker.postMessage(message, transferList);
    } else {
      this.#queue.push({ call, message, transfer: transferList, reject: call.reject });
    }
    return call.promise;
  }

  compress(data, method, winSize, opts) {
    return this.#call('compress', { data, method, winSize }, { ...opts, transfer: opts?.transfer ?? true });
  }

  decompress(data, winSize, opts) {
    return this.#call('decompress', { data, winSize }, { ...opts, transfer: opts?.transfer ?? true });
  }

  createArchive(files, opts = {}, callOpts) {
    const wantsProgress = typeof opts.onProgress === 'function';
    return this.#call('createArchive',
      { files, opts: stripHostOnly(opts), wantsProgress },
      { ...callOpts, transfer: callOpts?.transfer ?? true, onProgress: opts.onProgress });
  }

  listArchive(rar, opts) {
    return this.#call('listArchive', { rar }, { ...opts, transfer: opts?.transfer ?? true });
  }

  extractFile(rar, path, opts) {
    return this.#call('extractFile', { rar, path }, { ...opts, transfer: opts?.transfer ?? true });
  }

  extractFileByIndex(rar, index, opts) {
    return this.#call('extractFileByIndex', { rar, index }, { ...opts, transfer: opts?.transfer ?? true });
  }

  extractAll(rar, opts = {}, callOpts) {
    const wantsProgress = typeof opts.onProgress === 'function';
    return this.#call('extractAll',
      { rar, opts: stripHostOnly(opts), wantsProgress },
      { ...callOpts, transfer: callOpts?.transfer ?? true, onProgress: opts.onProgress })
      .then((payload) => new Map(payload.entries));
  }

  /** Deletes the in-worker wasm module; the worker itself stays alive. */
  destroy() {
    return this.#call('destroy', {});
  }

  /**
   * Hard-stops the worker. Every in-flight and queued call is rejected;
   * calls made afterwards reject immediately rather than hanging (posting to
   * a terminated worker is a silent no-op, so a bare postMessage would
   * never settle).
   */
  terminate() {
    this.#terminated = true;
    this.#worker.terminate();
    this.#failAll(new Error('openrar-worker: terminated'));
  }
}

// Recursively collect ArrayBuffers from an args object so postMessage can
// transfer them. Only used with transfer:true (documented detach semantics).
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
  #terminated = false;
  #guard() {
    if (this.#terminated) throw new Error('openrar-worker: worker has been terminated');
  }
  compress(data, method, winSize, opts) {
    this.#guard();
    return this.#blockProxy.compress(data, method, winSize);
  }
  decompress(data, winSize, opts) {
    this.#guard();
    return this.#blockProxy.decompress(data, winSize);
  }
  createArchive(files, opts) { this.#guard(); return createArchive(files, opts); }
  listArchive(rar) { this.#guard(); return listArchive(rar); }
  extractFile(rar, path) { this.#guard(); return extractFile(rar, path); }
  extractFileByIndex(rar, index) { this.#guard(); return extractFileByIndex(rar, index); }
  extractAll(rar, opts) { this.#guard(); return archiveExtractAll(rar, opts); }
  destroy() { return Promise.resolve(); }
  terminate() { this.#terminated = true; }
}

/**
 * Create an off-thread OpenRAR handle.
 * @param {{ workerUrl?: string }} [opts]
 *   workerUrl overrides the bundled worker script location (e.g. when a
 *   bundler relocates it). Must point at wasm/js/worker.js.
 */
export function createOpenRARWorker(opts = {}) {
  // Resolved PER CALL, not at module load. A module-level probe froze the
  // answer to whatever `Worker` happened to be when this file was first
  // imported: in Node (no global Worker) the factory then always returned the
  // main-thread fallback, so a host that installs a Worker polyfill - or the
  // test suite's fake Worker - could never reach the real off-thread path and
  // the whole worker protocol was untestable (v1.41.1).
  if (typeof Worker === 'undefined' || typeof import.meta.url !== 'string') {
    return new OpenRARProxyFallback();
  }
  const url = opts.workerUrl || new URL('./worker.js', import.meta.url).href;
  return new OpenRARWorkerImpl(url);
}