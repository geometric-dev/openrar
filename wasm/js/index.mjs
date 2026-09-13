// Thin ESM wrapper around Emscripten MODULARIZE factory.
// Re-export createOpenRAR and add convenience helpers for Uint8Array.
import createOpenRAR from '../dist/openrar.js';

export { createOpenRAR };
export default createOpenRAR;

// Convenience: one-shot compress/decompress (creates + destroys a module instance)
export async function compress(data, method = 3) {
  const m = await createOpenRAR();
  // embind VectorU8 path — accepts Uint8Array via typed_memory_view copy
  const src = data instanceof Uint8Array ? data : new Uint8Array(data);
  const out = m.compress(src, method);
  // m.compress returns Uint8Array view copy (slice) — detach before module GC
  return out instanceof Uint8Array ? out : new Uint8Array(out);
}

export async function decompress(data) {
  const m = await createOpenRAR();
  const src = data instanceof Uint8Array ? data : new Uint8Array(data);
  return m.decompress(src);
}
