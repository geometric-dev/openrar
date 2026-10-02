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


def scan_blocks(data, base, size, label, report):
    """Walk compression blocks in [base, base+size). Returns (total, reused, aligned).

    `aligned` is the real validator: a correct walk lands exactly on the end of
    the data area. A walk that overruns or stops short is parsing noise (raw
    stored bytes, or a data area that begins mid-block because a split file's
    block straddles a volume boundary), and its reuse count must not be believed.
    """
    off = base
    end = base + size
    total = 0
    reused = 0
    terminator = False
    while off < end:
        if off + 2 > end:
            break
        flags = data[off]
        byte_cnt = ((flags >> 3) & 3) + 1
        if off + 2 + byte_cnt > end:
            break
        bsize = 0
        for i in range(byte_cnt):
            bsize |= data[off + 2 + i] << (8 * i)
        if bsize > (end - off):
            break
        tables_present = bool(flags & 0x80)
        total += 1
        if not tables_present:
            reused += 1
        off += 2 + byte_cnt + bsize
        if bsize == 0:
            terminator = True
            break
    aligned = (off == end)
    return total, reused, (aligned, terminator)


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

            # Header size counts from the Header type field to end of extra area.
            type_pos = hdr_start + 4 + (pos - (hdr_start + 4))
            # recompute: position of the Header type vint
            type_pos = hdr_start + 4
            _, probe = read_vint(data, type_pos)
            type_pos = probe
            header_end = type_pos + hsize
            data_start = header_end
            next_pos = header_end + data_size

            if htype == 2 and data_size > 0:  # file header with a data area
                files += 1
                t, r, (aligned, term) = scan_blocks(data, data_start, data_size, f"file#{files}", report)
                if aligned:
                    grand_total += t
                    grand_reused += r
                    print(f"    file#{files}: WALK VALIDATED (lands exactly on data end)"
                          f" - {t} blocks, {r} with tables omitted")
                else:
                    print(f"    file#{files}: walk did NOT align - parse unreliable, ignored")
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