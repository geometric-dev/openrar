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
  buildOurArchive,
  freshDir,
  makeFixtureTree,
  runTool,
} from './helpers.mjs';
import { parseArchive } from '../rar5-coverage.js';

describe('RAR 5.0 Recovery Records (-rr) & Repair (r)', () => {
  it('creates in-archive recovery record (-rr5%), verifies headers and WinRAR listing', () => {
    const tree = freshDir('rec-create-tree');
    makeFixtureTree(tree);
    const out = freshDir('rec-create-out');
    const arc = join(out, 'archive.rar');

    const resAdd = runTool(OUR_EXE, ['a', '-y', '-m0', '-rr5%', arc, '.'], tree);
    assert.equal(resAdd.code, 0, `Archive creation failed: ${resAdd.output}`);

    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);

    // 1. Verify MAIN header flags & locator
    const mainBlock = parsed.blocks.find((b) => b.typeName === 'main');
    assert.ok(mainBlock, 'Main block must exist');
    assert.ok(mainBlock.archiveFlags?.includes('protect'), 'MHFL_PROTECT flag must be set');
    const locator = mainBlock.extra?.find((e) => e.type === 1);
    assert.ok(locator, 'Locator extra record must exist');
    assert.ok(locator.recoveryOffset > 0, 'Locator recoveryOffset must be > 0');

    // 2. Verify RR Service block
    const rrBlock = parsed.blocks.find((b) => b.file?.service === 'RR');
    assert.ok(rrBlock, 'RR service block must exist');
    assert.equal(rrBlock.file.name, 'RR');
    assert.ok(rrBlock.dataSize > 64, 'RR payload must contain struct header and parity stream');

    // 3. Dual-oracle test with WinRAR
    const resWinTest = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
    assert.equal(resWinTest.code, 0, `WinRAR test failed: ${resWinTest.output}`);

    // 4. Listing shows recovery record
    const resList = runTool(OUR_EXE, ['lt', arc], out);
    assert.ok(resList.output.toLowerCase().includes('recovery record'), 'List output should display recovery record');
  });

  it('repairs corrupted archive data (r) back to 100% byte-identical state', () => {
    const tree = freshDir('rec-rep-tree');
    const originalContent = 'RECOVERY RECORD TEST PAYLOAD: '.repeat(200);
    writeFileSync(join(tree, 'important.txt'), originalContent);
    const out = freshDir('rec-rep-out');
    const arc = join(out, 'protected.rar');

    // 1. Create protected archive with -rr5%
    runTool(OUR_EXE, ['a', '-y', '-m0', '-rr5%', arc, '.'], tree);
    const originalBuf = readFileSync(arc);

    // 2. Introduce single-sector corruption in file data area
    const corruptBuf = Buffer.from(originalBuf);
    // Offset 100 is inside the data payload of important.txt
    corruptBuf[100] ^= 0xff;
    corruptBuf[101] ^= 0xaa;
    writeFileSync(arc, corruptBuf);

    // Verify WinRAR test fails on corrupted archive
    const resDamaged = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
    assert.notEqual(resDamaged.code, 0, 'Damaged archive should fail WinRAR test');

    // 3. Run OpenRAR repair command (r)
    const resRepair = runTool(OUR_EXE, ['r', '-y', arc], out);
    assert.equal(resRepair.code, 0, `Repair failed: ${resRepair.output}`);

    // 4. Verify repaired archive passes WinRAR test
    const resRepairedTest = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
    assert.equal(resRepairedTest.code, 0, `Repaired archive failed WinRAR test: ${resRepairedTest.output}`);

    // 5. Verify byte-identical file extraction
    const extDir = freshDir('rec-rep-ext');
    const resExt = runTool(OUR_EXE, ['x', '-y', arc, extDir + '\\'], tree);
    assert.equal(resExt.code, 0, `Extraction failed: ${resExt.output}`);
    const extracted = readFileSync(join(extDir, 'important.txt'), 'utf8');
    assert.equal(extracted, originalContent, 'Extracted data must match original payload exactly');
  });

  it('repairs multi-file compressed archives (-m3) with -rr10%', () => {
    const tree = freshDir('rec-mf-tree');
    makeFixtureTree(tree);
    const out = freshDir('rec-mf-out');
    const arc = join(out, 'multi.rar');

    // 1. Create compressed archive with -rr10%
    runTool(OUR_EXE, ['a', '-y', '-m3', '-rr10%', arc, '.'], tree);
    const originalBuf = readFileSync(arc);

    // 2. Corrupt bytes at offset 150
    const corruptBuf = Buffer.from(originalBuf);
    corruptBuf[150] ^= 0x77;
    writeFileSync(arc, corruptBuf);

    // 3. Repair archive
    const resRep = runTool(OUR_EXE, ['r', '-y', arc], out);
    assert.equal(resRep.code, 0, `Repair failed: ${resRep.output}`);

    // 4. Verify WinRAR test passes
    const resWin = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
    assert.equal(resWin.code, 0, `WinRAR test on repaired archive failed: ${resWin.output}`);

    // 5. Extract and verify full fixture tree
    const extDir = freshDir('rec-mf-ext');
    const resExt = runTool(OUR_EXE, ['x', '-y', arc, extDir + '\\'], tree);
    assert.equal(resExt.code, 0, `Extraction failed: ${resExt.output}`);
    assert.ok(existsSync(join(extDir, 'data.bin')), 'Extracted data.bin must exist');
    assert.ok(existsSync(join(extDir, 'text.txt')), 'Extracted text.txt must exist');
  });

  it('detects unrecoverable damage exceeding parity capacity gracefully', () => {
    const tree = freshDir('rec-fail-tree');
    writeFileSync(join(tree, 'fail.txt'), 'A'.repeat(5000));
    const out = freshDir('rec-fail-out');
    const arc = join(out, 'unrec.rar');

    runTool(OUR_EXE, ['a', '-y', '-m0', '-rr3%', arc, '.'], tree);

    // Completely wipe 3000 bytes (exceeding 3% recovery threshold)
    const buf = readFileSync(arc);
    const corrupt = Buffer.from(buf);
    for (let i = 50; i < 3000; i++) corrupt[i] = 0;
    writeFileSync(arc, corrupt);

    const resRep = runTool(OUR_EXE, ['r', '-y', arc], out);
    assert.notEqual(resRep.code, 0, 'Repairing beyond capacity must report failure');
  });
});
