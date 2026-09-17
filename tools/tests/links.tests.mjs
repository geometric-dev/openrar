// Link / Redirection test suite for OpenRAR (-ol)
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { symlinkSync, writeFileSync, readlinkSync, existsSync } from 'node:fs';
import { parseArchive } from '../rar5-coverage.js';
import { DIR_SEP, freshDir, makeFixtureTree, buildOurArchive, runTool, OUR_EXE, WINRAR_UNRAR, readFileSync } from './helpers.mjs';

describe('RAR 5.0 Redirections and Links (-ol)', () => {
  it('archives and preserves symlinks with FHEXTRA_REDIR records', () => {
    const tree = freshDir('links-test-tree');
    makeFixtureTree(tree);

    // Create relative file symlinks
    let hasSymlinks = false;
    try {
      symlinkSync('text.txt', join(tree, 'link_to_text.txt'), 'file');
      symlinkSync('dirA/nested', join(tree, 'link_to_dir'), 'dir');
      hasSymlinks = true;
    } catch (e) {
      console.log('Skipping OS symlink creation (insufficient privileges):', e.message);
    }

    const out = freshDir('links-test-out');
    const arc = buildOurArchive({ tree, out, switches: ['-ol', '-m3'] });

    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);

    assert.equal(parsed.truncated, false, `Archive truncated: ${parsed.parseError}`);
    assert.equal(parsed.encryptedHeaders, false);

    if (hasSymlinks) {
      const fileLinkBlock = parsed.blocks.find((b) => b.file?.name === 'link_to_text.txt');
      assert.ok(fileLinkBlock, 'file symlink entry should exist in archive');
      const redirFileRec = fileLinkBlock.extra?.find((r) => r.type === 5);
      assert.ok(redirFileRec, 'FHEXTRA_REDIR record should be present on file link');
      assert.equal(redirFileRec.target, 'text.txt');

      const dirLinkBlock = parsed.blocks.find((b) => b.file?.name === 'link_to_dir');
      if (dirLinkBlock) {
        const redirDirRec = dirLinkBlock.extra?.find((r) => r.type === 5);
        assert.ok(redirDirRec, 'FHEXTRA_REDIR record should be present on dir link');
        // Forward slashes in target
        assert.ok(!redirDirRec.target.includes('\\'), 'target should use forward slashes');
        assert.equal(redirDirRec.target, 'dirA/nested');
        assert.equal(redirDirRec.flags & 1, 1, 'FHEXTRA_REDIR_DIR flag should be set for dir target');
      }
    }

    // Extraction round-trip with -ol
    const extOur = freshDir('links-test-ext');
    const resOur = runTool(OUR_EXE, ['x', '-y', '-ol', arc, extOur + DIR_SEP], tree);
    assert.equal(resOur.code, 0, `extract failed: ${resOur.output}`);

    if (hasSymlinks) {
      const extractedLink = join(extOur, 'link_to_text.txt');
      assert.ok(existsSync(extractedLink), 'extracted link file should exist');
    }
  });

  it('skips symlink creation when -ol- is specified on extraction', () => {
    const tree = freshDir('links-test-skip-tree');
    makeFixtureTree(tree);

    let hasSymlinks = false;
    try {
      symlinkSync('text.txt', join(tree, 'link_to_text.txt'), 'file');
      hasSymlinks = true;
    } catch (e) {
      // Ignore if no symlink privileges
    }

    if (!hasSymlinks) return;

    const out = freshDir('links-test-skip-out');
    const arc = buildOurArchive({ tree, out, switches: ['-ol', '-m3'] });

    const extSkip = freshDir('links-test-skip-ext');
    const res = runTool(OUR_EXE, ['x', '-y', '-ol-', arc, extSkip + DIR_SEP], tree);
    assert.equal(res.code, 0, `extract failed: ${res.output}`);

    const extractedLink = join(extSkip, 'link_to_text.txt');
    assert.equal(existsSync(extractedLink), false, 'link should NOT be extracted with -ol-');
  });

  it('verifies forward slashes in FHEXTRA_REDIR target encoding', () => {
    const tree = freshDir('links-test-norm-tree');
    makeFixtureTree(tree);

    let hasSymlinks = false;
    try {
      symlinkSync('d1/d2/d3/deep.txt', join(tree, 'link_deep.txt'), 'file');
      hasSymlinks = true;
    } catch (e) {
      // Ignore if no symlink privileges
    }

    if (!hasSymlinks) return;

    const out = freshDir('links-test-norm-out');
    const arc = buildOurArchive({ tree, out, switches: ['-ol', '-m0'] });

    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);

    const block = parsed.blocks.find((b) => b.file?.name === 'link_deep.txt');
    assert.ok(block, 'link_deep.txt entry should exist in archive');
    const redir = block.extra?.find((r) => r.type === 5);
    assert.ok(redir, 'FHEXTRA_REDIR should exist');
    assert.equal(redir.target, 'd1/d2/d3/deep.txt');
    assert.ok(!redir.target.includes('\\'), 'target must contain no backslashes');
  });
});
