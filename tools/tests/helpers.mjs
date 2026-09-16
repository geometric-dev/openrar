// Shared helpers for OpenRAR writer test suite.
import { execFileSync, execSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync, utimesSync } from 'node:fs';
import { delimiter, dirname, join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArchive } from '../rar5-coverage.js';

const TESTS_DIR = dirname(fileURLToPath(import.meta.url)); // tools/tests
export const REPO = dirname(dirname(TESTS_DIR));            // repo root
// OPENRAR_EXE overrides the binary location (used by the Linux CI job);
// the build-tree candidates are the local default.
export const OUR_EXE_CANDIDATES = [
  process.env.OPENRAR_EXE,
  join(REPO, 'build', 'openrar64', 'Release', 'openrar.exe'),
  join(REPO, 'build', 'openrar64', 'Debug', 'openrar.exe'),
  join(REPO, 'build', 'unrar64', 'Release', 'openrar.exe'),
  join(REPO, 'build', 'openrar'),
].filter(Boolean);
export const OUR_EXE = OUR_EXE_CANDIDATES.find((p) => existsSync(p)) ?? OUR_EXE_CANDIDATES[0];
export const RAR_EXE = process.env.RAR_EXE ?? 'C:\\Program Files\\WinRAR\\Rar.exe';
export const WINRAR_UNRAR = process.env.UNRAR_EXE ?? 'C:\\Program Files\\WinRAR\\UnRAR.exe';
// Trailing separator when handing an extraction destination to the CLI or the
// oracle: Windows tools want the backslash, POSIX takes the forward slash.
export const DIR_SEP = process.platform === 'win32' ? '\\' : '/';

// Oracle detection: a bare command name ("unrar") is resolved against PATH
// so the suite also works on non-Windows machines with a system unrar.
function resolveExe(p) {
  if (!p) return null;
  if (existsSync(p)) return p;
  if (!p.includes('\\') && !p.includes('/')) {
    for (const d of process.env.PATH?.split(delimiter) ?? []) {
      const candidate = join(d, p);
      if (existsSync(candidate)) return candidate;
    }
  }
  return null;
}
let oracleExeCache; // undefined = not probed yet
export function oracleAvailable() {
  if (oracleExeCache === undefined) oracleExeCache = resolveExe(WINRAR_UNRAR);
  return oracleExeCache !== null;
}

const TEST_ROOT = process.env.OPENRAR_TEST_ROOT ?? join(REPO, 'testrun');

export { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync };

export function freshDir(name) {
  const dir = join(TEST_ROOT, name);
  if (existsSync(dir)) rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  return dir;
}

// Deterministic fixture tree. All mtimes pinned so double-builds are
// byte-comparable.
export function makeFixtureTree(dir) {
  mkdirSync(join(dir, 'dirA', 'nested'), { recursive: true });
  mkdirSync(join(dir, 'uni', 'unicode'), { recursive: true });
  mkdirSync(join(dir, 'd1', 'd2', 'd3'), { recursive: true });

  writeFileSync(join(dir, 'text.txt'), 'The quick brown fox jumps over the lazy dog. '.repeat(200));
  writeFileSync(join(dir, 'empty.txt'), '');
  writeFileSync(join(dir, 'uni/unicode/cyr.txt'), 'cyrillic');
  writeFileSync(join(dir, 'd1/d2/d3/deep.txt'), 'deep\r\n');

  const bin = Buffer.alloc(50 * 1024);
  let s = 7;
  for (let i = 0; i < bin.length; i++) {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    bin[i] = (s >>> 10) & 0xff;
  }
  writeFileSync(join(dir, 'data.bin'), bin);

  pinTimes(dir);
}

function copyFileSample(src, dst) {
  writeFileSync(dst, readFileSync(src));
}

export function pinTimes(dir, isRoot = true) {
  if (isRoot && process.platform === 'win32') {
    try {
      execSync(`powershell -NoProfile -Command "Get-ChildItem -LiteralPath '${dir}' -Recurse -Force | ForEach-Object { $_.LastWriteTime = '2021-06-15T12:00:00Z'; $_.LastAccessTime = '2021-06-15T12:00:00Z'; $_.CreationTime = '2021-06-15T12:00:00Z' }; (Get-Item -LiteralPath '${dir}').LastWriteTime = '2021-06-15T12:00:00Z'"`);
    } catch {}
  }
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    const t = new Date('2021-06-15T12:00:00Z');
    try { statSync(full); } catch { continue; }
    if (!entry.isFile() && !entry.isDirectory()) continue;
    try { utimesSync(full, t, t); } catch {}
    if (entry.isDirectory()) pinTimes(full, false);
  }
}

export function runOurExe(args, cwd) {
  return execFileSync(OUR_EXE, args, { cwd, encoding: 'utf8' });
}

export function runTool(exe, args, cwd) {
  try {
    const out = execFileSync(exe, args, { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    return { code: 0, output: out };
  } catch (e) {
    return { code: e.status ?? 1, output: `${e.stdout ?? ''}${e.stderr ?? ''}` };
  }
}

export function buildOurArchive({ tree, out, switches = [], name = 'archive.rar' }) {
  const arc = join(out, name);
  runOurExe(['a', '-y', '-r', ...switches, '--', arc, '.'], tree);
  return arc;
}

export function walkFiles(root) {
  const out = [];
  for (const e of readdirSync(root, { withFileTypes: true })) {
    const full = join(root, e.name);
    if (e.isDirectory()) out.push(...walkFiles(full));
    else out.push(full);
  }
  return out.sort();
}

export function treesEqual(aRoot, bRoot) {
  const A = walkFiles(aRoot), B = walkFiles(bRoot);
  const relA = A.map((f) => relative(aRoot, f).replace(/\\/g, '/'));
  const relB = B.map((f) => relative(bRoot, f).replace(/\\/g, '/'));
  if (relA.join('|') !== relB.join('|')) return { ok: false, why: `file lists differ: ${relA} vs ${relB}` };
  for (let i = 0; i < A.length; i++) {
    const a = readFileSync(A[i]), b = readFileSync(B[i]);
    if (!a.equals(b)) return { ok: false, why: `content differs: ${relA[i]}` };
  }
  return { ok: true };
}

export function extractWith(exe, archive, dest) {
  mkdirSync(dest, { recursive: true });
  return runTool(exe, ['x', '-y', '--', archive, dest + sep]);
}

export function sha256(file) {
  return createHash('sha256').update(readFileSync(file)).digest('hex');
}
