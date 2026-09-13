// RAR 5.0 format coverage analyzer.
// Parses an archive into typed blocks/records and reports which format
// features are present, so writer capability gaps can be measured against
// reference implementations (WinRAR/Rar.exe).
//
// Usage: node rar5-coverage.js <archive.rar...>            human-readable
//        node rar5-coverage.js <archive.rar...> --json     machine-readable
'use strict';

const SIG = Buffer.from([0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00]);

const BLOCK_TYPES = { 1: 'main', 2: 'file', 3: 'service', 4: 'crypt', 5: 'end' };
const MAIN_FLAGS = ['volume', 'volnumber', 'solid', 'protect', 'lock'];
const FILE_FLAGS = ['directory', 'utime', 'crc32', 'unpunknown'];
const END_FLAGS = ['nextvolume'];

function findSig(b) {
  outer: for (let i = 0; i <= Math.min(b.length - 8, 0x400000); i++) {
    for (let j = 0; j < 8; j++) if (b[i + j] !== SIG[j]) continue outer;
    return i;
  }
  return -1;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

class Reader {
  constructor(buf, pos) { this.b = buf; this.p = pos; }
  u8() { return this.b[this.p++]; }
  u32() { const v = this.b.readUInt32LE(this.p); this.p += 4; return v; }
  u64() { const v = this.b.readBigUInt64LE(this.p); this.p += 8; return Number(v); }
  vint() {
    let v = 0, mul = 1;
    for (;;) {
      const B = this.b[this.p++];
      v += (B & 0x7f) * mul;
      if (!(B & 0x80)) return v;
      mul *= 128;
      // Allow spec-legal overlong encodings up to 10 bytes.
      if (mul > 2 ** 70) throw new Error('vint too long');
    }
  }
  bytes(n) { const s = this.b.subarray(this.p, this.p + n); this.p += n; return s; }
  str(n) { return this.bytes(n).toString('utf8'); }
}

function dictLabel(n, frac) {
  let kb = 128 * 2 ** n;
  let label = kb >= 1048576 ? `${kb / 1024}GB` : kb >= 1024 ? `${kb / 1024}MB` : `${kb}KB`;
  if (frac) label += `+${frac}/32`;
  return label;
}

function parseExtra(recordsOut, buf, start, end, kind) {
  const r = new Reader(buf, start);
  while (r.p < end) {
    const recStart = r.p;
    const size = r.vint();
    const recEnd = Math.min(r.p + size, end);
    const type = r.vint();
    const rec = { kind, type, offset: recStart };
    try {
      switch (kind === 'main' && type === 1 ? 'locator'
        : kind === 'main' && type === 2 ? 'meta'
          : type) {
        case 'locator': {
          const f = r.vint();
          rec.flags = f;
          if (f & 0x01) rec.quickOpenOffset = r.vint();
          if (f & 0x02) rec.recoveryOffset = r.vint();
          break;
        }
        case 'meta': {
          const f = r.vint();
          rec.flags = f;
          if (f & 0x01) { const nl = r.vint(); rec.name = r.str(nl); }
          if (f & 0x02) { rec.time = (f & 0x04) ? ((f & 0x08) ? r.u64() : r.u32()) : r.u64(); }
          break;
        }
        case 1: { // file encryption
          rec.cryptVersion = r.vint();
          rec.flags = r.vint();
          rec.kdfCount = r.u8();
          rec.salt = r.bytes(16).toString('hex');
          rec.iv = r.bytes(16).toString('hex');
          if (rec.flags & 0x01) rec.check = r.bytes(12).toString('hex');
          break;
        }
        case 2: { // file hash
          rec.hashType = r.vint();
          rec.digest = r.bytes(recEnd - r.p).toString('hex');
          break;
        }
        case 3: { // high precision time
          const f = r.vint();
          rec.flags = f;
          rec.unixFormat = !!(f & 0x01);
          rec.mtime = !!(f & 0x02);
          rec.ctime = !!(f & 0x04);
          rec.atime = !!(f & 0x08);
          rec.nanoseconds = !!(f & 0x10);
          break;
        }
        case 4: // file version
          rec.flags = r.vint();
          rec.version = r.vint();
          break;
        case 5: { // redirection
          rec.redirType = ['', 'unix-symlink', 'win-symlink', 'junction', 'hardlink', 'file-copy'][r.vint()] || 'unknown';
          rec.flags = r.vint();
          const nl = r.vint();
          rec.target = r.str(nl);
          break;
        }
        case 6: { // unix owner
          const f = r.vint();
          rec.ownerFlags = f;
          if (f & 0x01) { const l = r.vint(); rec.userName = r.str(l); }
          if (f & 0x02) { const l = r.vint(); rec.groupName = r.str(l); }
          if (f & 0x04) rec.uid = r.vint();
          if (f & 0x08) rec.gid = r.vint();
          break;
        }
        case 7: // service data
          rec.dataSize = recEnd - r.p;
          break;
        default:
          rec.unknown = true;
      }
    } catch (e) {
      rec.parseError = String(e.message);
    }
    recordsOut.push(rec);
    r.p = recEnd;
  }
}

function parseArchive(buf) {
  const result = { sfx: false, blocks: [], encryptedHeaders: false, truncated: false };
  const sigAt = findSig(buf);
  if (sigAt < 0) throw new Error('not a RAR5 archive');
  result.sfx = sigAt > 0;
  let pos = sigAt + 8;

  for (;;) {
    if (pos + 7 > buf.length) { result.truncated = pos < buf.length; break; }
    const blockStart = pos;
    if (process.env.RAR5DBG) console.error(`BLK start=${pos}`);
    const storedCrc = buf.readUInt32LE(pos); pos += 4;
    const sizePos = pos;
    const r = new Reader(buf, pos);
    let hdrSize, type, flags, extraSize = 0, dataSize = 0;
    try {
      hdrSize = r.vint();
      const bodyStart = r.p;
      const bodyEnd = bodyStart + hdrSize;
      if (bodyEnd > buf.length) { result.truncated = true; break; }
      type = r.vint();
      flags = r.vint();
      if (process.env.RAR5DBG) console.error(`  hdrSize=${hdrSize} type=${type} flags=0x${flags.toString(16)}`);
      if (flags & 0x0001) extraSize = r.vint();
      if (flags & 0x0002) dataSize = r.vint();

      const block = { offset: blockStart, type, typeName: BLOCK_TYPES[type] || `unknown${type}`, hdrSize, dataSize };
      block.split = !!(flags & 0x0008) || !!(flags & 0x0010);

      if (result.encryptedHeaders) break;

      if (type === 4) {
        result.encryptedHeaders = true;
        result.blocks.push(block);
        break; // Everything past this point is AES-CBC ciphertext.
      }

      if (type === 1) {
        const archFlags = r.vint();
        block.archiveFlags = MAIN_FLAGS.filter((_, i) => archFlags & (1 << i));
        block.rawArchiveFlags = archFlags;
        if (archFlags & 0x0002) block.volumeNumber = r.vint(); // Must consume before extras.
      }

      if (type === 2 || type === 3) {
        const fileFlags = r.vint();
        const unpSize = r.vint();
        const attributes = r.vint();
        let mtime = null, dataCrc = null;
        if (fileFlags & 0x0002) mtime = r.u32();
        if (fileFlags & 0x0004) dataCrc = r.u32();
        const compInfo = r.vint();
        const hostOs = r.vint();
        const nameLen = r.vint();
        const name = r.str(nameLen);
        block.file = {
          name,
          isService: type === 3,
          service: type === 3 ? name.toUpperCase() : null,
          flags: FILE_FLAGS.filter((_, i) => fileFlags & (1 << i)),
          unpSize,
          attributes,
          mtime,
          dataCrc,
          algoVersion: compInfo & 0x3f,
          solid: !!(compInfo & 0x40),
          method: (compInfo >> 7) & 7,
          dict: dictLabel((compInfo >> 11) & 0x1f, (compInfo >> 15) & 0x1f),
          rar5Compat: !!(compInfo & 0x100000),
          hostOs: hostOs === 0 ? 'windows' : hostOs === 1 ? 'unix' : `os${hostOs}`,
        };
      }

      if (type === 5) block.endFlags = END_FLAGS.filter((_, i) => flags & (1 << i));

      // NOTE: extraSize counts record contents EXCLUDING each record's own
      // size vint (verified against Rar.exe output), so the reliable way to
      // walk extras is forward from the current cursor until bodyEnd.
      if (flags & 0x0001) {
        block.extra = [];
        parseExtra(block.extra, buf, r.p, bodyEnd, type === 1 ? 'main' : 'file');
      }
      r.p = bodyEnd;

      // Header CRC check (covers size vint .. end of extra area).
      block.headerCrcOk = crc32(buf.subarray(sizePos, bodyEnd)) === storedCrc;
      block.dataOffset = bodyEnd;

      result.blocks.push(block);
      pos = bodyEnd + dataSize;

      if (type === 5) break;
      if (pos > buf.length) { result.truncated = true; break; }
    } catch (e) {
      result.truncated = true;
      result.parseError = `${e.message} @${pos}`;
      break;
    }
  }
  return result;
}

function analyze(parsed) {
  const f = {
    sfx: parsed.sfx,
    encryptedHeaders: parsed.encryptedHeaders,
    truncated: parsed.truncated,
    blockTypes: [],
    splitBlocks: false,
    archiveVolume: false, volnumber: false, archiveSolid: false, protectFlag: false, lock: false,
    locator: false, locatorQuickOpen: false, locatorRR: false,
    archiveMetadata: false,
    services: [],
    fileExtraRecords: [],
    blake2: false, crc32Field: false, utimeHeader: false, unpUnknown: false,
    htime: { any: false, mtime: false, ctime: false, atime: false, unixFormat: false, nanoseconds: false },
    fileVersion: false, redirection: [], unixOwner: false, serviceData: false,
    methods: [], dicts: [], algoVersions: [], solidFiles: false, rar5Compat: false,
    hostOs: [], dirEntries: false,
    nextVolumeEnd: false,
    files: 0, dirs: 0,
  };
  const S = {
    blockTypes: new Set(), services: new Set(), fileExtraRecords: new Set(),
    redirection: new Set(), methods: new Set(), dicts: new Set(), algoVersions: new Set(), hostOs: new Set(),
  };

  for (const b of parsed.blocks) {
    S.blockTypes.add(b.typeName);
    if (b.split) f.splitBlocks = true;
    if (b.archiveFlags) {
      f.archiveVolume ||= b.archiveFlags.includes('volume');
      f.volnumber ||= b.archiveFlags.includes('volnumber');
      f.archiveSolid ||= b.archiveFlags.includes('solid');
      f.protectFlag ||= b.archiveFlags.includes('protect');
      f.lock ||= b.archiveFlags.includes('lock');
    }
    if (b.endFlags && b.endFlags.length) f.nextVolumeEnd = true;
    for (const rec of b.extra || []) {
      const key = b.typeName === 'main'
        ? (rec.type === 1 ? 'locator' : rec.type === 2 ? 'metadata' : `mainExtra${rec.type}`)
        : `extra${rec.type}`;
      S.fileExtraRecords.add(key);
      if (key === 'locator') {
        f.locator = true;
        f.locatorQuickOpen |= !!(rec.flags & 0x01);
        f.locatorRR |= !!(rec.flags & 0x02);
      }
      if (key === 'metadata') f.archiveMetadata = true;
      if (rec.type === 2 && rec.hashType === 0) f.blake2 = true;
      if (rec.type === 3) {
        f.htime.any = true;
        f.htime.mtime ||= rec.mtime; f.htime.ctime ||= rec.ctime; f.htime.atime ||= rec.atime;
        f.htime.unixFormat ||= rec.unixFormat; f.htime.nanoseconds ||= rec.nanoseconds;
      }
      if (rec.type === 4) f.fileVersion = true;
      if (rec.type === 5) S.redirection.add(rec.redirType);
      if (rec.type === 6) f.unixOwner = true;
      if (rec.type === 7) f.serviceData = true;
    }
    if (b.file) {
      if (b.file.isService) {
        S.services.add(b.file.service || '?');
      } else {
        f.files++;
        if (b.file.flags.includes('directory')) { f.dirs++; f.dirEntries = true; }
        f.crc32Field ||= b.file.flags.includes('crc32');
        f.utimeHeader ||= b.file.flags.includes('utime');
        f.unpUnknown ||= b.file.flags.includes('unpunknown');
        S.methods.add(b.file.method);
        S.dicts.add(b.file.dict);
        S.algoVersions.add(b.file.algoVersion);
        f.solidFiles ||= b.file.solid;
        f.rar5Compat ||= b.file.rar5Compat;
        S.hostOs.add(b.file.hostOs);
      }
    }
  }
  f.blockTypes = [...S.blockTypes];
  f.services = [...S.services];
  f.fileExtraRecords = [...S.fileExtraRecords];
  f.redirection = [...S.redirection];
  f.methods = [...S.methods].sort();
  f.dicts = [...S.dicts];
  f.algoVersions = [...S.algoVersions].sort();
  f.hostOs = [...S.hostOs];
  return f;
}

function fmtFeature(name, v) {
  if (Array.isArray(v)) return v.length ? `${name}: ${v.join(',')}` : '';
  if (typeof v === 'object' && v !== null) {
    const keys = Object.keys(v).filter((k) => v[k]);
    return keys.length ? `${name}: ${keys.join(',')}` : '';
  }
  if (v === true) return name;
  if (!v) return '';
  return `${name}: ${v}`;
}

function report(path, buf) {
  const parsed = parseArchive(buf);
  const f = analyze(parsed);
  const lines = [`== ${path}${parsed.sfx ? ' (SFX)' : ''}${parsed.encryptedHeaders ? ' [ENCRYPTED HEADERS]' : ''}${parsed.truncated ? ' [TRUNCATED PARSE]' : ''}`];
  const out = [];
  for (const [k, v] of Object.entries(f)) {
    const s = fmtFeature(k, v);
    if (s) out.push(`  ${s}`);
  }
  lines.push(...out.sort());
  return { text: lines.join('\n'), features: f };
}

module.exports = { parseArchive, analyze };

if (require.main === module) {
  const json = process.argv.includes('--json');
  const paths = process.argv.slice(2).filter((p) => p !== '--json');
  if (paths.length === 0) {
    console.error('usage: node rar5-coverage.js <archive.rar> [...] [--json]');
    process.exit(1);
  }
  const all = {};
  for (const p of paths) {
    const buf = require('fs').readFileSync(p);
    const { text, features } = report(p, buf);
    all[p] = features;
    if (!json) console.log(text + '\n');
  }
  if (json) console.log(JSON.stringify(all, null, 2));
}
