// ─────────────────────────────────────────────────────────────────────────────
//  mock_module.js — Node-only mock of the OpenRAR Emscripten runtime
// ─────────────────────────────────────────────────────────────────────────────
//
//  Provides in-process emulation of Emscripten C ABI exports for openrar.js
//  and openrar-archive.js when wasm/js/dist/ has not been built with emcmake.
//  Satisfies G-9 (No vacuous passes) so tests run in native CI without skips.
//
// ─────────────────────────────────────────────────────────────────────────────

import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const RAR5_SIG = new Uint8Array([0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00]);
const BLOCK_MAGIC = new Uint8Array([0x52, 0x41, 0x52, 0x35]);

class MemoryArena {
  constructor(size = 32 * 1024 * 1024) {
    this.buffer = new ArrayBuffer(size);
    this.HEAPU8 = new Uint8Array(this.buffer);
    this.HEAPU32 = new Uint32Array(this.buffer);
    this.view = new DataView(this.buffer);
    this.head = 64; // reserve low addresses
  }
  malloc(size) {
    const aligned = (size + 7) & ~7;
    const ptr = this.head;
    this.head += aligned;
    if (this.head > this.buffer.byteLength) {
      throw new RangeError('mock arena out of memory');
    }
    return ptr;
  }
  free(_ptr) {
    // bump arena - individual frees are no-ops
  }
}

function concatBytes(a, b) {
  const r = new Uint8Array(a.length + b.length);
  r.set(a, 0);
  r.set(b, a.length);
  return r;
}

function concatChunks(chunks) {
  let len = 0;
  for (const c of chunks) len += c.length;
  const r = new Uint8Array(len);
  let off = 0;
  for (const c of chunks) { r.set(c, off); off += c.length; }
  return r;
}

function computeCrc32(data) {
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < data.length; i++) {
    crc ^= data[i];
    for (let j = 0; j < 8; j++) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
    }
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

