// ─────────────────────────────────────────────────────────────────────────────
//  quickjs-host.mjs — QuickJS-friendly helpers for openrar-wasm
// ─────────────────────────────────────────────────────────────────────────────
//
// When the WASM module is embedded in a QuickJS runtime (qjs-wasi,
// quickjs-emscripten, or a custom C host), the C API accepts (ptr, len)
// pairs that match what JS_GetTypedArrayBuffer() returns from QuickJS.
// The wins here are JS-side:
//
//   1. Detecting the host so we can skip the JS-side copy in compressHeap().
//   2. Returning a pointer + length + free() thunk so the host C code can
//      wrap the buffer in JS_NewArrayBuffer(ctx, ptr, len, free_fn, NULL, false)
//      for true zero-copy handoff (no JS-side copy, no extra malloc).
//
// On browsers/Node, use rar.compress() / rar.compressHeap() directly — the
// embind path is already optimised there. This module is opt-in.

import { getModule } from './openrar.js';

/** Soft feature check: are we running inside a QuickJS runtime? */
export function isQuickJS() {
  if (typeof globalThis === 'undefined') return false;
  if (globalThis.scriptArgs !== undefined) return true;          // qjs / qjs-wasi
  if (globalThis.__quickjs__ === true) return true;              // quickjs-emscripten flag
  if (typeof globalThis.structuredClone !== 'function' &&
      typeof globalThis.os !== 'undefined') return true;          // older standalone qjs
  return false;
}

/**
 * Compress without JS-side intermediate copies.
 *
 * On QuickJS hosts the returned object exposes (ptr, len, free) — the host
 * C code can wrap it in JS_NewArrayBuffer with the supplied free callback.
 * On non-QuickJS hosts the function falls back to rar.compressHeap() and
 * returns { fallback: true, bytes }.
 */
export async function compressZeroCopy(rar, src, method = 3, winSize = 2 * 1024 * 1024) {
  await rar.ready;
  const m = rar.module;
  const u = (src instanceof Uint8Array) ? src : new Uint8Array(src);
  if (!isQuickJS()) {
    return { fallback: true, bytes: await rar.compressHeap(u, method, winSize) };
  }
  const ptrIn = m._openrar_alloc(u.length);
  m.HEAPU8.set(u, ptrIn);
  const outPtr = m._malloc(4);
  const outLen = m._malloc(4);
  const ok = m.ccall('openrar_compress2', 'number',
    ['number', 'number', 'number', 'number', 'number', 'number'],
    [ptrIn, u.length, outPtr, outLen, method, winSize]);
  const heap32 = m.HEAP32;
  const p = heap32[outPtr >>> 2];
  const l = heap32[outLen >>> 2];
  m._free(outPtr);
  m._free(outLen);
  m._openrar_free(ptrIn);
  if (ok !== 1) return { fallback: true, bytes: null };
  return {
    fallback: false,
    ptr: p,
    len: l,
    free: () => m._openrar_free(p),
  };
}

/** Decompress counterpart of compressZeroCopy. */
export async function decompressZeroCopy(rar, src, winSize = 1024 * 1024) {
  await rar.ready;
  const m = rar.module;
  const u = (src instanceof Uint8Array) ? src : new Uint8Array(src);
  if (!isQuickJS()) {
    // Fallback: decompress into JS then copy out — caller pays one copy.
    const bytes = await rar.decompress(u);
    return { fallback: true, bytes };
  }
  const ptrIn = m._openrar_alloc(u.length);
  m.HEAPU8.set(u, ptrIn);
  const outPtr = m._malloc(4);
  const outLen = m._malloc(4);
  const ok = m.ccall('openrar_decompress2', 'number',
    ['number', 'number', 'number', 'number', 'number'],
    [ptrIn, u.length, outPtr, outLen, winSize]);
  const heap32 = m.HEAP32;
  const p = heap32[outPtr >>> 2];
  const l = heap32[outLen >>> 2];
  m._free(outPtr);
  m._free(outLen);
  m._openrar_free(ptrIn);
  if (ok !== 1) return { fallback: true, bytes: null };
  return { fallback: false, ptr: p, len: l, free: () => m._openrar_free(p) };
}
