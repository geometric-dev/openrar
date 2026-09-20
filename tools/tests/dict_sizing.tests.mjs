import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const OPENRAR = path.resolve('build/openrar64/Release/openrar.exe');
const UNRAR = path.resolve('C:/Users/Matt/dev/unrar/build/unrar64/Release/UnRAR.exe');
const SCRATCH = path.resolve('scratch_dict_tests');

function setup() {
  cleanup();
  fs.mkdirSync(SCRATCH, { recursive: true });
}

function cleanup() {
  try {
    if (fs.existsSync(SCRATCH)) {
      fs.rmSync(SCRATCH, { recursive: true, force: true });
    }
  } catch {
    // Ignore transient file lock on Windows
  }
}

test('RAR 7.0 Non-Power-of-Two Dictionary: -md48m creation, self-test & roundtrip', (t) => {
  setup();
  t.after(cleanup);

  const testFile = path.join(SCRATCH, 'sample.txt');
  const content = 'The quick brown fox jumps over the lazy dog. 1234567890\n'.repeat(400);
  fs.writeFileSync(testFile, content, 'utf8');

  const arc = path.join(SCRATCH, 'dict48m.rar');
  execFileSync(OPENRAR, ['a', '-md48m', '-m3', arc, testFile]);
  assert.ok(fs.existsSync(arc), 'Archive should be created');

  // Test with OpenRAR 't'
  const testOut = execFileSync(OPENRAR, ['t', arc], { encoding: 'utf8' });
  assert.match(testOut, /All OK|OK/, 'Self test should pass');

  // Extract with OpenRAR 'x'
  const outDir = path.join(SCRATCH, 'out_openrar');
  execFileSync(OPENRAR, ['x', '-y', arc, outDir + path.sep]);
  const extracted = fs.readFileSync(path.join(outDir, 'sample.txt'), 'utf8');
  assert.equal(extracted, content, 'Extracted content must match original byte-for-byte');
});

test('RAR 7.0 Non-Power-of-Two Dictionary: -md24m creation & self-test', (t) => {
  setup();
  t.after(cleanup);

  const testFile = path.join(SCRATCH, 'sample24.txt');
  const content = 'Non-power-of-two 24 MB dictionary test content string.\n'.repeat(500);
  fs.writeFileSync(testFile, content, 'utf8');

  const arc = path.join(SCRATCH, 'dict24m.rar');
  execFileSync(OPENRAR, ['a', '-md24m', '-m3', arc, testFile]);
  assert.ok(fs.existsSync(arc), 'Archive should be created');

  const testOut = execFileSync(OPENRAR, ['t', arc], { encoding: 'utf8' });
  assert.match(testOut, /All OK|OK/, 'Self test should pass');
});

test('RAR 7.0 Fractional Dictionary: -md1.5g and unit-less -md48 parsing', (t) => {
  setup();
  t.after(cleanup);

  const testFile = path.join(SCRATCH, 'sample_fract.txt');
  const content = 'Testing fractional dictionary syntax parsing.\n'.repeat(100);
  fs.writeFileSync(testFile, content, 'utf8');

  // Unit-less -md48 should default to 48 MB
  const arc48 = path.join(SCRATCH, 'dict48_unitless.rar');
  execFileSync(OPENRAR, ['a', '-md48', '-m3', arc48, testFile]);
  assert.ok(fs.existsSync(arc48), 'Archive with unitless -md48 should be created');
  const list48 = execFileSync(OPENRAR, ['lt', arc48], { encoding: 'utf8' });
  assert.match(list48, /48\s*m|50331648/i, 'Listing should reflect 48 MB dictionary');

  // Also verify with UnRAR lt that it identifies -md=48m
  const unrarLt48 = execFileSync(UNRAR, ['lt', arc48], { encoding: 'utf8' });
  assert.match(unrarLt48, /-md=48m/i, 'UnRAR lt must report -md=48m for unit-less -md48');

  // Fractional -md1.5g (1536 MB)
  const arc15g = path.join(SCRATCH, 'dict1.5g.rar');
  execFileSync(OPENRAR, ['a', '-md1.5g', '-m3', arc15g, testFile]);
  assert.ok(fs.existsSync(arc15g), 'Archive with -md1.5g should be created');
  const unrarLt15g = execFileSync(UNRAR, ['lt', arc15g], { encoding: 'utf8' });
  assert.match(unrarLt15g, /-md=1536m/i, 'UnRAR lt must report -md=1536m for fractional -md1.5g');
});

test('RAR 7.0 Dual-Oracle Interop: WinRAR / UnRAR 7.20+ verifies -md48m with FCI_RAR5_COMPAT', (t) => {
  setup();
  t.after(cleanup);

  const testFile = path.join(SCRATCH, 'oracle_input.txt');
  const content = 'Dual oracle cross-validation data payload for 48MB dictionary!\n'.repeat(600);
  fs.writeFileSync(testFile, content, 'utf8');

  const arc = path.join(SCRATCH, 'oracle_dict48.rar');
  execFileSync(OPENRAR, ['a', '-md48m', '-m3', arc, testFile]);

  // UnRAR technical listing should identify RAR 5.0(v50) format with -md=48m
  const unrarLt = execFileSync(UNRAR, ['lt', arc], { encoding: 'utf8' });
  assert.match(unrarLt, /-md=48m/i, 'UnRAR lt must show -md=48m');
  assert.match(unrarLt, /v50/i, 'UnRAR lt must identify as v50 due to FCI_RAR5_COMPAT');

  // UnRAR 't' test must pass 100% OK
  const unrarT = execFileSync(UNRAR, ['t', arc], { encoding: 'utf8' });
  assert.match(unrarT, /All OK/i, 'UnRAR test must report All OK');

  // UnRAR 'x' extract must extract identical payload
  const outDir = path.join(SCRATCH, 'unrar_out');
  execFileSync(UNRAR, ['x', '-y', arc, outDir + path.sep]);
  const extracted = fs.readFileSync(path.join(outDir, 'oracle_input.txt'), 'utf8');
  assert.equal(extracted, content, 'UnRAR extracted file must be byte-for-byte identical');
});