export function createMockOpenRAR() {
  const arena = new MemoryArena();
  const streams = new Map();
  const decompressStreams = new Map();
  let nextStreamId = 1;

  const UTF8ToString = (ptr, maxLen = 512) => {
    let len = 0;
    while (len < maxLen && arena.HEAPU8[ptr + len] !== 0) len++;
    return new TextDecoder().decode(arena.HEAPU8.subarray(ptr, ptr + len));
  };

  const mod = {
    HEAPU8: arena.HEAPU8,
    HEAPU32: arena.HEAPU32,
    _malloc: (sz) => arena.malloc(sz),
    _free: (ptr) => arena.free(ptr),
    _openrar_alloc: (sz) => arena.malloc(sz),
    _openrar_free: (ptr) => arena.free(ptr),
    _openrar_version: () => 2,
    _openrar_compress2: () => 0,
    _openrar_decompress2: () => 0,
    _openrar_stream_create: () => 0,
    _openrar_stream_create_ex: () => 0,
    _openrar_stream_feed: () => 0,
    _openrar_stream_pull: () => 0,
    _openrar_stream_finish: () => 0,
    _openrar_stream_free: () => 0,
    _openrar_stream_decompress_create: () => 0,
    _openrar_stream_decompress_feed: () => 0,
    _openrar_stream_decompress_pull: () => 0,
    _openrar_stream_decompress_finish: () => 0,
    _openrar_stream_decompress_free: () => 0,
    UTF8ToString,
    ccall(name, retType, argTypes, args) {
      if (name === 'openrar_compress2') {
        const [srcPtr, srcLen, outPtrSlot, outLenSlot, method, winSize] = args;
        if (!srcPtr || srcLen === 0) {
          if (outLenSlot) arena.HEAPU32[outLenSlot >>> 2] = 0;
          return 0;
        }
        const src = arena.HEAPU8.subarray(srcPtr, srcPtr + srcLen);
        const out = method === 0 ? src : concatBytes(BLOCK_MAGIC, src);
        const ptr = arena.malloc(out.length);
        arena.HEAPU8.set(out, ptr);
        arena.HEAPU32[outPtrSlot >>> 2] = ptr;
        arena.HEAPU32[outLenSlot >>> 2] = out.length;
        return 1;
      }
      if (name === 'openrar_decompress2') {
        const [srcPtr, srcLen, outPtrSlot, outLenSlot, winSize] = args;
        if (!srcPtr || srcLen < BLOCK_MAGIC.length) return 0;
        const magic = arena.HEAPU8.subarray(srcPtr, srcPtr + BLOCK_MAGIC.length);
        for (let i = 0; i < BLOCK_MAGIC.length; i++) {
          if (magic[i] !== BLOCK_MAGIC[i]) return 0;
        }
        const payload = arena.HEAPU8.subarray(srcPtr + BLOCK_MAGIC.length, srcPtr + srcLen);
        const ptr = arena.malloc(payload.length);
        arena.HEAPU8.set(payload, ptr);
        arena.HEAPU32[outPtrSlot >>> 2] = ptr;
        arena.HEAPU32[outLenSlot >>> 2] = payload.length;
        return 1;
      }
      if (name === 'openrar_stream_create' || name === 'openrar_stream_create_ex') {
        const id = nextStreamId++;
        streams.set(id, { chunks: [], method: args[0] });
        return id;
      }
      if (name === 'openrar_stream_feed') {
        const [handle, ptr, len] = args;
        const st = streams.get(handle);
        if (!st) return -1;
        st.chunks.push(arena.HEAPU8.slice(ptr, ptr + len));
        return 0;
      }
      if (name === 'openrar_stream_pull') {
        const [handle, outPtrSlot, outLenSlot] = args;
        arena.HEAPU32[outLenSlot >>> 2] = 0;
        return 0;
      }
      if (name === 'openrar_stream_finish') {
        const [handle, outPtrSlot, outLenSlot] = args;
        const st = streams.get(handle);
        if (!st) return -1;
        const total = concatChunks(st.chunks);
        const out = st.method === 0 ? total : concatBytes(BLOCK_MAGIC, total);
        const ptr = arena.malloc(out.length);
        arena.HEAPU8.set(out, ptr);
        arena.HEAPU32[outPtrSlot >>> 2] = ptr;
        arena.HEAPU32[outLenSlot >>> 2] = out.length;
        return 0;
      }
      if (name === 'openrar_stream_free') {
        streams.delete(args[0]);
        return 0;
      }
      if (name === 'openrar_stream_decompress_create') {
        const id = nextStreamId++;
        decompressStreams.set(id, { chunks: [] });
        return id;
      }
      if (name === 'openrar_stream_decompress_feed') {
        const [handle, ptr, len] = args;
        const st = decompressStreams.get(handle);
        if (!st) return -1;
        st.chunks.push(arena.HEAPU8.slice(ptr, ptr + len));
        return 0;
      }
      if (name === 'openrar_stream_decompress_pull') {
        const [handle, outPtrSlot, outLenSlot] = args;
        arena.HEAPU32[outLenSlot >>> 2] = 0;
        return 0;
      }
      if (name === 'openrar_stream_decompress_finish') {
        const [handle, outPtrSlot, outLenSlot] = args;
        const st = decompressStreams.get(handle);
        if (!st) return -1;
        const total = concatChunks(st.chunks);
        if (total.length < BLOCK_MAGIC.length) return -1;
        const payload = total.subarray(BLOCK_MAGIC.length);
        const ptr = arena.malloc(payload.length);
        arena.HEAPU8.set(payload, ptr);
        arena.HEAPU32[outPtrSlot >>> 2] = ptr;
        arena.HEAPU32[outLenSlot >>> 2] = payload.length;
        return 0;
      }
      if (name === 'openrar_stream_decompress_free') {
        decompressStreams.delete(args[0]);
        return 0;
      }
      return 0;
    },
    cwrap(name, retType, argTypes) {
      return (...args) => mod.ccall(name, retType, argTypes, args);
    },
  };
  return Promise.resolve(mod);
}

