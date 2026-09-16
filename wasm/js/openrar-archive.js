// ─────────────────────────────────────────────────────────────────────────────
//  openrar-archive.js — OpenRAR Archive API (RAR5) interface contract
// ─────────────────────────────────────────────────────────────────────────────
//
//  Lazy-loaded archive module. Consumers who only need block codec pay
//  ~250 KiB (openrar.js). Archive API loads ~1.5 MiB on demand.
//
//  C ABI: src/wasm/archive_api.{hpp,cpp}
//  JS surface mirrors docs/wasm-archive-spec.md §2.3 + revisions A-F.
//  Distinct EXPORT_NAME=createOpenRARArchive (spec §4.4) — no collision
//  with block codec's createOpenRAR.
//
// ─────────────────────────────────────────────────────────────────────────────

import createOpenRARArchiveRaw from '../dist/openrar_archive.js';
import { safeRead, requireExports } from './heap_helpers.js';

// v1: initial.
// v2: export set corrected (_openrar_archive_list_free was missing from the
//     build), WASM_BIGINT at the boundary, progress/cancel hooks
//     (create2/extract_all2 + openrar_archive_last_error_code).
export const ARCHIVE_WASM_API_VERSION = 2;

// Exports the wrapper requires. Enforced at boot so a stale dist fails with
// a rebuild hint instead of "undefined is not a function" mid-call.
const REQUIRED_ARCHIVE_EXPORTS = [
  '_openrar_archive_version',
  '_openrar_archive_open', '_openrar_archive_close',
  '_openrar_archive_handle_list', '_openrar_archive_handle_extract',
  '_openrar_archive_handle_extract_all',
  '_openrar_archive_list', '_openrar_archive_list_free',
  '_openrar_archive_extract', '_openrar_archive_extract_all',
  '_openrar_archive_create',
  '_openrar_archive_get_error',
  '_openrar_archive_alloc', '_openrar_archive_free',
  '_malloc', '_free', 'ccall', 'addFunction', 'removeFunction',
  'UTF8ToString', 'HEAPU8', 'HEAPU32',
];

let _bootPromise = null;
let _module = null;

export async function getArchiveModule() {
  if (_module) return _module;
  if (!_bootPromise) {
    _bootPromise = createOpenRARArchiveRaw().then((m) => {
      requireExports(m, REQUIRED_ARCHIVE_EXPORTS,
        'emcmake cmake --preset wasm-archive && cmake --build --preset wasm-archive');
      const v = m._openrar_archive_version();
      if (v !== ARCHIVE_WASM_API_VERSION) {
        throw new Error(
          `openrar-archive: ABI mismatch (host expects v${ARCHIVE_WASM_API_VERSION}, module is v${v}). ` +
          `Run "emcmake cmake --preset wasm-archive" to rebuild.`,
        );
      }
      _module = m;
      return m;
    });
  }
  return _bootPromise;
}

// ── Error model ────────────────────────────────────────────────────────────

export const RarErrorCode = Object.freeze({
  NOT_RAR: 'NOT_RAR',
  UNSUPPORTED_FEATURE: 'UNSUPPORTED_FEATURE',
  TRUNCATED: 'TRUNCATED',
  CRC_MISMATCH: 'CRC_MISMATCH',
  BAD_PASSWORD: 'BAD_PASSWORD',
  IO: 'IO',
  NOMEM: 'NOMEM',
  ABORTED: 'ABORTED',
  INVALID_ARG: 'INVALID_ARG',
  // Contract parity with src/archive/buffer_archive.hpp: returned only by the
  // native DLL _ex listing exports; the wasm surface never emits it (yet).
  ENCRYPTED: 'ENCRYPTED',
  // Contract parity with src/archive/rar_errors.hpp (-13/-14): a volume of a
  // multi-volume set is absent, or the handle/worker is busy with another op.
  MISSING_VOLUME: 'MISSING_VOLUME',
  BUSY: 'BUSY',
});

