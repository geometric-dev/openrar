// Structural format assertions: build archives with our writer and validate
// the resulting block/feature layout through the coverage parser.
import { describe, it, before } from 'node:test';
import assert from 'node:assert/strict';
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { parseArchive, analyze } from '../rar5-coverage.js';
import { freshDir, buildOurArchive, makeFixtureTree, readFileSync, existsSync, OUR_EXE, RAR_EXE, WINRAR_UNRAR, runTool, extractWith, treesEqual, sha256, REPO, oracleAvailable } from './helpers.mjs';

// An SFX module to prepend: WinRAR's Default.SFX when installed, otherwise
// the module openrar itself builds (build/openrar64/<cfg>/Default.SFX.exe or
// build/Default.SFX on POSIX). No module anywhere -> the SFX tests skip.
function sfxModule() {
  const candidates = [
    process.env.OPENRAR_SFX_MODULE,
    join(dirname(RAR_EXE), 'Default.SFX'),
    join(REPO, 'build', 'openrar64', 'Release', 'Default.SFX.exe'),
    join(REPO, 'build', 'openrar64', 'Debug', 'Default.SFX.exe'),
    join(REPO, 'build', 'Default.SFX'),
  ].filter(Boolean);
  return candidates.find((p) => existsSync(p)) ?? null;
}

const SCENARIOS = [
  { name: 'store-m0', switches: ['-m0'] },
  { name: 'normal-m3', switches: ['-m3'] },
  { name: 'best-m5', switches: ['-m5'] },
  { name: 'solid', switches: ['-s', '-m3'] },
  { name: 'comment', switches: ['-m3'] },
  { name: 'meta', switches: ['-ams', '-m3'] },
  { name: 'times-all', switches: ['-tsm4', '-tsc4', '-tsa4', '-m3'] },
  { name: 'qo-off', switches: ['-qo-', '-m3'] },
];

function buildParsed(scenario, tagSuffix = '') {
  const tree = freshDir(`fmt-${scenario.name}${tagSuffix}-tree`);
  makeFixtureTree(tree);
  const out = freshDir(`fmt-${scenario.name}${tagSuffix}-out`);
  const arc = buildOurArchive({ tree, out, switches: scenario.switches });
  const buf = readFileSync(arc);
  return { arc, buf, parsed: parseArchive(buf), features: analyze(parseArchive(buf)) };
}

