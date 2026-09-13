// ─────────────────────────────────────────────────────────────────────────────
//  heap_helpers.js — Shared utilities for talking to the Emscripten heap
// ─────────────────────────────────────────────────────────────────────────────
//
//  Used by:
//    - wasm/js/openrar.js              (block codec wrapper)
//    - wasm/js/openrar-archive.js      (archive API wrapper, future)
//
//  Provides:
//    - HEAPU8_BASE: the byte offset of HEAPU8 within its backing
//      ArrayBuffer (0 unless the module is mounted at a non-zero base).
//    - safeRead(ptr, len): copy bytes out of the WASM heap at (ptr, len).
//      SAFE across ALLOW_MEMORY_GROWTH=1 heap reallocations: copies the
//      bytes immediately, returns a JS Uint8Array detached from the heap.
//
//  Rules:
//    - Never call `HEAPU8.slice(ptr, ptr+len)` for buffers you intend to
//      use across multiple WASM calls — the heap may grow and invalidate.
//    - Always call `safeRead()` for output buffers that may be reused.
//    - `ptr === 0 || len === 0` returns an empty Uint8Array.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Assert every export the JS wrapper needs exists on the built module.
 * The numeric version pin cannot catch additive drift (exports added or
 * removed without a version bump) — this check can. Run once at module
 * init; a stale dist fails loudly here instead of with
 * "undefined is not a function" deep inside a call.
 *
 * @param {RawModule} m  the Emscripten module handle
 * @param {string[]} names  export names the wrapper requires
 * @param {string} hint  rebuild command shown in the error
 */
export function requireExports(m, names, hint) {
  const missing = names.filter((n) => !(n in m));
  if (missing.length > 0) {
    throw new Error(
      `openrar: stale build — module is missing [${missing.join(', ')}]. ` +
      `Rebuild the wasm module (${hint}).`,
    );
  }
}

/**
 * Copy `len` bytes out of the Emscripten heap at `ptr`.
 * Detached from the heap; safe across heap-growth reallocations.
 *
 * @param {RawModule} m  the Emscripten module handle (m.HEAPU8, m._malloc, etc.)
 * @param {number} ptr  heap pointer (may be from _openrar_alloc, _malloc, or a C return)
 * @param {number} len  byte count; must be ≤ remaining heap from `ptr`
 * @returns {Uint8Array} a fresh Uint8Array detached from the WASM heap
 */
export function safeRead(m, ptr, len) {
  if (ptr === 0 || len === 0) return new Uint8Array(0);
  if (!Number.isInteger(ptr) || !Number.isInteger(len) || len < 0) {
    throw new RangeError(`safeRead: invalid args ptr=${ptr} len=${len}`);
  }
  // `.slice()` copies the bytes (does not share storage), even if the
  // buffer is later reallocated.
  return new Uint8Array(m.HEAPU8.buffer, ptr, len).slice();
}

/**
 * Extract the byte offset of HEAPU8 within its backing ArrayBuffer.
 * Normally 0 — non-zero only if the module was instantiated with a
 * pre-allocated ArrayBuffer at a non-zero base (rare).
 */
export function heapU8Base(m) {
  return m.HEAPU8.byteOffset;
}

/**
 * Validate a pointer is in-bounds for the current heap. Cheap; use
 * before reading if `ptr` came from an untrusted C export.
 */
export function pointerInBounds(m, ptr, len) {
  if (ptr === 0 || len === 0) return true;
  const u8 = m.HEAPU8;
  return ptr >= u8.byteOffset &&
         ptr + len <= u8.byteOffset + u8.length;
}