const CODE_MAP = Object.freeze({
  [-1]: 'NOT_RAR',
  [-2]: 'UNSUPPORTED_FEATURE',
  [-3]: 'TRUNCATED',
  [-4]: 'CRC_MISMATCH',
  [-5]: 'NOMEM',
  [-6]: 'IO',
  [-7]: 'BAD_PASSWORD',
  [-9]: 'INVALID_ARG',
  [-11]: 'ABORTED',
  [-12]: 'ENCRYPTED',
  [-13]: 'MISSING_VOLUME',
  [-14]: 'BUSY',
});

export class RarError extends Error {
  constructor(code, detail, numericCode) {
    super(detail ? `${code}: ${detail}` : code);
    this.code = code;
    this.detail = detail;
    /** @type {number|undefined} the raw RarError C value, when known */
    this.numericCode = numericCode;
    this.name = 'RarError';
  }
  static fromCode(num, detail) {
    const code = CODE_MAP[num] || 'IO';
    return new RarError(code, detail, num);
  }
}

function throwIfError(rc, m) {
  if (rc === 0) return;
  const detail = getLastError(m);
  throw RarError.fromCode(rc, detail || undefined);
}

function getLastError(m) {
  const buf = m._malloc(512);
  const n = m.ccall('openrar_archive_get_error', 'number', ['number','number'], [buf, 512]);
  let s = '';
  if (n > 0) {
    s = m.UTF8ToString(buf, 512);
  }
  m._free(buf);
  return s;
}

// ── Helpers ────────────────────────────────────────────────────────────────

function toUint8(data) {
  if (data instanceof Uint8Array) return data;
  if (typeof data === 'string') return new TextEncoder().encode(data);
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  throw new TypeError('openrar-archive: expected Uint8Array/string/ArrayBuffer/View');
}

function validateMethod(method) {
  if (method !== 0 && method !== 3 && method !== 5) {
    throw new RarError('INVALID_ARG', `method must be 0, 3, or 5 (got ${method})`);
  }
}

function validateWindowLog2(w) {
  if (w < 1 || w > 4) throw new RarError('INVALID_ARG', `windowLog2 must be in [1,4] (got ${w})`);
}

// Default decompression-bomb guard for extractAll: refuse archives whose
// claimed uncompressed total exceeds this (0 = unlimited).
export const DEFAULT_MAX_OUTPUT_BYTES = 512 * 1024 * 1024;

// ── progress/cancel hooks (v2) ──────────────────────────────────────────────
// The C side calls back through function pointers registered with
// addFunction. The progress signature is (void* user, uint64_t done,
// uint64_t total) — with WASM_BIGINT=1 the u64 params arrive as BigInt.
function wireHooks(m, opts) {
  if (!opts || (!opts.onProgress && !opts.signal)) return null;
  const state = { fns: [], hooksPtr: 0 };
  if (opts.signal) {
    if (opts.signal.aborted) throw new RarError('ABORTED', 'aborted before start', -11);
    let cancelled = false;
    state.onAbort = () => { cancelled = true; };
    state.signal = opts.signal;
    opts.signal.addEventListener('abort', state.onAbort, { once: true });
    state.cancelFn = m.addFunction(() => (cancelled ? 1 : 0), 'ii');
    state.fns.push(state.cancelFn);
  }
  if (opts.onProgress) {
    state.progressFn = m.addFunction((done, total, _user) => {
      // A throw inside a wasm callback becomes a trap — user callback
      // errors are swallowed (documented).
      try { opts.onProgress(Number(done), Number(total)); } catch { /* non-fatal */ }
    }, 'vijj');
    state.fns.push(state.progressFn);
  }
  return state;
}

function attachHooks(m, state) {
  if (!state) return 0;
  // ArchiveHooks layout: progress(4) progress_user(4) cancel(4) cancel_user(4)
  const ptr = m._malloc(16);
  state.hooksPtr = ptr;
  m.HEAPU32[ptr >> 2] = state.progressFn || 0;
  m.HEAPU32[(ptr >> 2) + 1] = 0;
  m.HEAPU32[(ptr >> 2) + 2] = state.cancelFn || 0;
  m.HEAPU32[(ptr >> 2) + 3] = 0;
  return ptr;
}

