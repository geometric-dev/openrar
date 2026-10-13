// Thin ESM wrapper over the OpenRAR WASM wrapper.
//
// Convenience one-shot helpers on top of the OpenRAR class: each call
// instantiates the wrapper, does one job, and tears the instance down again.
//
// HISTORY / WHY THIS FILE WAS REWRITTEN: the previous version drove the raw
// Emscripten module directly and called `m.compress(src, method)` /
// `m.decompress(src)`. Those are embind bindings, and embind was REMOVED in
// WASM_API_VERSION 2 (see src/wasm/wasm_api.cpp: there is no embind layer,
// because embind's register_vector<uint8_t> cannot accept a typed array).
// Both helpers therefore threw `TypeError: m.compress is not a function` on
// every call. They were never noticed because this file is not in
// package.json "exports", so it was dead in the published package.
//
// The module factory itself is re-exported unchanged: `createOpenRAR` is the
// MODULARIZE factory from wasm/js/dist/openrar.js and still works as before.
import OpenRAR, { createOpenRAR, CompressionMethod } from './openrar.js';

export { createOpenRAR, OpenRAR, CompressionMethod };
export default OpenRAR;

/**
 * One-shot compress. Creates a wrapper, compresses, destroys the instance.
 * For repeated calls, construct `new OpenRAR()` once and reuse it - the
 * underlying wasm module is a shared singleton either way, but this path
 * pays the wrapper's per-instance teardown on every call.
 *
 * @param {Uint8Array|ArrayBuffer|ArrayBufferView} data
 * @param {number} [method] CompressionMethod member; default NORMAL (3).
 * @returns {Promise<Uint8Array>}
 */
export async function compress(data, method = CompressionMethod.NORMAL) {
  const rar = new OpenRAR();
  try {
    await rar.ready;
    return await rar.compress(data, method);
  } finally {
    rar.destroy();
  }
}

/**
 * One-shot decompress. Mirrors {@link compress}.
 *
 * @param {Uint8Array|ArrayBuffer|ArrayBufferView} data
 * @returns {Promise<Uint8Array>}
 */
export async function decompress(data) {
  const rar = new OpenRAR();
  try {
    await rar.ready;
    return await rar.decompress(data);
  } finally {
    rar.destroy();
  }
}