describe('RAR5 writer structural output', () => {
  for (const scenario of SCENARIOS) {
    describe(scenario.name, () => {
      const ctx = {};
      before(() => { Object.assign(ctx, buildParsed(scenario)); });

      it('parses cleanly (no truncation, no encryption)', () => {
        assert.equal(ctx.parsed.truncated, false);
        assert.equal(ctx.parsed.encryptedHeaders, false);
        assert.equal(ctx.parsed.parseError, undefined);
      });

      it('every header CRC verifies', () => {
        for (const b of ctx.parsed.blocks)
          assert.notEqual(b.headerCrcOk, false, `block @${b.offset} type=${b.typeName}`);
      });

      it('contains main, file and end blocks', () => {
        assert.ok(ctx.features.blockTypes.includes('main'));
        assert.ok(ctx.features.blockTypes.includes('file'));
        assert.ok(ctx.features.blockTypes.includes('end'));
      });

      it('stores >=6 entries with legal methods', () => {
        const files = ctx.parsed.blocks.filter((b) => b.typeName === 'file' && !b.file?.isService);
        assert.ok(files.length >= 6, `expected >=6 entries, got ${files.length}`);
        for (const b of files) {
          assert.ok(b.file.method <= 5, b.file.name);
          const isDir = b.file.flags.includes('directory');
          if (!isDir)
            assert.ok(b.file.flags.includes('crc32'), `missing CRC32 flag: ${b.file.name}`);
        }
      });

      it('writes high precision mtime record', () => {
        assert.equal(ctx.features.htime.any, true);
        assert.equal(ctx.features.htime.mtime, true);
      });

      it('emits directory entries', () => {
        assert.equal(ctx.features.dirEntries, true);
      });
    });
  }

  it('[-ed] stores no directory records; files still extract with paths', () => {
    const tree = freshDir('fmt-ed-tree');
    makeFixtureTree(tree);
    const out = freshDir('fmt-ed-out');
    const arc = buildOurArchive({ tree, out, switches: ['-ed'] });
    const parsed = parseArchive(readFileSync(arc));
    const files = parsed.blocks.filter((b) => b.typeName === 'file' && !b.file?.isService);
    assert.ok(files.length > 0, `entries: ${files.length}`);
    assert.equal(files.filter((b) => b.file.flags.includes('directory')).length, 0, 'directory records must be omitted');

    const ctrlArc = buildOurArchive({ tree, out, switches: [], name: 'control.rar' });
    const ctrlFiles = parseArchive(readFileSync(ctrlArc)).blocks
      .filter((b) => b.typeName === 'file' && !b.file?.isService);
    assert.ok(ctrlFiles.some((b) => b.file.flags.includes('directory')), 'control build must keep directory records');
    assert.deepEqual(
      files.map((b) => b.file.name),
      ctrlFiles.filter((b) => !b.file.flags.includes('directory')).map((b) => b.file.name),
    );

    const dest = freshDir('fmt-ed-x');
    const x = extractWith(OUR_EXE, arc, join(dest, 'x'));
    assert.equal(x.code, 0, x.output);
    const cmp = treesEqual(tree, join(dest, 'x'));
    assert.equal(cmp.ok, true, cmp.why);
  });

  it('[-sfx] prepends the module byte-exact, writes .exe, passes both testers', { skip: sfxModule() === null && 'no SFX module available (set OPENRAR_SFX_MODULE)' }, () => {
    const modulePath = sfxModule();
    const tree = freshDir('fmt-sfx-tree');
    makeFixtureTree(tree);
    const out = freshDir('fmt-sfx-out');
    const arc = buildOurArchive({ tree, out, switches: ['-sfx' + modulePath], name: 'sfx.rar' });
    const exe = join(out, 'sfx.exe');
    assert.ok(existsSync(exe), 'sfx.exe must be created');
    assert.ok(!existsSync(arc), 'the .rar intermediate must not remain');
    const buf = readFileSync(exe);
    const module = readFileSync(modulePath);
    assert.ok(buf.subarray(0, module.length).equals(module), 'module must be prepended byte-exact');
    const parsed = parseArchive(buf);
    assert.ok(parsed.sfx, 'signature must be found behind the module');
    assert.equal(parsed.truncated, false);
    for (const b of parsed.blocks)
      assert.notEqual(b.headerCrcOk, false, `block @${b.offset} type=${b.typeName}`);
    if (oracleAvailable()) {
      const wt = runTool(WINRAR_UNRAR, ['t', '-y', '--', exe]);
      assert.equal(wt.code, 0, wt.output);
    }

    const arc2 = buildOurArchive({ tree, out, switches: ['-sfx' + modulePath], name: 'sfx2.rar' });
    assert.equal(sha256(exe), sha256(join(out, 'sfx2.exe')), 'SFX output must be deterministic');
  });

  it('[-sfx] keeps .exe/.sfx names, rejects missing and oversized modules', { skip: sfxModule() === null && 'no SFX module available (set OPENRAR_SFX_MODULE)' }, () => {
    const modulePath = sfxModule();
    const tree = freshDir('fmt-sfx2-tree');
    makeFixtureTree(tree);
    const out = freshDir('fmt-sfx2-out');
    buildOurArchive({ tree, out, switches: ['-sfx' + modulePath], name: 'plain.rar' });
    assert.ok(existsSync(join(out, 'plain.exe')), 'extension must be set to .exe');
    assert.ok(!existsSync(join(out, 'plain.rar')), 'the .rar name must not remain');

    buildOurArchive({ tree, out, switches: ['-sfx' + modulePath], name: 'kept.sfx' });
    assert.ok(existsSync(join(out, 'kept.sfx')), 'existing .sfx name must be kept');

    const miss = runTool(OUR_EXE, ['a', '-y', '-sfxzzzmodule.sfx', '--', join(out, 'm.exe'), '.'], tree);
    assert.equal(miss.code, 6, miss.output);
    assert.ok(!existsSync(join(out, 'm.exe')), 'nothing must be created for a missing module');

    const bigModule = join(out, 'big.mod');
    writeFileSync(bigModule, Buffer.alloc(5 * 1024 * 1024));
    const big = runTool(OUR_EXE, ['a', '-y', '-sfx' + bigModule, '--', join(out, 'big.exe'), '.'], tree);
    assert.equal(big.code, 7, big.output);
    assert.ok(!existsSync(join(out, 'big.exe')), 'nothing must be created for an oversized module');
  });

  it('[store-m0] stores every entry with method 0', () => {
    const { parsed } = buildParsed(SCENARIOS[0]);
    const methods = new Set(
      parsed.blocks.filter((b) => b.file && !b.file.isService).map((b) => b.file.method),
    );
    assert.deepEqual([...methods], [0]);
  });

  it('[solid] sets MHFL_SOLID and per-file solid bits on compressed entries', () => {
    const { parsed } = buildParsed(SCENARIOS.find((s) => s.name === 'solid'));
    const main = parsed.blocks.find((b) => b.typeName === 'main');
    assert.deepEqual(main.archiveFlags, ['solid']);
    const compressed = parsed.blocks.filter((b) => b.file && !b.file.isService && b.file.method > 0);
    assert.ok(compressed.length > 1, 'need multiple compressed files');
    assert.equal(compressed[0].file.solid, false, 'first entry starts the solid stream');
    for (let i = 1; i < compressed.length; i++) {
      assert.equal(compressed[i].file.solid, true, compressed[i].file.name);
    }
  });

  it('[comment] places a CMT service header immediately after the main header with matching payload', () => {
    const cmtPath = join(freshDir('cmt-src'), 'cmt.txt');
    writeFileSync(cmtPath, 'OpenRAR test comment');
    const tree = freshDir('fmt-comment2-tree');
    makeFixtureTree(tree);
    const out = freshDir('fmt-comment2-out');
    const arc = buildOurArchive({ tree, out, switches: ['-m3', `-z${cmtPath}`] });
    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);
    const mainIdx = parsed.blocks.findIndex((b) => b.typeName === 'main');
    const svc = parsed.blocks[mainIdx + 1];
    assert.equal(svc.typeName, 'service');
    assert.equal(svc.file.service, 'CMT');
    assert.equal(svc.file.unpSize, Buffer.byteLength('OpenRAR test comment'));
    // Payload bytes must equal the comment text exactly.
    const payload = buf.subarray(svc.dataOffset, svc.dataOffset + svc.file.unpSize).toString('utf8');
    assert.equal(payload, 'OpenRAR test comment');
  });

  // QO/locator and MHEXTRA_METADATA writer features are deferred
  // ("Solid, quick-open, and recovery are optional" — 00-overview.md
  // design principles; QO/METADATA writer support listed
  // under "Deferred (skip per 00:113)"). Flip the flag when the writer gains
  // quick-open / archive-metadata support; the assertions below are the
  // contract those features must satisfy.
  const QO_METADATA_DEFERRED = true;

  it('[metadata] embeds the metadata extra record into the main header', { skip: QO_METADATA_DEFERRED && 'MHEXTRA_METADATA writer support deferred per 00-overview.md' }, () => {
    const { parsed } = buildParsed(SCENARIOS.find((s) => s.name === 'meta'));
    const main = parsed.blocks.find((b) => b.typeName === 'main');
    assert.ok(main.extra?.some((e) => e.type === 2), 'metadata record missing');
    const meta = main.extra.find((e) => e.type === 2);
    assert.notEqual(meta.name, undefined, 'metadata record should carry archive name');
  });

  it('[default] writes locator + QO service; [qo-off] writes neither', { skip: QO_METADATA_DEFERRED && 'QO/locator writer support deferred per 00-overview.md' }, () => {
    const def = buildParsed({ name: 'def-qo', switches: ['-m3'] });
    const mainDef = def.parsed.blocks.find((b) => b.typeName === 'main');
    assert.ok(mainDef.extra?.some((e) => e.type === 1));
    assert.ok(def.features.services.includes('QO'));

    const off = buildParsed(SCENARIOS.find((s) => s.name === 'qo-off'));
    const mainOff = off.parsed.blocks.find((b) => b.typeName === 'main');
    assert.ok(!mainOff.extra?.some((e) => e.type === 1));
    assert.ok(!off.features.services.includes('QO'));
  });

  it('[default] writes no locator and no QO while QO support is deferred; [qo-off] likewise', () => {
    for (const switches of [[], ['-qo-', '-m3']]) {
      const ctx = buildParsed({ name: `noqo-${switches.length}`, switches });
      const main = ctx.parsed.blocks.find((b) => b.typeName === 'main');
      assert.ok(!main.extra?.some((e) => e.type === 1), `locator written with ${switches}`);
      assert.ok(!ctx.features.services.includes('QO'), `QO written with ${switches}`);
    }
  });

  it('[times-all] htime record carries mtime, ctime and atime', () => {
    const { features } = buildParsed(SCENARIOS.find((s) => s.name === 'times-all'));
    assert.equal(features.htime.ctime, true);
    assert.equal(features.htime.atime, true);
  });

  describe('SFX module format handling', () => {
    it('detects SFX preamble and successfully tests and extracts archive', () => {
      const tree = freshDir('sfx-tree');
      makeFixtureTree(tree);
      const out = freshDir('sfx-out');
      const arc = buildOurArchive({ tree, out, switches: ['-m3'] });
      const rawArc = readFileSync(arc);

      // Create an SFX executable by prepending an arbitrary PE-like executable stub (64 KB)
      const stub = Buffer.alloc(64 * 1024, 0x90);
      stub.write('MZ', 0, 'ascii'); // standard DOS executable signature
      stub.write('PE\0\0', 0x80, 'ascii');

      const sfxArc = join(out, 'archive.exe');
      const sfxBuf = Buffer.concat([stub, rawArc]);
      writeFileSync(sfxArc, sfxBuf);

      // Coverage parser detection
      const parsed = parseArchive(sfxBuf);
      assert.equal(parsed.sfx, true);
      assert.equal(parsed.truncated, false);
      assert.equal(parsed.parseError, undefined);

      // Verify listing output reports SFX detail
      const listRes = runTool(OUR_EXE, ['l', sfxArc]);
      assert.equal(listRes.code, 0);
      assert.match(listRes.output, /SFX/i);

      // Test integrity
      const testRes = runTool(OUR_EXE, ['t', '-y', sfxArc]);
      assert.equal(testRes.code, 0);

      // Extract and verify files match original fixture tree
      const extractDest = join(out, 'extracted');
      const extRes = extractWith(OUR_EXE, sfxArc, extractDest);
      assert.equal(extRes.code, 0);

      const eq = treesEqual(tree, extractDest);
      assert.equal(eq.ok, true, eq.why);
    });

    it('supports large SFX preambles up to MAXSFXSIZE (4 MB)', () => {
      const tree = freshDir('sfx-large-tree');
      makeFixtureTree(tree);
      const out = freshDir('sfx-large-out');
      const arc = buildOurArchive({ tree, out, switches: ['-m0'] });
      const rawArc = readFileSync(arc);

      // Prepend 2 MB stub (within 4 MB MAXSFXSIZE)
      const stub = Buffer.alloc(2 * 1024 * 1024, 0xcc);
      const sfxArc = join(out, 'large_stub.exe');
      const sfxBuf = Buffer.concat([stub, rawArc]);
      writeFileSync(sfxArc, sfxBuf);

      const parsed = parseArchive(sfxBuf);
      assert.equal(parsed.sfx, true);

      const testRes = runTool(OUR_EXE, ['t', '-y', sfxArc]);
      assert.equal(testRes.code, 0);

      const extractDest = join(out, 'extracted');
      const extRes = extractWith(OUR_EXE, sfxArc, extractDest);
      assert.equal(extRes.code, 0);

      const eq = treesEqual(tree, extractDest);
      assert.equal(eq.ok, true, eq.why);
    });
  });
});
