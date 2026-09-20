import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import {
  existsSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
  statSync,
} from 'node:fs';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import {
  DIR_SEP,
  OUR_EXE,
  WINRAR_UNRAR,
  freshDir,
  oracleAvailable,
  runTool,
} from './helpers.mjs';

function sha256(buffer) {
  return createHash('sha256').update(buffer).digest('hex');
}

function makeStructuredPayload(sizeBytes, seed = 12345) {
  const buf = Buffer.alloc(sizeBytes);
  let s = seed;
  const nextRand = () => {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    return s;
  };

  let pos = 0;
  while (pos < sizeBytes) {
    const chunk = Math.min(sizeBytes - pos, 1024 + (nextRand() % 8192));
    if ((nextRand() % 3) === 0) {
      // Repeated run
      const val = nextRand() % 256;
      buf.fill(val, pos, pos + chunk);
    } else {
      for (let i = 0; i < chunk; ++i) {
        buf[pos + i] = (nextRand() % 95) + 32; // Printable ASCII text
      }
    }
    pos += chunk;
  }
  return buf;
}

describe('v1.21.0 Multi-Threaded Compression (-mt) & Block Pipeline Integration', () => {
  it('creates and extracts single large file archive with -mt4 (streaming pipeline > 16 MiB)', () => {
    const tree = freshDir('mt-stream-tree');
    const out = freshDir('mt-stream-out');
    const dest = freshDir('mt-stream-dest');

    // 18 MiB payload straddling the 16 MiB streaming pipeline threshold
    const payloadSize = 18 * 1024 * 1024;
    const payload = makeStructuredPayload(payloadSize, 777);
    const srcFile = join(tree, 'large_payload.bin');
    writeFileSync(srcFile, payload);
    const expectedHash = sha256(payload);

    const arc = join(out, 'streaming_mt4.rar');
    const resAdd = runTool(OUR_EXE, ['a', '-y', '-mt4', arc, 'large_payload.bin'], tree);
    assert.equal(resAdd.code, 0, `Archiving with -mt4 failed: ${resAdd.output}`);

    // Self test
    const resTest = runTool(OUR_EXE, ['t', '-y', arc], out);
    assert.equal(resTest.code, 0, `OpenRAR test failed: ${resTest.output}`);

    // Dual-oracle UnRAR test
    if (oracleAvailable()) {
      const resOracle = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resOracle.code, 0, `WinRAR 7.20 failed to test -mt4 archive: ${resOracle.output}`);
    }

    // Extract and verify exact hash
    const resExtract = runTool(OUR_EXE, ['x', '-y', arc, dest + DIR_SEP], out);
    assert.equal(resExtract.code, 0, `Extraction failed: ${resExtract.output}`);

    const extractedBuf = readFileSync(join(dest, 'large_payload.bin'));
    assert.equal(sha256(extractedBuf), expectedHash, 'Decompressed payload hash mismatch');
  });

  it('verifies compression ratio consistency between -mt1 and -mt4 (< 3% delta)', () => {
    const tree = freshDir('mt-ratio-tree');
    const out = freshDir('mt-ratio-out');

    const payloadSize = 8 * 1024 * 1024;
    const payload = makeStructuredPayload(payloadSize, 888);
    const srcFile = join(tree, 'sample.bin');
    writeFileSync(srcFile, payload);

    const arcMt1 = join(out, 'sample_mt1.rar');
    const resMt1 = runTool(OUR_EXE, ['a', '-y', '-mt1', '-m3', arcMt1, 'sample.bin'], tree);
    assert.equal(resMt1.code, 0, `Archiving with -mt1 failed: ${resMt1.output}`);
    const sizeMt1 = statSync(arcMt1).size;

    const arcMt4 = join(out, 'sample_mt4.rar');
    const resMt4 = runTool(OUR_EXE, ['a', '-y', '-mt4', '-m3', arcMt4, 'sample.bin'], tree);
    assert.equal(resMt4.code, 0, `Archiving with -mt4 failed: ${resMt4.output}`);
    const sizeMt4 = statSync(arcMt4).size;

    // Delta between chunked MT and single-threaded LZ stream
    const crMt1 = sizeMt1 / payloadSize;
    const crMt4 = sizeMt4 / payloadSize;
    const ratioDelta = Math.abs(crMt4 - crMt1);
    assert.ok(
      ratioDelta < 0.03,
      `Compression ratio delta between -mt1 (${(crMt1 * 100).toFixed(2)}%) and -mt4 (${(crMt4 * 100).toFixed(2)}%) exceeds 3%: ${(ratioDelta * 100).toFixed(2)}%`
    );

    if (oracleAvailable()) {
      const resOracle = runTool(WINRAR_UNRAR, ['t', '-y', arcMt4], out);
      assert.equal(resOracle.code, 0, `Oracle verification failed for -mt4: ${resOracle.output}`);
    }
  });

  it('handles multi-file batch archiving with -mt4 (Exclusive Concurrency Policy)', () => {
    const tree = freshDir('mt-batch-tree');
    const out = freshDir('mt-batch-out');
    const dest = freshDir('mt-batch-dest');

    const fileCount = 6;
    const hashes = {};
    for (let i = 0; i < fileCount; ++i) {
      const p = makeStructuredPayload(512 * 1024, 1000 + i);
      const name = `file_${i}.dat`;
      writeFileSync(join(tree, name), p);
      hashes[name] = sha256(p);
    }

    const arc = join(out, 'batch_mt.rar');
    const resAdd = runTool(OUR_EXE, ['a', '-y', '-mt4', arc, '.'], tree);
    assert.equal(resAdd.code, 0, `Multi-file batch archiving with -mt4 failed: ${resAdd.output}`);

    const resTest = runTool(OUR_EXE, ['t', '-y', arc], out);
    assert.equal(resTest.code, 0, `OpenRAR test of batch archive failed: ${resTest.output}`);

    if (oracleAvailable()) {
      const resOracle = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resOracle.code, 0, `WinRAR 7.20 failed to test batch archive: ${resOracle.output}`);
    }

    const resExtract = runTool(OUR_EXE, ['x', '-y', arc, dest + DIR_SEP], out);
    assert.equal(resExtract.code, 0, `Extraction of batch archive failed: ${resExtract.output}`);

    for (let i = 0; i < fileCount; ++i) {
      const name = `file_${i}.dat`;
      const extractedBuf = readFileSync(join(dest, name));
      assert.equal(sha256(extractedBuf), hashes[name], `Hash mismatch for ${name}`);
    }
  });

  it('supports bare -mt flag (auto-detecting hardware threads)', () => {
    const tree = freshDir('mt-bare-tree');
    const out = freshDir('mt-bare-out');

    const payload = makeStructuredPayload(1024 * 1024, 999);
    writeFileSync(join(tree, 'test.bin'), payload);

    const arc = join(out, 'bare_mt.rar');
    const resAdd = runTool(OUR_EXE, ['a', '-y', '-mt', arc, 'test.bin'], tree);
    assert.equal(resAdd.code, 0, `Bare -mt archiving failed: ${resAdd.output}`);

    const resTest = runTool(OUR_EXE, ['t', '-y', arc], out);
    assert.equal(resTest.code, 0, `Bare -mt archive test failed: ${resTest.output}`);

    if (oracleAvailable()) {
      const resOracle = runTool(WINRAR_UNRAR, ['t', '-y', arc], out);
      assert.equal(resOracle.code, 0, `WinRAR test failed for bare -mt: ${resOracle.output}`);
    }
  });
});
