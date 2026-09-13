// Runner for the OpenRAR writer test suite (node --test).
//
// Usage:
//   node tools/run-tests.cjs              # full suite (Windows + WinRAR local)
//   node tools/run-tests.cjs format roundtrip parity
//                                         # only suites matching these prefixes
//                                         # (the WinRAR-independent subset CI runs)
//
// Environment:
//   OPENRAR_EXE  path to the openrar binary (default: build-tree candidates)
//   UNRAR_EXE    path to a reference UnRAR; when absent, suites that need the
//                oracle skip their oracle asserts (structural checks still run)
'use strict';
const { spawnSync } = require('node:child_process');
const { existsSync } = require('node:fs');
const path = require('node:path');

const repo = path.dirname(__dirname);
const files = [
  // Core writer output — always-passing structural checks
  path.join(repo, 'tools', 'tests', 'format.tests.mjs'),
  path.join(repo, 'tools', 'tests', 'roundtrip.tests.mjs'),
  // Archive mutation commands (d/u/f/m/k)
  path.join(repo, 'tools', 'tests', 'mutation.tests.mjs'),
  // Small parity / coverage-gap fixtures
  path.join(repo, 'tools', 'tests', 'parity.tests.mjs'),
  // Multi-volume create (-v)
  path.join(repo, 'tools', 'tests', 'volume.tests.mjs'),
  // Recovery records (-rr) and repair (r)
  path.join(repo, 'tools', 'tests', 'recovery.tests.mjs'),
  // NTFS alternate data streams (-os)  [skip-guarded on non-Windows]
  path.join(repo, 'tools', 'tests', 'streams.tests.mjs'),
  // Redirections / symlinks (-ol)      [skip-guarded on privilege failures]
  path.join(repo, 'tools', 'tests', 'links.tests.mjs'),
  // Dictionary size flags (-md)
  path.join(repo, 'tools', 'tests', 'dictionary.tests.mjs'),
  // Compression filters (DELTA/E8/…)   [skip-guarded until implemented]
  path.join(repo, 'tools', 'tests', 'filters.tests.mjs'),
  // Encryption (-p / -hp)              [skip-guarded until implemented]
  path.join(repo, 'tools', 'tests', 'encryption.tests.mjs'),
];

const filters = process.argv.slice(2);
const selected = filters.length
  ? files.filter((f) => filters.some((prefix) =>
      path.basename(f).startsWith(prefix)))
  : files;

const ourExe = process.env.OPENRAR_EXE;
const foundExe = ourExe
  ? existsSync(ourExe)
  : existsSync(path.join(repo, 'build', 'openrar64', 'Release', 'openrar.exe')) ||
    existsSync(path.join(repo, 'build', 'unrar64', 'Release', 'openrar.exe')) ||
    // Single-config generators (Ninja on Linux/macOS) drop the binary at the
    // build root; also honour MSVC's flat Release directory.
    existsSync(path.join(repo, 'build', 'openrar')) ||
    existsSync(path.join(repo, 'build', 'Release', 'openrar.exe'));
if (!foundExe) {
  console.error('openrar binary not found (set OPENRAR_EXE or build first).');
  process.exit(2);
}

for (const f of selected)
  if (!existsSync(f)) {
    console.error(`missing test file: ${f}`);
    process.exit(2);
  }

const res = spawnSync(process.execPath, ['--test', ...selected], {
  stdio: 'inherit',
  env: process.env,
});
process.exitCode = res.status ?? 1;