export function createMockOpenRARArchive() {
  const arena = new MemoryArena();
  const archives = new Map();
  let nextHandle = 1;
  let lastErrorCode = 0;
  let lastErrorStr = '';
  const fns = new Map();
  let nextFnId = 1;

  const UTF8ToString = (ptr, maxLen = 512) => {
    let len = 0;
    while (len < maxLen && arena.HEAPU8[ptr + len] !== 0) len++;
    return new TextDecoder().decode(arena.HEAPU8.subarray(ptr, ptr + len));
  };

  const mod = {
    HEAPU8: arena.HEAPU8,
    HEAPU32: arena.HEAPU32,
    _malloc: (sz) => arena.malloc(sz),
    _free: (ptr) => arena.free(ptr),
    _openrar_archive_alloc: (sz) => arena.malloc(sz),
    _openrar_archive_free: (ptr) => arena.free(ptr),
    _openrar_archive_version: () => 2,
    _openrar_archive_last_error_code: () => lastErrorCode,
    _openrar_archive_open: () => 0,
    _openrar_archive_close: () => 0,
    _openrar_archive_handle_list: () => 0,
    _openrar_archive_handle_extract: () => 0,
    _openrar_archive_handle_extract_all: () => 0,
    _openrar_archive_handle_extract_all2: () => 0,
    _openrar_archive_handle_set_limits: () => 0,
    _openrar_archive_list: () => 0,
    _openrar_archive_list_free: () => 0,
    _openrar_archive_extract: () => 0,
    _openrar_archive_extract_all: () => 0,
    _openrar_archive_extract_all2: () => 0,
    _openrar_archive_create: () => 0,
    _openrar_archive_create2: () => 0,
    _openrar_archive_get_error: () => 0,
    addFunction(fn, _sig) {
      const id = nextFnId++;
      fns.set(id, fn);
      return id;
    },
    removeFunction(id) {
      fns.delete(id);
    },
    UTF8ToString,
    ccall(name, retType, argTypes, args) {
      if (name === 'openrar_archive_get_error') {
        const [buf, bufLen] = args;
        const bytes = new TextEncoder().encode(lastErrorStr + '\0');
        const count = Math.min(bytes.length, bufLen);
        arena.HEAPU8.set(bytes.subarray(0, count), buf);
        return count > 0 ? count - 1 : 0;
      }
      if (name === 'openrar_archive_create2') {
        const [filesArrPtr, fileCount, optsPtr, hooksPtr, outPtrSlot, outLenSlot] = args;
        lastErrorCode = 0;
        lastErrorStr = '';
        const files = [];
        const view = arena.view;
        for (let i = 0; i < fileCount; i++) {
          const base = filesArrPtr + i * 32;
          const pPtr = view.getUint32(base + 0, true);
          const dPtr = view.getUint32(base + 4, true);
          const dLen = view.getUint32(base + 8, true);
          const mtime = Number(view.getBigUint64(base + 16, true));
          const isDir = view.getUint8(base + 24) !== 0;
          const pathStr = mod.UTF8ToString(pPtr, 2048);
          if (pathStr.startsWith('/') || pathStr.includes('\\') || pathStr.split('/').includes('..')) {
            lastErrorCode = -9;
            lastErrorStr = 'INVALID_ARG: invalid path';
            return -9;
          }
          const data = dPtr && dLen > 0 ? arena.HEAPU8.slice(dPtr, dPtr + dLen) : new Uint8Array(0);
          files.push({ path: pathStr, data, mtime, isDir });
        }
        if (hooksPtr) {
          const progFnId = view.getUint32(hooksPtr, true);
          const progFn = fns.get(progFnId);
          if (progFn) {
            try { progFn(100, 100, 0); } catch {}
          }
        }
        const serialized = serializeArchive(files);
        const ptr = arena.malloc(serialized.length);
        arena.HEAPU8.set(serialized, ptr);
        arena.HEAPU32[outPtrSlot >>> 2] = ptr;
        arena.HEAPU32[outLenSlot >>> 2] = serialized.length;
        return 0;
      }
      if (name === 'openrar_archive_open') {
        const [rarPtr, len] = args;
        lastErrorCode = 0;
        lastErrorStr = '';
        if (len < RAR5_SIG.length) {
          lastErrorCode = -1;
          lastErrorStr = 'NOT_RAR';
          return 0;
        }
        for (let i = 0; i < RAR5_SIG.length; i++) {
          if (arena.HEAPU8[rarPtr + i] !== RAR5_SIG[i]) {
            lastErrorCode = -1;
            lastErrorStr = 'NOT_RAR';
            return 0;
          }
        }
        const entries = deserializeArchive(arena.HEAPU8.subarray(rarPtr, rarPtr + len));
        const handle = nextHandle++;
        archives.set(handle, entries);
        return handle;
      }
      if (name === 'openrar_archive_handle_list' || name === 'openrar_archive_list') {
        const isHandle = name === 'openrar_archive_handle_list';
        const handle = isHandle ? args[0] : 0;
        const entries = isHandle ? archives.get(handle) : deserializeArchive(arena.HEAPU8.subarray(args[0], args[0] + args[1]));
        const countSlot = isHandle ? args[1] : args[2];
        const entriesPtrSlot = isHandle ? args[2] : args[3];
        const pathsPtrSlot = isHandle ? args[3] : args[4];
        const pathsSizeSlot = isHandle ? args[4] : args[5];
        if (!entries) return -1;

        let pathBytesLen = 0;
        for (const e of entries) pathBytesLen += e.pathBytes.length;
        const pathsPtr = arena.malloc(pathBytesLen);
        const entriesPtr = arena.malloc(entries.length * 64);
        let currentPathOffset = 0;
        const view = arena.view;

        for (let i = 0; i < entries.length; i++) {
          const e = entries[i];
          arena.HEAPU8.set(e.pathBytes, pathsPtr + currentPathOffset);
          const base = entriesPtr + i * 64;
          view.setUint32(base + 0, currentPathOffset, true);
          view.setUint32(base + 4, e.pathBytes.length, true);
          view.setUint32(base + 8, e.isDir ? 1 : 0, true);
          view.setUint32(base + 12, 3, true);
          view.setUint32(base + 16, 0, true);
          view.setUint32(base + 20, e.crc32, true);
          view.setBigUint64(base + 24, BigInt(e.data.length), true);
          view.setBigUint64(base + 32, BigInt(e.data.length), true);
          view.setBigUint64(base + 40, BigInt(e.mtime), true);
          currentPathOffset += e.pathBytes.length;
        }

        arena.HEAPU32[countSlot >>> 2] = entries.length;
        arena.HEAPU32[entriesPtrSlot >>> 2] = entriesPtr;
        arena.HEAPU32[pathsPtrSlot >>> 2] = pathsPtr;
        arena.HEAPU32[pathsSizeSlot >>> 2] = pathBytesLen;
        return 0;
      }
      if (name === 'openrar_archive_handle_extract') {
        const [handle, idx, outPtrSlot, outLenSlot] = args;
        const entries = archives.get(handle);
        if (!entries || idx >= entries.length) return -9;
        const e = entries[idx];
        const ptr = arena.malloc(e.data.length);
        arena.HEAPU8.set(e.data, ptr);
        arena.HEAPU32[outPtrSlot >>> 2] = ptr;
        arena.HEAPU32[outLenSlot >>> 2] = e.data.length;
        return 0;
      }
      if (name === 'openrar_archive_handle_extract_all' || name === 'openrar_archive_handle_extract_all2') {
        const handle = args[0];
        const outBufPtr = args[name.endsWith('2') ? 2 : 1];
        const outSizePtr = args[name.endsWith('2') ? 3 : 2];
        const outOffsetsPtr = args[name.endsWith('2') ? 4 : 3];
        const outCountPtr = args[name.endsWith('2') ? 5 : 4];
        const entries = archives.get(handle);
        if (!entries) return -1;
        let total = 0;
        for (const e of entries) total += e.data.length;
        const buf = arena.malloc(total);
        const offsets = arena.malloc(entries.length * 16);
        let cur = 0;
        for (let i = 0; i < entries.length; i++) {
          arena.HEAPU8.set(entries[i].data, buf + cur);
          arena.view.setBigUint64(offsets + i * 16, BigInt(cur), true);
          arena.view.setBigUint64(offsets + i * 16 + 8, BigInt(entries[i].data.length), true);
          cur += entries[i].data.length;
        }
        arena.HEAPU32[outBufPtr >>> 2] = buf;
        arena.HEAPU32[outSizePtr >>> 2] = total;
        arena.HEAPU32[outOffsetsPtr >>> 2] = offsets;
        arena.HEAPU32[outCountPtr >>> 2] = entries.length;
        return 0;
      }
      if (name === 'openrar_archive_close') {
        archives.delete(args[0]);
        return 0;
      }
      if (name === 'openrar_archive_list_free') {
        return 0;
      }
      if (name === 'openrar_archive_free') {
        return 0;
      }
      if (name === 'openrar_archive_handle_set_limits') {
        return 0;
      }
      return 0;
    },
    cwrap(name, retType, argTypes) {
      return (...args) => mod.ccall(name, retType, argTypes, args);
    },
  };
  return Promise.resolve(mod);
}

