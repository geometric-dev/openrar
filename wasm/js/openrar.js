// ─────────────────────────────────────────────────────────────────────────────
//  openrar.js — OpenRAR WASM interface contract
// ─────────────────────────────────────────────────────────────────────────────
//
//  This module is the canonical ergonomic surface for the OpenRAR block codec
//  compiled to WebAssembly.
//
//  Two layers:
//
//    1. RAW MODULE  — Promise<RawModule> from the Emscripten MODULARIZE factory.
//                     1:1 with the C ABI in src/wasm/wasm_api.cpp (no embind).
//                     Use for FFI / ccall / cwrap / QuickJS hosts.
//
//    2. OpenRAR     — high-level async handle. Drives the C ABI directly
//                     (HEAPU8.set in, safeRead out) and auto-coerces
//                     string / Uint8Array / ArrayBuffer / View / Blob inputs.
//                     Recommended surface for browser/Node.
//
//  The block codec only produces RAR5 v0 compressed blocks, not full .rar
//  archives. For archive-level work, import ./openrar-archive.js (built with
//  the wasm-archive preset) — see wasm/README.md and docs/wasm-limitations.md.
//
//  Stability:
//    - The C ABI in src/wasm/wasm_api.{hpp,cpp} is the stability floor.
//    - Exports here map 1:1 to those exports; ABI bumps bump WASM_API_VERSION.
//    - JS classes (OpenRAR) follow semver and live in this repo.
//
// ─────────────────────────────────────────────────────────────────────────────

import createOpenRARRaw from '../dist/openrar.js';
import { safeRead, heapU8Base, pointerInBounds, requireExports } from './heap_helpers.js';

// Re-export the raw factory so advanced users can opt out of the wrapper.
export { createOpenRARRaw as createOpenRAR };

// Compression methods — match the integers in src/compress/compressor50.hpp.
export const CompressionMethod = Object.freeze({
  STORE:    0,
  FASTEST:  1,
  FAST:     2,
  NORMAL:   3,   // default
  GOOD:     4,
  BEST:     5,
});

// ABI version of the underlying WASM module. Bumped on any incompatible
// change to the C exports in src/wasm/wasm_api.cpp.
//
//   v1: initial release. Reset from v2 because v2 was never shipped.
//   v2: embind layer removed — RawModule is the raw C ABI surface
//       (_openrar_* exports only); the JS wrapper drives compress/decompress
//       through ccall.
export const WASM_API_VERSION = 2;

// Default dictionary window size for compress2 (matches src/wasm/wasm_api.cpp).
export const DEFAULT_WIN_SIZE = 2 * 1024 * 1024;

// Default dictionary window size for decompress2 (matches src/wasm/wasm_api.cpp).
export const DEFAULT_DECOMPRESS_WIN_SIZE = 1024 * 1024;

// Largest window size reachable from JS. The C ABI caps at 4 GiB, but ccall
// passes winSize as an i32 on wasm32 — anything above 2 GiB would truncate
// to garbage before the C validation ever sees it.
export const MAX_WIN_SIZE = 0x7ffff000; // 2 GiB - 4 KiB

/** @throws {RangeError} if winSize is not a usable i32 window size. */
function assertWinSize(winSize) {
  if (!Number.isInteger(winSize) || winSize <= 0 || winSize > MAX_WIN_SIZE) {
    throw new RangeError(
      `openrar: winSize must be an integer in (0, ${MAX_WIN_SIZE}] (got ${winSize})`,
    );
  }
}

/** @throws {RangeError} if method is not a CompressionMethod value. */
function assertMethod(method) {
  if (!Number.isInteger(method) || method < 0 || method > 5) {
    throw new RangeError(`openrar: method must be an integer 0..5 (got ${method})`);
  }
}

// Exports the wrapper requires on the raw module. Enforced at boot by
// requireExports so a stale dist fails with a rebuild hint, not a TypeError.
const REQUIRED_EXPORTS = [
  '_openrar_version', '_openrar_alloc', '_openrar_free',
  '_openrar_compress2', '_openrar_decompress2',
  '_malloc', '_free', 'ccall', 'HEAPU8', 'HEAPU32',
];

// ── Module bootstrap (singleton) ────────────────────────────────────────────
// Multiple OpenRAR instances share one WASM module. The first instantiation
// loads + links the .wasm file (~250 KiB); subsequent instances reuse it.
let _bootPromise = null;
let _module = null;

