// Test suite for RAR5 file version control (-ver[n])
import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import {
  DIR_SEP,
  OUR_EXE,
  RAR_EXE,
  WINRAR_UNRAR,
  existsSync,
  freshDir,
  oracleAvailable,
  readFileSync,
  runOurExe,
  runTool,
  writeFileSync,
} from './helpers.mjs';
import { parseArchive } from '../rar5-coverage.js';

test('versioning: -ver keeps multiple versions across updates', () => {
  const dir = freshDir('ver_accum');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'file.txt');

  writeFileSync(src, 'content v1\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'content v2\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'content v3\n');
  runOurExe(['a', '-ver', arc, src]);

  const parsed = parseArchive(readFileSync(arc));
  const fileBlocks = parsed.blocks.filter((b) => b.type === 2);
  assert.equal(fileBlocks.length, 3, 'archive should contain 3 entries');

  // Verify versions: entry 0 is v1, entry 1 is v2, entry 2 is current (unversioned)
  assert.equal((fileBlocks[0].extra || []).some((r) => r.type === 4 && r.version === 1), true);
  assert.equal((fileBlocks[1].extra || []).some((r) => r.type === 4 && r.version === 2), true);
  assert.equal((fileBlocks[2].extra || []).some((r) => r.type === 4), false);
});

test('versioning: -ver<n> prunes oldest versions when exceeding limit', () => {
  const dir = freshDir('ver_prune');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'sample.txt');

  writeFileSync(src, 'data 1\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'data 2\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'data 3\n');
  runOurExe(['a', '-ver', arc, src]);

  // Archive has sample.txt;1 (data 1), sample.txt;2 (data 2), sample.txt (data 3)
  // Now update with -ver2 (keep at most 2 historical versions)
  writeFileSync(src, 'data 4\n');
  runOurExe(['a', '-ver2', arc, src]);

  const parsed = parseArchive(readFileSync(arc));
  const fileBlocks = parsed.blocks.filter((b) => b.type === 2);
  assert.equal(fileBlocks.length, 3, 'archive should have 2 historical + 1 current = 3 entries');

  assert.equal((fileBlocks[0].extra || []).some((r) => r.type === 4 && r.version === 1), true);
  assert.equal((fileBlocks[1].extra || []).some((r) => r.type === 4 && r.version === 2), true);
  assert.equal((fileBlocks[2].extra || []).some((r) => r.type === 4), false);
});

test('versioning: default extraction extracts only latest version', () => {
  const dir = freshDir('ver_extract_default');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'note.txt');

  writeFileSync(src, 'initial\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'updated\n');
  runOurExe(['a', '-ver', arc, src]);

  const outDir = join(dir, 'out');
  runOurExe(['x', arc, outDir + DIR_SEP]);

  assert.equal(existsSync(join(outDir, 'note.txt')), true);
  assert.equal(existsSync(join(outDir, 'note.txt;1')), false);
  assert.equal(readFileSync(join(outDir, 'note.txt'), 'utf8'), 'updated\n');
});

test('versioning: -ver extraction extracts all versions with suffixes', () => {
  const dir = freshDir('ver_extract_all');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'doc.txt');

  writeFileSync(src, 'first\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'second\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'third\n');
  runOurExe(['a', '-ver', arc, src]);

  const outDir = join(dir, 'out');
  runOurExe(['x', '-ver', arc, outDir + DIR_SEP]);

  assert.equal(existsSync(join(outDir, 'doc.txt;1')), true);
  assert.equal(existsSync(join(outDir, 'doc.txt;2')), true);
  assert.equal(existsSync(join(outDir, 'doc.txt')), true);

  assert.equal(readFileSync(join(outDir, 'doc.txt;1'), 'utf8'), 'first\n');
  assert.equal(readFileSync(join(outDir, 'doc.txt;2'), 'utf8'), 'second\n');
  assert.equal(readFileSync(join(outDir, 'doc.txt'), 'utf8'), 'third\n');
});

