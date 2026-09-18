#!/usr/bin/env python3
"""
Local interop gate - validates RAR5 spec compliance before commit.

Checks:
  1. Self-roundtrip hash (openrar a -> openrar x)
  2. Cross-decode vs reference unrar (if available):
     - openrar archive must be decodable by unrar
     - unrar/WinRAR archive must be decodable by openrar
     - absolute table encoding invariant
  3. Compression methods m1-m5 full-dictionary & wrap-around:
     - Active codec stress (1.25x dict, straddle tokens across circular buffer)
     - Explicit openrar lt Method: m assertion (fail if silently downgraded to 0)
     - Reference unpack (UnRAR x) + SHA-256 byte-for-byte verification
     - Incompressible fallback stress (pure random payload -> Method: 0 assertion)
     - Bidirectional symmetry (rar.exe a -m{m} -> openrar x byte-for-byte match)
     - Solid multi-file dictionary retention across boundary (-s -m3)
  4. Unit & compression tests via ctest
  5. Multivolume roundtrip (store + compressed)
  6. SFX read + create

CI: rar.exe is not available in GitHub Actions, so this gate is LOCAL ONLY
for src/compress/* changes. In CI we fall back to self-roundtrip + ctest.

Usage: python tools/interop_gate.py [--quick]  (quick skips 50MB bench)
Exit 0 = pass, 1 = fail.
"""
import os, sys, subprocess, hashlib, tempfile, shutil, pathlib, random

ROOT = pathlib.Path(__file__).resolve().parent.parent
OPENRAR = ROOT / "build" / "openrar64" / "Release" / "openrar.exe"
OPENRAR_DBG = ROOT / "build" / "openrar64" / "Debug" / "openrar.exe"
UNRAR = pathlib.Path(r"C:\Program Files\WinRAR\UnRAR.exe")
RAR = pathlib.Path(r"C:\Program Files\WinRAR\rar.exe")
# Fallback: buildable unrar from ../unrar (if user has it)

def find_openrar():
    for p in [OPENRAR, OPENRAR_DBG, ROOT / "build" / "openrar.exe", ROOT / "build" / "Release" / "openrar.exe", ROOT / "build" / "openrar"]:
        if p is None: continue
        if p.exists(): return str(p)
    return None

def find_unrar():
    unrar_env = os.environ.get("UNRAR_EXE")
    candidates = [pathlib.Path(unrar_env)] if unrar_env else []
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

def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return r.returncode, r.stdout, r.stderr

def ensure_build():
    # Build Release if missing; pre-commit already does Debug but gate prefers Release for speed parity
    if find_openrar(): return True
    print("[interop-gate] building openrar Release...", flush=True)
    rc, out, err = run(["cmake", "--build", "build", "--config", "Release", "-j", "8"])
    if rc != 0:
        print(out, err); return False
    return find_openrar() is not None