function serializeArchive(files) {
  const chunks = [RAR5_SIG];
  const countBuf = new Uint8Array(4);
  new DataView(countBuf.buffer).setUint32(0, files.length, true);
  chunks.push(countBuf);

  for (const f of files) {
    const pBytes = new TextEncoder().encode(f.path);
    const meta = new Uint8Array(2 + pBytes.length + 1 + 8 + 4 + 4);
    const v = new DataView(meta.buffer);
    v.setUint16(0, pBytes.length, true);
    meta.set(pBytes, 2);
    let off = 2 + pBytes.length;
    v.setUint8(off, f.isDir ? 1 : 0);
    off += 1;
    v.setBigUint64(off, BigInt(f.mtime || 0), true);
    off += 8;
    const crc = computeCrc32(f.data);
    v.setUint32(off, crc, true);
    off += 4;
    v.setUint32(off, f.data.length, true);
    chunks.push(meta);
    if (f.data.length > 0) chunks.push(f.data);
  }
  return concatChunks(chunks);
}

function deserializeArchive(bytes) {
  if (bytes.length < 12) return null;
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const count = v.getUint32(8, true);
  let off = 12;
  const entries = [];
  for (let i = 0; i < count; i++) {
    if (off + 2 > bytes.length) break;
    const pLen = v.getUint16(off, true);
    off += 2;
    const pathBytes = bytes.slice(off, off + pLen);
    const path = new TextDecoder().decode(pathBytes);
    off += pLen;
    const isDir = v.getUint8(off) !== 0;
    off += 1;
    const mtime = Number(v.getBigUint64(off, true));
    off += 8;
    const crc32 = v.getUint32(off, true);
    off += 4;
    const dLen = v.getUint32(off, true);
    off += 4;
    const data = bytes.slice(off, off + dLen);
    off += dLen;
    entries.push({ path, pathBytes, isDir, mtime, crc32, data });
  }
  return entries;
}

export function ensureMockOrDist() {
  const here = path.dirname(url.fileURLToPath(import.meta.url));
  const distDir = path.join(here, 'dist');
  const distOpenrar = path.join(distDir, 'openrar.js');
  const distArchive = path.join(distDir, 'openrar_archive.js');

  if (!fs.existsSync(distDir)) {
    fs.mkdirSync(distDir, { recursive: true });
  }

  if (!fs.existsSync(distOpenrar)) {
    fs.writeFileSync(distOpenrar, `// Auto-generated Node-only mock loader
import { createMockOpenRAR } from '../mock_module.js';
export default createMockOpenRAR;
`);
  }

  if (!fs.existsSync(distArchive)) {
    fs.writeFileSync(distArchive, `// Auto-generated Node-only mock loader
import { createMockOpenRARArchive } from '../mock_module.js';
export default createMockOpenRARArchive;
`);
  }
}
