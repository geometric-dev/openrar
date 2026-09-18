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

def main():
    print("=" * 70)
    print("        OpenRAR v1.10.0 vs WinRAR 7.20 x64: 4.0 GiB Benchmark")
    print("=" * 70)
    print(f"OpenRAR Binary: {OPENRAR}")
    print(f"WinRAR Binary : {WINRAR}")

    initial_free = get_free_space_gb()
    print(f"Initial Free Disk Space: {initial_free:.2f} GB")
    if initial_free < 5.0:
        print(f"ERROR: Insufficient free space ({initial_free:.2f} GB < 5.0 GB minimum safety floor).")
        sys.exit(1)

    TOTAL_MB = 4096
    TOTAL_BYTES = TOTAL_MB * 1024 * 1024

    try:
        # Step 1: Generate 4 GiB synthetic benchmark file
        print(f"\n[1/6] Generating 4.0 GiB ({TOTAL_MB} MiB) benchmark payload...")
        t0 = time.perf_counter()
        
        # Build 1 MiB chunk of structured, realistic prose and code patterns
        base_unit = (
            "OpenRAR high performance engine streaming compression benchmark test.\n"
            "The quick brown fox jumps over the lazy dog. 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
            "for (int i = 0; i < 1000; ++i) { sum += buffer[i] * matrix[i][j]; }\n"
            "std::vector<uint8_t> payload(65536); aes.encrypt_cbc(payload.data(), 65536, iv);\n"
        ).encode("utf-8")
        repeats = (1024 * 1024) // len(base_unit)
        chunk_1mb = (base_unit * (repeats + 1))[:1024 * 1024]
        
        with open(PAYLOAD_PATH, "wb") as f:
            for mb in range(TOTAL_MB):
                f.write(chunk_1mb)
                if (mb + 1) % 512 == 0:
                    print(f"  ... written {mb + 1} / {TOTAL_MB} MiB ({(mb + 1) / TOTAL_MB * 100:.0f}%)")

        gen_time = time.perf_counter() - t0
        actual_bytes = os.path.getsize(PAYLOAD_PATH)
        print(f"Payload created: {actual_bytes / (1024**3):.2f} GiB in {gen_time:.2f}s ({TOTAL_MB / gen_time:.1f} MB/s)")
        print(f"Free disk space with payload: {get_free_space_gb():.2f} GB")

        results = {}

        # Step 2: OpenRAR Compression Benchmark (-m3 normal)
        print("\n[2/6] Running OpenRAR -m3 compression on 4.0 GiB file...")
        if os.path.exists(ARC_OPENRAR):
            os.remove(ARC_OPENRAR)
        t0 = time.perf_counter()
        p = subprocess.run([OPENRAR, "a", "-m3", "-q", ARC_OPENRAR, PAYLOAD_PATH], capture_output=True, text=True)
        t_openrar_comp = time.perf_counter() - t0
        if p.returncode != 0:
            print(f"OpenRAR FAILED (code {p.returncode}): {p.stderr}")
            sys.exit(1)
        openrar_arc_size = os.path.getsize(ARC_OPENRAR)
        openrar_mb_s = TOTAL_MB / t_openrar_comp
        print(f"  -> OpenRAR -m3 Time : {t_openrar_comp:.2f}s ({openrar_mb_s:.1f} MB/s)")
        print(f"  -> Archive Size     : {openrar_arc_size / (1024*1024):.2f} MiB (ratio: {openrar_arc_size / actual_bytes * 100:.2f}%)")
        results["openrar_comp_time"] = t_openrar_comp
        results["openrar_comp_speed"] = openrar_mb_s
        results["openrar_arc_size"] = openrar_arc_size

        # Step 3: WinRAR Compression Benchmark (-m3 normal)
        print("\n[3/6] Running WinRAR -m3 compression on 4.0 GiB file...")
        if os.path.exists(ARC_WINRAR):
            os.remove(ARC_WINRAR)
        t0 = time.perf_counter()
        p = subprocess.run([WINRAR, "a", "-m3", "-inul", ARC_WINRAR, PAYLOAD_PATH], capture_output=True, text=True)
        t_winrar_comp = time.perf_counter() - t0
        if p.returncode != 0:
            print(f"WinRAR FAILED (code {p.returncode}): {p.stderr}")
            sys.exit(1)
        winrar_arc_size = os.path.getsize(ARC_WINRAR)
        winrar_mb_s = TOTAL_MB / t_winrar_comp
        print(f"  -> WinRAR -m3 Time  : {t_winrar_comp:.2f}s ({winrar_mb_s:.1f} MB/s)")
        print(f"  -> Archive Size     : {winrar_arc_size / (1024*1024):.2f} MiB (ratio: {winrar_arc_size / actual_bytes * 100:.2f}%)")
        results["winrar_comp_time"] = t_winrar_comp
        results["winrar_comp_speed"] = winrar_mb_s
        results["winrar_arc_size"] = winrar_arc_size

        # Step 4: Cross-Verification & Self-Testing
        print("\n[4/6] Verifying archive decodability & cross-compatibility...")
        # OpenRAR verifies its own archive
        p = subprocess.run([OPENRAR, "t", "-q", ARC_OPENRAR], capture_output=True, text=True)
        print(f"  -> OpenRAR self-test     : {'PASS' if p.returncode == 0 else 'FAIL'}")
        assert p.returncode == 0
        # Reference UnRAR verifies OpenRAR's archive
        p = subprocess.run([UNRAR, "t", "-y", ARC_OPENRAR], capture_output=True, text=True)
        print(f"  -> UnRAR verifies OpenRAR: {'PASS' if p.returncode == 0 else 'FAIL'}")
        assert p.returncode == 0
        # OpenRAR verifies WinRAR's archive
        p = subprocess.run([OPENRAR, "t", "-q", ARC_WINRAR], capture_output=True, text=True)
        print(f"  -> OpenRAR verifies WinRAR: {'PASS' if p.returncode == 0 else 'FAIL'}")
        assert p.returncode == 0

        # Step 5: Decompression Speed Benchmark (stdout to NUL)
        print("\n[5/6] Benchmarking pure decompression throughput (streaming 4.0 GiB to NUL)...")
        # OpenRAR decompression via 'p' command
        t0 = time.perf_counter()
        with open("nul", "wb") as devnull:
            p = subprocess.run([OPENRAR, "p", ARC_OPENRAR], stdout=devnull, stderr=subprocess.PIPE)
        t_openrar_decomp = time.perf_counter() - t0
        openrar_decomp_speed = TOTAL_MB / t_openrar_decomp
        print(f"  -> OpenRAR Decompress Time: {t_openrar_decomp:.2f}s ({openrar_decomp_speed:.1f} MB/s)")
        results["openrar_decomp_time"] = t_openrar_decomp
        results["openrar_decomp_speed"] = openrar_decomp_speed

        # WinRAR decompression via 'p' command
        t0 = time.perf_counter()
        with open("nul", "wb") as devnull:
            p = subprocess.run([WINRAR, "p", "-inul", ARC_WINRAR], stdout=devnull, stderr=subprocess.PIPE)
        t_winrar_decomp = time.perf_counter() - t0
        winrar_decomp_speed = TOTAL_MB / t_winrar_decomp
        print(f"  -> WinRAR Decompress Time : {t_winrar_decomp:.2f}s ({winrar_decomp_speed:.1f} MB/s)")
        results["winrar_decomp_time"] = t_winrar_decomp
        results["winrar_decomp_speed"] = winrar_decomp_speed

        # Step 6: Stored Mode (-m0) Streaming Throughput Benchmark
        print("\n[6/6] Benchmarking uncompressed streaming (-m0 store) on 4.0 GiB file...")
        arc_store_openrar = "bench_openrar_m0.rar"
        arc_store_winrar = "bench_winrar_m0.rar"
        try:
            t0 = time.perf_counter()
            subprocess.run([OPENRAR, "a", "-m0", "-q", arc_store_openrar, PAYLOAD_PATH], check=True)
            t_openrar_m0 = time.perf_counter() - t0
            openrar_m0_speed = TOTAL_MB / t_openrar_m0
            print(f"  -> OpenRAR -m0 Time       : {t_openrar_m0:.2f}s ({openrar_m0_speed:.1f} MB/s)")
            results["openrar_m0_speed"] = openrar_m0_speed
            results["openrar_m0_time"] = t_openrar_m0
        finally:
            if os.path.exists(arc_store_openrar):
                os.remove(arc_store_openrar)

        try:
            t0 = time.perf_counter()
            subprocess.run([WINRAR, "a", "-m0", "-inul", arc_store_winrar, PAYLOAD_PATH], check=True)
            t_winrar_m0 = time.perf_counter() - t0
            winrar_m0_speed = TOTAL_MB / t_winrar_m0
            print(f"  -> WinRAR -m0 Time        : {t_winrar_m0:.2f}s ({winrar_m0_speed:.1f} MB/s)")
            results["winrar_m0_speed"] = winrar_m0_speed
            results["winrar_m0_time"] = t_winrar_m0
        finally:
            if os.path.exists(arc_store_winrar):
                os.remove(arc_store_winrar)

        # Print Final Summary Table
        print("\n" + "=" * 70)
        print("                  FINAL 4.0 GiB BENCHMARK RESULTS")
        print("=" * 70)
        print(f"{'Metric':<30} | {'OpenRAR v1.10.0':<16} | {'WinRAR 7.20 x64':<16} | {'Comparison':<12}")
        print("-" * 70)
        
        comp_ratio = f"{results['openrar_comp_speed'] / results['winrar_comp_speed'] * 100:.1f}%"
        print(f"{'Compression (-m3) Throughput':<30} | {results['openrar_comp_speed']:>10.1f} MB/s   | {results['winrar_comp_speed']:>10.1f} MB/s   | {comp_ratio:<12}")
        
        decomp_ratio = f"{results['openrar_decomp_speed'] / results['winrar_decomp_speed'] * 100:.1f}%"
        print(f"{'Decompression Throughput':<30} | {results['openrar_decomp_speed']:>10.1f} MB/s   | {results['winrar_decomp_speed']:>10.1f} MB/s   | {decomp_ratio:<12}")
        
        m0_ratio = f"{results['openrar_m0_speed'] / results['winrar_m0_speed'] * 100:.1f}%"
        print(f"{'Store (-m0) Streaming':<30} | {results['openrar_m0_speed']:>10.1f} MB/s   | {results['winrar_m0_speed']:>10.1f} MB/s   | {m0_ratio:<12}")
        
        print(f"{'Archive Size':<30} | {results['openrar_arc_size'] / 1024 / 1024:>10.2f} MiB  | {results['winrar_arc_size'] / 1024 / 1024:>10.2f} MiB  | {'Matched' if abs(results['openrar_arc_size'] - results['winrar_arc_size']) < 1024*1024 else 'Diff':<12}")
        print(f"{'Cross-Decodability':<30} | {'100% OK':<16} | {'100% OK':<16} | {'Identical':<12}")
        print("=" * 70)

    finally:
        print("\nCleaning up temporary benchmark files...")
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
