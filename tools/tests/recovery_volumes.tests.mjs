import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import {
  existsSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { join } from 'node:path';
import {
  DIR_SEP,
  OUR_EXE,
  WINRAR_UNRAR,
  freshDir,
  runTool,
  oracleAvailable,
} from './helpers.mjs';

describe('RAR 5.0 Standalone Recovery Volumes (.rev / -rv) & Parity Engine', () => {
  it('handles multi-chunk (>1 MiB per volume) RS16 parity generation and byte-identical repair', () => {
    const tree = freshDir('rec-multichunk-tree');
    // Generate 2.5 MiB of pseudorandom deterministic data
    const totalBytes = 2500 * 1024;
    const payload = Buffer.alloc(totalBytes);
    for (let i = 0; i < totalBytes; i++) {
      payload[i] = (i * 31 + 17) & 0xff;
    }
    writeFileSync(join(tree, 'large_payload.bin'), payload);

    const out = freshDir('rec-multichunk-out');
    const arc = join(out, 'multichunk.part1.rar');

    // 1. Create multi-volume set with -v1200k (each volume > 1 MiB CHUNK) and -rv2
    const resAdd = runTool(
      OUR_EXE,
      ['a', '-y', '-m0', '-v1200k', '-rv2', arc, 'large_payload.bin'],
      tree
    );
    assert.equal(resAdd.code, 0, `Multi-chunk volume creation failed: ${resAdd.output}`);

    const part1 = join(out, 'multichunk.part1.rar');
    const part2 = join(out, 'multichunk.part2.rar');
    const part3 = join(out, 'multichunk.part3.rar');
    const rev1 = join(out, 'multichunk.part1.rev');
    const rev2 = join(out, 'multichunk.part2.rev');

    assert.ok(existsSync(part1), 'part1 must exist');
    assert.ok(existsSync(part2), 'part2 must exist');
    assert.ok(existsSync(part3), 'part3 must exist');
    assert.ok(existsSync(rev1), 'rev1 must exist');
    assert.ok(existsSync(rev2), 'rev2 must exist');

    // Save originals for byte-identical comparison
    const origPart2 = readFileSync(part2);
    const origPart3 = readFileSync(part3);

    // 2. Delete part2 (which spans across the 1 MiB chunk boundary)
    rmSync(part2);
    assert.ok(!existsSync(part2), 'part2 was deleted');

    // 3. Repair by specifying part1
    const resRepair = runTool(OUR_EXE, ['r', '-y', part1], out);
    assert.equal(resRepair.code, 0, `Repair failed: ${resRepair.output}`);
    assert.ok(existsSync(part2), 'part2 must be reconstructed');

    const reconPart2 = readFileSync(part2);
    assert.deepEqual(reconPart2, origPart2, 'Reconstructed part2 must be byte-identical to original');

    // 4. Delete part3 and repair by specifying missing part3 directly!
    rmSync(part3);
    assert.ok(!existsSync(part3), 'part3 was deleted');

    const resRepairMissing = runTool(OUR_EXE, ['r', '-y', part3], out);
    assert.equal(
      resRepairMissing.code,
      0,
      `Direct repair of missing volume path failed: ${resRepairMissing.output}`
    );
    assert.ok(existsSync(part3), 'part3 must be reconstructed from direct repair invocation');

    const reconPart3 = readFileSync(part3);
    assert.deepEqual(reconPart3, origPart3, 'Reconstructed part3 must be byte-identical to original');

    // 5. Verify full extraction of all data
    const extDir = freshDir('rec-multichunk-ext');
    const resExt = runTool(OUR_EXE, ['x', '-y', part1, extDir + DIR_SEP], out);
    assert.equal(resExt.code, 0, `Extraction failed: ${resExt.output}`);
    const extracted = readFileSync(join(extDir, 'large_payload.bin'));
    assert.deepEqual(extracted, payload, 'Extracted file must match original 2.5 MiB payload byte-for-byte');
  });

  it('normalizes archive path in standalone rv command (supports base name)', () => {
    const tree = freshDir('rec-rv-norm-tree');
    writeFileSync(join(tree, 'file.dat'), Buffer.alloc(40 * 1024, 0xaa));

    const out = freshDir('rec-rv-norm-out');
    const arcPart1 = join(out, 'archive.part1.rar');
    const arcBase = join(out, 'archive.rar');

    // 1. Create multi-volume set without .rev
    const resAdd = runTool(OUR_EXE, ['a', '-y', '-m0', '-v15k', arcPart1, 'file.dat'], tree);
    assert.equal(resAdd.code, 0, `Archive creation failed: ${resAdd.output}`);

    const rev1 = join(out, 'archive.part1.rev');
    assert.ok(!existsSync(rev1), 'rev1 should not exist yet');

    // 2. Invoke rv command passing the non-existent base name 'archive.rar'
    const resRv = runTool(OUR_EXE, ['rv1', arcBase], out);
    assert.equal(resRv.code, 0, `rv with base name failed: ${resRv.output}`);
    assert.ok(existsSync(rev1), 'rev1 must exist after rv command with normalized base name');

    // 3. Test with percentage syntax 'rv50%'
    const resRvPct = runTool(OUR_EXE, ['rv50%', arcBase], out);
    assert.equal(resRvPct.code, 0, `rv percentage failed: ${resRvPct.output}`);
  });

  it('verifies dual-oracle compatibility with reference WinRAR / UnRAR 7.20', () => {
    if (!oracleAvailable()) return;

    const tree = freshDir('rec-oracle-tree');
    const payload = Buffer.alloc(50 * 1024, 0x3c);
    writeFileSync(join(tree, 'data.bin'), payload);

    const out = freshDir('rec-oracle-out');
    const arc = join(out, 'oracle_set.part1.rar');

    // Create multi-volume set with -rv1
    const resAdd = runTool(OUR_EXE, ['a', '-y', '-m0', '-v20k', '-rv1', arc, 'data.bin'], tree);
    assert.equal(resAdd.code, 0, `Creation failed: ${resAdd.output}`);

    const part1 = join(out, 'oracle_set.part1.rar');
    const part2 = join(out, 'oracle_set.part2.rar');

    // Official WinRAR test command on our volumes
    const resWinTest = runTool(WINRAR_UNRAR, ['t', '-y', part1], out);
    assert.equal(resWinTest.code, 0, `WinRAR test of OpenRAR volume set failed: ${resWinTest.output}`);

    // Delete part2 and let OpenRAR reconstruct
    rmSync(part2);
    const resRepair = runTool(OUR_EXE, ['r', '-y', part1], out);
    assert.equal(resRepair.code, 0, `OpenRAR repair failed: ${resRepair.output}`);

    // Official WinRAR tests the reconstructed volume
    const resWinReconTest = runTool(WINRAR_UNRAR, ['t', '-y', part1], out);
    assert.equal(
      resWinReconTest.code,
      0,
      `WinRAR test of reconstructed volume set failed: ${resWinReconTest.output}`
    );
  });
});
