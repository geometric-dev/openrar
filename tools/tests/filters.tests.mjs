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
  oracleAvailable,
  runTool, } from './helpers.mjs';
import { parseArchive } from '../rar5-coverage.js';

describe('RAR 5.0 / RAR 3.x Filter Specification & -mc Switch Handling', () => {
  it('accepts global filter disable switch (-mc-)', () => {
    const tree = freshDir('filter-mc-tree');
    makeFixtureTree(tree);
    const out = freshDir('filter-mc-out');
    const arc = join(out, 'arc_mc_disable.rar');

    const res = runTool(OUR_EXE, ['a', '-y', '-mc-', arc, '.'], tree);
    assert.equal(res.code, 0, `Archiving with -mc- failed: ${res.output}`);

    const resTest = runTool(OUR_EXE, ['t', '-y', arc], out);
    assert.equal(resTest.code, 0, `Testing -mc- archive failed: ${resTest.output}`);

    // Oracle-gated: without an UnRAR binary (Linux CI) the self test above
    // still verifies the archive.
    if (oracleAvailable()) {
      const resWinTest = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resWinTest.code, 0, `WinRAR testing -mc- archive failed: ${resWinTest.output}`);
    }
  });

  it('accepts explicit filter enable and disable switches (-mcE+, -mcE-, -mcD+, -mcD-)', () => {
    const tree = freshDir('filter-switches-tree');
    makeFixtureTree(tree);
    const out = freshDir('filter-switches-out');

    const filterConfigs = [
      { name: 'e8-force', switches: ['-mcE+'] },
      { name: 'e8-disable', switches: ['-mcE-'] },
      { name: 'e8-auto', switches: ['-mcE'] },
      { name: 'delta-force', switches: ['-mcD+'] },
      { name: 'delta-disable', switches: ['-mcD-'] },
      { name: 'delta-params', switches: ['-mc16:4D+'] },
      { name: 'longrange-force', switches: ['-mcL+'] },
      { name: 'exhaustive-disable', switches: ['-mcX-'] },
      { name: 'compound-filters', switches: ['-mcE+D-'] },
    ];

    for (const { name, switches } of filterConfigs) {
      const arc = join(out, `arc_${name}.rar`);
      const res = runTool(OUR_EXE, ['a', '-y', ...switches, arc, '.'], tree);
      assert.equal(res.code, 0, `Archiving with ${switches.join(' ')} failed: ${res.output}`);

      // Verify listing
      const resList = runTool(OUR_EXE, ['lt', arc], out);
      assert.equal(resList.code, 0, `Listing failed for ${name}: ${resList.output}`);

      // Test integrity with OpenRAR
      const resTest = runTool(OUR_EXE, ['t', '-y', arc], out);
      assert.equal(resTest.code, 0, `Self-test failed for ${name}: ${resTest.output}`);

      // Dual-oracle verification with WinRAR UnRAR (oracle-gated)
      if (oracleAvailable()) {
        const resWinTest = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
        assert.equal(resWinTest.code, 0, `WinRAR test failed for ${name}: ${resWinTest.output}`);
      }
    }
  });

  it('preserves byte-exact roundtrip extraction across various -mc filter modes', () => {
    const tree = freshDir('filter-extract-tree');
    makeFixtureTree(tree);
    const out = freshDir('filter-extract-out');
    const extractDir = freshDir('filter-extract-dest');

    const arc = join(out, 'arc_filtered.rar');
    const resAdd = runTool(OUR_EXE, ['a', '-y', '-mcE+', '-mcD-', '-m3', arc, '.'], tree);
    assert.equal(resAdd.code, 0, `Archiving failed: ${resAdd.output}`);

    const resX = runTool(OUR_EXE, ['x', '-y', arc, extractDir + DIR_SEP], out);
    assert.equal(resX.code, 0, `Extraction failed: ${resX.output}`);

    // Verify extracted files match original fixture
    const origText = readFileSync(join(tree, 'text.txt'));
    const extrText = readFileSync(join(extractDir, 'text.txt'));
    assert.deepEqual(extrText, origText, 'text.txt mismatch');

    const origBin = readFileSync(join(tree, 'data.bin'));
    const extrBin = readFileSync(join(extractDir, 'data.bin'));
    assert.deepEqual(extrBin, origBin, 'data.bin mismatch');
  });
});
