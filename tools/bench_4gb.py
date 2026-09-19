import os
import sys
import time
import shutil
import subprocess
import ctypes

def get_free_space_gb(path="."):
    free_bytes = ctypes.c_ulonglong(0)
    total_bytes = ctypes.c_ulonglong(0)
    total_free_bytes = ctypes.c_ulonglong(0)
    ctypes.windll.kernel32.GetDiskFreeSpaceExW(
        ctypes.c_wchar_p(os.path.abspath(path)),
        ctypes.byref(free_bytes),
        ctypes.byref(total_bytes),
        ctypes.byref(total_free_bytes)
    )
    return free_bytes.value / (1024**3)

OPENRAR = os.path.abspath("build/openrar64/Release/openrar.exe")
WINRAR = r"C:\Program Files\WinRAR\rar.exe"
UNRAR = r"C:\Program Files\WinRAR\UnRAR.exe"
PAYLOAD_PATH = os.path.abspath("payload_4gb.dat")
ARC_OPENRAR = os.path.abspath("bench_openrar_4gb.rar")
ARC_WINRAR = os.path.abspath("bench_winrar_4gb.rar")

METHOD_NAMES = {
    0: "m0 (Store)",
    1: "m1 (Fastest)",
    2: "m2 (Fast)",
    3: "m3 (Normal)",
    4: "m4 (Good)",
    5: "m5 (Best)",
}

PRIORITY_FLAGS = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0) if sys.platform == "win32" else 0

if sys.platform == "win32":
    try:
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00004000) # BELOW_NORMAL_PRIORITY_CLASS
    except Exception:
        pass