function unwireHooks(m, state) {
  if (!state) return;
  if (state.signal && state.onAbort) state.signal.removeEventListener('abort', state.onAbort);
  for (const f of state.fns) { try { m.removeFunction(f); } catch { /* table slot gone */ } }
  if (state.hooksPtr) { try { m._free(state.hooksPtr); } catch {} }
}

// ── Path validation (mirrors C++ validate_archive_path) ──────────────────

export function validateArchivePath(path) {
  if (typeof path !== 'string') return { ok: false, reason: 'path must be string' };
  if (path.length === 0) return { ok: false, reason: 'path is empty' };
  const bytes = new TextEncoder().encode(path);
  if (bytes.length > 2048) return { ok: false, reason: 'path exceeds 2048 bytes' };
  if (path[0] === '/') return { ok: false, reason: "path has leading '/'" };
  if (path.includes('\\')) return { ok: false, reason: 'path contains backslash' };
  if (path.includes('\0')) return { ok: false, reason: 'path contains NUL byte' };
  for (let i=0;i<path.length;i++) { const c=path.charCodeAt(i); if (c < 0x20) return { ok:false, reason:'path contains control character'}; }
  const segs = path.split('/');
  for (const s of segs) if (s === '..') return { ok:false, reason:"path contains '..' segment" };
  return { ok: true };
}

// ── ArchiveEntryOut layout (must match C++ struct) ────────────────────────
// ArchiveEntryOut is 64 bytes (see archive_api.hpp static_assert):
// offset 0: path_offset u32
// offset 4: path_len u32
// offset 8: is_dir u32
// offset 12: method u32
// offset 16: is_encrypted u32
// offset 20: crc32 u32
// offset 24: size u64
// offset 32: packed_size u64
// offset 40: mtime u64
// offset 48: _pad[0] u64
// offset 56: _pad[1] u64
const ENTRY_SIZE = 64;

function parseEntries(m, entriesPtr, count, pathsPtr) {
  const heapU8 = m.HEAPU8;
  // DataView over the same window HEAPU8 covers, so heap-relative pointer
  // arithmetic matches view offsets even if HEAPU8 is mounted at a base.
  const view = new DataView(heapU8.buffer, heapU8.byteOffset, heapU8.byteLength);
  const base = entriesPtr;
  const entries = [];
  for (let i=0;i<count;i++) {
    const off = base + i * ENTRY_SIZE;
    const path_offset = view.getUint32(off + 0, true);
    const path_len = view.getUint32(off + 4, true);
    const is_dir = view.getUint32(off + 8, true) !== 0;
    const method = view.getUint32(off + 12, true);
    const is_encrypted = view.getUint32(off + 16, true) !== 0;
    const crc32 = view.getUint32(off + 20, true);
    const size = Number(view.getBigUint64(off + 24, true));
    const packed_size = Number(view.getBigUint64(off + 32, true));
    const mtime = Number(view.getBigUint64(off + 40, true));
    const pathBytes = heapU8.subarray(pathsPtr + path_offset, pathsPtr + path_offset + path_len);
    const path = new TextDecoder().decode(pathBytes.slice());
    entries.push({ path, isDir: is_dir, size, packedSize: packed_size, mtime, crc32, method, isEncrypted: is_encrypted, index: i });
  }
  return entries;
}

// ── High-level API ────────────────────────────────────────────────────────

/**
 * Create a RAR5 archive from in-memory files (C ABI v2: create2).
 * @param {Array<{path:string,data:Uint8Array|string,mtime?:number}>} files
 *   mtime is UNIX seconds; omitted/0 means "now".
 * @param {{method?:0|3|5, windowLog2?:1|2|3|4,
 *          onProgress?:(done:number,total:number)=>void, signal?:AbortSignal}} opts
 *   windowLog2 maps to the dictionary window: 1=128 KiB, 2=256 KiB,
 *   3=512 KiB, 4=1 MiB (default 4).
 * @returns {Promise<Uint8Array>}
 */
