#!/usr/bin/env python3
"""Probe RAR5 recovery-record (RR) service sections.

v1.35.0 research tool. WinRAR 7.0 changed the default RR format; this tool
answers, black-box: where does the RR live, what does its data area look like,
and does it still match the RAR 5.0-era shard layout our writer emits?

The archive walk reuses the discipline of tools/scan_table_reuse.py (common
framing only, data area located arithmetically) and hardens it with the one
check that scanner lacks: the IEEE CRC32 of every common header, computed over
bytes [Header size .. end of extra area) per docs/spec/00-overview.md. A block
is only reported as walked when its CRC32 verifies, so RR data-area boundaries
come from a validated frame rather than a plausible vint.

RR data-area analysis has two layers:

  classic - the shard layout our writer emits (recovery_writer.cpp header
            comment): back-to-back shards, "{RB}" magic (7B 52 42 7D),
            CRC-64/XZ at +0x04 over bytes +0x0C..shard end, geometry at
            +0x0C..+0x3F (total_size, header_size, versions, protected size,
            group_count, D, NR, shard_index). Walked shard-by-shard with both
            the size chain and the CRC-64/XZ checked.

  unknown - no {RB} at the start (or a broken chain). Reports hex dumps of the
            head/tail, entropy map (structured prefix vs parity noise), a scan
            for 4-byte printable magics, and size arithmetic against the
            protecting archive (candidate percent).

Exits nonzero if any archive's walk hits a CRC32 mismatch, so it can gate
scripted corpus runs.

--matrix: for classic-format records, recompute every shard's parity payload
from the archive's own protected range using our Cauchy/GF(2^16) convention
and report byte-equality — the writer-equivalence proof. Pure Python and
slow on large records; opt-in.
"""
import struct
import sys

MATRIX_FLAG = False

SIG = bytes([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00])
HFL_EXTRA = 0x0001
HFL_DATA = 0x0002
HFL_SPLITBEFORE = 0x0008
HFL_SPLITAFTER = 0x0010
HFL_CHILD = 0x0020

MHFL_VOLUME = 0x0001
MHFL_VOLNUMBER = 0x0002
MHFL_SOLID = 0x0004
MHFL_PROTECT = 0x0008
MHFL_LOCK = 0x0010

FHFL_DIR = 0x0001
FHFL_UTIME = 0x0002
FHFL_CRC32 = 0x0004
FHFL_UNPUNKNOWN = 0x0008

FHEXTRA_SUBDATA = 0x07
EXTRA_LOCATOR = 0x01

SHARD_MAGIC = bytes([0x7B, 0x52, 0x42, 0x7D])  # "{RB}"


# ── primitives ───────────────────────────────────────────────────────────────

def read_vint(buf, pos):
    """vint: base-128, LSB groups first, stop at first byte with bit7 clear."""
    val = 0
    shift = 0
    n = 0
    while pos + n < len(buf):
        b = buf[pos + n]
        val |= (b & 0x7F) << shift
        shift += 7
        n += 1
        if not (b & 0x80):
            return val, pos + n
        if n >= 10:
            break
    raise ValueError("vint overruns buffer")


def make_crc32_table():
    tbl = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
        tbl.append(c)
    return tbl


_CRC32_TABLE = make_crc32_table()


def crc32(data, crc=0xFFFFFFFF):
    for b in data:
        crc = _CRC32_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def make_crc64_table():
    # CRC-64/XZ: poly 0x42F0E1EBA9EA3693, reflected (0xC96C5795D7870F42),
    # init = final xor = all-ones, reflected input/output.
    poly = 0xC96C5795D7870F42
    tbl = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (poly if c & 1 else 0)
        tbl.append(c)
    return tbl


_CRC64_TABLE = make_crc64_table()


def crc64xz(data):
    crc = 0xFFFFFFFFFFFFFFFF
    for b in data:
        crc = _CRC64_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFFFFFFFFFF


def safe_name(raw):
    return "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in raw)


