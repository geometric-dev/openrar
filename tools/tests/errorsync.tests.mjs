// Error-code sync contract: the C ABI error enum (src/archive/rar_errors.hpp),
// the JS wrapper map (wasm/js/openrar-archive.js CODE_MAP) and the TypeScript
// union (wasm/js/openrar-archive.d.ts RarErrorCode) must never drift — I1/I2
// found MISSING_VOLUME/BUSY missing from the JS surface, silently degrading
// them to generic 'IO'. This suite pins all three together. Parsing source
// text (not importing the module) keeps it independent of a wasm build.
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..', '..');

function parseCErrorCodes() {
  const text = readFileSync(join(REPO, 'src', 'archive', 'rar_errors.hpp'), 'utf8');
  const map = {};
  for (const m of text.matchAll(/RAR_ERR_([A-Z_0-9]+)\s*=\s*(-?\d+)/g)) {
    map[m[1]] = Number(m[2]);
  }
  return map;
}

function parseJsCodeMap() {
  const text = readFileSync(join(REPO, 'wasm', 'js', 'openrar-archive.js'), 'utf8');
  const block = text.match(/const CODE_MAP = Object\.freeze\(\{([\s\S]*?)\}\);/);
  assert.ok(block, 'CODE_MAP block found in openrar-archive.js');
  const map = {};
  for (const m of block[1].matchAll(/\[(-?\d+)\]:\s*'([A-Z_]+)'/g)) {
    map[Number(m[1])] = m[2];
  }
  return map;
}

function parseJsRarErrorCodeNames() {
  const text = readFileSync(join(REPO, 'wasm', 'js', 'openrar-archive.js'), 'utf8');
  const block = text.match(/export const RarErrorCode = Object\.freeze\(\{([\s\S]*?)\}\);/);
  assert.ok(block, 'RarErrorCode freeze found in openrar-archive.js');
  return new Set([...block[1].matchAll(/([A-Z_]+):/g)].map((m) => m[1]));
}

function parseTsUnionNames() {
  const text = readFileSync(join(REPO, 'wasm', 'js', 'openrar-archive.d.ts'), 'utf8');
  const block = text.match(/export type RarErrorCode =([\s\S]*?);/);
  assert.ok(block, 'RarErrorCode union found in openrar-archive.d.ts');
  return new Set([...block[1].matchAll(/'([A-Z_]+)'/g)].map((m) => m[1]));
}

// RAR_ERR_CRC_MISMATCH -> CRC_MISMATCH
const jsNameOf = (cName) => cName.replace(/^RAR_ERR_/, '');
// PARTIAL_OK is a SUCCESS code (1) — it must not appear in the error surface.
const isSuccess = (value) => value >= 0;

describe('C ABI <-> JS/TS error-code sync', () => {
  it('every negative C error code has a JS CODE_MAP entry with the expected name', () => {
    const c = parseCErrorCodes();
    const js = parseJsCodeMap();
    const negative = Object.entries(c).filter(([, v]) => !isSuccess(v));
    assert.ok(negative.length >= 12, `C enum parsed sanely (got ${negative.length})`);
    for (const [cName, value] of negative) {
      assert.equal(js[value], jsNameOf(cName),
        `C code ${cName} (${value}) missing or misnamed in JS CODE_MAP`);
    }
    for (const [value, name] of Object.entries(js)) {
      const cName = Object.keys(c).find((k) => c[k] === Number(value));
      assert.ok(cName, `JS CODE_MAP has code ${value} (${name}) with no C counterpart`);
    }
  });

  it('RarErrorCode freeze and the TS union carry exactly the negative C names', () => {
    const c = parseCErrorCodes();
    const expected = new Set(
      Object.entries(c)
        .filter(([, v]) => !isSuccess(v))
        .map(([n]) => jsNameOf(n)),
    );
    for (const surface of [parseJsRarErrorCodeNames(), parseTsUnionNames()]) {
      for (const name of expected) {
        assert.ok(surface.has(name), `'${name}' missing from ${[...surface].join(',')}`);
      }
      for (const name of surface) {
        assert.ok(expected.has(name), `'${name}' in surface has no negative C counterpart`);
      }
    }
  });
});
