// ─────────────────────────────────────────────────────────────────────────────
//  worker.js — module worker that runs all OpenRAR work off the main thread.
//
//  Usage (main thread):
//    const wr = createOpenRARWorker();            // from worker-host.mjs
//    const c = await wr.compress(bytes);
//    wr.terminate();
//
//  Protocol: { id, op, args } in; { id, ok, result } | { id, ok, error } out;
//  optional { id, type: 'progress', done, total } events during extractAll.
//  Binary results are transferred (zero-copy) to the main thread; inputs are
//  structured-cloned (copied) unless the host passes { transfer: true }.
//
//  This file must run as `new Worker(new URL('./worker.js', import.meta.url),
//  { type: 'module' })` — never import it directly from the main thread.
// ─────────────────────────────────────────────────────────────────────────────

import { OpenRAR } from './openrar.js';
import * as archive from './openrar-archive.js';

let blockHandle = null;
async function getBlock() {
  if (!blockHandle) {
    blockHandle = new OpenRAR();
    await blockHandle.ready;
  }
  return blockHandle;
}

self.onmessage = async (ev) => {
  const { id, op, args } = ev.data || {};
  if (typeof id !== 'number' || typeof op !== 'string') return;
  const onProgress = args && args.onProgress
    ? (done, total) => self.postMessage({ id, type: 'progress', done, total })
    : undefined;
  try {
    let result;
    let transfer = [];
    switch (op) {
      case 'compress': {
        result = await (await getBlock()).compress(args.data, args.method, args.winSize);
        transfer = [result.buffer];
        break;
      }
      case 'decompress': {
        result = await (await getBlock()).decompress(args.data, args.winSize);
        transfer = [result.buffer];
        break;
      }
      case 'createArchive': {
        result = await archive.createArchive(args.files, { ...args.opts, onProgress });
        transfer = [result.buffer];
        break;
      }
      case 'listArchive': {
        result = await archive.listArchive(args.rar);
        break;
      }
      case 'extractFile': {
        result = await archive.extractFile(args.rar, args.path);
        transfer = [result.buffer];
        break;
      }
      case 'extractFileByIndex': {
        result = await archive.extractFileByIndex(args.rar, args.index);
        transfer = [result.buffer];
        break;
      }
      case 'extractAll': {
        const map = await archive.extractAll(args.rar, { ...args.opts, onProgress });
        // Maps are structured-cloned as plain objects; keep it explicit:
        // send entries as [path, bytes] pairs and transfer every buffer.
        result = { entries: [...map.entries()] };
        transfer = result.entries.map(([, bytes]) => bytes.buffer);
        break;
      }
      case 'destroy': {
        if (blockHandle) blockHandle.destroy();
        blockHandle = null;
        result = null;
        break;
      }
      default:
        throw new Error(`openrar-worker: unknown op "${op}"`);
    }
    self.postMessage({ id, ok: true, result }, transfer);
  } catch (e) {
    self.postMessage({
      id,
      ok: false,
      error: {
        code: e.code || 'IO',
        message: String(e.message || e),
        numericCode: e.numericCode,
        name: e.name || 'Error',
      },
    });
  }
};