/** @returns {Promise<RawModule>} the underlying Emscripten module. */
export async function getModule() {
  if (_module) return _module;
  if (!_bootPromise) {
    _bootPromise = createOpenRARRaw().then((m) => {
      requireExports(m, REQUIRED_EXPORTS, 'emcmake cmake --preset wasm && cmake --build --preset wasm');
      const v = m._openrar_version();
      if (v !== WASM_API_VERSION) {
        // Surface version mismatch — caller may need to rebuild wasm/.
        throw new Error(
          `openrar-wasm: ABI mismatch (host expects v${WASM_API_VERSION}, module is v${v}). ` +
          `Run "emcmake cmake --preset wasm" to rebuild.`,
        );
      }
      _module = m;
      return m;
    });
  }
  return _bootPromise;
}

// ── Input coercion ────────────────────────────────────────────────────────

/**
 * Synchronous input coercion. Accepts string, Uint8Array, ArrayBuffer, or
 * an ArrayBufferView. Throws for Blobs and other async-only inputs —
 * callers with a Blob must use {@link blobToBytes} first.
 */
export function toUint8(data) {
  if (data instanceof Uint8Array) return data;
  if (typeof data === 'string') return new TextEncoder().encode(data);
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  throw new TypeError(
    'openrar: expected Uint8Array, string, ArrayBuffer, or ArrayBufferView',
  );
}

/**
 * Async coercion: adds Blob and Response support via .arrayBuffer().
 * Use this for browser File / fetch Response inputs.
 */
export async function blobToBytes(data) {
  if (data instanceof Uint8Array) return data;
  if (typeof data.arrayBuffer === 'function') {
    return new Uint8Array(await data.arrayBuffer());
  }
  throw new TypeError('openrar: cannot convert value to Uint8Array');
}

async function resolveInput(data) {
  if (data instanceof Uint8Array) return data;
  if (typeof data === 'string') return new TextEncoder().encode(data);
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  if (data && typeof data.arrayBuffer === 'function') {
    return new Uint8Array(await data.arrayBuffer());
  }
  throw new TypeError('openrar: unsupported input type');
}

// ── Encoding helpers ──────────────────────────────────────────────────────

/** Lowercase hex string from bytes. */
export function hex(data) {
  const u = toUint8(data);
  let s = '';
  for (let i = 0; i < u.length; i++) {
    const b = u[i];
    s += (b < 0x10 ? '0' : '') + b.toString(16);
  }
  return s;
}

/** Standard base64 (btoa/Buffer). */
export function base64Encode(data) {
  const u = toUint8(data);
  if (typeof Buffer !== 'undefined') return Buffer.from(u).toString('base64');
  let s = '';
  for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]);
  return globalThis.btoa(s);
}