export async function createArchive(files, opts = {}) {
  const m = await getArchiveModule();
  const method = opts.method ?? 3;
  validateMethod(method);
  const windowLog2 = opts.windowLog2 ?? 4;
  validateWindowLog2(windowLog2);
  if (!Array.isArray(files) || files.length === 0) throw new RarError('INVALID_ARG', 'files must be non-empty array');

  const fileCount = files.length;
  // Single ownership: every heap allocation is freed exactly once in the
  // `finally` block — never inline on error paths (a double free corrupts
  // the shared singleton heap for all later calls).
  let filesArrPtr = 0, optsPtr = 0, outPtr = 0, outLenPtr = 0, outHeapPtr = 0;
  let hooksPtr = 0;
  const allocatedPtrs = [];
  const hooks = wireHooks(m, opts);
  try {
    // ArchiveInputFile (32 bytes each — layout in archive_api.hpp):
    //   0 path, 4 data, 8 data_len, 12 pad, 16 mtime(u64), 24 is_dir, 25.. reserved
    filesArrPtr = m._malloc(fileCount * 32);
    const view = new DataView(m.HEAPU8.buffer, m.HEAPU8.byteOffset, m.HEAPU8.byteLength);
    for (let i = 0; i < fileCount; i++) {
      const f = files[i];
      const vp = validateArchivePath(f.path);
      if (!vp.ok) throw new RarError('INVALID_ARG', `path ${f.path}: ${vp.reason}`);
      const isDir = f.path.endsWith('/');
      const pathBytes = new TextEncoder().encode(f.path + '\0');
      const pPtr = m._malloc(pathBytes.length);
      m.HEAPU8.set(pathBytes, pPtr);
      allocatedPtrs.push(pPtr);

      const dataBytes = isDir ? new Uint8Array(0) : toUint8(f.data ?? '');
      let dPtr = 0;
      if (dataBytes.length > 0) {
        dPtr = m._malloc(dataBytes.length);
        m.HEAPU8.set(dataBytes, dPtr);
        allocatedPtrs.push(dPtr);
      }
      const base = filesArrPtr + i * 32;
      view.setUint32(base + 0, pPtr, true);
      view.setUint32(base + 4, dPtr, true);
      view.setUint32(base + 8, dataBytes.length, true);
      view.setBigUint64(base + 16, BigInt(Math.trunc(f.mtime ?? 0)), true);
      view.setUint8(base + 24, isDir ? 1 : 0);
      // reserved bytes stay zero
    }

    optsPtr = m._malloc(8);
    m.HEAPU32[optsPtr >> 2] = method;
    m.HEAPU32[(optsPtr >> 2) + 1] = windowLog2;
    hooksPtr = attachHooks(m, hooks);

    outPtr = m._malloc(4);
    outLenPtr = m._malloc(4);
    const rc = m.ccall('openrar_archive_create2', 'number',
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [filesArrPtr, fileCount, optsPtr, hooksPtr, outPtr, outLenPtr]);
    if (rc !== 0) throwIfError(rc, m);

    outHeapPtr = m.HEAPU32[outPtr >> 2];
    const outLen = m.HEAPU32[outLenPtr >> 2];
    const result = safeRead(m, outHeapPtr, outLen);
    m._openrar_archive_free(outHeapPtr);
    outHeapPtr = 0;
    return result;
  } finally {
    unwireHooks(m, hooks);
    for (const p of allocatedPtrs) { try { m._free(p); } catch {} }
    for (const p of [filesArrPtr, optsPtr, hooksPtr, outPtr, outLenPtr]) {
      if (p) { try { m._free(p); } catch {} }
    }
    if (outHeapPtr) { try { m._openrar_archive_free(outHeapPtr); } catch {} }
  }
}

/**
 * List entries in a RAR archive.
 * @param {Uint8Array} rarBytes
 * @returns {Promise<Array<{path:string,isDir:boolean,size:number,packedSize:number,mtime:number,crc32:number,method:number,isEncrypted:boolean,index:number}>>}
 */
