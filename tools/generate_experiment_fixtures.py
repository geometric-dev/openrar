#!/usr/bin/env python3
"""
Generate deterministic experiment fixtures for Wave 1b (PROBE-PREDICT) & Wave 3.
Corpus includes:
  - BMP 8/16/24/32 bpp at widths 1919/1920/1921 (padding on/off)
  - uncompressed TIFF LE+BE
  - TGA type 2
  - PPM/PGM (synthetic ramps + seeded noise)
  - 5.1/7.1 WAV at 16/24/32-bit
  - JUNK-prefixed WAV
  - BW64/RF64
  - AIFF
  - CAF
  - 4 public-domain Kodak photographs (converted to uncompressed 24-bit BMP)
"""

import hashlib
import math
import os
import random
import struct
import urllib.request
from pathlib import Path
from PIL import Image
import io

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "tests" / "fixtures" / "experiment"
OUT_DIR.mkdir(parents=True, exist_ok=True)

SEED = 42
RNG = random.Random(SEED)

def make_ramp_noise_image(width, height, channels):
    """Generate image pixel data: smooth spatial gradients with small seeded noise."""
    data = bytearray(width * height * channels)
    idx = 0
    for y in range(height):
        fy = y / max(1, height - 1)
        for x in range(width):
            fx = x / max(1, width - 1)
            noise = RNG.randint(-3, 3)
            if channels == 1:
                val = int((fx * 180 + fy * 60 + noise)) & 0xFF
                data[idx] = val
                idx += 1
            elif channels == 3: # B, G, R
                b = int((fx * 200 + fy * 30 + noise)) & 0xFF
                g = int((fx * 80 + fy * 160 + noise)) & 0xFF
                r = int((fx * 40 + fy * 200 + noise)) & 0xFF
                data[idx] = b
                data[idx + 1] = g
                data[idx + 2] = r
                idx += 3
            elif channels == 4: # B, G, R, A
                b = int((fx * 200 + fy * 30 + noise)) & 0xFF
                g = int((fx * 80 + fy * 160 + noise)) & 0xFF
                r = int((fx * 40 + fy * 200 + noise)) & 0xFF
                a = 255
                data[idx] = b
                data[idx + 1] = g
                data[idx + 2] = r
                data[idx + 3] = a
                idx += 4
    return data