export function base64Decode(s) {
  const bin = globalThis.atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// ── OpenRAR: high-level handle ─────────────────────────────────────────────

/**
 * Async handle to the WASM block codec.
 *
 *   const rar = new OpenRAR();
 *   await rar.ready;
 *   const c = await rar.compress(data);
 *   const d = await rar.decompress(c);
 *   rar.destroy();   // optional — frees the wasm heap (all handles share it)
 */
export class OpenRAR {
  /** @type {Promise<void>} */
  ready;
  /** @type {number} ABI version reported by the loaded module. */
  apiVersion = 0;

  constructor() {
    this.ready = (async () => {
      this.#mod = await getModule();
      this.apiVersion = this.#mod._openrar_version();
    })();
  }

  /** @type {RawModule | null} */
  #mod = null;
  /** @type {boolean} */
  #destroyed = false;

  /** Underlying Emscripten module handle. Throws if not ready or destroyed. */
  get module() {
    if (this.#destroyed) throw new Error('OpenRAR destroyed');
    if (!this.#mod) throw new Error('OpenRAR not ready — await .ready first');
    return this.#mod;
  }

  /** Semver-style version string derived from apiVersion. */
  async versionString() {
    await this.ready;
    return `OpenRAR WASM ${this.apiVersion}.0`;
  }

  /**
   * Delete the Emscripten module, freeing the wasm heap. All OpenRAR
   * instances share one module, so the first destroy() tears it down for
   * every live handle and invalidates any HeapBuffer pointers — other
   * instances then fail on use. The next getModule() re-instantiates
   * fresh.
   */
  destroy() {
    if (this.#destroyed) return;
    this.#destroyed = true;
    const m = _module;
    _module = null;
    _bootPromise = null;
    if (m && typeof m.delete === 'function') {
      try { m.delete(); } catch { /* already freed */ }
    }
  }

  // ── Block codec (single in-memory buffer) ─────────────────────────────

  /**
   * Compress a single buffer using the RAR5 v0 block codec. Driven through
   * the C ABI (one memcpy in, one copy out) — no per-element marshalling.
   * Returns the block-framed payload that {@link decompress} accepts.
   * Throws on failure (bad args, OOM).
   *
   * Note: `method: STORE (0)` is a passthrough — the returned bytes are
   * the input unchanged and are NOT valid block framing; only outputs of
   * methods 1..5 are accepted by {@link decompress}.
   *
   * @param {InputData} data  string / Uint8Array / ArrayBuffer / View / Blob
   * @param {number} [method=3]  one of {@link CompressionMethod}
   * @param {number} [winSize=DEFAULT_WIN_SIZE]  dictionary window in bytes
   */
  async compress(data, method = CompressionMethod.NORMAL, winSize = DEFAULT_WIN_SIZE) {
    await this.ready;
    const m = this.module; // throws if destroyed
    assertMethod(method);
    assertWinSize(winSize);
    const src = await resolveInput(data);
    const inPtr = m._openrar_alloc(src.length);
    try {
      if (src.length > 0) m.HEAPU8.set(src, inPtr);
      const outPtrSlot = m._malloc(4);
      const outLenSlot = m._malloc(4);
      try {
        const ok = m.ccall('openrar_compress2', 'number',
          ['number', 'number', 'number', 'number', 'number', 'number'],
          [inPtr, src.length, outPtrSlot, outLenSlot, method, winSize]);
        if (ok !== 1) {
          throw new Error('openrar: compression failed (input rejected or out of memory)');
        }
        const len = m.HEAPU32[outLenSlot >>> 2];
        const outPtr = m.HEAPU32[outPtrSlot >>> 2];
        if (len === 0 || outPtr === 0) return new Uint8Array(0);
        const out = safeRead(m, outPtr, len);
        m._openrar_free(outPtr);
        return out;
      } finally {
        m._free(outPtrSlot);
        m._free(outLenSlot);
      }
    } finally {
      m._openrar_free(inPtr);
    }
  }

  /**
   * Decompress a buffer produced by {@link compress} (method 1..5).
   * Returns the original bytes. Throws on corrupt input.
   *
   * @param {InputData} data
   * @param {number} [winSize=DEFAULT_DECOMPRESS_WIN_SIZE]  window used at compress time
   */
  async decompress(data, winSize = DEFAULT_DECOMPRESS_WIN_SIZE) {
    await this.ready;
    const m = this.module; // throws if destroyed
    assertWinSize(winSize);
    const src = await resolveInput(data);
    const inPtr = m._openrar_alloc(src.length);
    try {
      if (src.length > 0) m.HEAPU8.set(src, inPtr);
      const outPtrSlot = m._malloc(4);
      const outLenSlot = m._malloc(4);
      try {
        const ok = m.ccall('openrar_decompress2', 'number',
          ['number', 'number', 'number', 'number', 'number'],
          [inPtr, src.length, outPtrSlot, outLenSlot, winSize]);
        if (ok !== 1) {
          throw new Error('openrar: decompression failed (corrupt, truncated, or unsupported input)');
        }
        const len = m.HEAPU32[outLenSlot >>> 2];
        const outPtr = m.HEAPU32[outPtrSlot >>> 2];
        if (len === 0 || outPtr === 0) return new Uint8Array(0);
        const out = safeRead(m, outPtr, len);
        m._openrar_free(outPtr);
        return out;
      } finally {
        m._free(outPtrSlot);
        m._free(outLenSlot);
      }
    } finally {
      m._openrar_free(inPtr);
    }
  }

  // ── Streaming compression ─────────────────────────────────────────────

  /**
   * Compress a WHATWG ReadableStream incrementally. The encoder keeps the
   * full RAR5 look-ahead semantics, so the concatenated output is
   * byte-identical to compress() of the same input; memory stays bounded by
   * the window + one chunk instead of the whole input.
   *
   * For the reverse direction, see {@link decompressStream} and
   * {@link decompressStreamChunks}.
   *
   * @param {ReadableStream<Uint8Array>} readable
   * @param {{method?:number, winSize?:number, signal?:AbortSignal}} [opts]
   * @returns {Promise<Uint8Array>}
   */
  async compressStream(readable, opts = {}) {
    await this.ready;
    const m = this.module; // throws if destroyed
    const method = opts.method ?? CompressionMethod.NORMAL;
    const winSize = opts.winSize ?? DEFAULT_WIN_SIZE;
    assertMethod(method);
    assertWinSize(winSize);
    const handle = m.ccall('openrar_stream_create', 'number', ['number', 'number'], [method, winSize]);
    if (!handle) throw new Error('openrar: failed to create streaming encoder');
    try {
      const reader = readable.getReader();
      for (;;) {
        if (opts.signal && opts.signal.aborted) throw new Error('openrar: aborted');
        const { done, value } = await reader.read();
        if (done) break;
        if (!value || value.length === 0) continue;
        const ptr = m._openrar_alloc(value.length);
        try {
          m.HEAPU8.set(value, ptr);
          const rc = m.ccall('openrar_stream_feed', 'number', ['number', 'number', 'number'],
            [handle, ptr, value.length]);
          if (rc !== 0) throw new Error('openrar: streaming compression failed');
        } finally {
          m._openrar_free(ptr);
        }
      }
      const outPtrSlot = m._malloc(4);
      const outLenSlot = m._malloc(4);
      try {
        const rc = m.ccall('openrar_stream_finish', 'number', ['number', 'number', 'number'],
          [handle, outPtrSlot, outLenSlot]);
        if (rc !== 0) throw new Error('openrar: streaming compression failed at finish');
        const len = m.HEAPU32[outLenSlot >>> 2];
        const outPtr = m.HEAPU32[outPtrSlot >>> 2];
        if (len === 0 || outPtr === 0) return new Uint8Array(0);
        const out = safeRead(m, outPtr, len);
        m._openrar_free(outPtr);
        return out;
      } finally {
        m._free(outPtrSlot);
        m._free(outLenSlot);
      }
    } finally {
      m.ccall('openrar_stream_free', null, ['number'], [handle]);
    }
  }

  /**
   * Compress a WHATWG ReadableStream incrementally, yielding compressed chunks
   * as blocks are completed. Memory is bounded by the window size + chunk size.
   *
   * @param {ReadableStream<Uint8Array>} readable
   * @param {{method?:number, winSize?:number, signal?:AbortSignal}} [opts]
   * @returns {AsyncGenerator<Uint8Array, void, unknown>}
   */
  async *compressStreamChunks(readable, opts = {}) {
    await this.ready;
    const m = this.module; // throws if destroyed
    const method = opts.method ?? CompressionMethod.NORMAL;
    const winSize = opts.winSize ?? DEFAULT_WIN_SIZE;
    assertMethod(method);
    assertWinSize(winSize);
    const handle = m.ccall('openrar_stream_create', 'number', ['number', 'number'], [method, winSize]);
    if (!handle) throw new Error('openrar: failed to create streaming encoder');
    try {
      const reader = readable.getReader();
      const outPtrSlot = m._malloc(4);
      const outLenSlot = m._malloc(4);
      try {
        for (;;) {
          if (opts.signal && opts.signal.aborted) throw new Error('openrar: aborted');
          const { done, value } = await reader.read();
          if (done) break;
          if (!value || value.length === 0) continue;
          const ptr = m._openrar_alloc(value.length);
          try {
            m.HEAPU8.set(value, ptr);
            const rc = m.ccall('openrar_stream_feed', 'number', ['number', 'number', 'number'],
              [handle, ptr, value.length]);
            if (rc !== 0) throw new Error('openrar: streaming compression failed');
          } finally {
            m._openrar_free(ptr);
          }
          const pullRc = m.ccall('openrar_stream_pull', 'number', ['number', 'number', 'number'],
            [handle, outPtrSlot, outLenSlot]);
          if (pullRc !== 0) throw new Error('openrar: streaming compression pull failed');
          const len = m.HEAPU32[outLenSlot >>> 2];
          const outPtr = m.HEAPU32[outPtrSlot >>> 2];
          if (len > 0 && outPtr !== 0) {
            const chunk = safeRead(m, outPtr, len);
            m._openrar_free(outPtr);
            yield chunk;
          }
        }
        const rc = m.ccall('openrar_stream_finish', 'number', ['number', 'number', 'number'],
          [handle, outPtrSlot, outLenSlot]);
        if (rc !== 0) throw new Error('openrar: streaming compression failed at finish');
        const len = m.HEAPU32[outLenSlot >>> 2];
        const outPtr = m.HEAPU32[outPtrSlot >>> 2];
        if (len > 0 && outPtr !== 0) {
          const chunk = safeRead(m, outPtr, len);
          m._openrar_free(outPtr);
          yield chunk;
        }
      } finally {
        m._free(outPtrSlot);
        m._free(outLenSlot);
      }
    } finally {
      m.ccall('openrar_stream_free', null, ['number'], [handle]);
    }
  }

  // ── Streaming decompression ───────────────────────────────────────────

  /**
   * Decompress a WHATWG ReadableStream incrementally.
   *
   * @param {ReadableStream<Uint8Array>} readable
   * @param {{winSize?:number, signal?:AbortSignal}} [opts]
   * @returns {Promise<Uint8Array>}
   */
  async decompressStream(readable, opts = {}) {
    await this.ready;
    const m = this.module; // throws if destroyed
    const winSize = opts.winSize ?? DEFAULT_DECOMPRESS_WIN_SIZE;
    assertWinSize(winSize);
    const handle = m.ccall('openrar_stream_decompress_create', 'number', ['number'], [winSize]);
    if (!handle) throw new Error('openrar: failed to create streaming decoder');
    try {
      const reader = readable.getReader();
      for (;;) {
        if (opts.signal && opts.signal.aborted) throw new Error('openrar: aborted');
        const { done, value } = await reader.read();
        if (done) break;
        if (!value || value.length === 0) continue;
        const ptr = m._openrar_alloc(value.length);
        try {
          m.HEAPU8.set(value, ptr);
          const rc = m.ccall('openrar_stream_decompress_feed', 'number', ['number', 'number', 'number'],
            [handle, ptr, value.length]);
          if (rc !== 0) throw new Error('openrar: streaming decompression failed');
        } finally {
          m._openrar_free(ptr);
        }
      }
      const outPtrSlot = m._malloc(4);
      const outLenSlot = m._malloc(4);
      try {
        const rc = m.ccall('openrar_stream_decompress_finish', 'number', ['number', 'number', 'number'],
          [handle, outPtrSlot, outLenSlot]);
        if (rc !== 0) throw new Error('openrar: streaming decompression failed at finish');
        const len = m.HEAPU32[outLenSlot >>> 2];
        const outPtr = m.HEAPU32[outPtrSlot >>> 2];
        if (len === 0 || outPtr === 0) return new Uint8Array(0);
        const out = safeRead(m, outPtr, len);
        m._openrar_free(outPtr);
        return out;
      } finally {
        m._free(outPtrSlot);
        m._free(outLenSlot);
      }
    } finally {
      m.ccall('openrar_stream_decompress_free', null, ['number'], [handle]);
    }
  }

  /**
   * Decompress a WHATWG ReadableStream incrementally, yielding uncompressed
   * chunks as they are decoded. Memory is bounded by the window size + chunk size.
   *
   * @param {ReadableStream<Uint8Array>} readable
   * @param {{winSize?:number, signal?:AbortSignal}} [opts]
   * @returns {AsyncGenerator<Uint8Array, void, unknown>}
   */
  async *decompressStreamChunks(readable, opts = {}) {
    await this.ready;
    const m = this.module;
    const winSize = opts.winSize ?? DEFAULT_DECOMPRESS_WIN_SIZE;
    assertWinSize(winSize);
    const handle = m.ccall('openrar_stream_decompress_create', 'number', ['number'], [winSize]);
    if (!handle) throw new Error('openrar: failed to create streaming decoder');
    try {
      const reader = readable.getReader();
      const outPtrSlot = m._malloc(4);
      const outLenSlot = m._malloc(4);
      try {
        for (;;) {
          if (opts.signal && opts.signal.aborted) throw new Error('openrar: aborted');
          const { done, value } = await reader.read();
          if (done) break;
          if (!value || value.length === 0) continue;
          const ptr = m._openrar_alloc(value.length);
          try {
            m.HEAPU8.set(value, ptr);
            const rc = m.ccall('openrar_stream_decompress_feed', 'number', ['number', 'number', 'number'],
              [handle, ptr, value.length]);
            if (rc !== 0) throw new Error('openrar: streaming decompression failed');
          } finally {
            m._openrar_free(ptr);
          }
          const pullRc = m.ccall('openrar_stream_decompress_pull', 'number', ['number', 'number', 'number'],
            [handle, outPtrSlot, outLenSlot]);
          if (pullRc !== 0) throw new Error('openrar: streaming decompression pull failed');
          const len = m.HEAPU32[outLenSlot >>> 2];
          const outPtr = m.HEAPU32[outPtrSlot >>> 2];
          if (len > 0 && outPtr !== 0) {
            const chunk = safeRead(m, outPtr, len);
            m._openrar_free(outPtr);
            yield chunk;
          }
        }
        const rc = m.ccall('openrar_stream_decompress_finish', 'number', ['number', 'number', 'number'],
          [handle, outPtrSlot, outLenSlot]);
        if (rc !== 0) throw new Error('openrar: streaming decompression failed at finish');
        const len = m.HEAPU32[outLenSlot >>> 2];
        const outPtr = m.HEAPU32[outPtrSlot >>> 2];
        if (len > 0 && outPtr !== 0) {
          const chunk = safeRead(m, outPtr, len);
          m._openrar_free(outPtr);
          yield chunk;
        }
      } finally {
        m._free(outPtrSlot);
        m._free(outLenSlot);
      }
    } finally {
      m.ccall('openrar_stream_decompress_free', null, ['number'], [handle]);
    }
  }

  // ── Low-level C ABI (escape hatch) ────────────────────────────────────
  //
  // Direct access for callers that already have WASM heap pointers. Use
  // safeRead() (below) when reading output buffers — the heap may grow and
  // invalidate pointers between operations.

  /** Allocates a raw buffer inside the WASM heap. Caller must {@link free} it. */
  async alloc(n) { await this.ready; return this.#mod._openrar_alloc(n >>> 0); }
  /** Frees a buffer previously returned by {@link alloc} or {@link compressHeap}. */
  async free(ptr) { await this.ready; this.#mod._openrar_free(ptr); }

  /**
   * Compress directly into a WASM heap buffer (no JS-side output copy).
   *
   * Memory contract: the returned `{ ptr, len, free }` exposes a live heap
   * allocation. Calling ANY other WASM function may invalidate it via
   * {@link https://emscripten.org/docs/porting/connecting_cpp_and_javascript/Interacting-with-code.html#aliasing}
   * (`ALLOW_MEMORY_GROWTH=1`). Use {@link safeRead} immediately and copy
   * bytes to JS before any further WASM call.
   */
  async compressHeap(src, method = CompressionMethod.NORMAL, winSize = DEFAULT_WIN_SIZE) {
    await this.ready;
    assertMethod(method);
    assertWinSize(winSize);
    const m = this.#mod;
    const u = await resolveInput(src);
    const inPtr = m._openrar_alloc(u.length);
    m.HEAPU8.set(u, inPtr);
    const outPtr = m._malloc(4);
    const outLen = m._malloc(4);
    const ok = m.ccall('openrar_compress2', 'number',
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [inPtr, u.length, outPtr, outLen, method, winSize]);
    const heapU32 = m.HEAPU32;
    const len = heapU32[outLen >>> 2];
    const ptr = heapU32[outPtr >>> 2];
    let result;
    if (ok !== 1) {
      result = null;
    } else if (len === 0 || ptr === 0) {
      // Empty input compresses to an empty output — success, not failure.
      result = { ptr: 0, len: 0, free() {} };
    } else {
      result = { ptr, len, free: () => m._openrar_free(ptr) };
    }
    m._openrar_free(inPtr);
    m._free(outPtr);
    m._free(outLen);
    return result;
  }

  /**
   * Copy bytes out of the WASM heap at the given pointer+length, returning
   * a JS Uint8Array. SAFE across heap growth (copies before returning).
   * The caller is still responsible for calling {@link free} on `ptr`.
   */
  safeRead(ptr, len) {
    return safeRead(this.module, ptr, len);
  }

  /** Base offset of HEAPU8 within its backing ArrayBuffer (usually 0). */
  get heapBase() {
    return heapU8Base(this.module);
  }

  /** Cheap bounds check: does `ptr..ptr+len` lie inside the current heap? */
  isPointerValid(ptr, len = 0) {
    return pointerInBounds(this.module, ptr, len);
  }
}

// Default export: the high-level handle.
export default OpenRAR;