export async function listArchive(rar) {
  const m = await getArchiveModule();
  const rarU8 = toUint8(rar);
  const rarPtr = m._malloc(rarU8.length);
  m.HEAPU8.set(rarU8, rarPtr);
  const countPtr = m._malloc(4);
  const entriesPtrPtr = m._malloc(4);
  const pathsPtrPtr = m._malloc(4);
  const pathsSizePtr = m._malloc(4);
  const rc = m.ccall('openrar_archive_list', 'number',
    ['number','number','number','number','number','number'],
    [rarPtr, rarU8.length, countPtr, entriesPtrPtr, pathsPtrPtr, pathsSizePtr]);
  const count = m.HEAPU32[countPtr>>2];
  const entriesPtr = m.HEAPU32[entriesPtrPtr>>2];
  const pathsPtr = m.HEAPU32[pathsPtrPtr>>2];
  const pathsSize = m.HEAPU32[pathsSizePtr>>2];
  m._free(rarPtr); m._free(countPtr); m._free(entriesPtrPtr); m._free(pathsPtrPtr); m._free(pathsSizePtr);
  if (rc !== 0) {
    if (entriesPtr) m._openrar_archive_free(entriesPtr);
    if (pathsPtr) m._openrar_archive_free(pathsPtr);
    throwIfError(rc, m);
  }
  const entries = parseEntries(m, entriesPtr, count, pathsPtr);
  m.ccall('openrar_archive_list_free', null, ['number','number','number'], [entriesPtr, pathsPtr, pathsSize]);
  return entries;
}

/**
 * Extract a single file by path.
 */
export async function extractFile(rar, path) {
  const m = await getArchiveModule();
  const entries = await listArchive(rar);
  const idx = entries.findIndex(e => e.path === path);
  if (idx === -1) throw new RarError('INVALID_ARG', `entry not found: ${path}`);
  return extractFileByIndex(rar, idx);
}

export async function extractFileByIndex(rar, index) {
  const m = await getArchiveModule();
  const rarU8 = toUint8(rar);
  const rarPtr = m._malloc(rarU8.length);
  m.HEAPU8.set(rarU8, rarPtr);
  const outPtrPtr = m._malloc(4);
  const outLenPtr = m._malloc(4);
  const rc = m.ccall('openrar_archive_extract', 'number',
    ['number','number','number','number','number'],
    [rarPtr, rarU8.length, index, outPtrPtr, outLenPtr]);
  const outPtr = m.HEAPU32[outPtrPtr>>2];
  const outLen = m.HEAPU32[outLenPtr>>2];
  m._free(rarPtr); m._free(outPtrPtr); m._free(outLenPtr);
  if (rc !== 0) {
    if (outPtr) m._openrar_archive_free(outPtr);
    throwIfError(rc, m);
  }
  const result = safeRead(m, outPtr, outLen);
  m._openrar_archive_free(outPtr);
  return result;
}

/**
 * Extract all files. Returns Map path->bytes.
 *
 * Default path extracts per entry through an archive handle: peak memory is
 * the largest entry + the results map (no concatenated heap buffer), with
 * progress and abort checks between entries. `opts.bulk` uses the one-shot
 * C extract_all instead — one scan, but ~2× the archive's uncompressed size
 * in memory.
 *
 * @param {Uint8Array} rar
 * @param {{onProgress?:(done:number,total:number)=>void, signal?:AbortSignal,
 *          bulk?:boolean, maxOutputBytes?:number}} opts
 */
export async function extractAll(rar, opts = {}) {
  const h = await openArchive(rar);
  try {
    return await h.extractAll(opts);
  } finally {
    h.close();
  }
}

// ── Handle API (openArchive) — avoids double scan ─────────────────────

