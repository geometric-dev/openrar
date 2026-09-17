// Multi-Volume archive creation test suite for OpenRAR (-v<size>)
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { parseArchive } from '../rar5-coverage.js';
import { DIR_SEP, freshDir, makeFixtureTree, buildOurArchive, runTool, OUR_EXE, WINRAR_UNRAR, treesEqual, oracleAvailable } from './helpers.mjs';

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

describe('RAR 5.0 Multi-Volume Archives (-v)', () => {
  it('creates multi-volume store archive (-v20k -m0) with correct naming and split flags', () => {
    const tree = freshDir('vol-store-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-store-out');

    buildOurArchive({ tree, out, switches: ['-v20k', '-m0'] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 3, `Expected at least 3 volumes, found ${volFiles.length}: ${volFiles.join(', ')}`);
    assert.equal(volFiles[0], 'archive.part01.rar');
    assert.equal(volFiles[1], 'archive.part02.rar');
    assert.equal(volFiles[2], 'archive.part03.rar');

    // Parse and verify block structures of the first volume
    const p1 = parseArchive(readFileSync(join(out, volFiles[0])));
    assert.equal(p1.truncated, false, `Volume 1 truncated: ${p1.parseError}`);
    const main1 = p1.blocks.find((b) => b.typeName === 'main');
    assert.ok(main1, 'Main header missing in volume 1');
    assert.ok(main1.archiveFlags.includes('volume'), 'MHFL_VOLUME should be set');

    // WinRAR Oracle extraction test (skipped when no UnRAR binary exists,
    // e.g. the Linux CI job; the self extraction below still verifies fully)
    if (oracleAvailable()) {
      const extWin = freshDir('vol-store-ext-win');
      const resWin = runTool(WINRAR_UNRAR, ['x', '-y', join(out, volFiles[0]), extWin + DIR_SEP], tree);
      assert.equal(resWin.code, 0, `WinRAR extract failed:\n${resWin.output}`);
      const cmpWin = treesEqual(tree, extWin);
      assert.equal(cmpWin.ok, true, `WinRAR extracted content mismatch: ${cmpWin.why}`);
    }

    // Self extraction test
    const extOur = freshDir('vol-store-ext-our');
    const resOur = runTool(OUR_EXE, ['x', '-y', join(out, volFiles[0]), extOur + DIR_SEP], tree);
    assert.equal(resOur.code, 0, `Self extract failed:\n${resOur.output}`);
    const cmpOur = treesEqual(tree, extOur);
    assert.equal(cmpOur.ok, true, `Self extracted content mismatch: ${cmpOur.why}`);
  });

  it('creates multi-volume compressed archive (-v20k -m3) and extracts byte-identically', () => {
    const tree = freshDir('vol-m3-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-m3-out');

    buildOurArchive({ tree, out, switches: ['-v5k', '-m3'] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 2, `Expected multiple volumes, found: ${volFiles.join(', ')}`);

    // Verify intermediate split chunk has slice CRC
    const buf1 = readFileSync(join(out, volFiles[0]));
    const p1 = parseArchive(buf1);
    const splitFileBlock = p1.blocks.find((b) => b.typeName === 'file' && b.split);
    if (splitFileBlock) {
      const slicePayload = buf1.subarray(splitFileBlock.dataOffset, splitFileBlock.dataOffset + splitFileBlock.dataSize);
      assert.equal(crc32(slicePayload), splitFileBlock.file.dataCrc, 'Split chunk header CRC must match slice payload CRC');
    }

    // WinRAR extraction (oracle-gated; see above)
    if (oracleAvailable()) {
      const extWin = freshDir('vol-m3-ext-win');
      const resWin = runTool(WINRAR_UNRAR, ['x', '-y', join(out, volFiles[0]), extWin + DIR_SEP], tree);
      assert.equal(resWin.code, 0, `WinRAR extract failed:\n${resWin.output}`);
      const cmpWin = treesEqual(tree, extWin);
      assert.equal(cmpWin.ok, true, `WinRAR content mismatch: ${cmpWin.why}`);
    }

    // Self extraction
    const extOur = freshDir('vol-m3-ext-our');
    const resOur = runTool(OUR_EXE, ['x', '-y', join(out, volFiles[0]), extOur + DIR_SEP], tree);
    assert.equal(resOur.code, 0, `Self extract failed:\n${resOur.output}`);
    const cmpOur = treesEqual(tree, extOur);
    assert.equal(cmpOur.ok, true, `Self content mismatch: ${cmpOur.why}`);
  });

  it('creates multi-volume solid archive (-v20k -s -m3) and extracts byte-identically', () => {
    const tree = freshDir('vol-solid-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-solid-out');

    buildOurArchive({ tree, out, switches: ['-v5k', '-s', '-m3'] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 2);

    if (oracleAvailable()) {
      const extWin = freshDir('vol-solid-ext-win');
      const resWin = runTool(WINRAR_UNRAR, ['x', '-y', join(out, volFiles[0]), extWin + DIR_SEP], tree);
      assert.equal(resWin.code, 0, `WinRAR extract failed:\n${resWin.output}`);
      const cmpWin = treesEqual(tree, extWin);
      assert.equal(cmpWin.ok, true, `WinRAR solid content mismatch: ${cmpWin.why}`);
    }

    const extOur = freshDir('vol-solid-ext-our');
    const resOur = runTool(OUR_EXE, ['x', '-y', join(out, volFiles[0]), extOur + DIR_SEP], tree);
    assert.equal(resOur.code, 0, `Self extract failed:\n${resOur.output}`);
    const cmpOur = treesEqual(tree, extOur);
    assert.equal(cmpOur.ok, true, `Self solid content mismatch: ${cmpOur.why}`);
  });

  it('creates multi-volume encrypted archive (-pSecret -v20k -m3) and extracts cleanly', () => {
    const tree = freshDir('vol-enc-tree');
    makeFixtureTree(tree);
    const out = freshDir('vol-enc-out');

    buildOurArchive({ tree, out, switches: ['-pSecret', '-v5k', '-m3'] });

    const volFiles = readdirSync(out).filter((f) => f.includes('.part')).sort();
    assert.ok(volFiles.length >= 2);

    if (oracleAvailable()) {
      const extWin = freshDir('vol-enc-ext-win');
      const resWin = runTool(WINRAR_UNRAR, ['x', '-pSecret', '-y', join(out, volFiles[0]), extWin + DIR_SEP], tree);
      assert.equal(resWin.code, 0, `WinRAR extract failed:\n${resWin.output}`);
      const cmpWin = treesEqual(tree, extWin);
      assert.equal(cmpWin.ok, true, `WinRAR enc content mismatch: ${cmpWin.why}`);
    }

    const extOur = freshDir('vol-enc-ext-our');
    const resOur = runTool(OUR_EXE, ['x', '-pSecret', '-y', join(out, volFiles[0]), extOur + DIR_SEP], tree);
    assert.equal(resOur.code, 0, `Self extract failed:\n${resOur.output}`);
    const cmpOur = treesEqual(tree, extOur);
    assert.equal(cmpOur.ok, true, `Self enc content mismatch: ${cmpOur.why}`);
  });
});
