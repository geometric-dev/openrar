// Multi-Volume Encryption (-hp with -v) and Metadata Parity (-z, -k) test suite
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { parseArchive } from '../rar5-coverage.js';
import { DIR_SEP, freshDir, makeFixtureTree, buildOurArchive, runTool, OUR_EXE, WINRAR_UNRAR, treesEqual, oracleAvailable } from './helpers.mjs';

describe('RAR 5.0 Multi-Volume Encryption & Metadata Parity (-hp, -z, -k with -v)', () => {
  const PASSWORD = 'VolSecretKey987!';
  const WRONG_PWD = 'WrongSecretKey!';

  it('creates multi-volume archive with header encryption (-hp -v5k -m3) and verifies via oracle', () => {
    const tree = freshDir('vol-hp-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-hp-out');

    buildOurArchive({ tree, out, switches: ['-v5k', '-m3', `-hp${PASSWORD}`] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 2, `Expected multiple volumes, got ${volFiles.length}: ${volFiles.join(', ')}`);

    // Verify each volume begins with RAR5 signature followed by HEAD_CRYPT block (0x04)
    for (const vf of volFiles) {
      const data = readFileSync(join(out, vf));
      assert.ok(data.length > 16, `${vf} is too small`);
      // RAR5 signature: 52 61 72 21 1A 07 01 00
      assert.equal(data[0], 0x52);
      assert.equal(data[1], 0x61);
      assert.equal(data[2], 0x72);
      assert.equal(data[3], 0x21);
      assert.equal(data[4], 0x1A);
      assert.equal(data[5], 0x07);
      assert.equal(data[6], 0x01);
      assert.equal(data[7], 0x00);
      // HEAD_CRYPT vint is at offset 13 (after 8 bytes signature + 4 bytes CRC + 1 byte header size vint)
      assert.equal(data[13], 0x04, `${vf} does not have HEAD_CRYPT (0x04) as first block`);
    }

    const firstVol = join(out, volFiles[0]);

    // 1. Listing without password must fail
    const resNoPw = runTool(OUR_EXE, ['l', firstVol], tree);
    assert.notEqual(resNoPw.code, 0, 'Listing -hp multi-volume without password should fail');

    // 2. Listing with wrong password must fail
    const resWrong = runTool(OUR_EXE, ['l', `-p${WRONG_PWD}`, firstVol], tree);
    assert.notEqual(resWrong.code, 0, 'Listing -hp multi-volume with wrong password should fail');

    // 3. Test integrity with correct password
    const resTest = runTool(OUR_EXE, ['t', `-p${PASSWORD}`, firstVol], tree);
    assert.equal(resTest.code, 0, `Self test failed: ${resTest.output}`);

    // 4. Extract with correct password and compare trees
    const extOur = freshDir('vol-hp-ext-our');
    const resOur = runTool(OUR_EXE, ['x', '-y', `-p${PASSWORD}`, firstVol, extOur + DIR_SEP], tree);
    assert.equal(resOur.code, 0, `Self extract failed: ${resOur.output}`);
    const cmpOur = treesEqual(tree, extOur);
    assert.ok(cmpOur.ok, `Self content mismatch: ${cmpOur.why}`);

    // 5. WinRAR Oracle verification
    if (oracleAvailable()) {
      const resWinList = runTool(WINRAR_UNRAR, ['lt', `-p${PASSWORD}`, firstVol], tree);
      assert.equal(resWinList.code, 0, `WinRAR list failed: ${resWinList.output}`);

      const resWinTest = runTool(WINRAR_UNRAR, ['t', `-p${PASSWORD}`, firstVol], tree);
      assert.equal(resWinTest.code, 0, `WinRAR test failed: ${resWinTest.output}`);

      const extWin = freshDir('vol-hp-ext-win');
      const resWinExt = runTool(WINRAR_UNRAR, ['x', '-y', `-p${PASSWORD}`, firstVol, extWin + DIR_SEP], tree);
      assert.equal(resWinExt.code, 0, `WinRAR extract failed: ${resWinExt.output}`);
      const cmpWin = treesEqual(tree, extWin);
      assert.ok(cmpWin.ok, `WinRAR content mismatch: ${cmpWin.why}`);
    }
  });

  it('creates multi-volume archive with comment (-z -v5k -m3) and verifies comment presence', () => {
    const tree = freshDir('vol-cmt-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-cmt-out');

    const commentPath = join(tree, 'comment.txt');
    const commentText = 'OpenRAR multi-volume comment test signature: Alpha-Bravo-42!';
    writeFileSync(commentPath, commentText, 'utf8');

    buildOurArchive({ tree, out, switches: ['-v5k', '-m3', `-z${commentPath}`] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 2, `Expected multiple volumes, got ${volFiles.length}`);

    const firstVol = join(out, volFiles[0]);

    // Check with our listing
    const resList = runTool(OUR_EXE, ['lt', firstVol], tree);
    assert.equal(resList.code, 0, `Our list failed: ${resList.output}`);
    assert.ok(resList.output.includes('Alpha-Bravo-42'), `Comment not found in openrar lt output: ${resList.output}`);

    // Verify with UnRAR oracle
    if (oracleAvailable()) {
      const resWin = runTool(WINRAR_UNRAR, ['lt', firstVol], tree);
      assert.equal(resWin.code, 0, `WinRAR lt failed: ${resWin.output}`);
      assert.ok(resWin.output.includes('Alpha-Bravo-42'), `Comment not found in WinRAR lt output: ${resWin.output}`);
    }
  });

  it('creates multi-volume archive with lock (-k -v5k -m3) and prevents mutation', () => {
    const tree = freshDir('vol-lock-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-lock-out');

    buildOurArchive({ tree, out, switches: ['-v5k', '-m3', '-k'] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 2, `Expected multiple volumes, got ${volFiles.length}`);

    // Parse each volume and verify MHFL_LOCK (0x0004) is set
    for (const vf of volFiles) {
      const p = parseArchive(readFileSync(join(out, vf)));
      const mb = p.blocks.find((b) => b.typeName === 'main');
      assert.ok(mb, `Missing main block in ${vf}`);
      assert.ok(mb.archiveFlags.includes('lock'), `MHFL_LOCK should be set in ${vf}`);
    }

    const firstVol = join(out, volFiles[0]);

    // Attempting to add a file to locked volume archive must fail
    const dummyFile = join(tree, 'extra.txt');
    writeFileSync(dummyFile, 'forbidden addition');
    const resAdd = runTool(OUR_EXE, ['a', firstVol, dummyFile], tree);
    assert.notEqual(resAdd.code, 0, 'Adding to locked multi-volume archive should fail');

    // WinRAR Oracle verification
    if (oracleAvailable()) {
      const resWin = runTool(WINRAR_UNRAR, ['lt', firstVol], tree);
      assert.equal(resWin.code, 0, `WinRAR lt failed: ${resWin.output}`);
      assert.ok(resWin.output.toLowerCase().includes('lock'), `Lock attribute missing in WinRAR lt output: ${resWin.output}`);
    }
  });

  it('locks existing multi-volume archive via command k', () => {
    const tree = freshDir('vol-cmd-k-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-cmd-k-out');

    buildOurArchive({ tree, out, switches: ['-v5k', '-m3'] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 2, `Expected multiple volumes, got ${volFiles.length}`);
    const firstVol = join(out, volFiles[0]);

    // Ensure it was unlocked initially
    const pBefore = parseArchive(readFileSync(firstVol));
    const mbBefore = pBefore.blocks.find((b) => b.typeName === 'main');
    assert.ok(!mbBefore.archiveFlags.includes('lock'), 'Archive should be unlocked initially');

    // Run command 'k'
    const resK = runTool(OUR_EXE, ['k', firstVol], tree);
    assert.equal(resK.code, 0, `openrar k failed: ${resK.output}`);

    // Verify all volumes now have MHFL_LOCK
    for (const vf of volFiles) {
      const p = parseArchive(readFileSync(join(out, vf)));
      const mb = p.blocks.find((b) => b.typeName === 'main');
      assert.ok(mb, `Missing main block in ${vf}`);
      assert.ok(mb.archiveFlags.includes('lock'), `MHFL_LOCK should be set in ${vf} after command k`);
    }

    // Verify WinRAR oracle sees lock
    if (oracleAvailable()) {
      const resWin = runTool(WINRAR_UNRAR, ['lt', firstVol], tree);
      assert.equal(resWin.code, 0, `WinRAR lt failed: ${resWin.output}`);
      assert.ok(resWin.output.toLowerCase().includes('lock'), `Lock attribute missing in WinRAR lt output: ${resWin.output}`);
    }
  });

  it('combines -hp, -z, -k with -v in a single multi-volume set', () => {
    const tree = freshDir('vol-combo-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-combo-out');

    const commentPath = join(tree, 'comment.txt');
    const commentText = 'Multi-volume encrypted and locked combo comment!';
    writeFileSync(commentPath, commentText, 'utf8');

    buildOurArchive({ tree, out, switches: ['-v5k', '-m3', `-hp${PASSWORD}`, `-z${commentPath}`, '-k'] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 2, `Expected multiple volumes, got ${volFiles.length}`);
    const firstVol = join(out, volFiles[0]);

    // 1. Listing without password fails
    const resNoPw = runTool(OUR_EXE, ['lt', firstVol], tree);
    assert.notEqual(resNoPw.code, 0, 'Listing without password should fail');

    // 2. Listing with password shows comment and lock
    const resList = runTool(OUR_EXE, ['lt', `-p${PASSWORD}`, firstVol], tree);
    assert.equal(resList.code, 0, `Listing failed: ${resList.output}`);
    assert.ok(resList.output.includes('Multi-volume encrypted and locked combo comment'), 'Comment missing');
    assert.ok(resList.output.toLowerCase().includes('lock'), 'Lock status missing');

    // 3. Extraction with password roundtrips cleanly
    const extOur = freshDir('vol-combo-ext-our');
    const resOur = runTool(OUR_EXE, ['x', '-y', `-p${PASSWORD}`, firstVol, extOur + DIR_SEP], tree);
    assert.equal(resOur.code, 0, `Extraction failed: ${resOur.output}`);
    const cmpOur = treesEqual(tree, extOur);
    assert.ok(cmpOur.ok, `Content mismatch: ${cmpOur.why}`);

    // 4. WinRAR Oracle test
    if (oracleAvailable()) {
      const resWinList = runTool(WINRAR_UNRAR, ['lt', `-p${PASSWORD}`, firstVol], tree);
      assert.equal(resWinList.code, 0, `WinRAR lt failed: ${resWinList.output}`);
      assert.ok(resWinList.output.includes('Multi-volume encrypted and locked combo comment'), 'WinRAR comment missing');

      const resWinTest = runTool(WINRAR_UNRAR, ['t', `-p${PASSWORD}`, firstVol], tree);
      assert.equal(resWinTest.code, 0, `WinRAR test failed: ${resWinTest.output}`);

      const extWin = freshDir('vol-combo-ext-win');
      const resWinExt = runTool(WINRAR_UNRAR, ['x', '-y', `-p${PASSWORD}`, firstVol, extWin + DIR_SEP], tree);
      assert.equal(resWinExt.code, 0, `WinRAR extract failed: ${resWinExt.output}`);
      const cmpWin = treesEqual(tree, extWin);
      assert.ok(cmpWin.ok, `WinRAR extract mismatch: ${cmpWin.why}`);
    }
  });
});