def generate_bmp(filename, width, height, bpp):
    """Generate uncompressed Windows BMP file."""
    path = OUT_DIR / filename
    row_data_bytes = (width * bpp + 7) // 8
    row_stride = ((row_data_bytes + 3) // 4) * 4
    pad_bytes = row_stride - row_data_bytes
    
    num_colors = 256 if bpp == 8 else 0
    palette_bytes = num_colors * 4
    header_size = 14 + 40 + palette_bytes
    image_size = row_stride * height
    file_size = header_size + image_size
    
    # 1. BITMAPFILEHEADER
    hdr = bytearray(14)
    hdr[0:2] = b'BM'
    struct.pack_into('<IHHI', hdr, 2, file_size, 0, 0, header_size)
    
    # 2. BITMAPINFOHEADER
    info = bytearray(40)
    struct.pack_into('<IIIHHIIIIII', info, 0, 40, width, height, 1, bpp, 0, image_size, 2835, 2835, num_colors, 0)
    
    # 3. Palette if 8-bit
    pal = bytearray()
    if bpp == 8:
        for i in range(256):
            pal.extend(struct.pack('<BBBB', i, i, i, 0)) # Grayscale palette
            
    # 4. Pixel data (bottom-up)
    ch = 1 if bpp == 8 else (3 if bpp == 24 else 4)
    if bpp == 16:
        # 16-bit RGB 5-5-5
        raw = bytearray()
        for y in range(height):
            for x in range(width):
                r5 = int((x / width) * 31) & 0x1F
                g5 = int((y / height) * 31) & 0x1F
                b5 = ((x + y) & 0x1F)
                val = (r5 << 10) | (g5 << 5) | b5
                raw.extend(struct.pack('<H', val))
            raw.extend(b'\x00' * pad_bytes)
    else:
        pix = make_ramp_noise_image(width, height, ch)
        raw = bytearray()
        for y in range(height):
            # bottom-up row index
            row_start = y * width * ch
            raw.extend(pix[row_start : row_start + width * ch])
            raw.extend(b'\x00' * pad_bytes)
            
    path.write_bytes(hdr + info + pal + raw)

def generate_ppm_pgm():
    """Generate binary PPM (P6) and PGM (P5)."""
    w, h = 256, 256
    # PPM P6
    p6_hdr = f"P6\n{w} {h}\n255\n".encode('ascii')
    p6_pix = make_ramp_noise_image(w, h, 3)
    # Convert BGR to RGB
    rgb = bytearray(len(p6_pix))
    for i in range(0, len(p6_pix), 3):
        rgb[i] = p6_pix[i+2]
        rgb[i+1] = p6_pix[i+1]
        rgb[i+2] = p6_pix[i]
    (OUT_DIR / "sample.ppm").write_bytes(p6_hdr + rgb)
    
    # PGM P5
    p5_hdr = f"P5\n{w} {h}\n255\n".encode('ascii')
    p5_pix = make_ramp_noise_image(w, h, 1)
    (OUT_DIR / "sample.pgm").write_bytes(p5_hdr + p5_pix)

def generate_tga():
    """Generate uncompressed true-color TGA type 2."""
    w, h = 256, 256
    hdr = bytearray(18)
    hdr[2] = 2 # uncompressed true-color
    struct.pack_into('<HHB', hdr, 12, w, h, 24)
    hdr[17] = 0x20 # top-to-bottom
    pix = make_ramp_noise_image(w, h, 3) # BGR
    (OUT_DIR / "sample.tga").write_bytes(hdr + pix)

def generate_tiff():
    """Generate uncompressed TIFF LE and BE."""
    w, h = 128, 128
    pix = make_ramp_noise_image(w, h, 3)
    # RGB
    rgb = bytearray(len(pix))
    for i in range(0, len(pix), 3):
        rgb[i] = pix[i+2]
        rgb[i+1] = pix[i+1]
        rgb[i+2] = pix[i]

    # Little-endian TIFF
    le = bytearray()
    le.extend(b'II\x2A\x00') # magic
    le.extend(struct.pack('<I', 8)) # offset to IFD
    # IFD: 9 entries
    num_tags = 9
    le.extend(struct.pack('<H', num_tags))
    image_data_offset = 8 + 2 + num_tags * 12 + 4 + 6 # right after IFD + bps array
    # tag format: tag(2), type(2), count(4), value/offset(4)
    # SHORT=3, LONG=4
    le.extend(struct.pack('<HHII', 256, 4, 1, w)) # ImageWidth
    le.extend(struct.pack('<HHII', 257, 4, 1, h)) # ImageLength
    bps_offset = 8 + 2 + num_tags * 12 + 4
    le.extend(struct.pack('<HHII', 258, 3, 3, bps_offset)) # BitsPerSample
    le.extend(struct.pack('<HHII', 259, 3, 1, 1)) # Compression = 1 (none)
    le.extend(struct.pack('<HHII', 262, 3, 1, 2)) # PhotometricInterpretation = 2 (RGB)
    le.extend(struct.pack('<HHII', 273, 4, 1, image_data_offset)) # StripOffsets
    le.extend(struct.pack('<HHII', 277, 3, 1, 3)) # SamplesPerPixel = 3
    le.extend(struct.pack('<HHII', 278, 4, 1, h)) # RowsPerStrip
    le.extend(struct.pack('<HHII', 279, 4, 1, len(rgb))) # StripByteCounts
    le.extend(struct.pack('<I', 0)) # next IFD
    # BitsPerSample data: 8, 8, 8
    le.extend(struct.pack('<HHH', 8, 8, 8))
    le.extend(rgb)
    (OUT_DIR / "sample_le.tif").write_bytes(le)

    # Big-endian TIFF
    be = bytearray()
    be.extend(b'MM\x00\x2A') # magic
    be.extend(struct.pack('>I', 8)) # offset to IFD
    be.extend(struct.pack('>H', num_tags))
    be.extend(struct.pack('>HHII', 256, 4, 1, w))
    be.extend(struct.pack('>HHII', 257, 4, 1, h))
    be.extend(struct.pack('>HHII', 258, 3, 3, bps_offset))
    be.extend(struct.pack('>HHII', 259, 3, 1, 1))
    be.extend(struct.pack('>HHII', 262, 3, 1, 2))
    be.extend(struct.pack('>HHII', 273, 4, 1, image_data_offset))
    be.extend(struct.pack('>HHII', 277, 3, 1, 3))
    be.extend(struct.pack('>HHII', 278, 4, 1, h))
    be.extend(struct.pack('>HHII', 279, 4, 1, len(rgb)))
    be.extend(struct.pack('>I', 0))
    be.extend(struct.pack('>HHH', 8, 8, 8))
    be.extend(rgb)
    (OUT_DIR / "sample_be.tif").write_bytes(be)

def make_multichannel_audio(num_samples, channels, bits_per_sample):
    """Generate multi-channel correlated audio samples."""
    bytes_per_sample = bits_per_sample // 8
    raw = bytearray(num_samples * channels * bytes_per_sample)
    idx = 0
    freqs = [440.0, 554.37, 659.25, 880.0, 330.0, 220.0, 700.0, 1100.0]
    sample_rate = 48000.0
    for i in range(num_samples):
        for ch in range(channels):
            f = freqs[ch % len(freqs)]
            # smooth sinusoidal wave + correlated baseline
            val = math.sin(2.0 * math.pi * f * (i / sample_rate)) * 0.7
            # Add small cross-channel correlation
            val += math.sin(2.0 * math.pi * 100.0 * (i / sample_rate)) * 0.2
            
            if bits_per_sample == 16:
                ival = int(val * 32767.0)
                struct.pack_into('<h', raw, idx, max(-32768, min(32767, ival)))
                idx += 2
            elif bits_per_sample == 24:
                ival = int(val * 8388607.0)
                ival = max(-8388608, min(8388607, ival))
                uval = ival & 0xFFFFFF
                raw[idx] = uval & 0xFF
                raw[idx+1] = (uval >> 8) & 0xFF
                raw[idx+2] = (uval >> 16) & 0xFF
                idx += 3
            elif bits_per_sample == 32:
                ival = int(val * 2147483647.0)
                struct.pack_into('<i', raw, idx, max(-2147483648, min(2147483647, ival)))
                idx += 4
    return raw

def generate_wav(filename, channels, bits_per_sample, num_samples=4000, junk_prefix_bytes=0):
    """Generate WAV file with optional JUNK prefix chunk."""
    raw_audio = make_multichannel_audio(num_samples, channels, bits_per_sample)
    data_size = len(raw_audio)
    byte_rate = 48000 * channels * (bits_per_sample // 8)
    block_align = channels * (bits_per_sample // 8)
    
    fmt_chunk = bytearray()
    fmt_chunk.extend(b'fmt ')
    fmt_chunk.extend(struct.pack('<IHHIIHH', 16, 1, channels, 48000, byte_rate, block_align, bits_per_sample))
    
    data_chunk = bytearray()
    data_chunk.extend(b'data')
    data_chunk.extend(struct.pack('<I', data_size))
    data_chunk.extend(raw_audio)
    
    junk_chunk = bytearray()
    if junk_prefix_bytes > 0:
        junk_chunk.extend(b'JUNK')
        junk_chunk.extend(struct.pack('<I', junk_prefix_bytes))
        junk_chunk.extend(b'\x00' * junk_prefix_bytes)
        
    riff_body = junk_chunk + fmt_chunk + data_chunk
    riff_hdr = b'RIFF' + struct.pack('<I', len(riff_body) + 4) + b'WAVE'
    
    (OUT_DIR / filename).write_bytes(riff_hdr + riff_body)

def generate_rf64(filename, channels, bits_per_sample, num_samples=4000):
    """Generate RF64 audio file with ds64 chunk."""
    raw_audio = make_multichannel_audio(num_samples, channels, bits_per_sample)
    data_size = len(raw_audio)
    byte_rate = 48000 * channels * (bits_per_sample // 8)
    block_align = channels * (bits_per_sample // 8)
    
    # ds64 chunk (28 bytes size)
    ds64 = bytearray()
    ds64.extend(b'ds64')
    ds64.extend(struct.pack('<IQQQI', 28, data_size + 100, data_size, num_samples, 0))
    
    fmt = bytearray()
    fmt.extend(b'fmt ')
    fmt.extend(struct.pack('<IHHIIHH', 16, 1, channels, 48000, byte_rate, block_align, bits_per_sample))
    
    data = bytearray()
    data.extend(b'data')
    data.extend(struct.pack('<I', 0xFFFFFFFF)) # -1 for RF64 data chunk
    data.extend(raw_audio)
    
    body = ds64 + fmt + data
    hdr = b'RF64' + struct.pack('<I', 0xFFFFFFFF) + b'WAVE'
    (OUT_DIR / filename).write_bytes(hdr + body)

def generate_aiff(filename, channels, bits_per_sample, num_samples=4000):
    """Generate Apple AIFF (big-endian) audio file."""
    bytes_per_sample = bits_per_sample // 8
    raw_le = make_multichannel_audio(num_samples, channels, bits_per_sample)
    # Convert to big-endian
    raw_be = bytearray(len(raw_le))
    for i in range(0, len(raw_le), bytes_per_sample):
        sample = raw_le[i : i + bytes_per_sample]
        raw_be[i : i + bytes_per_sample] = sample[::-1]
        
    # COMM chunk
    comm = bytearray()
    comm.extend(b'COMM')
    # 80-bit IEEE 754 float for 48000.0: exponent 0x400E, mantissa 0xBB80000000000000
    sample_rate_80 = b'\x40\x0E\xBB\x80\x00\x00\x00\x00\x00\x00'
    comm.extend(struct.pack('>IHhIh', 18, channels, num_samples, bits_per_sample, 0)[:12])
    comm = b'COMM' + struct.pack('>IhIh', 18, channels, num_samples, bits_per_sample) + sample_rate_80
    
    # SSND chunk
    ssnd = b'SSND' + struct.pack('>III', len(raw_be) + 8, 0, 0) + raw_be
    
    body = comm + ssnd
    hdr = b'FORM' + struct.pack('>I', len(body) + 4) + b'AIFF'
    (OUT_DIR / filename).write_bytes(hdr + body)

def generate_caf(filename, channels, bits_per_sample, num_samples=4000):
    """Generate Core Audio Format (CAF) file."""
    bytes_per_sample = bits_per_sample // 8
    raw_le = make_multichannel_audio(num_samples, channels, bits_per_sample)
    raw_be = bytearray(len(raw_le))
    for i in range(0, len(raw_le), bytes_per_sample):
        sample = raw_le[i : i + bytes_per_sample]
        raw_be[i : i + bytes_per_sample] = sample[::-1]
        
    hdr = b'caff' + struct.pack('>HH', 1, 0) # version 1, flags 0
    # desc chunk (AudioFormatDescription, 32 bytes)
    desc_data = struct.pack('>dIIIIII', 48000.0, 0x6C70636D, 0, bytes_per_sample * channels, 1, channels, bits_per_sample)
    desc = b'desc' + struct.pack('>Q', len(desc_data)) + desc_data
    # data chunk
    data = b'data' + struct.pack('>QI', len(raw_be) + 4, 0) + raw_be
    (OUT_DIR / filename).write_bytes(hdr + desc + data)

def fetch_kodak_photos():
    """Fetch 4 public-domain Kodak photographs and convert to uncompressed 24-bit BMP."""
    base_url = "https://r0k.us/graphics/kodak/kodak"
    photo_ids = ["01", "02", "03", "04"]
    for pid in photo_ids:
        url = f"{base_url}/kodim{pid}.png"
        out_name = f"photo_kodim{pid}.bmp"
        out_path = OUT_DIR / out_name
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
            with urllib.request.urlopen(req, timeout=15) as resp:
                png_bytes = resp.read()
            img = Image.open(io.BytesIO(png_bytes)).convert("RGB")
            # Save as uncompressed BMP (24 bpp)
            img.save(out_path, format="BMP")
            print(f"  [OK] Downloaded and converted {out_name} ({img.width}x{img.height})")
        except Exception as e:
            print(f"  [WARN] Failed to fetch photo {url}: {e}")

def main():
    print(f"Generating experiment fixtures in {OUT_DIR}...")
    
    # 1. BMP suite: widths 1919, 1920, 1921 across 8, 16, 24, 32 bpp
    for bpp in [8, 16, 24, 32]:
        for w in [1919, 1920, 1921]:
            generate_bmp(f"bmp_w{w}_{bpp}bpp.bmp", w, 32, bpp)
            
    # 2. TIFF LE & BE
    generate_tiff()
    
    # 3. TGA type 2
    generate_tga()
    
    # 4. PPM / PGM
    generate_ppm_pgm()
    
    # 5. Multi-channel audio (5.1 and 7.1 at 16, 24, 32 bit)
    for ch, name in [(6, "5.1"), (8, "7.1")]:
        for bps in [16, 24, 32]:
            generate_wav(f"audio_{name}_{bps}bit.wav", ch, bps)
            
    # 6. JUNK-prefixed WAV
    generate_wav("audio_junk_prefixed.wav", 2, 16, junk_prefix_bytes=128)
    
    # 7. BW64 / RF64
    generate_rf64("audio_rf64_5.1_24bit.rf64", 6, 24)
    
    # 8. AIFF
    generate_aiff("audio_sample_5.1_16bit.aiff", 6, 16)
    
    # 9. CAF
    generate_caf("audio_sample_5.1_24bit.caf", 6, 24)

    # 9b. UTF-16 LE text fixture to test channels=2 hypothesis
    sample_text = "The quick brown fox jumps over the lazy dog. OpenRAR high performance archiving.\n" * 100
    (OUT_DIR / "text_utf16_le.txt").write_bytes(sample_text.encode('utf-16le'))

    # 10. Fetch 4 public-domain Kodak photographs
    print("Fetching authentic public-domain Kodak photographs...")
    fetch_kodak_photos()
    
    # 11. Write manifest
    manifest_path = OUT_DIR / "manifest.sha256"
    lines = []
    for fname in sorted(os.listdir(OUT_DIR)):
        if fname == "manifest.sha256": continue
        p = OUT_DIR / fname
        if p.is_file():
            h = hashlib.sha256(p.read_bytes()).hexdigest()
            lines.append(f"{h}  {fname}\n")
    manifest_path.write_text("".join(lines), encoding="utf-8")
    print(f"Manifest written with {len(lines)} files to {manifest_path}")

if __name__ == "__main__":
    main()
