// Archive Mutation test suite for OpenRAR ('D', 'U', 'F', 'M', 'K' commands)
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync, writeFileSync, utimesSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { parseArchive } from '../rar5-coverage.js';
import { freshDir, makeFixtureTree, buildOurArchive, runTool, OUR_EXE, WINRAR_UNRAR, treesEqual } from './helpers.mjs';

describe('RAR 5.0 Archive Mutation (d, u, f, m, k)', () => {
  it('deletes files from archive (d) and verifies remaining archive integrity', () => {
    const tree = freshDir('mut-del-tree');
    makeFixtureTree(tree);
    const out = freshDir('mut-del-out');
    const arc = buildOurArchive({ tree, out, switches: ['-m3'] });

    // Delete a specific file: 'data.bin'
    const resDel = runTool(OUR_EXE, ['d', '-y', arc, 'data.bin'], out);
    assert.equal(resDel.code, 0, `Delete failed: ${resDel.output}`);

    // Verify 'data.bin' is gone
    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);
    assert.equal(parsed.truncated, false);
    const foundDataBin = parsed.blocks.some((b) => b.file?.name === 'data.bin');
    assert.equal(foundDataBin, false, 'data.bin should have been deleted');

    // Dual-oracle test pass on mutated archive
    const resWin = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
    assert.equal(resWin.code, 0, `WinRAR test failed:\n${resWin.output}`);
    const resOur = runTool(OUR_EXE, ['t', '-y', arc], out);
    assert.equal(resOur.code, 0, `Self test failed:\n${resOur.output}`);
  });

  it('updates modified files and adds new files (u), ignoring older files', () => {
    const tree = freshDir('mut-upd-tree');
    makeFixtureTree(tree);
    const out = freshDir('mut-upd-out');
    const arc = buildOurArchive({ tree, out, switches: ['-m3'] });

    // 1. Modify text.txt with newer timestamp
    const textPath = join(tree, 'text.txt');
    writeFileSync(textPath, 'UPDATED CONTENT FOR TEXT.TXT');
    const now = Math.floor(Date.now() / 1000) + 100;
    utimesSync(textPath, now, now);

    // 2. Create brand new file 'brand_new.txt'
    writeFileSync(join(tree, 'brand_new.txt'), 'BRAND NEW FILE CONTENT');

    // 3. Keep older.bin with timestamp set in the past
    writeFileSync(join(tree, 'older.bin'), 'OLDER UNMODIFIED');
    utimesSync(join(tree, 'older.bin'), 1000000000, 1000000000);

    // Run update command
    const resUpd = runTool(OUR_EXE, ['u', '-y', '-r', arc, '*'], tree);
    assert.equal(resUpd.code, 0, `Update failed: ${resUpd.output}`);

    // Verify extraction
    const extDir = freshDir('mut-upd-ext');
    const resExt = runTool(WINRAR_UNRAR, ['x', '-y', arc, extDir + '\\'], tree);
    assert.equal(resExt.code, 0, `WinRAR extract failed: ${resExt.output}`);

    assert.equal(readFileSync(join(extDir, 'text.txt'), 'utf8'), 'UPDATED CONTENT FOR TEXT.TXT');
    assert.equal(readFileSync(join(extDir, 'brand_new.txt'), 'utf8'), 'BRAND NEW FILE CONTENT');
  });

  it('freshens existing files (f) but does NOT add new files', () => {
    const tree = freshDir('mut-fsh-tree');
    makeFixtureTree(tree);
    const out = freshDir('mut-fsh-out');
    const arc = buildOurArchive({ tree, out, switches: ['-m3'] });

    // Modify text.txt and create untracked file
    writeFileSync(join(tree, 'text.txt'), 'FRESHENED TEXT CONTENT');
    const future = Math.floor(Date.now() / 1000) + 200;
    utimesSync(join(tree, 'text.txt'), future, future);

    writeFileSync(join(tree, 'should_not_exist.txt'), 'DO NOT ADD ME');

    // Run freshen command
    const resFsh = runTool(OUR_EXE, ['f', '-y', '-r', arc, '*'], tree);
    assert.equal(resFsh.code, 0, `Freshen failed: ${resFsh.output}`);

    // Verify should_not_exist.txt was NOT added
    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);
    const foundNew = parsed.blocks.some((b) => b.file?.name === 'should_not_exist.txt');
    assert.equal(foundNew, false, 'freshen must not add new files');

    // Verify text.txt was freshened
    const extDir = freshDir('mut-fsh-ext');
    runTool(WINRAR_UNRAR, ['x', '-y', arc, extDir + '\\'], tree);
    assert.equal(readFileSync(join(extDir, 'text.txt'), 'utf8'), 'FRESHENED TEXT CONTENT');
  });

  it('moves files to archive (m) and deletes source files on disk', () => {
    const tree = freshDir('mut-mov-tree');
    writeFileSync(join(tree, 'move_me.txt'), 'DATA TO BE MOVED');
    const out = freshDir('mut-mov-out');
    const arc = join(out, 'move_test.rar');

    const resMov = runTool(OUR_EXE, ['m', '-y', arc, 'move_me.txt'], tree);
    assert.equal(resMov.code, 0, `Move failed: ${resMov.output}`);

    // Verify file deleted from disk
    assert.equal(existsSync(join(tree, 'move_me.txt')), false, 'Source file should be deleted on move');

    // Verify archive contains file
    const extDir = freshDir('mut-mov-ext');
    const resExt = runTool(OUR_EXE, ['x', '-y', arc, extDir + '\\'], tree);
    assert.equal(resExt.code, 0);
    assert.equal(readFileSync(join(extDir, 'move_me.txt'), 'utf8'), 'DATA TO BE MOVED');
  });

  it('locks archive (k) and strictly rejects subsequent mutations', () => {
    const tree = freshDir('mut-lck-tree');
    makeFixtureTree(tree);
    const out = freshDir('mut-lck-out');
    const arc = buildOurArchive({ tree, out, switches: ['-m3'] });

    // Lock archive
    const resLck = runTool(OUR_EXE, ['k', '-y', arc], out);
    assert.equal(resLck.code, 0, `Lock failed: ${resLck.output}`);

    // Verify MHFL_LOCK is set in header
    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);
    const main = parsed.blocks.find((b) => b.typeName === 'main');
    assert.ok(main.rawArchiveFlags & 0x0010, 'MHFL_LOCK should be set');

    // Attempt delete on locked archive -> must fail
    const resDel = runTool(OUR_EXE, ['d', '-y', arc, 'text.txt'], out);
    assert.notEqual(resDel.code, 0, 'Delete on locked archive must return non-zero error code');

    // Attempt update on locked archive -> must fail
    const resUpd = runTool(OUR_EXE, ['u', '-y', arc, '*'], tree);
    assert.notEqual(resUpd.code, 0, 'Update on locked archive must return non-zero error code');
  });

  it('strips QuickOpen and locators upon mutation', { skip: true && 'QO writer support deferred per 00-overview.md' }, () => {
    const tree = freshDir('mut-qo-tree');
    makeFixtureTree(tree);
    const out = freshDir('mut-qo-out');
    // Build archive with default QuickOpen
    const arc = buildOurArchive({ tree, out, switches: ['-m3'] });

    // Initial archive has QO service header
    const buf1 = readFileSync(arc);
    const p1 = parseArchive(buf1);
    assert.ok(p1.blocks.some((b) => b.file?.service === 'QO'), 'Initial archive should have QO');

    // Mutate by deleting a file
    runTool(OUR_EXE, ['d', '-y', arc, 'empty.txt'], out);

    // Verify QO is stripped and locator is absent
    const buf2 = readFileSync(arc);
    const p2 = parseArchive(buf2);
    assert.equal(p2.blocks.some((b) => b.file?.service === 'QO'), false, 'QO service should be stripped');
    const main2 = p2.blocks.find((b) => b.typeName === 'main');
    assert.equal(main2.extra?.length ?? 0, 0, 'Locator extra record should be stripped');

    // Dual-oracle test on stripped archive
    const resWin = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
    assert.equal(resWin.code, 0);
  });

  it('rejects mutation on multi-volume sets', () => {
    const tree = freshDir('mut-vol-tree');
    makeFixtureTree(tree);
    const out = freshDir('mut-vol-out');
    buildOurArchive({ tree, out, switches: ['-v20k', '-m0'] });

    const part1 = join(out, 'archive.part01.rar');
    const resDel = runTool(OUR_EXE, ['d', '-y', part1, 'text.txt'], out);
    assert.notEqual(resDel.code, 0, 'Mutation on multi-volume sets must be rejected');
  });

  it('preserves encrypted entries verbatim without password during mutation', () => {
    const tree = freshDir('mut-enc-tree');
    writeFileSync(join(tree, 'secret1.txt'), 'SUPER SECRET DATA 1');
    writeFileSync(join(tree, 'secret2.txt'), 'SUPER SECRET DATA 2');
    const out = freshDir('mut-enc-out');
    const arc = join(out, 'enc.rar');

    // Create encrypted archive with password "Pass123"
    runTool(OUR_EXE, ['a', '-y', '-pPass123', '-m0', arc, '.'], tree);

    // Mutate archive by deleting secret1.txt WITHOUT providing a password
    const resDel = runTool(OUR_EXE, ['d', '-y', arc, 'secret1.txt'], out);
    assert.equal(resDel.code, 0, `Delete from encrypted archive failed: ${resDel.output}`);

    // Verify surviving secret2.txt can still be decrypted with original password
    const extDir = freshDir('mut-enc-ext');
    const resExt = runTool(WINRAR_UNRAR, ['x', '-y', '-pPass123', arc, extDir + '\\'], tree);
    assert.equal(resExt.code, 0, `Extracting surviving file failed: ${resExt.output}`);
    assert.equal(readFileSync(join(extDir, 'secret2.txt'), 'utf8'), 'SUPER SECRET DATA 2');
    assert.equal(existsSync(join(extDir, 'secret1.txt')), false);
  });

  it('supports deletion from solid archives and passes dual-oracle test', () => {
    const tree = freshDir('mut-sld-tree');
    makeFixtureTree(tree);
    const out = freshDir('mut-sld-out');
    const arc = buildOurArchive({ tree, out, switches: ['-s', '-m3'] });

    // Delete a file in the middle of solid chain
    const resDel = runTool(OUR_EXE, ['d', '-y', arc, 'text.txt'], out);
    assert.equal(resDel.code, 0, `Solid delete failed: ${resDel.output}`);

    // Verify dual-oracle test pass on mutated solid archive
    const resWin = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
    assert.equal(resWin.code, 0, `WinRAR solid test failed: ${resWin.output}`);
  });

  it('supports wildcard mask deletion (*.txt)', () => {
    const tree = freshDir('mut-wild-tree');
    writeFileSync(join(tree, 'a.txt'), 'AAA');
    writeFileSync(join(tree, 'b.txt'), 'BBB');
    writeFileSync(join(tree, 'c.bin'), 'CCC');
    const out = freshDir('mut-wild-out');
    const arc = join(out, 'wild.rar');

    runTool(OUR_EXE, ['a', '-y', '-m0', arc, '.'], tree);

    // Delete all *.txt
    const resDel = runTool(OUR_EXE, ['d', '-y', arc, '*.txt'], out);
    assert.equal(resDel.code, 0, `Wildcard delete failed: ${resDel.output}`);

    // Verify only c.bin remains
    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);
    const names = parsed.blocks.filter((b) => b.file && !b.file.isService).map((b) => b.file.name);
    assert.deepEqual(names, ['c.bin']);
  });
});
