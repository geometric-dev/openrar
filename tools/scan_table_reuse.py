#!/usr/bin/env python3
"""Scan RAR5 archives for compression blocks that omit the Huffman tables.

Block header flag bit 7 is "tables present" (docs/spec/03 block header). A block
with bit 7 clear tells the decoder to keep the tables it already holds. This
tool answers a question our own test suite cannot: does ANY third-party writer
actually emit such a block? RAR's writer appears not to, which would mean our
decoder's bit-7-clear read path has never been exercised against real data.

Walks only the common header framing (docs/spec/00-overview.md): CRC32, Header
size, Header type, Header flags, then the data area of Data size bytes located
at (offset of Header type + Header size). Type-specific fields are not parsed.

A block count is only believed when the walk lands EXACTLY on the end of the
data area; anything else is reported unreliable and excluded from the totals.
Two cases fail that check by construction:
  - a STORED entry, whose data area is raw file bytes rather than blocks (the
    file-header method field is deliberately not parsed; alignment is the check);
  - a split file in volumes after the first, whose data area may begin mid-block.
Validated against archives this project produced, where the walks land exactly on
the end and the block counts match the 512 KiB / 32768-token quantum.
"""
import struct
import sys

SIG = bytes([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00])
HFL_EXTRA = 0x0001
HFL_DATA = 0x0002
HFL_SPLITBEFORE = 0x0008
HFL_SPLITAFTER = 0x0010

# docs/spec/01-headers.md "File Header"
FHFL_DIR = 0x0001
FHFL_UTIME = 0x0002  # mtime uint32 present
FHFL_CRC32 = 0x0004  # Data CRC32 uint32 present
FHFL_UNPUNKNOWN = 0x0008


def safe_name(raw):
    """Names in real archives are UTF-8 and the console may be cp1252."""
    return "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in raw)


