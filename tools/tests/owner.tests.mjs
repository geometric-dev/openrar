// Test suite for POSIX User and Group Ownership (FHEXTRA_OWNER 0x06, -ow, -og)
import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import {
  DIR_SEP,
  OUR_EXE,
  RAR_EXE,
  WINRAR_UNRAR,
  existsSync,
  freshDir,
  oracleAvailable,
  readFileSync,
  runOurExe,
  runTool,
  writeFileSync,
} from './helpers.mjs';
import { parseArchive } from '../rar5-coverage.js';

test('owner: -og<group> records symbolic group in FHEXTRA_OWNER', () => {
  const dir = freshDir('owner_group_sym');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'file.txt');
  writeFileSync(src, 'owner test file content\n');

  runOurExe(['a', '-ogStaff', arc, src]);

  const parsed = parseArchive(readFileSync(arc));
  const fileBlocks = parsed.blocks.filter((b) => b.type === 2);
  assert.equal(fileBlocks.length, 1, 'archive should contain 1 file block');

  const ownerRec = (fileBlocks[0].extra || []).find((r) => r.type === 6);
  assert.ok(ownerRec, 'FHEXTRA_OWNER (type 6) record must be present');
  assert.equal(ownerRec.groupName, 'Staff');
  assert.ok(ownerRec.ownerFlags & 0x02, 'flag 0x02 for group name must be set');
});

test('owner: -og<gid> records numeric GID in FHEXTRA_OWNER', () => {
  const dir = freshDir('owner_group_num');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'file.txt');
  writeFileSync(src, 'numeric gid test\n');

  runOurExe(['a', '-og1000', arc, src]);

  const parsed = parseArchive(readFileSync(arc));
  const fileBlocks = parsed.blocks.filter((b) => b.type === 2);
  assert.equal(fileBlocks.length, 1, 'archive should contain 1 file block');

  const ownerRec = (fileBlocks[0].extra || []).find((r) => r.type === 6);
  assert.ok(ownerRec, 'FHEXTRA_OWNER record must be present');
  assert.equal(ownerRec.gid, 1000);
  assert.ok(ownerRec.ownerFlags & 0x08, 'flag 0x08 for numeric GID must be set');
});

test('owner: --owner and --group record both user and group in FHEXTRA_OWNER', () => {
  const dir = freshDir('owner_user_group');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'file.txt');
  writeFileSync(src, 'user and group test\n');

  runOurExe(['a', '--owner=builder', '--group=engineers', arc, src]);

  const parsed = parseArchive(readFileSync(arc));
  const fileBlocks = parsed.blocks.filter((b) => b.type === 2);
  assert.equal(fileBlocks.length, 1, 'archive should contain 1 file block');

  const ownerRec = (fileBlocks[0].extra || []).find((r) => r.type === 6);
  assert.ok(ownerRec, 'FHEXTRA_OWNER record must be present');
  assert.equal(ownerRec.userName, 'builder');
  assert.equal(ownerRec.groupName, 'engineers');
  assert.ok(ownerRec.ownerFlags & 0x01, 'flag 0x01 for user name must be set');
  assert.ok(ownerRec.ownerFlags & 0x02, 'flag 0x02 for group name must be set');
});

test('owner: lt technical listing displays User/Group', () => {
  const dir = freshDir('owner_list_lt');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'sample.txt');
  writeFileSync(src, 'sample data\n');

  runOurExe(['a', '--owner=alice', '--group=devs', arc, src]);

  const listOut = runOurExe(['lt', arc]);
  assert.ok(listOut.includes('User/Group:  alice / devs'), `Technical listing should contain user/group: ${listOut}`);
});

test('owner: dual-oracle interop with WinRAR 7.20', (t) => {
  if (!oracleAvailable()) {
    t.skip('WinRAR UnRAR oracle not available');
    return;
  }

  const dir = freshDir('owner_oracle');
  const arc = join(dir, 'arc.rar');
  const src = join(dir, 'oracle_file.txt');
  writeFileSync(src, 'dual oracle owner test\n');

  runOurExe(['a', '--owner=root', '--group=wheel', arc, src]);

  // 1. UnRAR lists technical details
  const unrarList = runTool(WINRAR_UNRAR, ['lt', arc]);
  assert.equal(unrarList.code, 0, `UnRAR lt failed: ${unrarList.output}`);
  assert.ok(
    unrarList.output.includes('root') || unrarList.output.includes('wheel') || unrarList.output.includes('User/Group'),
    `UnRAR should recognize user/group metadata: ${unrarList.output}`
  );

  // 2. UnRAR extracts archive cleanly
  const unrarOut = join(dir, 'unrar_ext');
  const extRes = runTool(WINRAR_UNRAR, ['x', '-y', arc, unrarOut + DIR_SEP]);
  assert.equal(extRes.code, 0, `UnRAR extract failed: ${extRes.output}`);
  assert.equal(existsSync(join(unrarOut, 'oracle_file.txt')), true);
  assert.equal(readFileSync(join(unrarOut, 'oracle_file.txt'), 'utf8'), 'dual oracle owner test\n');
});