export class OpenRARArchive {
  constructor(handle, entries, rarBytes) {
    this.handle = handle;
    this.entries = entries;
    this.rarBytes = rarBytes;
    this._closed = false;
  }
  list() {
    if (this._closed) throw new RarError('INVALID_ARG', 'archive closed');
    return this.entries;
  }
  async extract(path) {
    if (this._closed) throw new RarError('INVALID_ARG', 'archive closed');
    const idx = this.entries.findIndex(e => e.path === path);
    if (idx === -1) throw new RarError('INVALID_ARG', `entry not found: ${path}`);
    return this.extractByIndex(idx);
  }
  async extractByIndex(index) {
    if (this._closed) throw new RarError('INVALID_ARG', 'archive closed');
    const m = await getArchiveModule();
    const outPtrPtr = m._malloc(4);
    const outLenPtr = m._malloc(4);
    const rc = m.ccall('openrar_archive_handle_extract', 'number',
      ['number','number','number','number'], [this.handle, index, outPtrPtr, outLenPtr]);
    const outPtr = m.HEAPU32[outPtrPtr>>2];
    const outLen = m.HEAPU32[outLenPtr>>2];
    m._free(outPtrPtr); m._free(outLenPtr);
    if (rc !== 0) { if (outPtr) m._openrar_archive_free(outPtr); throwIfError(rc, m); }
    const result = safeRead(m, outPtr, outLen);
    m._openrar_archive_free(outPtr);
    return result;
  }
  async extractAll(opts = {}) {
    if (this._closed) throw new RarError('INVALID_ARG', 'archive closed');
    const m = await getArchiveModule();
    const maxOut = opts.maxOutputBytes ?? DEFAULT_MAX_OUTPUT_BYTES;

    if (opts.bulk) {
      // One-shot: single scan, single concatenated buffer (~2× payload peak).
      const hooks = wireHooks(m, opts);
      let hooksPtr = 0;
      const bufPtrPtr = m._malloc(4);
      const bufSizePtr = m._malloc(4);
      const offsetsPtrPtr = m._malloc(4);
      const countPtr = m._malloc(4);
      try {
        hooksPtr = attachHooks(m, hooks);
        const rc = m.ccall('openrar_archive_handle_extract_all2', 'number',
          ['number', 'number', 'number', 'number', 'number', 'number'],
          [this.handle, hooksPtr, bufPtrPtr, bufSizePtr, offsetsPtrPtr, countPtr]);
        const bufPtr = m.HEAPU32[bufPtrPtr >> 2];
        const bufSize = m.HEAPU32[bufSizePtr >> 2];
        const offsetsPtr = m.HEAPU32[offsetsPtrPtr >> 2];
        const count = m.HEAPU32[countPtr >> 2];
        if (rc !== 0) {
          if (bufPtr) m._openrar_archive_free(bufPtr);
          if (offsetsPtr) m._openrar_archive_free(offsetsPtr);
          throwIfError(rc, m);
        }
        const buf = safeRead(m, bufPtr, bufSize);
        const view = new DataView(m.HEAPU8.buffer, m.HEAPU8.byteOffset, m.HEAPU8.byteLength);
        const result = new Map();
        for (let i = 0; i < count; i++) {
          const off = Number(view.getBigUint64(offsetsPtr + i * 16, true));
          const sz = Number(view.getBigUint64(offsetsPtr + i * 16 + 8, true));
          result.set(this.entries[i].path, buf.subarray(off, off + sz).slice());
        }
        m._openrar_archive_free(bufPtr);
        m._openrar_archive_free(offsetsPtr);
        return result;
      } finally {
        unwireHooks(m, hooks);
        m._free(bufPtrPtr); m._free(bufSizePtr); m._free(offsetsPtrPtr); m._free(countPtr);
      }
    }

    // Default: per-entry extraction. Peak memory is the largest entry plus
    // the results map; progress and abort land between entries.
    let totalBytes = 0;
    for (const e of this.entries) totalBytes += e.size;
    if (maxOut > 0 && totalBytes > maxOut) {
      throw new RarError('NOMEM',
        `archive claims ${totalBytes} uncompressed bytes; exceeds maxOutputBytes ${maxOut} ` +
        `(pass a higher maxOutputBytes or bulk:true if this is expected)`, -5);
    }
    const result = new Map();
    let doneBytes = 0;
    if (opts.onProgress) opts.onProgress(0, totalBytes);
    for (let i = 0; i < this.entries.length; i++) {
      if (opts.signal && opts.signal.aborted) {
        throw new RarError('ABORTED', `aborted after ${i}/${this.entries.length} entries`, -11);
      }
      const bytes = await this.extractByIndex(i);
      result.set(this.entries[i].path, bytes);
      doneBytes += this.entries[i].size;
      if (opts.onProgress) opts.onProgress(doneBytes, totalBytes);
    }
    return result;
  }
  close() {
    if (this._closed) return;
    this._closed = true;
    getArchiveModule().then(m => {
      try { m.ccall('openrar_archive_close', null, ['number'], [this.handle]); } catch {}
    });
    this.entries = null;
  }
  get pathCount() { return this.entries ? this.entries.length : 0; }
}

