import { execFileSync } from 'node:child_process';
import { mkdirSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const seedDir = join(repoRoot, 'tests', 'fuzz', 'seeds');
const tmpDir = join(repoRoot, 'build', 'seed_staging');
const exe = join(repoRoot, 'build', 'openrar64', 'Release', 'openrar.exe');

mkdirSync(seedDir, { recursive: true });
mkdirSync(tmpDir, { recursive: true });

const f1 = join(tmpDir, 'file1.txt');
const f2 = join(tmpDir, 'file2.txt');
writeFileSync(f1, 'Fuzzing Seed Data 1: The quick brown fox jumps over the lazy dog.\n');
writeFileSync(f2, 'Fuzzing Seed Data 2: 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\n');

console.log('Generating seed archives with openrar.exe...');

// 1. QuickOpen seed
execFileSync(exe, ['a', '-y', '-m0', '-qo+', join(seedDir, 'seed_qo.rar'), 'file1.txt', 'file2.txt'], { cwd: tmpDir });

// 2. Metadata extra record seed (-ams)
execFileSync(exe, ['a', '-y', '-m0', '-ams', join(seedDir, 'seed_metadata.rar'), 'file1.txt'], { cwd: tmpDir });

// 3. Recovery record seed (-rr5%)
execFileSync(exe, ['a', '-y', '-m0', '-rr5%', join(seedDir, 'seed_rr.rar'), 'file1.txt'], { cwd: tmpDir });

// 4. Multi-volume set with -rv
const volPayload = Buffer.alloc(10 * 1024, 0x37);
writeFileSync(join(tmpDir, 'large.bin'), volPayload);
execFileSync(exe, ['a', '-y', '-m0', '-v3k', '-rv1', join(seedDir, 'seed_vol.part1.rar'), 'large.bin'], { cwd: tmpDir });

rmSync(tmpDir, { recursive: true, force: true });
console.log('Seed generation complete.');