def main():
    print("=" * 80)
    print("        OpenRAR v1.13.0 vs WinRAR 7.20 x64: 4.0 GiB Benchmark (m0 - m5)")
    print("        (Running with BELOW_NORMAL priority to keep IDE/UI responsive)")
    print("=" * 80)
    print(f"OpenRAR Binary: {OPENRAR}")
    print(f"WinRAR Binary : {WINRAR}")

    initial_free = get_free_space_gb()
    print(f"Initial Free Disk Space: {initial_free:.2f} GB", flush=True)
    if initial_free < 5.0 and not os.path.exists(PAYLOAD_PATH):
        print(f"ERROR: Insufficient free space ({initial_free:.2f} GB < 5.0 GB minimum safety floor).")
        sys.exit(1)

    TOTAL_MB = 4096
    TOTAL_BYTES = TOTAL_MB * 1024 * 1024

    methods = [0, 1, 2, 3, 4, 5]
    if len(sys.argv) > 1:
        custom_methods = [int(x) for x in sys.argv[1:] if x.isdigit() and int(x) in range(6)]
        if custom_methods:
            methods = custom_methods

    print(f"Target Compression Methods: {[f'-m{m}' for m in methods]}\n", flush=True)

    results = {}

    try:
        # Step 1: Check or generate 4 GiB synthetic benchmark file
        if os.path.exists(PAYLOAD_PATH) and os.path.getsize(PAYLOAD_PATH) == TOTAL_BYTES:
            print(f"[Payload] Reusing existing 4.0 GiB benchmark payload: {PAYLOAD_PATH}", flush=True)
            actual_bytes = os.path.getsize(PAYLOAD_PATH)
        else:
            print(f"[Payload] Generating 4.0 GiB ({TOTAL_MB} MiB) benchmark payload...", flush=True)
            t0 = time.perf_counter()
            base_unit = (
                "OpenRAR high performance engine streaming compression benchmark test.\n"
                "The quick brown fox jumps over the lazy dog. 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
                "for (int i = 0; i < 1000; ++i) { sum += buffer[i] * matrix[i][j]; }\n"
                "std::vector<uint8_t> payload(65536); aes.encrypt_cbc(payload.data(), 65536, iv);\n"
            ).encode("utf-8")
            repeats = (1024 * 1024) // len(base_unit)
            chunk_1mb = (base_unit * (repeats + 1))[:1024 * 1024]
            chunk_64mb = chunk_1mb * 64
            
            with open(PAYLOAD_PATH, "wb") as f:
                for i in range(TOTAL_MB // 64):
                    f.write(chunk_64mb)
                    written_mb = (i + 1) * 64
                    if written_mb % 1024 == 0:
                        print(f"  ... written {written_mb} / {TOTAL_MB} MiB ({written_mb / TOTAL_MB * 100:.0f}%)", flush=True)

            gen_time = time.perf_counter() - t0
            actual_bytes = os.path.getsize(PAYLOAD_PATH)
            print(f"Payload created: {actual_bytes / (1024**3):.2f} GiB in {gen_time:.2f}s ({TOTAL_MB / gen_time:.1f} MB/s)", flush=True)

        print(f"Current free disk space: {get_free_space_gb():.2f} GB\n", flush=True)

        for m in methods:
            m_name = METHOD_NAMES.get(m, f"m{m}")
            print("-" * 80)
            print(f"  BENCHMARKING LEVEL: -{m_name}")
            print("-" * 80, flush=True)
            m_res = {}

            # Short breather to let OS I/O queues and pagecache settle
            time.sleep(1.0)

            if m == 0:
                # For m0 (Store), archives are 4.0 GiB uncompressed.
                # To prevent disk exhaustion, test OpenRAR and WinRAR sequentially with immediate cleanup.
                print(f"  [1/4] Running OpenRAR -m0 compression...", flush=True)
                if os.path.exists(ARC_OPENRAR):
                    os.remove(ARC_OPENRAR)
                t0 = time.perf_counter()
                p = subprocess.run([OPENRAR, "a", "-m0", "-q", ARC_OPENRAR, PAYLOAD_PATH],
                                   capture_output=True, text=True, creationflags=PRIORITY_FLAGS)
                t_openrar_comp = time.perf_counter() - t0
                if p.returncode != 0:
                    print(f"  OpenRAR FAILED (code {p.returncode}): {p.stderr}")
                    sys.exit(1)
                openrar_arc_size = os.path.getsize(ARC_OPENRAR)
                openrar_comp_speed = TOTAL_MB / t_openrar_comp
                m_res["openrar_comp_time"] = t_openrar_comp
                m_res["openrar_comp_speed"] = openrar_comp_speed
                m_res["openrar_arc_size"] = openrar_arc_size
                m_res["openrar_ratio"] = (openrar_arc_size / actual_bytes) * 100
                print(f"  -> OpenRAR Comp: {t_openrar_comp:6.2f}s | {openrar_comp_speed:7.1f} MB/s | Size: {openrar_arc_size / (1024*1024):7.2f} MiB ({m_res['openrar_ratio']:5.2f}%)", flush=True)

                print(f"  [2/4] Testing OpenRAR -m0 decompression & verification...", flush=True)
                p_openrar_self = subprocess.run([OPENRAR, "t", "-q", ARC_OPENRAR], capture_output=True, text=True, creationflags=PRIORITY_FLAGS)
                p_unrar_openrar = subprocess.run([UNRAR, "t", "-y", ARC_OPENRAR], capture_output=True, text=True, creationflags=PRIORITY_FLAGS)

                t0 = time.perf_counter()
                with open("nul", "wb") as devnull:
                    p = subprocess.run([OPENRAR, "p", ARC_OPENRAR], stdout=devnull, stderr=subprocess.DEVNULL, creationflags=PRIORITY_FLAGS)
                t_openrar_decomp = time.perf_counter() - t0
                openrar_decomp_speed = TOTAL_MB / t_openrar_decomp
                m_res["openrar_decomp_time"] = t_openrar_decomp
                m_res["openrar_decomp_speed"] = openrar_decomp_speed
                print(f"  -> OpenRAR Decomp: {t_openrar_decomp:6.2f}s ({openrar_decomp_speed:7.1f} MB/s)", flush=True)

                # Delete OpenRAR archive immediately so disk space is completely freed before WinRAR runs
                if os.path.exists(ARC_OPENRAR):
                    os.remove(ARC_OPENRAR)
                time.sleep(1.0)

                print(f"  [3/4] Running WinRAR -m0 compression...", flush=True)
                if os.path.exists(ARC_WINRAR):
                    os.remove(ARC_WINRAR)
                t0 = time.perf_counter()
                p = subprocess.run([WINRAR, "a", "-m0", "-inul", ARC_WINRAR, PAYLOAD_PATH],
                                   capture_output=True, text=True, creationflags=PRIORITY_FLAGS)
                t_winrar_comp = time.perf_counter() - t0
                if p.returncode != 0:
                    print(f"  WinRAR FAILED (code {p.returncode}): {p.stderr}")
                    sys.exit(1)
                winrar_arc_size = os.path.getsize(ARC_WINRAR)
                winrar_comp_speed = TOTAL_MB / t_winrar_comp
                m_res["winrar_comp_time"] = t_winrar_comp
                m_res["winrar_comp_speed"] = winrar_comp_speed
                m_res["winrar_arc_size"] = winrar_arc_size
                m_res["winrar_ratio"] = (winrar_arc_size / actual_bytes) * 100
                print(f"  -> WinRAR  Comp: {t_winrar_comp:6.2f}s | {winrar_comp_speed:7.1f} MB/s | Size: {winrar_arc_size / (1024*1024):7.2f} MiB ({m_res['winrar_ratio']:5.2f}%)", flush=True)

                print(f"  [4/4] Testing WinRAR -m0 decompression & verification...", flush=True)
                p_openrar_winrar = subprocess.run([OPENRAR, "t", "-q", ARC_WINRAR], capture_output=True, text=True, creationflags=PRIORITY_FLAGS)

                t0 = time.perf_counter()
                with open("nul", "wb") as devnull:
                    p = subprocess.run([WINRAR, "p", "-inul", ARC_WINRAR], stdout=devnull, stderr=subprocess.DEVNULL, creationflags=PRIORITY_FLAGS)
                t_winrar_decomp = time.perf_counter() - t0
                winrar_decomp_speed = TOTAL_MB / t_winrar_decomp
                m_res["winrar_decomp_time"] = t_winrar_decomp
                m_res["winrar_decomp_speed"] = winrar_decomp_speed
                print(f"  -> WinRAR Decomp: {t_winrar_decomp:6.2f}s ({winrar_decomp_speed:7.1f} MB/s)", flush=True)

                all_ok = (p_openrar_self.returncode == 0 and p_unrar_openrar.returncode == 0 and p_openrar_winrar.returncode == 0)
                m_res["verify"] = "PASS" if all_ok else "FAIL"
                print(f"  -> Dual-Oracle Integrity & Cross-Decodability: {m_res['verify']}\n", flush=True)

                if os.path.exists(ARC_WINRAR):
                    os.remove(ARC_WINRAR)
                time.sleep(1.0)

            else:
                # For m1..m5, compressed archives are only ~2 MiB, taking negligible disk space.
                # 1. OpenRAR Compression
                if os.path.exists(ARC_OPENRAR):
                    os.remove(ARC_OPENRAR)
                print(f"  [1/4] Running OpenRAR -m{m} compression...", flush=True)
                t0 = time.perf_counter()
                p = subprocess.run([OPENRAR, "a", f"-m{m}", "-q", ARC_OPENRAR, PAYLOAD_PATH],
                                   capture_output=True, text=True, creationflags=PRIORITY_FLAGS)
                t_openrar_comp = time.perf_counter() - t0
                if p.returncode != 0:
                    print(f"  OpenRAR FAILED (code {p.returncode}): {p.stderr}")
                    sys.exit(1)
                openrar_arc_size = os.path.getsize(ARC_OPENRAR)
                openrar_comp_speed = TOTAL_MB / t_openrar_comp
                m_res["openrar_comp_time"] = t_openrar_comp
                m_res["openrar_comp_speed"] = openrar_comp_speed
                m_res["openrar_arc_size"] = openrar_arc_size
                m_res["openrar_ratio"] = (openrar_arc_size / actual_bytes) * 100
                print(f"  -> OpenRAR Comp: {t_openrar_comp:6.2f}s | {openrar_comp_speed:7.1f} MB/s | Size: {openrar_arc_size / (1024*1024):7.2f} MiB ({m_res['openrar_ratio']:5.2f}%)", flush=True)

                time.sleep(0.5)

                # 2. WinRAR Compression
                if os.path.exists(ARC_WINRAR):
                    os.remove(ARC_WINRAR)
                print(f"  [2/4] Running WinRAR -m{m} compression...", flush=True)
                t0 = time.perf_counter()
                p = subprocess.run([WINRAR, "a", f"-m{m}", "-inul", ARC_WINRAR, PAYLOAD_PATH],
                                   capture_output=True, text=True, creationflags=PRIORITY_FLAGS)
                t_winrar_comp = time.perf_counter() - t0
                if p.returncode != 0:
                    print(f"  WinRAR FAILED (code {p.returncode}): {p.stderr}")
                    sys.exit(1)
                winrar_arc_size = os.path.getsize(ARC_WINRAR)
                winrar_comp_speed = TOTAL_MB / t_winrar_comp
                m_res["winrar_comp_time"] = t_winrar_comp
                m_res["winrar_comp_speed"] = winrar_comp_speed
                m_res["winrar_arc_size"] = winrar_arc_size
                m_res["winrar_ratio"] = (winrar_arc_size / actual_bytes) * 100
                print(f"  -> WinRAR  Comp: {t_winrar_comp:6.2f}s | {winrar_comp_speed:7.1f} MB/s | Size: {winrar_arc_size / (1024*1024):7.2f} MiB ({m_res['winrar_ratio']:5.2f}%)", flush=True)

                time.sleep(0.5)

                # 3. Cross-Verification
                print(f"  [3/4] Dual-oracle cross-verification...", flush=True)
                p_openrar_self = subprocess.run([OPENRAR, "t", "-q", ARC_OPENRAR], capture_output=True, text=True, creationflags=PRIORITY_FLAGS)
                p_unrar_openrar = subprocess.run([UNRAR, "t", "-y", ARC_OPENRAR], capture_output=True, text=True, creationflags=PRIORITY_FLAGS)
                p_openrar_winrar = subprocess.run([OPENRAR, "t", "-q", ARC_WINRAR], capture_output=True, text=True, creationflags=PRIORITY_FLAGS)
                all_ok = (p_openrar_self.returncode == 0 and p_unrar_openrar.returncode == 0 and p_openrar_winrar.returncode == 0)
                m_res["verify"] = "PASS" if all_ok else "FAIL"
                if not all_ok:
                    print(f"  -> VERIFY FAIL: openrar_self={p_openrar_self.returncode}, unrar_openrar={p_unrar_openrar.returncode}, openrar_winrar={p_openrar_winrar.returncode}")
                else:
                    print(f"  -> Dual-Oracle Integrity & Cross-Decodability: 100% PASS", flush=True)

                time.sleep(0.5)

                # 4. Decompression Throughput
                print(f"  [4/4] Decompression throughput...", flush=True)
                t0 = time.perf_counter()
                with open("nul", "wb") as devnull:
                    p = subprocess.run([OPENRAR, "p", ARC_OPENRAR], stdout=devnull, stderr=subprocess.DEVNULL, creationflags=PRIORITY_FLAGS)
                t_openrar_decomp = time.perf_counter() - t0
                openrar_decomp_speed = TOTAL_MB / t_openrar_decomp
                m_res["openrar_decomp_time"] = t_openrar_decomp
                m_res["openrar_decomp_speed"] = openrar_decomp_speed

                time.sleep(0.5)

                t0 = time.perf_counter()
                with open("nul", "wb") as devnull:
                    p = subprocess.run([WINRAR, "p", "-inul", ARC_WINRAR], stdout=devnull, stderr=subprocess.DEVNULL, creationflags=PRIORITY_FLAGS)
                t_winrar_decomp = time.perf_counter() - t0
                winrar_decomp_speed = TOTAL_MB / t_winrar_decomp
                m_res["winrar_decomp_time"] = t_winrar_decomp
                m_res["winrar_decomp_speed"] = winrar_decomp_speed
                print(f"  -> OpenRAR Decomp: {t_openrar_decomp:6.2f}s ({openrar_decomp_speed:7.1f} MB/s) | WinRAR Decomp: {t_winrar_decomp:6.2f}s ({winrar_decomp_speed:7.1f} MB/s)\n", flush=True)

                # Clean up archives immediately to conserve disk space
                if os.path.exists(ARC_OPENRAR):
                    os.remove(ARC_OPENRAR)
                if os.path.exists(ARC_WINRAR):
                    os.remove(ARC_WINRAR)

            results[m] = m_res

        # Print Final Comprehensive Summary Table
        print("=" * 125)
        print("                                       FINAL 4.0 GiB BENCHMARK RESULTS (m0 - m5)")
        print("=" * 125)
        header = f"{'Method':<8} | {'OpenRAR Comp':<13} | {'WinRAR Comp':<13} | {'Comp Spdup':<10} | {'OpenRAR Dec':<13} | {'WinRAR Dec':<13} | {'Dec Spdup':<10} | {'OpenRAR Sz':<10} | {'WinRAR Sz':<10} | {'Verify':<6}"
        print(header)
        print("-" * 125)

        for m in methods:
            r = results[m]
            m_label = f"-m{m}"
            o_comp = f"{r['openrar_comp_speed']:>6.1f} MB/s"
            w_comp = f"{r['winrar_comp_speed']:>6.1f} MB/s"
            comp_diff = (r['openrar_comp_speed'] / r['winrar_comp_speed']) * 100
            comp_spdup = f"{comp_diff:>6.1f}%"
            o_dec = f"{r['openrar_decomp_speed']:>6.1f} MB/s"
            w_dec = f"{r['winrar_decomp_speed']:>6.1f} MB/s"
            dec_diff = (r['openrar_decomp_speed'] / r['winrar_decomp_speed']) * 100
            dec_spdup = f"{dec_diff:>6.1f}%"
            o_size = f"{r['openrar_arc_size'] / (1024*1024):>7.2f} MB"
            w_size = f"{r['winrar_arc_size'] / (1024*1024):>7.2f} MB"
            v = r['verify']
            print(f"{m_label:<8} | {o_comp:<13} | {w_comp:<13} | {comp_spdup:<10} | {o_dec:<13} | {w_dec:<13} | {dec_spdup:<10} | {o_size:<10} | {w_size:<10} | {v:<6}")

        print("=" * 125)

    finally:
        print("\nCleaning up temporary benchmark files...", flush=True)
        for p in [PAYLOAD_PATH, ARC_OPENRAR, ARC_WINRAR]:
            if os.path.exists(p):
                try:
                    os.remove(p)
                    print(f"  Removed: {os.path.basename(p)}")
                except Exception as e:
                    print(f"  Failed removing {p}: {e}")
        final_free = get_free_space_gb()
        print(f"Final Free Disk Space: {final_free:.2f} GB (restored)")

if __name__ == "__main__":
    main()
