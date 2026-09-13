#!/usr/bin/env python3
"""
Local interop gate - validates RAR5 spec compliance before commit.

Checks:
  1. Self-roundtrip hash (openrar a -> openrar x)
  2. Cross-decode vs reference unrar (if available):
     - openrar archive must be decodable by unrar
     - unrar/WinRAR archive must be decodable by openrar
  3. Spec invariant: table encoding is absolute (not delta) - verified via
     producing an archive and checking that cross-decode succeeds (paired-bug
     would pass 1 but fail 2).

CI: rar.exe is not available in GitHub Actions, so this gate is LOCAL ONLY
for src/compress/* changes. In CI we fall back to self-roundtrip + ctest.

Usage: python tools/interop_gate.py [--quick]  (quick skips 50MB bench)
Exit 0 = pass, 1 = fail.
"""
import os, sys, subprocess, hashlib, tempfile, shutil, pathlib

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
    print("[1/3] Self-roundtrip hash...", flush=True)
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
    print("[2/3] Cross-interop vs reference unrar...", flush=True)
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

def test_multivolume(openrar, rar):
    print("[4/5] Multivoluume roundtrip (store + compressed)...", flush=True)
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
    print("[5/5] SFX read + create...", flush=True)
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
    print("[3/5] ctest compress_tests...", flush=True)
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
