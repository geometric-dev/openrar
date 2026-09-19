// ─────────────────────────────────────────────────────────────────────────────
//  openrar.d.ts — OpenRAR WASM interface contract
// ─────────────────────────────────────────────────────────────────────────────
//
//  Stability:
//    - WASM_API_VERSION is pinned in JS to the C ABI version reported by the
//      loaded module. Mismatch throws at instantiation time.
//    - RawModule is 1:1 with the C ABI in src/wasm/wasm_api.cpp (v2: no
//      embind). All other types here follow semver.

export const enum CompressionMethod {
  STORE   = 0,
  FASTEST = 1,
  FAST    = 2,
  NORMAL  = 3,
  GOOD    = 4,
  BEST    = 5,
}

export const WASM_API_VERSION: number;
export const DEFAULT_WIN_SIZE: number;
export const DEFAULT_DECOMPRESS_WIN_SIZE: number;
/** Largest window size reachable from JS (i32 ccall boundary): 2 GiB - 4 KiB. */
export const MAX_WIN_SIZE: number;

// 1:1 surface with the C ABI exports of src/wasm/wasm_api.cpp. Stable, ABI-versioned.
export interface RawModule {
  _openrar_version(): number;
  _openrar_alloc(n: number): number;
  _openrar_free(ptr: number): void;
  /** 1 on success, 0 on failure. On success *outPtr/*outLen are set; free with _openrar_free. */
  _openrar_compress(src: number, srcLen: number, outPtr: number, outLen: number, method: number): number;
  _openrar_decompress(src: number, srcLen: number, outPtr: number, outLen: number): number;
  _openrar_compress2(src: number, srcLen: number, outPtr: number, outLen: number, method: number, winSize: number): number;
  _openrar_decompress2(src: number, srcLen: number, outPtr: number, outLen: number, winSize: number): number;
  // Streaming encoder (additive in v2). feed() copies bytes in; finish()
  // returns byte-identical output to compress2 of the same input.
  _openrar_stream_create(method: number, winSize: number): number;
  _openrar_stream_feed(handle: number, src: number, n: number): number;
  _openrar_stream_finish(handle: number, outPtr: number, outLen: number): number;
  _openrar_stream_free(handle: number): void;
  // Streaming decoder (v1.11).
  _openrar_stream_decompress_create(winSize: number): number;
  _openrar_stream_decompress_feed(handle: number, src: number, n: number): number;
  _openrar_stream_decompress_finish(handle: number, outPtr: number, outLen: number): number;
  _openrar_stream_decompress_pull(handle: number, outPtr: number, outLen: number): number;
  _openrar_stream_decompress_free(handle: number): void;
  _malloc(n: number): number;
  _free(ptr: number): void;
  ccall<T = unknown>(ident: string, returnType: string | null, argTypes: string[], args: unknown[]): T;
  cwrap(ident: string, returnType: string | null, argTypes: string[]): (...args: unknown[]) => unknown;
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  /** MODULARIZE lifetime hook: frees the wasm heap. */
  delete(): void;
}

export type CreateOpenRAR = () => Promise<RawModule>;
export declare const createOpenRAR: CreateOpenRAR;

export type InputData =
  | Uint8Array
  | string
  | ArrayBuffer
  | ArrayBufferView
  | Blob
  | Response;

export declare function toUint8(data: InputData): Uint8Array;
export declare function blobToBytes(data: Blob | Response | InputData): Promise<Uint8Array>;

export declare function hex(data: InputData): string;
export declare function base64Encode(data: InputData): string;
export declare function base64Decode(s: string): Uint8Array;

export declare class OpenRAR {
  readonly ready: Promise<void>;
  apiVersion: number;
  readonly module: RawModule;

  constructor();
  versionString(): Promise<string>;

  /**
   * Free the wasm heap by deleting the shared module. All OpenRAR instances
   * share one module, so the first destroy() tears it down for every live
   * handle and invalidates HeapBuffer pointers. After destroy() this handle
   * throws on use; the next getModule() re-instantiates.
   */
  destroy(): void;

  /**
   * Compress with the RAR5 v0 block codec. Throws on failure.
   * Note: method STORE (0) is a passthrough — the result is the input
   * unchanged and is NOT accepted by decompress().
   */
  compress(data: InputData, method?: CompressionMethod, winSize?: number): Promise<Uint8Array>;

  /** Decompress output of compress (methods 1..5). Throws on corrupt input. */
  decompress(data: InputData, winSize?: number): Promise<Uint8Array>;

  /**
   * Compress a ReadableStream incrementally. Output is byte-identical to
   * compress() of the same concatenated input.
   */
  compressStream(
    readable: ReadableStream<Uint8Array>,
    opts?: { method?: CompressionMethod; winSize?: number; signal?: AbortSignal },
  ): Promise<Uint8Array>;

  /**
   * Decompress a ReadableStream incrementally, returning the concatenated uncompressed bytes.
   */
  decompressStream(
    readable: ReadableStream<Uint8Array>,
    opts?: { winSize?: number; signal?: AbortSignal },
  ): Promise<Uint8Array>;

  /**
   * Decompress a ReadableStream incrementally, yielding uncompressed chunks as they are decoded.
   * Memory is bounded by the window size + chunk size.
   */
  decompressStreamChunks(
    readable: ReadableStream<Uint8Array>,
    opts?: { winSize?: number; signal?: AbortSignal },
  ): AsyncGenerator<Uint8Array, void, unknown>;

  // Low-level C ABI (escape hatch for FFI / QuickJS hosts).
  alloc(n: number): Promise<number>;
  free(ptr: number): Promise<void>;
  /** Compress directly into a wasm heap buffer. Null on failure. */
  compressHeap(
    src: InputData,
    method?: CompressionMethod,
    winSize?: number,
  ): Promise<HeapBuffer | null>;

  /** Copy bytes out of WASM heap at (ptr, len). Safe across heap growth. */
  safeRead(ptr: number, len: number): Uint8Array;

  /** Base offset of HEAPU8 within its backing ArrayBuffer (usually 0). */
  readonly heapBase: number;

  /** Cheap bounds check: does `ptr..ptr+len` lie inside the current heap? */
  isPointerValid(ptr: number, len?: number): boolean;
}

export interface HeapBuffer {
  ptr: number;
  len: number;
  free(): void;
}

export declare function getModule(): Promise<RawModule>;

export default OpenRAR;
