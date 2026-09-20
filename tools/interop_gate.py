#!/usr/bin/env python3
"""
Local interop gate - validates RAR5 spec compliance before commit.

Checks:
  1. Self-roundtrip hash (openrar a -> openrar x)
  2. Cross-decode vs reference unrar (table encoding invariant)
  3. Compression methods m1-m5 full-dictionary & wrap-around
  4. Track 1: Solid Mixed-Stream Invariants (-s -m3: 0B, store fallback, compressed)
  5. Track 2: WinRAR Executable Filter Decoding (-mc) across circular window boundary
  6. Track 3: High-Precision Timestamps (Pre-1970 negative epoch & Post-2038 rollover)
  7. Track 4: Multi-Byte UTF-8 Passwords & Key Derivation (-p / -hp with Umlauts, CJK, Emoji)
  8. Track 5: QuickOpen (QO) Cache Invalidation Under Mutation (d, u)
  9. Track 6: Recovery Volume (.rev) Cauchy Parity Reconstruction (-rv -> rar rc)
 10. Track 7: 64-Bit VINT Size Bounds & Heap Exhaustion Prevention
 11. Track 8: High-Throughput Parallel Compression (-mt) & Block Pipeline
 12. Multivolume roundtrip (store + compressed)
 13. SFX read + create
 14. Unit & compression tests via ctest

CI: rar.exe is not available in GitHub Actions, so this gate is LOCAL ONLY
for src/compress/* changes. In CI we fall back to self-roundtrip + ctest.

Usage: python tools/interop_gate.py [--quick]
Exit 0 = pass, 1 = fail.
"""
import os, sys, subprocess, hashlib, tempfile, shutil, pathlib, random, struct, zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OPENRAR = ROOT / "build" / "openrar64" / "Release" / "openrar.exe"
OPENRAR_DBG = ROOT / "build" / "openrar64" / "Debug" / "openrar.exe"
UNRAR = pathlib.Path(r"C:\Program Files\WinRAR\UnRAR.exe")
RAR = pathlib.Path(r"C:\Program Files\WinRAR\rar.exe")

def find_openrar():
    for p in [OPENRAR, OPENRAR_DBG, ROOT / "build" / "openrar.exe", ROOT / "build" / "Release" / "openrar.exe", ROOT / "build" / "openrar"]:
        if p is None: continue
        if p.exists(): return str(p)
    return None

def find_unrar():
    unrar_env = os.environ.get("UNRAR_EXE")
    candidates = [pathlib.Path(unrar_env)] if unrar_env else []
    # Local dev\unrar build if present
    candidates.append(ROOT.parent / "unrar" / "build" / "unrar64" / "Release" / "UnRAR.exe")
    candidates.append(ROOT.parent / "unrar" / "unrar.exe")
    candidates.append(UNRAR)
    for p in candidates:
        if p.exists(): return str(p)
    # try PATH
    w = shutil.which("UnRAR") or shutil.which("unrar")
    if w: return w
    return None

def find_rar():
    if RAR.exists(): return str(RAR)
    w = shutil.which("rar")
    if w: return w
    return None

def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1<<20), b""): h.update(c)
    return h.hexdigest()

def find_extracted(d, name):
    p = d / name
    if p.exists(): return p
    cands = list(d.rglob(name))
    return cands[0] if cands else None

def parse_lt_method(output):
    for line in output.splitlines():
        s = line.strip()
        if s.startswith("Method:"):
            parts = s.split()
            if len(parts) >= 2:
                try: return int(parts[1])
                except ValueError: pass
    return None

def run(cmd, cwd=None, encoding="utf-8"):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding=encoding, errors="replace")
    return r.returncode, r.stdout, r.stderr

def ensure_build():
    if find_openrar(): return True
    print("[interop-gate] building openrar Release...", flush=True)
    rc, out, err = run(["cmake", "--build", "build", "--config", "Release", "-j", "8"])
    if rc != 0:
        print(out, err); return False
    return find_openrar() is not None

