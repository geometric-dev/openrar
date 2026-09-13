// NTFS Alternate Data Streams test suite for OpenRAR (-os)
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { writeFileSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { parseArchive } from '../rar5-coverage.js';
import { freshDir, buildOurArchive, runTool, OUR_EXE, readFileSync } from './helpers.mjs';

describe('RAR 5.0 NTFS Alternate Data Streams (-os)', () => {
  it('archives, parses, and restores NTFS streams', { skip: process.platform !== 'win32' }, () => {
    const tree = freshDir('streams-test-tree');
    const testFile = join(tree, 'stream_file.txt');
    writeFileSync(testFile, 'Main data stream content.\n');

    // Create alternate data streams via PowerShell
    try {
      execFileSync('powershell.exe', [
        '-NoProfile',
        '-Command',
        `Set-Content -Path "${testFile}" -Stream "CustomADS" -Value "Custom stream payload 12345"; ` +
        `Set-Content -Path "${testFile}" -Stream "Zone.Identifier" -Value "[ZoneTransfer]\`nZoneId=3"`
      ]);
    } catch (e) {
      console.log('Skipping ADS creation failure:', e.message);
      return;
    }

    const out = freshDir('streams-test-out');
    const arc = buildOurArchive({ tree, out, switches: ['-os', '-m3'] });

    const buf = readFileSync(arc);
    const parsed = parseArchive(buf);

    assert.equal(parsed.truncated, false, `Archive truncated: ${parsed.parseError}`);
    assert.equal(parsed.encryptedHeaders, false);

    // Verify service blocks
    const stmBlocks = parsed.blocks.filter((b) => (b.type === 3 || b.typeName === 'service') && b.file?.name === 'STM');
    assert.ok(stmBlocks.length >= 2, `Expected at least 2 STM service blocks, found ${stmBlocks.length}`);

    for (const b of stmBlocks) {
      const extraSubData = b.extra?.find((r) => r.type === 7);
      assert.ok(extraSubData, 'STM block should contain FHEXTRA_SUBDATA record');
    }

    // Extraction round-trip
    const extOur = freshDir('streams-test-ext');
    const resOur = runTool(OUR_EXE, ['x', '-y', arc, extOur + '\\'], tree);
    assert.equal(resOur.code, 0, `extract failed: ${resOur.output}`);

    const extractedFile = join(extOur, 'stream_file.txt');
    assert.ok(existsSync(extractedFile), 'extracted file should exist');

    // Verify extracted streams via PowerShell
    const adsCheck = execFileSync('powershell.exe', [
      '-NoProfile',
      '-Command',
      `$c1 = Get-Content -Path "${extractedFile}" -Stream "CustomADS" -Raw; ` +
      `$c2 = Get-Content -Path "${extractedFile}" -Stream "Zone.Identifier" -Raw; ` +
      `Write-Output "$c1|||$c2"`
    ], { encoding: 'utf8' });

    assert.ok(adsCheck.includes('Custom stream payload 12345'), 'CustomADS content should be restored');
    assert.ok(adsCheck.includes('ZoneId=3'), 'Zone.Identifier content should be restored');
  });
});