test('versioning: -ver<n> extraction extracts only specific version without suffix', () => {
  const dir = freshDir('ver_extract_specific');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'item.txt');

  writeFileSync(src, 'v1\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'v2\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'v3\n');
  runOurExe(['a', '-ver', arc, src]);

  const outDir = join(dir, 'out');
  runOurExe(['x', '-ver1', arc, outDir + DIR_SEP]);

  assert.equal(existsSync(join(outDir, 'item.txt')), true);
  assert.equal(existsSync(join(outDir, 'item.txt;1')), false);
  assert.equal(readFileSync(join(outDir, 'item.txt'), 'utf8'), 'v1\n');
});

test('versioning: targeted extraction by versioned filename', () => {
  const dir = freshDir('ver_extract_targeted');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'target.txt');

  writeFileSync(src, 'alpha\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'beta\n');
  runOurExe(['a', '-ver', arc, src]);

  const outDir = join(dir, 'out');
  runOurExe(['x', arc, 'target.txt;1', outDir + DIR_SEP]);

  assert.equal(existsSync(join(outDir, 'target.txt;1')), true);
  assert.equal(readFileSync(join(outDir, 'target.txt;1'), 'utf8'), 'alpha\n');
});

test('versioning: dual-oracle interop with WinRAR 7.20', (t) => {
  if (!oracleAvailable()) {
    t.skip('WinRAR UnRAR oracle not available');
    return;
  }

  const dir = freshDir('ver_oracle');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'oracle.txt');

  writeFileSync(src, 'oracle content v1\n');
  runOurExe(['a', '-ver', arc, src]);

  writeFileSync(src, 'oracle content v2\n');
  runOurExe(['a', '-ver', arc, src]);

  // 1. UnRAR extracts default (latest only)
  const unrarOutDefault = join(dir, 'unrar_default');
  runTool(WINRAR_UNRAR, ['x', '-y', arc, unrarOutDefault + DIR_SEP]);
  assert.equal(existsSync(join(unrarOutDefault, 'oracle.txt')), true);
  assert.equal(existsSync(join(unrarOutDefault, 'oracle.txt;1')), false);
  assert.equal(readFileSync(join(unrarOutDefault, 'oracle.txt'), 'utf8'), 'oracle content v2\n');

  // 2. UnRAR extracts -ver (all versions)
  const unrarOutAll = join(dir, 'unrar_all');
  runTool(WINRAR_UNRAR, ['x', '-y', '-ver', arc, unrarOutAll + DIR_SEP]);
  assert.equal(existsSync(join(unrarOutAll, 'oracle.txt;1')), true);
  assert.equal(existsSync(join(unrarOutAll, 'oracle.txt')), true);
  assert.equal(readFileSync(join(unrarOutAll, 'oracle.txt;1'), 'utf8'), 'oracle content v1\n');
  assert.equal(readFileSync(join(unrarOutAll, 'oracle.txt'), 'utf8'), 'oracle content v2\n');

  // 3. OpenRAR unpacks an archive created by WinRAR rar.exe
  if (existsSync(RAR_EXE)) {
    const winrarArc = join(dir, 'winrar_arc.rar');
    writeFileSync(join(dir, 'from_winrar.txt'), 'created by winrar v1\n');
    runTool(RAR_EXE, ['a', '-y', '-ver', 'winrar_arc.rar', 'from_winrar.txt'], dir);
    writeFileSync(join(dir, 'from_winrar.txt'), 'created by winrar v2\n');
    runTool(RAR_EXE, ['a', '-y', '-ver', 'winrar_arc.rar', 'from_winrar.txt'], dir);

    const ourExtract = join(dir, 'our_from_winrar');
    runOurExe(['x', '-ver', winrarArc, ourExtract + DIR_SEP]);
    assert.equal(existsSync(join(ourExtract, 'from_winrar.txt;1')), true);
    assert.equal(existsSync(join(ourExtract, 'from_winrar.txt')), true);
    assert.equal(readFileSync(join(ourExtract, 'from_winrar.txt;1'), 'utf8'), 'created by winrar v1\n');
    assert.equal(readFileSync(join(ourExtract, 'from_winrar.txt'), 'utf8'), 'created by winrar v2\n');
  }
});