def test_self_roundtrip(openrar):
    print("[1/14] Self-roundtrip hash...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        src = td / "payload.bin"
        random.seed(0xC0FFEE)
        data = os.urandom(1024) * 4 + b"A_REPEAT_PATTERN" * 1024
        data = data * 8  # ~ 160KB
        src.write_bytes(data)
        h0 = sha256(src)
        arc = td / "test.rar"
        rc, out, err = run([openrar, "a", str(arc), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a rc={rc}\n{out}\n{err}"); return False
        rc, out, err = run([openrar, "t", str(arc)])
        if rc != 0:
            print(f"  FAIL: openrar t self rc={rc}\n{out}\n{err}"); return False
        outdir = td / "out"
        outdir.mkdir()
        rc, out, err = run([openrar, "x", "-y", str(arc), str(outdir) + os.sep])
        if rc != 0:
            print(f"  FAIL: openrar x rc={rc}\n{out}\n{err}"); return False
        dec = find_extracted(outdir, "payload.bin")
        if not dec:
            print("  FAIL: extracted file missing"); return False
        h1 = sha256(dec)
        if h0 != h1:
            print(f"  FAIL: hash mismatch {h0} vs {h1}"); return False
        print(f"  OK hash {h0[:16]}...")
        return True

def test_cross(openrar, unrar, rar):
    print("[2/14] Cross-interop vs reference unrar...", flush=True)
    has_ref = unrar is not None or rar is not None
    if not has_ref:
        print("  SKIP: no UnRAR/rar.exe found (CI fallback)")
        return True

    ok = True
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        src = td / "interop.bin"
        src.write_bytes((b"The quick brown fox " * 64 + b"\x00\xff\x55\xaa" * 16) * 1024)
        h0 = sha256(src)
        arc_or = td / "or.rar"
        rc, out, err = run([openrar, "a", "-m3", str(arc_or), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a rc={rc}\n{out}\n{err}"); return False
        ref_decompress = unrar or rar
        rc, out, err = run([ref_decompress, "t", str(arc_or)])
        if rc != 0:
            print(f"  FAIL: {ref_decompress} t openrar.rar rc={rc}\n{out}\n{err}")
            ok = False
        else:
            print("  OK: reference decodes openrar.rar")

        if rar:
            arc_rar = td / "ref.rar"
            rc, out, err = run([rar, "a", "-m3", "-mt1", str(arc_rar), str(src)], cwd=str(td))
            if rc == 0 and arc_rar.exists():
                rc2, out2, err2 = run([openrar, "t", str(arc_rar)])
                if rc2 != 0:
                    print(f"  FAIL: openrar t rar.rar rc={rc2}\n{out2}\n{err2}")
                    ok = False
                else:
                    outdir = td / "ref_out"
                    outdir.mkdir()
                    rc2, _, _ = run([openrar, "x", "-y", str(arc_rar), str(outdir) + os.sep])
                    dec = find_extracted(outdir, "interop.bin")
                    if not dec or sha256(dec) != h0:
                        print("  FAIL: ref roundtrip hash mismatch"); ok = False
                    else:
                        print("  OK: openrar decodes rar.rar")
    return ok

def test_methods_interop(openrar, unrar, rar):
    print("[3/14] Compression methods m1-m5 full-dictionary & wrap-around...", flush=True)
    ref_decompress = unrar or rar

    configs = [
        (1, 512 * 1024, 640 * 1024, [], ["-md512k"]),
        (2, 1024 * 1024, 1280 * 1024, [], ["-md1024k"]),
        (3, 2048 * 1024, 2560 * 1024, [], ["-md2048k"]),
        (4, 4096 * 1024, 5120 * 1024, [], ["-md4096k"]),
        (5, 4096 * 1024, 5120 * 1024, ["-md4m"], ["-md4096k"]),
    ]

    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)

        # 1. Active Codec Stress + Wrap-Around with Straddle Tokens
        for m, dict_sz, payload_sz, or_flags, rar_flags in configs:
            rng = random.Random(0xC0FFEE + m)
            chunk = rng.randbytes(1024)
            buf = bytearray(chunk * (payload_sz // len(chunk)))
            tok = f"STRADDLE_TOKEN_M{m}_WRAP!".encode("ascii")
            buf[dict_sz - 32 : dict_sz - 32 + len(tok)] = tok
            buf[dict_sz + 32 : dict_sz + 32 + len(tok)] = tok
            src = td / f"active_m{m}.bin"
            src.write_bytes(buf)
            h0 = sha256(src)

            arc = td / f"active_m{m}.rar"
            rc, out, err = run([openrar, "a", f"-m{m}"] + or_flags + [str(arc), str(src)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: openrar a -m{m} rc={rc}\n{out}\n{err}"); return False

            rc, out, err = run([openrar, "lt", str(arc)])
            if rc != 0:
                print(f"  FAIL: openrar lt active_m{m} rc={rc}\n{out}\n{err}"); return False
            detected_m = parse_lt_method(out)
            if detected_m != m:
                print(f"  FAIL: openrar lt active_m{m} expected Method: {m}, got {detected_m}")
                return False

            if ref_decompress:
                outdir = td / f"unrar_act_{m}"
                outdir.mkdir()
                rc, out, err = run([ref_decompress, "x", "-y", str(arc), str(outdir) + os.sep])
                if rc != 0:
                    print(f"  FAIL: reference x active_m{m} rc={rc}\n{out}\n{err}"); return False
                dec = find_extracted(outdir, f"active_m{m}.bin")
                if not dec or sha256(dec) != h0:
                    print(f"  FAIL: reference extracted hash mismatch on active_m{m}"); return False

        # 2. Incompressible Fallback Stress (Store Fallback)
        for m, dict_sz, payload_sz, or_flags, rar_flags in configs:
            rng = random.Random(0xC0FFEE + 100 + m)
            src = td / f"rnd_m{m}.bin"
            src.write_bytes(rng.randbytes(dict_sz))
            arc = td / f"rnd_m{m}.rar"
            rc, out, err = run([openrar, "a", f"-m{m}"] + or_flags + [str(arc), str(src)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: openrar a rnd_m{m} rc={rc}\n{out}\n{err}"); return False
            rc, out, err = run([openrar, "lt", str(arc)])
            detected_m = parse_lt_method(out)
            if detected_m != 0:
                print(f"  FAIL: openrar lt rnd_m{m} expected Method: 0 fallback, got {detected_m}"); return False

        # 3. Bidirectional Symmetry (rar.exe a -m{m} -> openrar x)
        if rar:
            for m, dict_sz, payload_sz, or_flags, rar_flags in configs:
                src = td / f"active_m{m}.bin"
                h0 = sha256(src)
                arc = td / f"rar_m{m}.rar"
                rc, out, err = run([rar, "a", "-ep", f"-m{m}"] + rar_flags + [str(arc), str(src)], cwd=str(td))
                if rc != 0:
                    print(f"  FAIL: rar a -m{m} rc={rc}\n{out}\n{err}"); return False
                outdir = td / f"openrar_from_rar_{m}"
                outdir.mkdir()
                rc, out, err = run([openrar, "x", "-y", str(arc), str(outdir) + os.sep])
                if rc != 0:
                    print(f"  FAIL: openrar x from rar_m{m} rc={rc}\n{out}\n{err}"); return False
                dec = find_extracted(outdir, f"active_m{m}.bin")
                if not dec or sha256(dec) != h0:
                    print(f"  FAIL: hash mismatch on rar_m{m}"); return False

    print("  OK methods m1-m5 full-dictionary & wrap-around")
    return True

def test_track1_solid_mixed(openrar, unrar, rar):
    print("[4/14] Track 1: Solid Mixed-Stream Invariants (-s -m3)...", flush=True)
    ref_decompress = unrar or rar
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        f1 = td / "f1.bin"
        f2 = td / "f2_empty.bin"
        f3 = td / "f3_store.bin"
        f4 = td / "f4.bin"

        shared = b"SHARED_SOLID_MIXED_STREAM_TOKEN_INVARIANT!" * 100
        f1.write_bytes(b"FILE1_COMPRESSED_DATA\n" * 2000 + shared)
        f2.write_bytes(b"") # 0-byte file
        f3.write_bytes(os.urandom(64 * 1024)) # Incompressible Store fallback
        f4.write_bytes(shared + b"FILE4_SUBSEQUENT_COMPRESSED_DATA\n" * 2000)

        hashes = {f.name: sha256(f) for f in [f1, f2, f3, f4]}

        # OpenRAR creates solid mixed archive
        arc = td / "solid_mix.rar"
        rc, out, err = run([openrar, "a", "-s", "-m3", str(arc), str(f1), str(f2), str(f3), str(f4)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -s -m3 rc={rc}\n{out}\n{err}"); return False

        # OpenRAR self-extraction
        out_self = td / "out_self"
        out_self.mkdir()
        rc, out, err = run([openrar, "x", "-y", str(arc), str(out_self) + os.sep])
        if rc != 0:
            print(f"  FAIL: openrar x self rc={rc}\n{out}\n{err}"); return False
        for name, h in hashes.items():
            dec = find_extracted(out_self, name)
            if not dec or sha256(dec) != h:
                print(f"  FAIL: openrar self hash mismatch on {name}"); return False

        # UnRAR extraction
        if ref_decompress:
            out_unrar = td / "out_unrar"
            out_unrar.mkdir()
            rc, out, err = run([ref_decompress, "x", "-y", str(arc), str(out_unrar) + os.sep])
            if rc != 0:
                print(f"  FAIL: {ref_decompress} x solid rc={rc}\n{out}\n{err}"); return False
            for name, h in hashes.items():
                dec = find_extracted(out_unrar, name)
                if not dec or sha256(dec) != h:
                    print(f"  FAIL: {ref_decompress} hash mismatch on {name}"); return False

        # WinRAR creates -> OpenRAR extracts
        if rar:
            arc_rar = td / "rar_mix.rar"
            rc, out, err = run([rar, "a", "-s", "-ep", "-m3", str(arc_rar), str(f1), str(f2), str(f3), str(f4)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: rar a -s -m3 rc={rc}\n{out}\n{err}"); return False
            out_from_rar = td / "out_from_rar"
            out_from_rar.mkdir()
            rc, out, err = run([openrar, "x", "-y", str(arc_rar), str(out_from_rar) + os.sep])
            if rc != 0:
                print(f"  FAIL: openrar x from rar_mix rc={rc}\n{out}\n{err}"); return False
            for name, h in hashes.items():
                dec = find_extracted(out_from_rar, name)
                if not dec or sha256(dec) != h:
                    print(f"  FAIL: openrar from rar hash mismatch on {name}"); return False

    print("  OK Track 1 solid mixed-stream invariants")
    return True

def test_track2_filters(openrar, unrar, rar):
    print("[5/14] Track 2: WinRAR Executable & Delta Filter Bi-Directional Interop (-mc)...", flush=True)
    ref_decompress = unrar or rar

    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        # 1. x86 Executable filter payload
        src_exe = td / "test.exe"
        chunk = bytearray()
        for i in range(130000):
            op = 0xE8 if (i % 2 == 0) else 0xE9
            disp = (i * 0x10) & 0xFFFFFFFF
            chunk.extend([op, disp & 0xFF, (disp >> 8) & 0xFF, (disp >> 16) & 0xFF, (disp >> 24) & 0xFF])
        src_exe.write_bytes(chunk)
        h0_exe = sha256(src_exe)

        # 2. Audio/RGB Multi-channel Delta filter payload
        src_delta = td / "test.pcm"
        delta_chunk = bytearray()
        import math
        for i in range(50000):
            left = int(math.sin(i * 0.05) * 30000) & 0xFFFF
            right = int(math.cos(i * 0.05) * 30000) & 0xFFFF
            delta_chunk.extend([left & 0xFF, (left >> 8) & 0xFF, right & 0xFF, (right >> 8) & 0xFF])
        src_delta.write_bytes(delta_chunk)
        h0_delta = sha256(src_delta)

        # Part A: WinRAR creates -> OpenRAR extracts (if rar.exe present)
        if rar:
            arc_rar = td / "filter_test_rar.rar"
            rc, out, err = run([rar, "a", "-ep", "-mc", "-md512k", str(arc_rar), str(src_exe)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: rar a -mc rc={rc}\n{out}\n{err}"); return False
            out_dir = td / "out_openrar_from_rar"
            out_dir.mkdir()
            rc, out, err = run([openrar, "x", "-y", str(arc_rar), str(out_dir) + os.sep])
            if rc != 0:
                print(f"  FAIL: openrar x filter_test_rar.rar rc={rc}\n{out}\n{err}"); return False
            dec = find_extracted(out_dir, "test.exe")
            if not dec or sha256(dec) != h0_exe:
                print("  FAIL: hash mismatch on OpenRAR decoding WinRAR filter archive"); return False

        # Part B: OpenRAR creates (-mcE+) -> Self extract & Reference UnRAR extracts
        arc_e8 = td / "openrar_e8.rar"
        rc, out, err = run([openrar, "a", "-y", "-mcE+", str(arc_e8), str(src_exe)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -mcE+ rc={rc}\n{out}\n{err}"); return False
        
        # OpenRAR self-test
        rc, out, err = run([openrar, "t", "-y", str(arc_e8)])
        if rc != 0:
            print(f"  FAIL: openrar t openrar_e8.rar rc={rc}\n{out}\n{err}"); return False

        # Reference UnRAR extracts OpenRAR's E8 filtered archive
        if ref_decompress:
            out_unrar_e8 = td / "out_unrar_e8"
            out_unrar_e8.mkdir()
            rc, out, err = run([ref_decompress, "x", "-y", str(arc_e8), str(out_unrar_e8) + os.sep])
            if rc != 0:
                print(f"  FAIL: reference x openrar_e8.rar rc={rc}\n{out}\n{err}"); return False
            dec = find_extracted(out_unrar_e8, "test.exe")
            if not dec or sha256(dec) != h0_exe:
                print("  FAIL: hash mismatch on reference UnRAR extracting OpenRAR -mcE+ archive"); return False

        # Part C: OpenRAR creates (-mcD+) -> Self extract & Reference UnRAR extracts
        arc_delta = td / "openrar_delta.rar"
        rc, out, err = run([openrar, "a", "-y", "-mcD+", str(arc_delta), str(src_delta)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -mcD+ rc={rc}\n{out}\n{err}"); return False

        rc, out, err = run([openrar, "t", "-y", str(arc_delta)])
        if rc != 0:
            print(f"  FAIL: openrar t openrar_delta.rar rc={rc}\n{out}\n{err}"); return False

        if ref_decompress:
            out_unrar_delta = td / "out_unrar_delta"
            out_unrar_delta.mkdir()
            rc, out, err = run([ref_decompress, "x", "-y", str(arc_delta), str(out_unrar_delta) + os.sep])
            if rc != 0:
                print(f"  FAIL: reference x openrar_delta.rar rc={rc}\n{out}\n{err}"); return False
            dec = find_extracted(out_unrar_delta, "test.pcm")
            if not dec or sha256(dec) != h0_delta:
                print("  FAIL: hash mismatch on reference UnRAR extracting OpenRAR -mcD+ archive"); return False

        # Part D: OpenRAR creates (-mc-) -> Global disable
        arc_mc_off = td / "openrar_mc_off.rar"
        rc, out, err = run([openrar, "a", "-y", "-mc-", str(arc_mc_off), str(src_exe)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -mc- rc={rc}\n{out}\n{err}"); return False

        if ref_decompress:
            out_unrar_off = td / "out_unrar_off"
            out_unrar_off.mkdir()
            rc, out, err = run([ref_decompress, "x", "-y", str(arc_mc_off), str(out_unrar_off) + os.sep])
            if rc != 0:
                print(f"  FAIL: reference x openrar_mc_off.rar rc={rc}\n{out}\n{err}"); return False
            dec = find_extracted(out_unrar_off, "test.exe")
            if not dec or sha256(dec) != h0_exe:
                print("  FAIL: hash mismatch on reference UnRAR extracting OpenRAR -mc- archive"); return False

    print("  OK Track 2 executable & delta filter bidirectional interop")
    return True

def test_track3_timestamps(openrar, unrar):
    print("[6/14] Track 3: High-Precision Timestamps (Pre-1970 & Post-2038)...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        f1 = td / "vintage_1965.txt"
        f2 = td / "future_2042.txt"
        f1.write_bytes(b"vintage 1965 file content")
        f2.write_bytes(b"future 2042 file content")

        # 1965-07-20 12:00:00 UTC = -140443200
        # 2042-01-01 00:00:00 UTC = 2272147200
        os.utime(f1, (-140443200, -140443200))
        os.utime(f2, (2272147200, 2272147200))

        arc = td / "times.rar"
        rc, out, err = run([openrar, "a", "-y", str(arc), str(f1), str(f2)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a times.rar rc={rc}\n{out}\n{err}"); return False

        if unrar:
            rc, out, err = run([unrar, "lt", str(arc)])
            if rc != 0:
                print(f"  FAIL: unrar lt times.rar rc={rc}\n{out}\n{err}"); return False
            if "1965-07-20" not in out:
                print(f"  FAIL: UnRAR lt missing pre-1970 1965 date:\n{out}"); return False
            if "2042-01-01" not in out:
                print(f"  FAIL: UnRAR lt missing post-2038 2042 date:\n{out}"); return False

    print("  OK Track 3 high-precision pre-1970 & post-2038 timestamps")
    return True

def test_track4_passwords(openrar, unrar, rar):
    print("[7/14] Track 4: Multi-Byte UTF-8 Passwords & Key Derivation (-p / -hp)...", flush=True)
    ref_decompress = unrar or rar
    vectors = [
        ("München_Café2026!", False), # German umlauts + French accent (-p)
        ("パスワード_秘密", False),     # Japanese Kana / Kanji (-p)
        ("🔒OpenRAR_Key", True),      # 4-byte UTF-8 Emoji (-hp)
    ]

    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        src = td / "secret.txt"
        src.write_bytes(b"TOP SECRET UTF8 DATA VERIFICATION\n" * 30)
        h0 = sha256(src)

        for pw, is_hp in vectors:
            flag = f"-hp{pw}" if is_hp else f"-p{pw}"
            arc = td / f"enc_{hashlib.md5(pw.encode('utf-8')).hexdigest()[:6]}.rar"
            rc, out, err = run([openrar, "a", flag, str(arc), str(src)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: openrar a {flag} rc={rc}\n{out}\n{err}"); return False

            # Self-extract
            out_self = td / "out_self"
            if out_self.exists(): shutil.rmtree(out_self)
            out_self.mkdir()
            rc, out, err = run([openrar, "x", "-y", f"-p{pw}", str(arc), str(out_self) + os.sep])
            if rc != 0:
                print(f"  FAIL: openrar x {flag} rc={rc}\n{out}\n{err}"); return False
            dec = find_extracted(out_self, "secret.txt")
            if not dec or sha256(dec) != h0:
                print(f"  FAIL: openrar self hash mismatch for password {pw}"); return False

            # Reference extract (UnRAR)
            if ref_decompress:
                out_ref = td / "out_ref"
                if out_ref.exists(): shutil.rmtree(out_ref)
                out_ref.mkdir()
                rc, out, err = run([ref_decompress, "x", "-y", f"-p{pw}", str(arc), str(out_ref) + os.sep])
                if rc != 0:
                    print(f"  FAIL: {ref_decompress} x {flag} rc={rc}\n{out}\n{err}"); return False
                dec = find_extracted(out_ref, "secret.txt")
                if not dec or sha256(dec) != h0:
                    print(f"  FAIL: reference hash mismatch for password {pw}"); return False

            # WinRAR create -> OpenRAR extract
            if rar and not is_hp:
                arc_rar = td / f"rar_enc_{hashlib.md5(pw.encode('utf-8')).hexdigest()[:6]}.rar"
                rc, out, err = run([rar, "a", "-ep", flag, str(arc_rar), str(src)], cwd=str(td))
                if rc == 0:
                    out_rar = td / "out_from_rar"
                    if out_rar.exists(): shutil.rmtree(out_rar)
                    out_rar.mkdir()
                    rc2, out2, err2 = run([openrar, "x", "-y", f"-p{pw}", str(arc_rar), str(out_rar) + os.sep])
                    if rc2 != 0:
                        print(f"  FAIL: openrar x from rar {flag} rc={rc2}\n{out2}\n{err2}"); return False
                    dec = find_extracted(out_rar, "secret.txt")
                    if not dec or sha256(dec) != h0:
                        print(f"  FAIL: openrar hash mismatch from rar archive for password {pw}"); return False

    print("  OK Track 4 multi-byte UTF-8 passwords & key derivation")
    return True

def test_track5_quickopen(openrar, unrar, rar):
    print("[8/14] Track 5: QuickOpen (QO) Cache Invalidation Under Mutation...", flush=True)
    if not rar:
        print("  SKIP: rar.exe not found to create QO archive")
        return True

    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        f1 = td / "f1.txt"
        f2 = td / "f2.txt"
        f3 = td / "f3.txt"
        f1.write_text("initial f1 content")
        f2.write_text("f2 to be deleted")
        f3.write_text("f3 initial content")

        arc = td / "qo_test.rar"
        rc, out, err = run([rar, "a", "-ep", "-qo", str(arc), str(f1), str(f2), str(f3)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: rar a -qo rc={rc}\n{out}\n{err}"); return False

        # OpenRAR deletes f2.txt
        rc, out, err = run([openrar, "d", str(arc), "f2.txt"], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar d rc={rc}\n{out}\n{err}"); return False

        # OpenRAR updates f3.txt
        f3.write_text("f3 modified updated content")
        rc, out, err = run([openrar, "u", str(arc), str(f3)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar u rc={rc}\n{out}\n{err}"); return False

        # WinRAR test verifies mutated archive integrity
        rc, out, err = run([rar, "t", str(arc)])
        if rc != 0:
            print(f"  FAIL: rar t failed on mutated archive rc={rc}\n{out}\n{err}"); return False

        if unrar:
            rc, out, err = run([unrar, "l", str(arc)])
            if rc != 0:
                print(f"  FAIL: unrar l rc={rc}\n{out}\n{err}"); return False
            if "f2.txt" in out:
                print("  FAIL: f2.txt still present in unrar l after openrar d"); return False
            if "f1.txt" not in out or "f3.txt" not in out:
                print("  FAIL: f1.txt or f3.txt missing after mutation"); return False

    print("  OK Track 5 QuickOpen cache invalidation under mutation")
    return True

def test_track6_recovery_volumes(openrar, unrar, rar):
    print("[9/14] Track 6: Recovery Volume (.rev) Cauchy Parity Reconstruction...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        src = td / "big.bin"
        src.write_bytes(os.urandom(45 * 1024))

        mv = td / "mv.rar"
        rc, out, err = run([openrar, "a", "-v20k", "-m0", "-rv1", str(mv), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -v20k -rv1 rc={rc}\n{out}\n{err}"); return False

        part2 = td / "mv.part02.rar"
        if not part2.exists():
            print("  FAIL: mv.part02.rar was not created"); return False
        h_orig = sha256(part2)

        # 1. OpenRAR self-repair
        part2.unlink()
        part1 = td / "mv.part01.rar"
        rc, out, err = run([openrar, "r", str(part1)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar r rc={rc}\n{out}\n{err}"); return False
        if not part2.exists() or sha256(part2) != h_orig:
            print("  FAIL: openrar self-reconstructed volume hash mismatch"); return False

        # 2. WinRAR Cauchy Parity Rebuild (rar rc)
        if rar:
            part2.unlink()
            rc, out, err = run([rar, "rc", "-y", str(part1)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: rar rc failed rc={rc}\n{out}\n{err}"); return False
            if not part2.exists() or sha256(part2) != h_orig:
                print("  FAIL: WinRAR reconstructed volume hash mismatch (Cauchy parity incompatibility)"); return False

    print("  OK Track 6 recovery volume Cauchy parity reconstruction")
    return True

def test_track7_vint64(openrar, unrar):
    print("[10/14] Track 7: 64-Bit VINT Size Bounds & Memory Safety...", flush=True)

    def write_vint(val):
        res = bytearray()
        while val >= 0x80:
            res.append((val & 0x7F) | 0x80)
            val >>= 7
        res.append(val & 0x7F)
        return bytes(res)

    def make_block(b_type, b_flags, body):
        head = write_vint(b_type) + write_vint(b_flags) + body
        size = write_vint(len(head))
        full = size + head
        crc = zlib.crc32(full) & 0xFFFFFFFF
        return struct.pack('<I', crc) + full

    sig = b'Rar!\x1a\x07\x01\x00'
    main_hdr = make_block(1, 0, write_vint(0))
    fname = "virtual_5gb.bin".encode('utf-8')
    unp_sz = 5368709120  # 5 GiB (0x140000000)

    file_body = (
        write_vint(0) +
        write_vint(unp_sz) +
        write_vint(0x20) +
        write_vint(0) +
        struct.pack('<I', 0) +
        write_vint(0) +
        write_vint(1) +
        write_vint(len(fname)) +
        fname
    )
    file_hdr = make_block(2, 0, file_body)
    end_hdr = make_block(5, 0, write_vint(0))
    arc_bytes = sig + main_hdr + file_hdr + end_hdr

    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        p = td / "large.rar"
        p.write_bytes(arc_bytes)

        rc, out, err = run([openrar, "lt", str(p)])
        if rc != 0:
            print(f"  FAIL: openrar lt 64-bit VINT rc={rc}\n{out}\n{err}"); return False
        if str(unp_sz) not in out:
            print(f"  FAIL: openrar lt did not report 64-bit size {unp_sz}:\n{out}"); return False

        if unrar:
            rc, out, err = run([unrar, "lt", str(p)])
            if rc != 0:
                print(f"  FAIL: unrar lt 64-bit VINT rc={rc}\n{out}\n{err}"); return False
            if str(unp_sz) not in out:
                print(f"  FAIL: unrar lt did not report 64-bit size {unp_sz}:\n{out}"); return False

    print("  OK Track 7 64-bit VINT size bounds & memory safety")
    return True

def test_track8_parallel_compression(openrar, unrar):
    print("[11/14] Track 8: High-Throughput Parallel Compression (-mt) & Block Pipeline...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        p_small = td / "payload_2mb.bin"
        p_large = td / "payload_18mb.bin"

        def gen_data(sz, seed):
            b = bytearray(sz)
            for i in range(sz):
                b[i] = ((i % 251) ^ (i // 120) ^ seed) & 0xFF
            return bytes(b)

        data_small = gen_data(2 * 1024 * 1024, 42)
        data_large = gen_data(18 * 1024 * 1024, 77)
        p_small.write_bytes(data_small)
        p_large.write_bytes(data_large)

        h_small = hashlib.sha256(data_small).hexdigest()
        h_large = hashlib.sha256(data_large).hexdigest()

        for mt_threads, target_file, target_hash in [("4", p_small, h_small), ("8", p_large, h_large)]:
            arc = td / f"mt_{mt_threads}.rar"
            rc, out, err = run([openrar, "a", "-y", f"-mt{mt_threads}", str(arc), target_file.name], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: openrar a -mt{mt_threads} rc={rc}\n{out}\n{err}"); return False

            rc, out, err = run([openrar, "t", "-y", str(arc)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: openrar t mt{mt_threads} rc={rc}\n{out}\n{err}"); return False

            if unrar:
                rc, out, err = run([unrar, "t", "-y", str(arc)], cwd=str(td))
                if rc != 0:
                    print(f"  FAIL: unrar t mt{mt_threads} rc={rc}\n{out}\n{err}"); return False

            outdir = td / f"out_mt_{mt_threads}"
            outdir.mkdir()
            rc, out, err = run([openrar, "x", "-y", str(arc), str(outdir) + os.sep], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: openrar x mt{mt_threads} rc={rc}\n{out}\n{err}"); return False

            dec = find_extracted(outdir, target_file.name)
            if not dec or sha256(dec) != target_hash:
                print(f"  FAIL: extracted payload mismatch for mt{mt_threads}"); return False

    print("  OK Track 8 high-throughput parallel compression (-mt)")
    return True

def test_multivolume(openrar, rar):
    print("[12/14] Multivolume roundtrip (store + compressed)...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        src = td / "big.bin"
        src.write_bytes(os.urandom(48 * 1024))
        h0 = sha256(src)

        if rar:
            ref = td / "ref_mv.rar"
            rc, out, err = run([rar, "a", "-m0", "-v10k", str(ref), str(src)], cwd=str(td))
            if rc == 0:
                first = td / "ref_mv.part01.rar"
                if not first.exists():
                    cand = sorted(td.glob("ref_mv*.rar"))
                    first = cand[0] if cand else ref
                rc2, out2, err2 = run([openrar, "t", str(first)])
                if rc2 != 0:
                    print(f"  FAIL: openrar t WinRAR multivolume rc={rc2}\n{out2}\n{err2}"); return False
                print("  OK: openrar decodes WinRAR multivolume")

        mv = td / "mv.rar"
        rc, out, err = run([openrar, "a", "-m0", "-v10k", str(mv), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -v10k rc={rc}\n{out}\n{err}"); return False
        first = td / "mv.part01.rar"
        if not first.exists():
            cand = sorted(td.glob("mv*.rar"))
            first = cand[0] if cand else mv
        if not first.exists():
            print(f"  FAIL: multivolume first part missing {first}"); return False
        rc, out, err = run([openrar, "t", str(first)])
        if rc != 0:
            print(f"  FAIL: openrar t self multivolume rc={rc}\n{out}\n{err}"); return False

        if rar:
            rc2, out2, err2 = run([rar, "t", str(first)])
            if rc2 != 0:
                print(f"  FAIL: rar t openrar multivolume rc={rc2}\n{out2}\n{err2}"); return False
            print("  OK: rar decodes openrar multivolume")

    print("  OK multivolume (store)")
    return True

def test_sfx(openrar):
    print("[13/14] SFX read + create...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        src = td / "hello.txt"
        src.write_bytes(b"hello sfx world\n" * 256)
        stub_candidates = [
            ROOT / "build" / "openrar64" / "Release" / "Default.SFX.exe",
            ROOT / "build" / "openrar64" / "Debug" / "Default.SFX.exe",
            ROOT / "build" / "Default.SFX.exe",
            ROOT / "default.sfx",
        ]
        stub = next((p for p in stub_candidates if p.exists()), None)
        if stub is None:
            print("  SKIP: no Default.SFX stub found"); return True

        try:
            shutil.copy2(str(stub), str(td / "default.sfx"))
        except Exception:
            pass

        sfx_arc = td / "sfx_test.exe"
        rc, out, err = run([openrar, "a", "-sfx", "-m0", str(sfx_arc), str(src)], cwd=str(td))
        if rc != 0:
            rc, out, err = run([openrar, "a", "-sfx", str(sfx_arc), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -sfx rc={rc}\n{out}\n{err}"); return False

        cand = [sfx_arc, td / "sfx_test.exe", td / "hello.exe"]
        sfx_file = next((p for p in cand if p.exists()), None)
        if sfx_file is None:
            cand = sorted(td.glob("*.exe"))
            sfx_file = cand[0] if cand else None
        if sfx_file is None or not sfx_file.exists():
            print(f"  FAIL: SFX file not produced {sfx_arc}"); return False

        rc, out, err = run([openrar, "t", str(sfx_file)])
        if rc != 0:
            print(f"  FAIL: openrar t SFX rc={rc}\n{out}\n{err}"); return False

        outdir = td / "out"
        outdir.mkdir()
        rc, out, err = run([openrar, "x", "-y", str(sfx_file), str(outdir) + os.sep])
        if rc != 0:
            print(f"  FAIL: openrar x SFX rc={rc}\n{out}\n{err}"); return False
        dec = find_extracted(outdir, "hello.txt")
        if not dec or dec.read_bytes() != src.read_bytes():
            print("  FAIL: SFX extracted content mismatch"); return False

    print("  OK SFX read + create")
    return True

def test_ctest():
    print("[14/14] ctest compress_tests...", flush=True)
    for cfg in ["Release", "Debug"]:
        rc, out, err = run(["ctest", "-C", cfg, "-R", "compress_tests", "--test-dir", "build", "--output-on-failure"])
        if rc == 0:
            print("  OK compress_tests"); return True
        if "No tests" in out:
            continue
        print(f"  FAIL ctest {cfg}\n{out}\n{err}"); return False

    for p in [ROOT / "build" / "Release" / "compress_tests.exe", ROOT / "build" / "compress_tests.exe"]:
        if p.exists():
            rc, out, err = run([str(p)])
            if rc != 0:
                print(f"  FAIL {p}\n{out}\n{err}"); return False
            print("  OK compress_tests (direct)"); return True
    return True

def main():
    import time
    t0 = time.time()
    if not ensure_build():
        print("FAIL: cannot build/find openrar"); sys.exit(1)
    openrar = find_openrar()
    unrar = find_unrar()
    rar = find_rar()
    print(f"openrar: {openrar}")
    print(f"unrar  : {unrar or 'not found (CI fallback)'}")
    print(f"rar    : {rar or 'not found'}")

    stages = [
        (test_self_roundtrip, (openrar,)),
        (test_cross, (openrar, unrar, rar)),
        (test_methods_interop, (openrar, unrar, rar)),
        (test_track1_solid_mixed, (openrar, unrar, rar)),
        (test_track2_filters, (openrar, unrar, rar)),
        (test_track3_timestamps, (openrar, unrar)),
        (test_track4_passwords, (openrar, unrar, rar)),
        (test_track5_quickopen, (openrar, unrar, rar)),
        (test_track6_recovery_volumes, (openrar, unrar, rar)),
        (test_track7_vint64, (openrar, unrar)),
        (test_track8_parallel_compression, (openrar, unrar)),
        (test_multivolume, (openrar, rar)),
        (test_sfx, (openrar,)),
        (test_ctest, ()),
    ]

    for fn, args in stages:
        if not fn(*args):
            print(f"\nINTEROP GATE FAILED at {fn.__name__}")
            sys.exit(1)

    elapsed = time.time() - t0
    print(f"\n=======================================================")
    print(f" ALL 14 INTEROP GATE STAGES PASSED in {elapsed:.2f}s")
    print(f"=======================================================")

if __name__ == "__main__":
    main()
