// Encryption test suite for OpenRAR (-p and -hp)
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { freshDir, makeFixtureTree, buildOurArchive, runTool, OUR_EXE, WINRAR_UNRAR, treesEqual } from './helpers.mjs';

describe('RAR 5.0 Encryption (-p / -hp)', () => {
  const PASSWORD = 'CorrectHorseBatteryStaple123!';
  const WRONG_PWD = 'WrongPassword!';

  describe('File data encryption (-p)', () => {
    for (const method of ['-m0', '-m3', '-m5']) {
      it(`encrypts data under ${method} and round-trips cleanly`, () => {
        const tree = freshDir(`enc-p-${method}-tree`);
        makeFixtureTree(tree);
        const out = freshDir(`enc-p-${method}-out`);
        const arc = buildOurArchive({ tree, out, switches: [method, `-p${PASSWORD}`] });

        // 1. Extract with our extractor
        const extOur = freshDir(`enc-p-${method}-ext-our`);
        const resOur = runTool(OUR_EXE, ['x', '-y', `-p${PASSWORD}`, arc, extOur + '\\'], tree);
        assert.equal(resOur.code, 0, `our extract failed: ${resOur.output}`);

        // Compare extracted files with original tree
        const eq = treesEqual(tree, extOur);
        assert.ok(eq.ok, eq.why);

        // 2. Oracle test with WinRAR if available
        const resWinRar = runTool(WINRAR_UNRAR, ['t', `-p${PASSWORD}`, arc], tree);
        if (resWinRar.code !== 127) { // 127 = binary not found
          assert.equal(resWinRar.code, 0, `WinRAR test failed: ${resWinRar.output}`);
        }

        // 3. Fast failure with wrong password
        const extWrong = freshDir(`enc-p-${method}-ext-wrong`);
        const resWrong = runTool(OUR_EXE, ['x', '-y', `-p${WRONG_PWD}`, arc, extWrong + '\\'], tree);
        assert.notEqual(resWrong.code, 0, 'Extraction with wrong password should fail');
      });
    }
  });

  describe('Header encryption (-hp)', () => {
    it('encrypts header stream, prevents listing without password, and verifies via oracle', () => {
      const tree = freshDir('enc-hp-tree');
      makeFixtureTree(tree);
      const out = freshDir('enc-hp-out');
      const arc = buildOurArchive({ tree, out, switches: ['-m3', `-hp${PASSWORD}`] });

      // 1. Listing without password should fail
      const resNoPwd = runTool(OUR_EXE, ['l', arc], tree);
      assert.notEqual(resNoPwd.code, 0, 'Listing -hp archive without password should fail');

      // 2. Extraction with correct password
      const extOur = freshDir('enc-hp-ext-our');
      const resOur = runTool(OUR_EXE, ['x', '-y', `-p${PASSWORD}`, arc, extOur + '\\'], tree);
      assert.equal(resOur.code, 0, `extract failed: ${resOur.output}`);

      const eq = treesEqual(tree, extOur);
      assert.ok(eq.ok, eq.why);

      // 3. Fast rejection with wrong password
      const resWrong = runTool(OUR_EXE, ['l', `-p${WRONG_PWD}`, arc], tree);
      assert.notEqual(resWrong.code, 0, 'Listing with wrong password should fail');

      // 4. WinRAR oracle verification
      const resWinRar = runTool(WINRAR_UNRAR, ['t', `-p${PASSWORD}`, arc], tree);
      if (resWinRar.code !== 127) {
        assert.equal(resWinRar.code, 0, `WinRAR test failed: ${resWinRar.output}`);
      }
    });
  });
});
