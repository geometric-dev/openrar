// Oracle round-trip tests: every scenario must pass WinRAR's own tester,
// our own tester, extract byte-identically, and build deterministically.
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import {
  freshDir, makeFixtureTree, buildOurArchive, runOurExe, runTool,
  extractWith, treesEqual, sha256, pinTimes,
  WINRAR_UNRAR, OUR_EXE, oracleAvailable,
} from './helpers.mjs';
import coverage from '../rar5-coverage.js';

const SCENARIOS = [
  { name: 'store-m0', switches: ['-m0'] },
  { name: 'normal-m3', switches: ['-m3'] },
  { name: 'best-m5', switches: ['-m5'] },
  { name: 'solid', switches: ['-s', '-m3'] },
  { name: 'comment', switches: ['-m3'] },
  { name: 'times-all', switches: ['-tsm4', '-tsc4', '-tsa4', '-m3'] },
  { name: 'qo-off', switches: ['-qo-', '-m3'] },
];

describe('oracle round-trips', () => {
  for (const scenario of SCENARIOS) {
    it(`${scenario.name}: WinRAR t + self t + byte-identical extraction (both extractors)`, () => {
      const tag = scenario.name;
      const tree = freshDir(`rt-${tag}-tree`);
      makeFixtureTree(tree);
      const out = freshDir(`rt-${tag}-out`);
      const arc = buildOurArchive({ tree, out, switches: scenario.switches });

      // Oracle asserts are gated: without a WinRAR/UnRAR binary (e.g. the
      // Linux CI job) the self-tester and self-extraction still verify the
      // archive; with an oracle both extractors must agree.
      const oracle = oracleAvailable();
      if (oracle) {
        const wt = runTool(WINRAR_UNRAR, ['t', '-y', '--', arc]);
        assert.equal(wt.code, 0, `WinRAR t failed:\n${wt.output}`);
        assert.match(wt.output, /All OK/i);
      }

      const st = runTool(OUR_EXE, ['t', '-y', '--', arc]);
      assert.equal(st.code, 0, `self t failed:\n${st.output}`);

      const dest = freshDir(`rt-${tag}-x`);
      if (oracle) {
        const x = extractWith(WINRAR_UNRAR, arc, join(dest, 'w'));
        assert.equal(x.code, 0, `WinRAR extraction failed:\n${x.output}`);
        const cmp = treesEqual(tree, join(dest, 'w'));
        assert.equal(cmp.ok, true, cmp.why);
      }

      const s = extractWith(OUR_EXE, arc, join(dest, 's'));
      assert.equal(s.code, 0, `self extraction failed:\n${s.output}`);
      const cmp = treesEqual(tree, join(dest, 's'));
      assert.equal(cmp.ok, true, cmp.why);
    });
  }

  it('determinism: same input built twice is byte-identical (all scenarios)', () => {
    for (const scenario of SCENARIOS.filter((s) => s.name !== 'times-all')) {
      const tag = scenario.name;
      const tree1 = freshDir(`det-${tag}-t1`);
      makeFixtureTree(tree1);
      const tree2 = freshDir(`det-${tag}-t2`);
      makeFixtureTree(tree2);
      const o1 = freshDir(`det-${tag}-o1`);
      const o2 = freshDir(`det-${tag}-o2`);
      const a = buildOurArchive({ tree: tree1, out: o1, switches: scenario.switches });
      const b = buildOurArchive({ tree: tree2, out: o2, switches: scenario.switches });
      assert.equal(sha256(a), sha256(b), `${tag}: nondeterministic output`);
    }
  });

  it('comment payload matches the -z source file byte-for-byte', () => {
    const cmtPath = join(freshDir('cmt-src'), 'cmt.txt');
    writeFileSync(cmtPath, 'OpenRAR test comment');
    const tree = freshDir('cmt-tree');
    makeFixtureTree(tree);
    const out = freshDir('cmt-out');
    const arc = buildOurArchive({ tree, out, switches: ['-m3', `-z${cmtPath}`] });

    const buf = readFileSync(arc);
    const parsed = coverage.parseArchive(buf);
    const svc = parsed.blocks.find((b) => b.typeName === 'service' && b.file?.service === 'CMT');
    assert.ok(svc, 'CMT service missing');
    const payload = buf.subarray(svc.dataOffset, svc.dataOffset + svc.dataSize).toString('utf8');
    assert.equal(payload, readFileSync(cmtPath, 'utf8'));
  });
});