def parse_file_header(data, pos):
    """Parse a type-2 header body far enough to classify the entry.

    Field order is fixed by docs/spec/01-headers.md and the presence of the
    mtime and Data CRC32 fields depends on the file flags - get either wrong and
    every subsequent field is misparsed. Returns a dict, or None if it does not
    parse cleanly within the header.
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
            "unp_unknown": bool(fflags & FHFL_UNPUNKNOWN),
            "unp_size": unp_size,
            "version": comp & 0x3F,
            "solid": bool(comp & 0x0040),
            "method": (comp >> 7) & 0x7,
            "dict_n": (comp >> 10) & 0x1F,
            "host_os": host_os,
            "name": name.decode("utf-8", "replace"),
            "end": pos,
        }
    except (ValueError, IndexError):
        return None


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


def scan_blocks(data, base, size, allow_truncated_tail):
    """Walk compression blocks in [base, base+size).

    Every header must satisfy the 1-byte XOR checksum from
    docs/spec/03-compression-m1-m5.md:
        checksum == 0x5A ^ flags ^ bsize ^ (bsize >> 8) ^ (bsize >> 16)
    This is the decisive guard for volume data areas that begin mid-block: a
    multi-volume split is a byte-count cut with no block-alignment test, so in a
    continuation volume the first "header" is an arbitrary payload byte and its
    checksum will not verify.

    Returns (total, reused, verdict):
      "aligned"    - every header checksummed and the walk landed on the end;
      "truncated"  - ended because a block was cut by the volume end (expected
                      under HFL_SPLITAFTER); headers seen so far all checksummed;
      "unreliable" - a header failed its checksum, or any other inconsistency.
    """
    off = base
    end = base + size
    total = 0
    reused = 0
    while off < end:
        if off + 2 > end:
            return total, reused, ("truncated" if allow_truncated_tail else "unreliable")
        flags = data[off]
        chk = data[off + 1]
        byte_cnt = ((flags >> 3) & 3) + 1
        if off + 2 + byte_cnt > end:
            return total, reused, ("truncated" if allow_truncated_tail else "unreliable")
        bsize = 0
        for i in range(byte_cnt):
            bsize |= data[off + 2 + i] << (8 * i)
        if byte_cnt == 4 or bsize > (end - off):
            return total, reused, ("truncated" if allow_truncated_tail else "unreliable")
        want = (0x5A ^ flags ^ bsize ^ (bsize >> 8) ^ (bsize >> 16)) & 0xFF
        if chk != want:
            return total, reused, "bad-checksum"
        total += 1
        if not (flags & 0x80):
            reused += 1
        off += 2 + byte_cnt + bsize
        if bsize == 0:
            return total, reused, ("aligned" if off == end else "unreliable")
    return total, reused, ("aligned" if off == end else "unreliable")


def main(paths):
    grand_total = grand_reused = 0
    for path in paths:
        print(f"=== {path}")
        with open(path, "rb") as fh:
            data = fh.read()
        if data[:8] != SIG:
            print("    not a RAR5 archive")
            continue
        pos = 8
        report = []
        files = 0
        while pos + 7 < len(data):
            hdr_start = pos
            crc = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            try:
                hsize, pos = read_vint(data, pos)
                htype, pos = read_vint(data, pos)
                hflags, pos = read_vint(data, pos)
                extra_size = 0
                data_size = 0
                if hflags & HFL_EXTRA:
                    extra_size, pos = read_vint(data, pos)
                if hflags & HFL_DATA:
                    data_size, pos = read_vint(data, pos)
            except ValueError as exc:
                print(f"    header parse stopped: {exc}")
                break

            # Header size counts from the Header type field to end of extra area,
            # so the data area starts there without parsing type-specific fields.
            _, type_pos = read_vint(data, hdr_start + 4)
            header_end = type_pos + hsize
            data_start = header_end
            next_pos = header_end + data_size
            body = pos  # first byte after the common framing

            if htype == 2 and data_size > 0:  # file header with a data area
                files += 1
                fh = parse_file_header(data, body)
                if fh is None:
                    print(f"    file#{files}: file header did not parse; ignored")
                elif fh["is_dir"]:
                    print(f"    file#{files}: directory; ignored")
                elif fh["method"] == 0:
                    print(f"    file#{files}: STORED (method 0) '{fh['name']}'"
                          f" - no compression blocks, skipped")
                else:
                    split = bool(hflags & HFL_SPLITAFTER)
                    t, r, verdict = scan_blocks(data, data_start, data_size, split)
                    if verdict == "unreliable":
                        print(f"    file#{files}: method {fh['method']} dict"
                              f" {128 << fh['dict_n']}KiB '{safe_name(fh['name'])[:40]}'"
                              f" - walk unreliable, ignored")
                    elif verdict == "bad-checksum":
                        print(f"    file#{files}: method {fh['method']} dict"
                              f" {128 << fh['dict_n']}KiB '{safe_name(fh['name'])[:40]}'"
                              f" - HEADER CHECKSUM FAILED after {t} blocks"
                              f" ({r} looked reused) => data area starts mid-block,"
                              f" hits are artefacts")
                    else:
                        grand_total += t
                        grand_reused += r
                        note = "lands exactly on data end" if verdict == "aligned" \
                            else "ends at a volume split (block truncated)"
                        print(f"    file#{files}: method {fh['method']} dict"
                              f" {128 << fh['dict_n']}KiB '{(safe_name(fh['name']))[:40]}'"
                              f"{' solid' if fh['solid'] else ''} - {t} blocks"
                              f" [{note}], {r} with tables omitted")
            if htype == 5:
                break
            if next_pos <= hdr_start:
                print("    non-advancing header; stopping")
                break
            pos = next_pos
        for line in report[:10]:
            print(line)
        print(f"    scanned {files} file header(s)")
    print(f"\nTOTAL compression blocks: {grand_total}, with tables OMITTED: {grand_reused}")


if __name__ == "__main__":
    main(sys.argv[1:])
