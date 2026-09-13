// Parity fixtures for gaps identified in rar5-coverage
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { parseArchive, analyze } from '../rar5-coverage.js';
import { freshDir, buildOurArchive, makeFixtureTree } from './helpers.mjs';

describe('Parity fixtures (lock)', () => {
  it('lock flag (-k) sets MHFL_LOCK', () => {
    const tree = freshDir('parity-lock-tree');
    makeFixtureTree(tree);
    const out = freshDir('parity-lock-out');
    const arc = buildOurArchive({ tree, out, switches: ['-k', '-m3'] });
    const buf = readFileSync(arc);
    const p = parseArchive(buf);
    const main = p.blocks.find(b => b.typeName === 'main');
    assert.ok(main.archiveFlags.includes('lock'), `expected lock flag, got ${main.archiveFlags}`);
  });
});