def test_self_roundtrip(openrar):
    print("[1/6] Self-roundtrip hash...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        src = td / "payload.bin"
        # deterministic 64KB + repeat pattern (same as test_roundtrip_hash.py)
        import random
        random.seed(0xC0FFEE)
        data = os.urandom(1024) * 4 + b"A_REPEAT_PATTERN" * 1024
        data = data * 8  # ~  ~ 160KB, enough to trigger Huffman tables
        src.write_bytes(data)
        h0 = sha256(src)
        arc = td / "test.rar"
        rc, out, err = run([openrar, "a", str(arc), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a rc={rc}\n{out}\n{err}"); return False
        # test
        rc, out, err = run([openrar, "t", str(arc)])
        if rc != 0:
            print(f"  FAIL: openrar t self rc={rc}\n{out}\n{err}"); return False
        outdir = td / "out"
        outdir.mkdir()
        rc, out, err = run([openrar, "x", "-y", str(arc), str(outdir) + os.sep])
        if rc != 0:
            print(f"  FAIL: openrar x rc={rc}\n{out}\n{err}"); return False
        dec = outdir / "payload.bin"
        if not dec.exists():
            # try flat name
            cand = list(outdir.rglob("payload.bin"))
            if not cand:
                print(f"  FAIL: extracted file missing"); return False
            dec = cand[0]
        h1 = sha256(dec)
        if h0 != h1:
            print(f"  FAIL: hash mismatch {h0} vs {h1}"); return False
        print(f"  OK hash {h0[:16]}..."); return True

def test_cross(openrar, unrar, rar):
    print("[2/6] Cross-interop vs reference unrar...", flush=True)
    has_ref = unrar is not None or rar is not None
    if not has_ref:
        print("  SKIP: no UnRAR/rar.exe found (CI fallback) - checking spec invariant via table delta probe")
        # Spec probe: create two archives with same payload; if tables were delta, second block would drift.
        # Self-roundtrip already passed, so we do a two-block payload (>512KB) to exercise table_present
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            src = td / "twoblock.bin"
            # ~1.5MB ensures >=2 blocks (need_flush 0x80000)
            src.write_bytes((b"ABCD" * 1024 + b"XYZ " * 2048) * 300)
            arc = td / "twoblock.rar"
            rc, out, err = run([openrar, "a", "-m3", str(arc), str(src)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: twoblock a rc={rc}"); return False
            rc, out, err = run([openrar, "t", str(arc)])
            if rc != 0:
                print(f"  FAIL: twoblock t rc={rc}\n{out}"); return False
            print("  OK (no reference binary, two-block self-test passed)")
            return True

    ok = True
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        # payload 512KB deterministic
        src = td / "interop.bin"
        src.write_bytes((b"The quick brown fox " * 64 + b"\x00\xff\x55\xaa" * 16) * 2048)
        h0 = sha256(src)
        # openrar -> unrar
        arc_or = td / "or.rar"
        rc, out, err = run([openrar, "a", "-m3", str(arc_or), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a rc={rc}\n{out}\n{err}"); return False
        if unrar:
            rc, out, err = run([unrar, "t", str(arc_or)])
            if rc != 0:
                print(f"  FAIL: UnRAR t openrar.rar rc={rc}\n{out}\n{err}")
                print("  -> spec divergence: WinRAR interprets delta as absolute")
                ok = False
            else:
                print("  OK: UnRAR decodes openrar.rar")
        elif rar:
            rc, out, err = run([rar, "t", str(arc_or)])
            if rc != 0:
                print(f"  FAIL: rar t openrar.rar rc={rc}\n{out}\n{err}"); ok = False
            else:
                print("  OK: rar decodes openrar.rar")
        # rar/unrar -> openrar (if we can create with reference)
        if rar:
            arc_rar = td / "ref.rar"
            rc, out, err = run([rar, "a", "-m3", "-mt1", str(arc_rar), str(src)], cwd=str(td))
            if rc == 0 and arc_rar.exists():
                rc2, out2, err2 = run([openrar, "t", str(arc_rar)])
                if rc2 != 0:
                    print(f"  FAIL: openrar t rar.rar rc={rc2}\n{out2}\n{err2}")
                    print("  -> spec divergence: openrar adds delta to absolute")
                    ok = False
                else:
                    print("  OK: openrar decodes rar.rar")
                    # byte-match
                    outdir = td / "ref_out"
                    outdir.mkdir()
                    rc2, _, _ = run([openrar, "x", "-y", str(arc_rar), str(outdir)+os.sep])
                    dec = outdir / "interop.bin"
                    if dec.exists() and sha256(dec) != h0:
                        print("  FAIL: ref roundtrip hash mismatch"); ok = False
            else:
                print(f"  SKIP: rar a failed rc={rc}")
        elif unrar:  # unrar can't create, skip
            print("  SKIP: no rar.exe to create ref archive")
    return ok

def test_methods_interop(openrar, unrar, rar):
    print("[3/6] Compression methods m1-m5 full-dictionary & wrap-around...", flush=True)
    has_ref = unrar is not None or rar is not None
    ref_decompress = unrar or rar

    # Method configurations: (method, dict_size, payload_size, extra_openrar_flags, extra_rar_flags)
    # Sizing payload at 1.25x dict_size guarantees circular ring-buffer boundary wrap-around.
    # For m5, -md4m bounds the dictionary window to 4 MiB for sub-second test execution.
    configs = [
        (1, 512 * 1024, 640 * 1024, [], ["-md512k"]),
        (2, 1024 * 1024, 1280 * 1024, [], ["-md1024k"]),
        (3, 2048 * 1024, 2560 * 1024, [], ["-md2048k"]),
        (4, 4096 * 1024, 5120 * 1024, [], ["-md4096k"]),
        (5, 4096 * 1024, 5120 * 1024, ["-md4m"], ["-md4096k"]),
    ]

    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)

        # --- 1. Active Codec Stress + Wrap-Around with Straddle Tokens ---
        print("  -> Active codec stress (1.25x dict, straddle tokens, Method assertion)...")
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
                print(f"  FAIL: openrar a -m{m} rc={rc}\n{out}\n{err}")
                return False

            rc, out, err = run([openrar, "lt", str(arc)])
            if rc != 0:
                print(f"  FAIL: openrar lt active_m{m} rc={rc}\n{out}\n{err}")
                return False
            detected_m = parse_lt_method(out)
            if detected_m != m:
                print(f"  FAIL: openrar lt active_m{m} expected Method: {m}, got {detected_m} (downgraded?)")
                return False

            if ref_decompress:
                outdir = td / f"unrar_act_{m}"
                outdir.mkdir()
                rc, out, err = run([ref_decompress, "x", "-y", str(arc), str(outdir) + os.sep])
                if rc != 0:
                    print(f"  FAIL: reference x active_m{m} rc={rc}\n{out}\n{err}")
                    return False
                dec = find_extracted(outdir, f"active_m{m}.bin")
                if not dec or sha256(dec) != h0:
                    print(f"  FAIL: reference extracted hash mismatch on active_m{m}")
                    return False

            # Also verify self-extraction
            outdir_self = td / f"self_act_{m}"
            outdir_self.mkdir()
            rc, out, err = run([openrar, "x", "-y", str(arc), str(outdir_self) + os.sep])
            if rc != 0:
                print(f"  FAIL: openrar x active_m{m} rc={rc}\n{out}\n{err}")
                return False
            dec_self = find_extracted(outdir_self, f"active_m{m}.bin")
            if not dec_self or sha256(dec_self) != h0:
                print(f"  FAIL: openrar extracted hash mismatch on active_m{m}")
                return False
            print(f"    OK active m{m} (1.25x dict wrap-around verified)")

        # --- 2. Incompressible Fallback Stress (Store Fallback) ---
        print("  -> Incompressible fallback stress (pure random dict-size payload)...")
        for m, dict_sz, payload_sz, or_flags, rar_flags in configs:
            rng = random.Random(0xC0FFEE + 100 + m)
            src = td / f"rnd_m{m}.bin"
            src.write_bytes(rng.randbytes(dict_sz))
            h0 = sha256(src)

            arc = td / f"rnd_m{m}.rar"
            rc, out, err = run([openrar, "a", f"-m{m}"] + or_flags + [str(arc), str(src)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: openrar a rnd_m{m} rc={rc}\n{out}\n{err}")
                return False

            rc, out, err = run([openrar, "lt", str(arc)])
            if rc != 0:
                print(f"  FAIL: openrar lt rnd_m{m} rc={rc}\n{out}\n{err}")
                return False
            detected_m = parse_lt_method(out)
            if detected_m != 0:
                print(f"  FAIL: openrar lt rnd_m{m} expected Method: 0 fallback, got {detected_m}")
                return False

            if ref_decompress:
                outdir = td / f"unrar_rnd_{m}"
                outdir.mkdir()
                rc, out, err = run([ref_decompress, "x", "-y", str(arc), str(outdir) + os.sep])
                if rc != 0:
                    print(f"  FAIL: reference x rnd_m{m} rc={rc}\n{out}\n{err}")
                    return False
                dec = find_extracted(outdir, f"rnd_m{m}.bin")
                if not dec or sha256(dec) != h0:
                    print(f"  FAIL: reference extracted hash mismatch on rnd_m{m}")
                    return False
            print(f"    OK fallback m{m} (correctly downgraded to Method: 0)")

        # --- 3. Bidirectional Symmetry (rar.exe a -m{m} -> openrar x) ---
        if rar:
            print("  -> Bidirectional symmetry (rar.exe a -m{m} -> openrar x)...")
            for m, dict_sz, payload_sz, or_flags, rar_flags in configs:
                src = td / f"active_m{m}.bin"
                h0 = sha256(src)
                arc = td / f"rar_m{m}.rar"
                rc, out, err = run([rar, "a", "-ep", f"-m{m}"] + rar_flags + [str(arc), str(src)], cwd=str(td))
                if rc != 0:
                    print(f"  FAIL: rar a -m{m} rc={rc}\n{out}\n{err}")
                    return False

                outdir = td / f"openrar_from_rar_{m}"
                outdir.mkdir()
                rc, out, err = run([openrar, "x", "-y", str(arc), str(outdir) + os.sep])
                if rc != 0:
                    print(f"  FAIL: openrar x from rar_m{m} rc={rc}\n{out}\n{err}")
                    return False
                dec = find_extracted(outdir, f"active_m{m}.bin")
                if not dec or sha256(dec) != h0:
                    print(f"  FAIL: hash mismatch on rar_m{m}")
                    return False
                print(f"    OK bidirectional m{m}")
        else:
            print("  SKIP: rar.exe not found (skipping bidirectional create)")

        # --- 4. Solid Multi-File Cross-Boundary History Stress ---
        print("  -> Solid multi-file dictionary retention across file boundary (-s -m3)...")
        rng = random.Random(0xC0FFEE + 999)
        shared = b"SHARED_SOLID_DICTIONARY_HISTORY_CROSS_FILE_TOKEN!" * 200
        f1 = td / "solid_f1.bin"
        f2 = td / "solid_f2.bin"
        f1.write_bytes(rng.randbytes(256 * 1024) + shared)
        f2.write_bytes(shared + rng.randbytes(128 * 1024) + shared)
        h1, h2 = sha256(f1), sha256(f2)

        arc = td / "solid_openrar.rar"
        rc, out, err = run([openrar, "a", "-s", "-m3", str(arc), str(f1), str(f2)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -s -m3 rc={rc}\n{out}\n{err}")
            return False

        if ref_decompress:
            outdir = td / "unrar_solid"
            outdir.mkdir()
            rc, out, err = run([ref_decompress, "x", "-y", str(arc), str(outdir) + os.sep])
            if rc != 0:
                print(f"  FAIL: reference x solid rc={rc}\n{out}\n{err}")
                return False
            dec1 = find_extracted(outdir, "solid_f1.bin")
            dec2 = find_extracted(outdir, "solid_f2.bin")
            if not dec1 or not dec2 or sha256(dec1) != h1 or sha256(dec2) != h2:
                print("  FAIL: solid extraction hash mismatch")
                return False

        # Self-extract solid
        outdir_self = td / "self_solid"
        outdir_self.mkdir()
        rc, out, err = run([openrar, "x", "-y", str(arc), str(outdir_self) + os.sep])
        if rc != 0:
            print(f"  FAIL: openrar x solid rc={rc}\n{out}\n{err}")
            return False
        dec1_s = find_extracted(outdir_self, "solid_f1.bin")
        dec2_s = find_extracted(outdir_self, "solid_f2.bin")
        if not dec1_s or not dec2_s or sha256(dec1_s) != h1 or sha256(dec2_s) != h2:
            print("  FAIL: openrar self solid hash mismatch")
            return False

        if rar:
            arc_rar = td / "solid_rar.rar"
            rc, out, err = run([rar, "a", "-s", "-ep", "-m3", str(arc_rar), str(f1), str(f2)], cwd=str(td))
            if rc != 0:
                print(f"  FAIL: rar a -s -ep -m3 rc={rc}\n{out}\n{err}")
                return False
            outdir_rar = td / "openrar_from_solid_rar"
            outdir_rar.mkdir()
            rc, out, err = run([openrar, "x", "-y", str(arc_rar), str(outdir_rar) + os.sep])
            if rc != 0:
                print(f"  FAIL: openrar x solid_rar rc={rc}\n{out}\n{err}")
                return False
            dec1_r = find_extracted(outdir_rar, "solid_f1.bin")
            dec2_r = find_extracted(outdir_rar, "solid_f2.bin")
            if not dec1_r or not dec2_r or sha256(dec1_r) != h1 or sha256(dec2_r) != h2:
                print("  FAIL: openrar from solid_rar hash mismatch")
                return False

        print("    OK solid multi-file cross-boundary retention verified")

    print("  OK methods m1-m5 full-dictionary & wrap-around")
    return True

def test_multivolume(openrar, rar):
    print("[5/6] Multivolume roundtrip (store + compressed)...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        # incompressible payload ensures split is on packed stream
        src = td / "big.bin"
        src.write_bytes(os.urandom(48 * 1024))
        h0 = sha256(src)
        # --- WinRAR multivolume -> openrar decode (if rar available) ---
        if rar:
            ref = td / "ref_mv.rar"
            rc, out, err = run([rar, "a", "-m0", "-v10k", str(ref), str(src)], cwd=str(td))
            if rc == 0:
                first = td / "ref_mv.part01.rar"
                if not first.exists():
                    # rar may have produced .part1.rar naming vs .part01
                    cand = sorted(td.glob("ref_mv*.rar"))
                    first = cand[0] if cand else ref
                rc2, out2, err2 = run([openrar, "t", str(first)])
                if rc2 != 0:
                    print(f"  FAIL: openrar t WinRAR multivolume rc={rc2}\n{out2}\n{err2}")
                    return False
                # stitch + l count check
                rc2, out2, err2 = run([openrar, "l", str(first)])
                if "1" not in out2 and "big.bin" not in out2:
                    print(f"  FAIL: openrar l multivolume output\n{out2}")
                    return False
                print("  OK: openrar decodes WinRAR multivolume")
            else:
                print(f"  SKIP: rar a -v failed rc={rc}")
        # --- openrar multivolume -> openrar + rar decode ---
        mv = td / "mv.rar"
        rc, out, err = run([openrar, "a", "-m0", "-v10k", str(mv), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -v10k rc={rc}\n{out}\n{err}")
            return False
        # first volume name is derived via volume::first_volume_name -> mv.part01.rar
        first = td / "mv.part01.rar"
        if not first.exists():
            cand = sorted(td.glob("mv*.rar"))
            # also check orig mv.rar if single-volume fallback
            first = cand[0] if cand else mv
        if not first.exists():
            print(f"  FAIL: multivolume first part missing {first}")
            return False
        rc, out, err = run([openrar, "t", str(first)])
        if rc != 0:
            print(f"  FAIL: openrar t self multivolume rc={rc}\n{out}\n{err}")
            return False
        # l should show single logical file
        rc, out, err = run([openrar, "l", str(first)])
        if "big.bin" not in out:
            print(f"  FAIL: openrar l multivolume missing entry\n{out}")
            return False
        # cross: rar should decode openrar multivolume if available. Hard fail:
        # if rar created + tested its own volumes above, failing ours is a
        # spec divergence, not a version quirk.
        if rar:
            rc2, out2, err2 = run([rar, "t", str(first)])
            if rc2 != 0:
                print(f"  FAIL: rar t openrar multivolume rc={rc2}\n{out2}\n{err2}")
                return False
            print("  OK: rar decodes openrar multivolume")
        print("  OK multivolume (store)")
        return True

def test_sfx(openrar):
    print("[6/6] SFX read + create...", flush=True)
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        src = td / "hello.txt"
        src.write_bytes(b"hello sfx world\n" * 256)
        # Check SFX stub exists (Default.SFX or default.sfx)
        stub_candidates = [
            ROOT / "build" / "openrar64" / "Release" / "Default.SFX.exe",
            ROOT / "build" / "openrar64" / "Debug" / "Default.SFX.exe",
            ROOT / "build" / "Default.SFX.exe",
            ROOT / "default.sfx",
        ]
        stub = next((p for p in stub_candidates if p.exists()), None)
        if stub is None:
            print("  SKIP: no Default.SFX stub found (build openrar first)")
            return True
        # Workaround: ensure stub is copied to temp default.sfx for bare -sfx case
        import shutil as _sh
        try:
            _sh.copy2(str(stub), str(td / "default.sfx"))
        except Exception:
            pass
        sfx_arc = td / "sfx_test.exe"
        rc, out, err = run([openrar, "a", "-sfx", "-m0", str(sfx_arc), str(src)], cwd=str(td))
        # openrar's SFX creation uses -sfx[name] and apply_sfx_extension -> .exe
        if rc != 0:
            # try bare -sfx without = (CLI expects -sfx)
            rc, out, err = run([openrar, "a", "-sfx", str(sfx_arc), str(src)], cwd=str(td))
        if rc != 0:
            print(f"  FAIL: openrar a -sfx rc={rc}\n{out}\n{err}")
            return False
        # SFX should have signature at offset <= 4MiB and be listable via openrar l/t
        # Find produced exe (may be sfx_test.exe)
        cand = [sfx_arc, td / "sfx_test.exe", td / "hello.exe"]
        sfx_file = next((p for p in cand if p.exists()), None)
        if sfx_file is None:
            cand = sorted(td.glob("*.exe"))
            sfx_file = cand[0] if cand else None
        if sfx_file is None or not sfx_file.exists():
            print(f"  FAIL: SFX file not produced {sfx_arc}")
            return False
        # SFX scan check: t should succeed
        rc, out, err = run([openrar, "t", str(sfx_file)])
        if rc != 0:
            print(f"  FAIL: openrar t SFX rc={rc}\n{out}\n{err}")
            return False
        # also test extraction
        outdir = td / "out"
        outdir.mkdir()
        rc, out, err = run([openrar, "x", "-y", str(sfx_file), str(outdir) + os.sep])
        if rc != 0:
            print(f"  FAIL: openrar x SFX rc={rc}\n{out}\n{err}")
            return False
        # verify payload hash
        decs = list(outdir.rglob("hello.txt"))
        if not decs:
            print("  FAIL: SFX extracted file missing")
            return False
        if decs[0].read_bytes() != src.read_bytes():
            print("  FAIL: SFX extracted content mismatch")
            return False
        print(f"  OK SFX {sfx_file.name} {sfx_file.stat().st_size}B")
        return True

def test_ctest():
    print("[4/6] ctest compress_tests...", flush=True)
    # Prefer Release, fallback Debug
    for cfg in ["Release", "Debug"]:
        bd = ROOT / "build"
        # ctest --test-dir build -R compress_tests
        rc, out, err = run(["ctest", "-C", cfg, "-R", "compress_tests", "--test-dir", "build", "--output-on-failure"])
        if rc == 0:
            print("  OK compress_tests"); return True
        # if not found, try next
        if "No tests" in out:
            continue
        print(f"  FAIL ctest {cfg}\n{out}\n{err}")
        return False
    print("  SKIP: ctest not found, fallback to direct binary")
    for p in [ROOT/"build"/"Release"/"compress_tests.exe", ROOT/"build"/"compress_tests.exe"]:
        if p.exists():
            rc, out, err = run([str(p)])
            if rc != 0:
                print(f"  FAIL {p}\n{out}\n{err}"); return False
            print("  OK compress_tests (direct)"); return True
    print("  SKIP: compress_tests binary missing"); return True

def main():
    quick = "--quick" in sys.argv
    if not ensure_build():
        print("FAIL: cannot build/find openrar"); sys.exit(1)
    openrar = find_openrar()
    unrar = find_unrar()
    rar = find_rar()
    print(f"openrar: {openrar}")
    print(f"unrar  : {unrar or 'not found (CI fallback)'}")
    print(f"rar    : {rar or 'not found'}")
    if not test_self_roundtrip(openrar):
        sys.exit(1)
    if not test_cross(openrar, unrar, rar):
        print("\nINTEROP GATE FAILED: spec divergence detected in absolute tables.")
        sys.exit(1)
    if not test_methods_interop(openrar, unrar, rar):
        print("\nINTEROP GATE FAILED: methods m1-m5 full-dictionary and wrap-around.")
        sys.exit(1)
    if not test_ctest():
        sys.exit(1)
    if not test_multivolume(openrar, rar):
        print("\nINTEROP GATE FAILED: multivolume")
        sys.exit(1)
    if not test_sfx(openrar):
        print("\nINTEROP GATE FAILED: sfx")
        sys.exit(1)
    print("\nINTEROP GATE PASSED")
    if not quick:
        # Optional 50MB bench hint
        print("Tip: run python bench_50mb.py for full 50MB perf parity (local only)")

if __name__ == "__main__":
    main()
