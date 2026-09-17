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
import { DIR_SEP, OUR_EXE,
  WINRAR_UNRAR,
  buildOurArchive,
  freshDir,
  makeFixtureTree,
  runTool,
  oracleAvailable, } from './helpers.mjs';
import { parseArchive, analyze } from '../rar5-coverage.js';

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

    // 3. Dual-oracle test with WinRAR (oracle-gated)
    if (oracleAvailable()) {
      const resWinTest = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resWinTest.code, 0, `WinRAR test failed: ${resWinTest.output}`);
    }

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

    // Verify WinRAR test fails on corrupted archive (oracle-gated; the self
    // repair + extraction below still verify the whole path everywhere)
    if (oracleAvailable()) {
      const resDamaged = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.notEqual(resDamaged.code, 0, 'Damaged archive should fail WinRAR test');
    }

    // 3. Run OpenRAR repair command (r)
    const resRepair = runTool(OUR_EXE, ['r', '-y', arc], out);
    assert.equal(resRepair.code, 0, `Repair failed: ${resRepair.output}`);

    // 4. Verify repaired archive passes WinRAR test (oracle-gated)
    if (oracleAvailable()) {
      const resRepairedTest = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resRepairedTest.code, 0, `Repaired archive failed WinRAR test: ${resRepairedTest.output}`);
    }

    // 5. Verify byte-identical file extraction
    const extDir = freshDir('rec-rep-ext');
    const resExt = runTool(OUR_EXE, ['x', '-y', arc, extDir + DIR_SEP], tree);
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

    // 4. Verify WinRAR test passes (oracle-gated)
    if (oracleAvailable()) {
      const resWin = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resWin.code, 0, `WinRAR test on repaired archive failed: ${resWin.output}`);
    }

    // 5. Extract and verify full fixture tree
    const extDir = freshDir('rec-mf-ext');
    const resExt = runTool(OUR_EXE, ['x', '-y', arc, extDir + DIR_SEP], tree);
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

  it('strips QuickOpen locator and QO service block when adding -rr to a QO-enabled archive', () => {
    // Structural regression test for the QO+RR locator size-mismatch bug
    // (commit 9b672b0). add_recovery_record must strip both the QO service
    // block and locator_qo_offset. If it doesn't, the combined QO+RR locator
    // extra record is 10 bytes larger than the source's QO-only locator,
    // shifting the QO block's real position while the stored offset points
    // to the old location — WinRAR rejects this as "Main archive header is
    // corrupt".
    const tree = freshDir('rec-qo-strip-tree');
    makeFixtureTree(tree);
    const out = freshDir('rec-qo-strip-out');
    const arc = join(out, 'qo_then_rr.rar');

    // Step 1: Create a QO-enabled archive (default behaviour writes QO).
    const resAdd = runTool(OUR_EXE, ['a', '-y', '-m0', arc, '.'], tree);
    assert.equal(resAdd.code, 0, `QO archive creation failed: ${resAdd.output}`);

    // Confirm QO was written (pre-condition).
    const beforeBuf = readFileSync(arc);
    const beforeFeatures = analyze(parseArchive(beforeBuf));
    assert.ok(beforeFeatures.locatorQuickOpen,
      'Source archive must have QO locator (pre-condition)');
    assert.ok(
      parseArchive(beforeBuf).blocks.some((b) => b.file?.service === 'QO'),
      'Source archive must have QO service block (pre-condition)',
    );

    // Step 2: Add recovery record to the QO-enabled archive.
    const resRr = runTool(OUR_EXE, ['a', '-y', '-rr5%', arc], out);
    assert.equal(resRr.code, 0, `Adding RR to QO archive failed: ${resRr.output}`);

    // Step 3: Structural assertions — QO must be fully stripped.
    const afterBuf = readFileSync(arc);
    const afterParsed = parseArchive(afterBuf);
    const afterFeatures = analyze(afterParsed);

    assert.ok(!afterFeatures.locatorQuickOpen,
      'After adding RR, locatorQuickOpen flag must be absent (QO locator stripped)');
    assert.ok(!afterParsed.blocks.some((b) => b.file?.service === 'QO'),
      'After adding RR, QO service block must be absent');
    assert.ok(afterFeatures.locatorRR,
      'After adding RR, locatorRR flag must be present');
    assert.ok(afterParsed.blocks.some((b) => b.file?.service === 'RR'),
      'After adding RR, RR service block must be present');

    // Step 4: WinRAR oracle validation (catches "Main archive header is corrupt").
    if (oracleAvailable()) {
      const resWin = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resWin.code, 0,
        `WinRAR rejected the QO+RR archive: ${resWin.output}`);
    }
  });

  it('applying -rr twice (idempotency) produces a valid archive', () => {
    // Regression guard: calling add_recovery_record on an archive that already
    // has an RR (but no QO, since the first -rr strips it) must not corrupt
    // the main header or introduce stale locator offsets.
    const tree = freshDir('rec-idempotent-tree');
    makeFixtureTree(tree);
    const out = freshDir('rec-idempotent-out');
    const arc = join(out, 'double_rr.rar');

    // First -rr pass.
    runTool(OUR_EXE, ['a', '-y', '-m0', '-rr5%', arc, '.'], tree);
    assert.ok(existsSync(arc), 'Archive must exist after first -rr');

    // Second -rr pass on an already-RR archive.
    const resRr2 = runTool(OUR_EXE, ['a', '-y', '-rr5%', arc], out);
    assert.equal(resRr2.code, 0, `Second -rr pass failed: ${resRr2.output}`);

    // Structural check: single RR block, no QO block.
    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);
    const features = analyze(parsed);
    const rrBlocks = parsed.blocks.filter((b) => b.file?.service === 'RR');
    assert.equal(rrBlocks.length, 1,
      'Exactly one RR service block must be present after double -rr');
    assert.ok(!features.locatorQuickOpen,
      'QO locator must be absent after double -rr');
    assert.ok(features.locatorRR,
      'RR locator must be present after double -rr');

    // Oracle validation.
    if (oracleAvailable()) {
      const resWin = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resWin.code, 0,
        `WinRAR rejected double-rr archive: ${resWin.output}`);
    }

    // Extraction must still work.
    const extDir = freshDir('rec-idempotent-ext');
    const resExt = runTool(OUR_EXE, ['x', '-y', arc, extDir + DIR_SEP], tree);
    assert.equal(resExt.code, 0,
      `Extraction failed after double -rr: ${resExt.output}`);
  });
});