export async function openArchive(rar) {
  const m = await getArchiveModule();
  const rarU8 = toUint8(rar);
  const rarPtr = m._malloc(rarU8.length);
  m.HEAPU8.set(rarU8, rarPtr);
  const handle = m.ccall('openrar_archive_open', 'number', ['number','number'], [rarPtr, rarU8.length]);
  m._free(rarPtr);
  if (handle === 0) {
    // The C side records the real code — don't guess NOT_RAR.
    const code = m._openrar_archive_last_error_code();
    const detail = getLastError(m);
    throw RarError.fromCode(code || -1, detail || 'NOT_RAR');
  }
  // Fetch entries via handle
  const countPtr = m._malloc(4);
  const entriesPtrPtr = m._malloc(4);
  const pathsPtrPtr = m._malloc(4);
  const pathsSizePtr = m._malloc(4);
  const rc = m.ccall('openrar_archive_handle_list', 'number',
    ['number','number','number','number','number'], [handle, countPtr, entriesPtrPtr, pathsPtrPtr, pathsSizePtr]);
  const count = m.HEAPU32[countPtr>>2];
  const entriesPtr = m.HEAPU32[entriesPtrPtr>>2];
  const pathsPtr = m.HEAPU32[pathsPtrPtr>>2];
  m._free(countPtr); m._free(entriesPtrPtr); m._free(pathsPtrPtr); m._free(pathsSizePtr);
  if (rc !== 0) {
    m.ccall('openrar_archive_close', null, ['number'], [handle]);
    throwIfError(rc, m);
  }
  const entries = parseEntries(m, entriesPtr, count, pathsPtr);
  m.ccall('openrar_archive_list_free', null, ['number','number','number'], [entriesPtr, pathsPtr, 0]);
  return new OpenRARArchive(handle, entries, rarU8);
}

export function destroy() {
  const m = _module;
  _module = null;
  _bootPromise = null;
  // Delete the module to actually free the wasm heap. Any live
  // OpenRARArchive handles become invalid; the next getArchiveModule()
  // re-instantiates fresh.
  if (m && typeof m.delete === 'function') {
    try { m.delete(); } catch { /* already freed */ }
  }
}

// ── Fetch helper (browser) ──────────────────────────────────────────────────

/**
 * Download a .rar archive with progress and open it as a handle.
 *
 * Streams the response body so callers get download progress; the full
 * archive is materialised in memory (the wasm32 heap ceiling applies —
 * archives beyond ~1 GiB should use a server-side extraction instead).
 *
 * @param {string|URL|Request} url
 * @param {{onProgress?:(received:number,total:number)=>void,
 *          signal?:AbortSignal, maxArchiveBytes?:number}} [opts]
 * @returns {Promise<OpenRARArchive>}
 */
export async function openrarFromFetch(url, opts = {}) {
  const resp = await fetch(url, { signal: opts.signal });
  if (!resp.ok) throw new RarError('IO', `fetch failed: HTTP ${resp.status}`, -6);
  const total = Number(resp.headers.get('content-length') || 0);

  if (!resp.body) {
    // Older engines without streaming: one-shot.
    const buf = new Uint8Array(await resp.arrayBuffer());
    if (opts.onProgress) opts.onProgress(buf.length, buf.length);
    return openArchive(buf);
  }

  const reader = resp.body.getReader();
  const chunks = [];
  let received = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      received += value.length;
      if (opts.maxArchiveBytes && received > opts.maxArchiveBytes) {
        await reader.cancel();
        throw new RarError('NOMEM',
          `archive exceeds maxArchiveBytes (${opts.maxArchiveBytes})`, -5);
      }
      if (opts.onProgress) opts.onProgress(received, total);
    }
  } finally {
    reader.releaseLock?.();
  }
  const rar = new Uint8Array(received);
  let off = 0;
  for (const c of chunks) { rar.set(c, off); off += c.length; }
  if (opts.onProgress) opts.onProgress(received, received);
  return openArchive(rar);
}