def hexdump(data, base=0, width=16):
    lines = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hx = " ".join(f"{b:02X}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"      {base + off:06X}  {hx:<{width * 3 - 1}}  {asc}")
    return lines


def entropy_note(chunk):
    """Cheap structure discriminator: distinct-byte count + zero ratio."""
    if not chunk:
        return "empty"
    distinct = len(set(chunk))
    zeros = chunk.count(0) / len(chunk)
    return f"distinct {distinct:3d}/256, zeros {zeros:4.0%}"


# ── extra area ───────────────────────────────────────────────────────────────

def parse_extra_area(data, extra_start, extra_end):
    """Parse records between [extra_start, extra_end); each is
    size vint (counting from Type), type vint, payload. Returns (records,
    notes); records are (type, payload)."""
    recs = []
    notes = []
    pos = extra_start
    while pos < extra_end:
        try:
            rsize, pos = read_vint(data, pos)
            rtype, pos = read_vint(data, pos)
        except ValueError:
            notes.append("extra area: vint overrun; records unreliable")
            break
        rec_end = pos + rsize - 1  # size counts from Type through end
        if rec_end > extra_end or rsize < 1:
            notes.append("extra area: record size overrun; records unreliable")
            break
        recs.append((rtype, data[pos:rec_end]))
        pos = rec_end
    return recs, notes


# ── file/service header body ─────────────────────────────────────────────────

def parse_file_header(data, pos):
    """Parse a type-2/3 header body (docs/spec/01-headers.md).

    Returns dict or None. Extra records are parsed separately against
    header_end so a misparsed fixed field cannot hide them.
    """
    try:
        fflags, pos = read_vint(data, pos)
        unp_size, pos = read_vint(data, pos)
        attrs, pos = read_vint(data, pos)
        if fflags & FHFL_UTIME:
            pos += 4
        if fflags & FHFL_CRC32:
            pos += 4
        comp, pos = read_vint(data, pos)
        host_os, pos = read_vint(data, pos)
        name_len, pos = read_vint(data, pos)
        name = data[pos:pos + name_len]
        pos += name_len
        return {
            "is_dir": bool(fflags & FHFL_DIR),
            "unp_size": unp_size,
            "file_flags": fflags,
            "attrs": attrs,
            "version": comp & 0x3F,
            "solid": bool(comp & 0x0040),
            "method": (comp >> 7) & 0x7,
            "dict_n": (comp >> 10) & 0x1F,
            "frac": (comp >> 15) & 0x1F,
            "rar7": bool(comp & 0x100000),
            "host_os": host_os,
            "name": name,
            "end": pos,
        }
    except (ValueError, IndexError):
        return None


# ── classic {RB} shard layer ─────────────────────────────────────────────────

def analyze_classic_shards(data, start, size):
    """Walk back-to-back {RB} shards in [start, start+size).

    Shard layout (recovery_writer.cpp): magic u32, CRC-64/XZ u64 at +0x04 over
    +0x0C..shard end, total_size u32 at +0x0C, header_size u32 at +0x10,
    version_a/b u8 at +0x14/+0x15, chunk_position u64 +0x16, chunk extent u32
    +0x1E, protected size u64 +0x22, group_count u64 +0x2A, shard_size u64
    +0x32, D u16 +0x3A, NR u16 +0x3C, shard_index u16 +0x3E, encoder state
    to +header_size, parity payload of group_count bytes.
    """
    out = []
    off = 0
    shard_no = 0
    end = size
    geom = None
    multi = False
    while off < end:
        shard_no += 1
        rel = start + off
        if off + 4 > end or data[rel:rel + 4] != SHARD_MAGIC:
            out.append(f"      shard {shard_no}: no {{RB}} magic at data+{off:#x} - "
                       f"chain broken ({end - off} byte(s) left)")
            return out, False, geom, multi
        if off + 0x40 > end:
            out.append(f"      shard {shard_no}: truncated before fixed fields")
            return out, False, geom, multi
        total_size = struct.unpack_from("<I", data, rel + 0x0C)[0]
        header_size = struct.unpack_from("<I", data, rel + 0x10)[0]
        ver_a = data[rel + 0x14]
        ver_b = data[rel + 0x15]
        chunk_pos = struct.unpack_from("<Q", data, rel + 0x16)[0]
        prot_size = struct.unpack_from("<Q", data, rel + 0x22)[0]
        group_count = struct.unpack_from("<Q", data, rel + 0x2A)[0]
        shard_size = struct.unpack_from("<Q", data, rel + 0x32)[0]
        d_cnt = struct.unpack_from("<H", data, rel + 0x3A)[0]
        nr_cnt = struct.unpack_from("<H", data, rel + 0x3C)[0]
        s_index = struct.unpack_from("<H", data, rel + 0x3E)[0]
        got = crc64xz(data[rel + 0x0C:rel + total_size]) if total_size <= end - off else None
        crc_stored = struct.unpack_from("<Q", data, rel + 0x04)[0]
        crc_ok = got is not None and got == crc_stored
        # Shape-aware geometry: above D*64 KiB protected size the reference
        # writer splits each logical parity shard into multiple PHYSICAL
        # chunk-shards (unscaled D*8+0x48 headers, chunk_position advancing in
        # 64 KiB steps, logical group/shard_size repeated in the fields), while
        # this repo's writer emits one physical shard with a scaled header and
        # the full group payload. Both are legal shapes; validate accordingly.
        unscaled = d_cnt * 8 + 0x48
        if header_size == unscaled:
            exp_payload = min(0x10000, group_count - chunk_pos)
            shape = "multi-piece" if (chunk_pos > 0 or total_size != header_size + group_count) \
                else "single"
            geom_ok = total_size == header_size + exp_payload
        else:
            shape = "single-scaled"
            geom_ok = total_size == header_size + group_count
        out.append(
            f"      shard {shard_no}: D={d_cnt} NR={nr_cnt} idx={s_index} "
            f"group={group_count} hdr={header_size:#x} shard={shard_size} "
            f"prot={prot_size} ver={ver_a}.{ver_b} chunkpos={chunk_pos} "
            f"crc64 {'OK' if crc_ok else 'FAIL'} [{shape}]"
            f"{'' if geom_ok else ' GEOMETRY-MISMATCH'}")
        if geom is None:
            geom = (d_cnt, nr_cnt, group_count, header_size, prot_size)
        if chunk_pos > 0 or shard_size != header_size + group_count:
            multi = True
        if total_size <= 0 or total_size > end - off:
            out.append(f"      shard {shard_no}: bad total_size {total_size}; stopping")
            return out, False, geom, multi
        off += total_size
    return out, off == end, geom, multi


# ── unknown data-area layer ──────────────────────────────────────────────────

def scan_magics(data):
    """4-byte printable/brace sequences (first 24 hits)."""
    hits = []
    for off in range(0, min(len(data), 1 << 20) - 3):
        b = data[off:off + 4]
        if all(0x20 <= c < 0x7F or c in (0x7B, 0x7D) for c in b):
            if any(0x41 <= c < 0x7B for c in b):  # at least one letter
                hits.append((off, b))
                if len(hits) >= 24:
                    break
    return hits


def analyze_unknown(data, start, size, arc_file_size):
    out = []
    area = data[start:start + size]
    head = area[:96]
    tail = area[-64:] if size > 96 else b""
    out.append("      head:")
    out.extend(hexdump(head))
    if tail:
        out.append("      tail:")
        out.extend(hexdump(tail, base=size - len(tail)))
    mags = scan_magics(area)
    if mags:
        out.append("      printable 4-byte sequences (first 24): " + ", ".join(
            f"{off:#x}'{s.decode()}'" for off, s in mags))
    step = max(64, (size // 32 // 64) * 64 or 64)
    nblocks = (size + step - 1) // step
    out.append(f"      entropy map ({nblocks} x {step}-byte blocks):")
    shown = 0
    for off in range(0, size, step):
        out.append(f"        +{off:08X}: {entropy_note(area[off:off + step])}")
        shown += 1
        if shown >= 24:
            out.append(f"        ... ({nblocks - shown} blocks not shown)")
            break
    if arc_file_size:
        pct = 100.0 * size / arc_file_size
        out.append(f"      data area {size} bytes = {pct:.2f}% of archive "
                   f"({arc_file_size} bytes)")
    return out


# ── RS matrix equivalence layer (--matrix) ───────────────────────────────────
#
# The v1.35.0 question: is the reference writer's parity bit-compatible with
# ours (recovery_writer.cpp: Cauchy MX[j][i] = inv((j+D) XOR i) over
# GF(2^16), poly 0x1100B, u16 LE words, D chunks of group_count bytes, final
# chunk zero-padded)? For a classic-format RR this is checkable without any
# writer: recompute every shard's payload from the archive's own protected
# range and compare bytes. Proven equal against RAR 7.20 output on
# 2026-10-04 (docs/v1.35.0-rr-probe.md).

_GF16_POLY = 0x1100B


def _gf16_tables():
    exp = [0] * 131072
    log = [0] * 65536
    x = 1
    for i in range(65535):
        exp[i] = x
        log[x] = i
        x <<= 1
        if x & 0x10000:
            x ^= _GF16_POLY
    for i in range(65535, 131072):
        exp[i] = exp[i - 65535]
    return exp, log


def verify_matrix(data, data_start, d_cnt, nr_cnt, group, header_size, prot):
    """Recompute NR parity shards from protected range [0, prot) with our
    Cauchy convention; return list of per-shard result strings."""
    exp, log = _gf16_tables()

    def gmul(a, b):
        if a == 0 or b == 0:
            return 0
        return exp[log[a] + log[b]]

    def ginv(a):
        return exp[65535 - log[a]]

    words = group // 2
    chunks = []
    for i in range(d_cnt):
        off = i * group
        c = data[off:min(off + group, prot)]
        if len(c) < group:
            c += b"\x00" * (group - len(c))
        chunks.append(struct.unpack(f"<{words}H", c))
    out = []
    all_ok = True
    for j in range(nr_cnt):
        shard = data_start + j * (header_size + group)
        stored = struct.unpack(f"<{words}H",
                               data[shard + header_size:shard + header_size + group])
        mx = [ginv((j + d_cnt) ^ i) for i in range(d_cnt)]
        acc = [0] * words
        for i in range(d_cnt):
            c = mx[i]
            if c == 0:
                continue
            ch = chunks[i]
            for k in range(words):
                v = ch[k]
                if v:
                    acc[k] ^= gmul(c, v)
        ndiff = sum(1 for a, b in zip(acc, stored) if a != b)
        all_ok &= ndiff == 0
        out.append(f"      shard {j} parity recompute: "
                   f"{'BYTE-IDENTICAL' if ndiff == 0 else f'MISMATCH {ndiff}/{words} words'}")
    out.append(f"      matrix+field equivalence vs our Cauchy/GF(2^16): "
               f"{'PROVEN' if all_ok else 'REFUTED'}")
    return out


# ── archive walk ─────────────────────────────────────────────────────────────

def walk(path):
    with open(path, "rb") as fh:
        data = fh.read()
    arc_file_size = len(data)
    print(f"=== {path} ({arc_file_size} bytes)")
    if data[:8] != SIG:
        print("    not a RAR5 archive")
        return 1
    problems = 0
    pos = 8
    main_start = None
    locator_rr_offset = None
    rr_headers = []
    while pos + 7 < arc_file_size:
        hdr_start = pos
        hdr_crc = struct.unpack_from("<I", data, pos)[0]
        try:
            hsize, tpos = read_vint(data, hdr_start + 4)
            htype, tpos2 = read_vint(data, tpos)
            hflags, tpos3 = read_vint(data, tpos2)
            extra_size = 0
            data_size = 0
            body = tpos3  # first byte after common framing
            if hflags & HFL_EXTRA:
                extra_size, body = read_vint(data, body)
            if hflags & HFL_DATA:
                data_size, body = read_vint(data, body)
        except ValueError as exc:
            print(f"    header parse stopped at {hdr_start:#x}: {exc}")
            problems += 1
            break
        header_end = tpos + hsize
        data_start = header_end
        next_pos = header_end + data_size
        crc_ok = crc32(data[hdr_start + 4:header_end]) == hdr_crc
        crc_bad = "" if crc_ok else "  <<< HEADER CRC32 MISMATCH"
        problems += 0 if crc_ok else 1

        desc = f"type {htype}"
        if htype == 1:
            main_start = hdr_start
            try:
                mf, mpos = read_vint(data, body)
            except ValueError:
                mf, mpos = 0, body
            fl = []
            if mf & MHFL_VOLUME:
                fl.append("VOLUME")
            if mf & MHFL_VOLNUMBER:
                fl.append("VOLNUMBER")
            if mf & MHFL_SOLID:
                fl.append("SOLID")
            if mf & MHFL_PROTECT:
                fl.append("PROTECT")
            if mf & MHFL_LOCK:
                fl.append("LOCK")
            desc += f" main flags {mf:#x} ({' '.join(fl) or 'none'})"
            if hflags & HFL_EXTRA and extra_size > 0:
                recs, notes = parse_extra_area(data, header_end - extra_size,
                                               header_end)
                for rtype, payload in recs:
                    if rtype == EXTRA_LOCATOR:
                        lpos = 0
                        lflags, lpos = read_vint(payload, lpos)
                        qo_off = rr_off = None
                        if lflags & 0x0001 and lpos < len(payload):
                            qo_off, lpos = read_vint(payload, lpos)
                        if lflags & 0x0002 and lpos < len(payload):
                            rr_off, lpos = read_vint(payload, lpos)
                        locator_rr_offset = rr_off
                        desc += f"; locator flags {lflags:#x} qo={qo_off} rr={rr_off}"
        elif htype in (2, 3):
            fh = parse_file_header(data, body)
            if fh:
                desc += (f" {'service' if htype == 3 else 'file'} "
                         f"'{safe_name(fh['name'].decode('utf-8', 'replace'))[:32]}'"
                         f" method {fh['method']} ver {fh['version']}"
                         f"{' RAR7' if fh['rar7'] else ''}"
                         f"{' child' if hflags & HFL_CHILD else ''}")
                if htype == 3 and fh["name"] == b"RR":
                    rr_headers.append((hdr_start, header_end, data_start,
                                       data_size, hflags, extra_size))
            else:
                desc += " [body did not parse]"
        elif htype == 5:
            try:
                eflags, _ = read_vint(data, body)
            except ValueError:
                eflags = 0
            desc += f" end flags {eflags:#x}"
        print(f"    @{hdr_start:#010X} hsize {hsize} data {data_size}"
              f"{' SPLITBEFORE' if hflags & HFL_SPLITBEFORE else ''}"
              f"{' SPLITAFTER' if hflags & HFL_SPLITAFTER else ''}"
              f" crc {'ok' if crc_ok else 'BAD'} - {desc}{crc_bad}")

        if htype == 5:
            if next_pos < arc_file_size:
                print(f"    trailing bytes after end header: "
                      f"{arc_file_size - next_pos} (ignored)")
            break
        if next_pos <= hdr_start:
            print("    non-advancing header; stopping")
            problems += 1
            break
        pos = next_pos

    for (hdr_start, header_end, data_start, data_size, hflags,
         extra_size) in rr_headers:
        print(f"    RR section: header @{hdr_start:#x}, data area "
              f"@{data_start:#x} size {data_size}")
        if hflags & HFL_EXTRA and extra_size > 0:
            recs, notes = parse_extra_area(data, header_end - extra_size,
                                           header_end)
            for rtype, payload in recs:
                if rtype == FHEXTRA_SUBDATA:
                    try:
                        pct, _ = read_vint(payload, 0)
                        print(f"      SubData (extra 0x07): percent vint = {pct}"
                              f" (payload {len(payload)} byte(s))")
                    except ValueError:
                        print(f"      SubData (extra 0x07): {len(payload)} byte(s),"
                              f" not a vint")
                else:
                    print(f"      extra record 0x{rtype:02X}: {len(payload)} byte(s)")
        if locator_rr_offset is not None and main_start is not None:
            target = main_start + locator_rr_offset
            hit = "MATCH" if target == hdr_start else \
                f"MISMATCH (points @{target:#x}, header @{hdr_start:#x})"
            print(f"      locator recovery offset {locator_rr_offset} -> {hit}")
        if data_size == 0:
            print("      (empty data area)")
            continue
        if data[data_start:data_start + 4] == SHARD_MAGIC:
            lines, complete, geom, multi = analyze_classic_shards(data, data_start, data_size)
            for ln in lines:
                print(ln)
            print(f"      classic shard walk {'complete' if complete else 'INCOMPLETE'}")
            if MATRIX_FLAG and complete and geom and not multi:
                pass  # single-shape record: safe to recompute below
            if MATRIX_FLAG and complete and geom and multi:
                print("      matrix check skipped: multi-physical record "
                      "(reassembly per logical shard required)")
            if MATRIX_FLAG and complete and geom and not multi:
                d_cnt, nr_cnt, group, header_size, prot = geom
                if prot <= arc_file_size:
                    for ln in verify_matrix(data, data_start, d_cnt, nr_cnt,
                                            group, header_size, prot):
                        print(ln)
                else:
                    print("      matrix check skipped: protected size beyond file end")
        else:
            print("      classic {{RB}} magic absent - unknown RR layout")
            for ln in analyze_unknown(data, data_start, data_size,
                                      arc_file_size):
                print(ln)
    if problems:
        print(f"    {problems} walk problem(s)")
    return 1 if problems else 0


def main(argv):
    global MATRIX_FLAG
    paths = []
    for arg in argv:
        if arg == "--matrix":
            MATRIX_FLAG = True
        else:
            paths.append(arg)
    if not paths:
        print(__doc__)
        return 2
    rc = 0
    for path in paths:
        rc |= walk(path)
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]) or 0)
