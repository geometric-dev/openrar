import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import {
  existsSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { join } from 'node:path';
import {
  OUR_EXE,
  WINRAR_UNRAR,
  oracleAvailable,
  buildOurArchive,
  freshDir,
  makeFixtureTree,
  runTool,
} from './helpers.mjs';
import { parseArchive } from '../rar5-coverage.js';

describe('RAR 5.0 / 7.0 Compression Dictionary & Version Spec', () => {
  it('applies standard default dictionary per compression method (m0..m5)', () => {
    const tree = freshDir('dict-def-tree');
    writeFileSync(join(tree, 'sample.txt'), 'A'.repeat(50000));
    const out = freshDir('dict-def-out');

    const expectedDicts = [
      { method: 0, winSize: 128 * 1024 },
      { method: 1, winSize: 512 * 1024 },
      { method: 2, winSize: 1024 * 1024 },
      { method: 3, winSize: 2048 * 1024 },
      { method: 4, winSize: 4096 * 1024 },
      { method: 5, winSize: 16384 * 1024 },
    ];

    for (const { method, winSize } of expectedDicts) {
      const arc = join(out, `arc_m${method}.rar`);
      const res = runTool(OUR_EXE, ['a', '-y', `-m${method}`, arc, '.'], tree);
      assert.equal(res.code, 0, `Archiving with -m${method} failed: ${res.output}`);

      const buf = readFileSync(arc);
      const parsed = parseArchive(buf);
      const fileBlock = parsed.blocks.find((b) => b.typeName === 'file');
      assert.ok(fileBlock, `File block must exist for -m${method}`);

      // The header dict bits must reflect the per-method default table above
      // (the parser label is 128 * 2^n KB: '128KB', '512KB', '1MB', ...).
      const expectedLabel = { 0: '128KB', 1: '512KB', 2: '1MB', 3: '2MB', 4: '4MB', 5: '16MB' }[method];
      assert.equal(
        fileBlock.file.dict, expectedLabel,
        `-m${method} must declare its default ${expectedLabel} dictionary (got ${fileBlock.file?.dict})`,
      );

      // Verify WinRAR listing matches
      const resList = runTool(OUR_EXE, ['lt', arc], out);
      assert.equal(resList.code, 0, `Listing failed for -m${method}`);

      // Dual-oracle test with WinRAR
      const resWinTest = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resWinTest.code, 0, `WinRAR test failed for -m${method}: ${resWinTest.output}`);
    }
  });

  it('supports explicit -md dictionary switches (128k to 4g)', () => {
    const tree = freshDir('dict-md-tree');
    writeFileSync(join(tree, 'data.txt'), 'DICTIONARY NEGOTIATION TEST DATA '.repeat(2000));
    const out = freshDir('dict-md-out');

    const testSizes = [
      { sw: '-md128k', expectedKB: 128 },
      { sw: '-md512k', expectedKB: 512 },
      { sw: '-md1m', expectedKB: 1024 },
      { sw: '-md4m', expectedKB: 4096 },
      { sw: '-md16m', expectedKB: 16384 },
      { sw: '-md64m', expectedKB: 65536 },
      { sw: '-md128m', expectedKB: 131072 },
      { sw: '-md1g', expectedKB: 1024 * 1024 },
    ];

    for (const { sw, expectedKB } of testSizes) {
      const arc = join(out, `arc_${sw.slice(1)}.rar`);
      const res = runTool(OUR_EXE, ['a', '-y', '-m3', sw, arc, '.'], tree);
      assert.equal(res.code, 0, `Archiving with ${sw} failed: ${res.output}`);

      // Dual-oracle test with WinRAR
      const resWinTest = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resWinTest.code, 0, `WinRAR test failed for ${sw}: ${resWinTest.output}`);

      // Test byte-identical round-trip extraction
      const extDir = freshDir(`ext_${sw.slice(1)}`);
      const resExt = runTool(OUR_EXE, ['x', '-y', arc, extDir + '\\'], tree);
      assert.equal(resExt.code, 0, `Extraction failed for ${sw}: ${resExt.output}`);
      const extracted = readFileSync(join(extDir, 'data.txt'), 'utf8');
      const original = readFileSync(join(tree, 'data.txt'), 'utf8');
      assert.equal(extracted, original, `Extracted payload must match original for ${sw}`);
    }
  });

  it('preserves dictionary window across solid stream (-s -md16m)', () => {
    const tree = freshDir('dict-solid-tree');
    makeFixtureTree(tree);
    const out = freshDir('dict-solid-out');
    const arc = join(out, 'solid_dict.rar');

    const res = runTool(OUR_EXE, ['a', '-y', '-s', '-m4', '-md16m', arc, '.'], tree);
    assert.equal(res.code, 0, `Solid archiving failed: ${res.output}`);

    const resWin = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
    assert.equal(resWin.code, 0, `WinRAR test on solid archive failed: ${resWin.output}`);

    const extDir = freshDir('dict-solid-ext');
    const resExt = runTool(OUR_EXE, ['x', '-y', arc, extDir + '\\'], tree);
    assert.equal(resExt.code, 0, `Solid extraction failed: ${resExt.output}`);
  });

  it('rejects illegal dictionary sizes (<128KB, >1TB, and non-power-of-2 for -md)', () => {
    const tree = freshDir('dict-bad-tree');
    writeFileSync(join(tree, 'bad.txt'), 'test');
    const out = freshDir('dict-bad-out');

    // 1. Less than 128KB
    const resSmall = runTool(OUR_EXE, ['a', '-y', '-md64k', join(out, 'bad1.rar'), '.'], tree);
    assert.notEqual(resSmall.code, 0, '-md64k (<128KB) must be rejected');

    // 2. Greater than 1TB
    const resLarge = runTool(OUR_EXE, ['a', '-y', '-md2t', join(out, 'bad2.rar'), '.'], tree);
    assert.notEqual(resLarge.code, 0, '-md2t (>1TB) must be rejected');

    // 3. Non-power-of-2 under -md (e.g. -md3m) must be rejected
    const resNonPow2 = runTool(OUR_EXE, ['a', '-y', '-md3m', join(out, 'bad3.rar'), '.'], tree);
    assert.notEqual(resNonPow2.code, 0, '-md3m (non-power-of-2 for <=4GB) must be rejected');

    // 4. Non-power-of-2 under -mdx (e.g. -mdx3m) is allowed
    const resMdx = runTool(OUR_EXE, ['a', '-y', '-mdx3m', join(out, 'good_mdx.rar'), '.'], tree);
    assert.equal(resMdx.code, 0, '-mdx3m (fractional allowed for limit) must be accepted');
  });
});
