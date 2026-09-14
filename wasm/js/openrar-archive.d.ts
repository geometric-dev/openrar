// ─────────────────────────────────────────────────────────────────────────────
//  openrar-archive.d.ts — Archive API (RAR5) interface contract
// ─────────────────────────────────────────────────────────────────────────────
//
//  Matches wasm/js/openrar-archive.js against the v2 C ABI
//  (src/wasm/archive_api.hpp). The .d.ts is the source of truth for TS
//  consumers; check-exports.mjs keeps the raw module surface honest.

import type { InputData } from './openrar.js';

export const ARCHIVE_WASM_API_VERSION: number;

export type RarErrorCode =
  | 'NOT_RAR'
  | 'UNSUPPORTED_FEATURE'
  | 'TRUNCATED'
  | 'CRC_MISMATCH'
  | 'BAD_PASSWORD'
  | 'IO'
  | 'NOMEM'
  | 'ABORTED'
  | 'INVALID_ARG'
  | 'ENCRYPTED';

export class RarError extends Error {
  readonly code: RarErrorCode;
  readonly detail?: string;
  /** Raw RarError C value, when known. */
  readonly numericCode?: number;
  constructor(code: RarErrorCode, detail?: string, numericCode?: number);
  static fromCode(num: number, detail?: string): RarError;
}

export interface RarEntry {
  path: string;             // forward slashes; '/' suffix iff isDir
  size: number;             // uncompressed
  packedSize: number;
  mtime: number;            // UNIX seconds (0 if unset on disk)
  crc32: number;            // 0 if unknown (UNVERIFIED)
  method: 0 | 3 | 5;
  isDir: boolean;
  isEncrypted: boolean;
  index: number;
}

export interface CreateArchiveInput {
  path: string;
  data: Uint8Array | string;
  /** UNIX seconds; omitted/0 means "now". */
  mtime?: number;
}

export interface CreateArchiveOptions {
  method?: 0 | 3 | 5;
  /** Dictionary window: 1=128 KiB, 2=256 KiB, 3=512 KiB, 4=1 MiB. Default 4. */
  windowLog2?: 1 | 2 | 3 | 4;
  /** Cumulative monotonic (bytes done, bytes total); ~once per entry. */
  onProgress?: (done: number, total: number) => void;
  /** Polled once per entry; throws ABORTED. */
  signal?: AbortSignal;
}

export function createArchive(files: CreateArchiveInput[], opts?: CreateArchiveOptions): Promise<Uint8Array>;

/** Buffer inputs: Blob/Response must be converted with blobToBytes() first. */
export type ArchiveBytes = Uint8Array | ArrayBuffer | ArrayBufferView | string;

export function listArchive(rar: ArchiveBytes): Promise<RarEntry[]>;

export function extractFile(rar: ArchiveBytes, path: string): Promise<Uint8Array>;

export function extractFileByIndex(rar: ArchiveBytes, index: number): Promise<Uint8Array>;

export interface ExtractAllOptions {
  /** Cumulative monotonic (bytes done, bytes total). */
  onProgress?: (done: number, total: number) => void;
  signal?: AbortSignal;
  /**
   * Use the one-shot C extract_all (single scan) instead of the default
   * per-entry loop. Single scan, but ~2× the uncompressed size in memory.
   */
  bulk?: boolean;
  /** Bomb guard: refuse archives claiming more uncompressed bytes. Default 512 MiB; 0 disables. */
  maxOutputBytes?: number;
}

export function extractAll(rar: ArchiveBytes, opts?: ExtractAllOptions): Promise<Map<string, Uint8Array>>;

/**
 * Handle-based archive. Construct via openArchive() — the constructor is
 * internal (it wraps a raw C handle).
 */
export class OpenRARArchive {
  private constructor(handle: number, entries: RarEntry[], rarBytes: Uint8Array);
  list(): RarEntry[];
  extract(path: string): Promise<Uint8Array>;
  extractByIndex(index: number): Promise<Uint8Array>;
  extractAll(opts?: ExtractAllOptions): Promise<Map<string, Uint8Array>>;
  close(): void;
  get pathCount(): number;
}

export function openArchive(rar: ArchiveBytes): Promise<OpenRARArchive>;

export function validateArchivePath(path: string): { ok: true } | { ok: false; reason: string };

/** Deletes the shared module, freeing the wasm heap. Live handles are invalidated. */
export function destroy(): void;

// Raw archive module (advanced)
export interface RawArchiveModule {
  _openrar_archive_version(): number;
  _openrar_archive_list(data: number, size: number, countPtr: number, entriesPtrPtr: number, pathsPtrPtr: number, pathsSizePtr: number): number;
  _openrar_archive_list_free(entries: number, paths: number, pathsSize: number): void;
  _openrar_archive_extract(data: number, size: number, index: number, outPtrPtr: number, outLenPtr: number): number;
  _openrar_archive_extract_all(data: number, size: number, bufPtrPtr: number, bufSizePtr: number, offsetsPtrPtr: number, countPtr: number): number;
  _openrar_archive_extract_all2(data: number, size: number, hooks: number, bufPtrPtr: number, bufSizePtr: number, offsetsPtrPtr: number, countPtr: number): number;
  _openrar_archive_create(pathsArr: number, dataArr: number, sizesArr: number, fileCount: number, method: number, windowLog2: number, outPtrPtr: number, outLenPtr: number): number;
  _openrar_archive_create2(filesArr: number, fileCount: number, opts: number, hooks: number, outPtrPtr: number, outLenPtr: number): number;
  _openrar_archive_open(data: number, size: number): number;
  _openrar_archive_close(handle: number): void;
  _openrar_archive_handle_list(handle: number, countPtr: number, entriesPtrPtr: number, pathsPtrPtr: number, pathsSizePtr: number): number;
  _openrar_archive_handle_extract(handle: number, index: number, outPtrPtr: number, outLenPtr: number): number;
  _openrar_archive_handle_extract_all(handle: number, bufPtrPtr: number, bufSizePtr: number, offsetsPtrPtr: number, countPtr: number): number;
  _openrar_archive_handle_extract_all2(handle: number, hooks: number, bufPtrPtr: number, bufSizePtr: number, offsetsPtrPtr: number, countPtr: number): number;
  _openrar_archive_get_error(buf: number, len: number): number;
  _openrar_archive_last_error_code(): number;
  _openrar_archive_alloc(n: number): number;
  _openrar_archive_free(ptr: number): void;
  _malloc(n: number): number;
  _free(ptr: number): void;
  ccall<T>(ident: string, returnType: string | null, argTypes: string[], args: unknown[]): T;
  cwrap(ident: string, returnType: string | null, argTypes: string[]): (...args: unknown[]) => unknown;
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  UTF8ToString(ptr: number, max?: number): string;
  addFunction(fn: (...args: unknown[]) => unknown, sig: string): number;
  removeFunction(ptr: number): void;
}

export function getArchiveModule(): Promise<RawArchiveModule>;
