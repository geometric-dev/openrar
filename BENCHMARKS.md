# OpenRAR RARMark v2.0 Hardware Benchmarks

Automated multi-platform performance audit and community hardware leaderboard for **OpenRAR 2.0**.

The built-in benchmark (`openrar b`) exercises the complete in-memory engine:
1. **Multi-Threaded Memory Bandwidth:** Measures host DRAM/cache streaming copy bandwidth in GB/s across active threads.
2. **Single-Thread Compression:** Benchmarks core LZ/Huffman algorithm throughput without thread overhead.
3. **Multi-Thread Compression:** Measures parallel solid-block chunking and thread-scaling efficiency across all CPU cores.
4. **Parallel Decompression:** Stresses multi-threaded decompression, CRC32 calculation, and memory write-back (saturating up to 1.0+ GB/s).
5. **Bit-Exact Integrity Gate:** Verifies that 100% of decompressed bytes match the original corpus CRC32 (`decompression_verified: true`).

$$\text{RARMark Score} = \frac{0.6 \times \text{MT Compression (KB/s)} + 0.4 \times \text{MT Decompression (KB/s)}}{10}$$

---

## Overall RARMark Leaderboard

| Rank | Processor / System | Arch | Threads | Vector SIMD | Memory Bandwidth | Single-Thread | Multi-Thread | Parallel Decompress | RARMark Score |
|:---:|---|:---:|:---:|---|:---:|:---:|:---:|:---:|:---:|
| 1 | **Apple Silicon (M-Series ARM64)**<br>_GitHub Actions macOS Runner (macOS 15 Sequoia)_ | ARM64 | 3T | `NEON LZ, NEON BLAKE2sp, AES, SHA, CRC` | 68.4 GB/s | 25.7 MB/s | 69.5 MB/s (2.70x) | 1001.4 MB/s | **45,290** |
| 2 | **Apple Silicon (M-Series ARM64)**<br>_GitHub Actions macOS Runner (macOS 15 Sequoia)_ | ARM64 | 3T | `NEON LZ, NEON BLAKE2sp, AES, SHA, CRC` | 68.4 GB/s | 25.7 MB/s | 69.5 MB/s (2.70x) | 1001.4 MB/s | **45,290** |
| 3 | **AMD EPYC 7763 64-Core Processor**<br>_GitHub Actions Windows Runner (Windows Server 2022)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, SHA-NI, PCLMUL CRC` | 29.8 GB/s | 18.1 MB/s | 42.3 MB/s (2.34x) | 685.5 MB/s | **30,680** |
| 4 | **AMD EPYC 7763 64-Core Processor**<br>_GitHub Actions Windows Runner (Windows Server 2022)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, SHA-NI, PCLMUL CRC` | 29.8 GB/s | 18.1 MB/s | 42.3 MB/s (2.34x) | 685.5 MB/s | **30,680** |
| 5 | **AMD EPYC 7763 64-Core Processor**<br>_GitHub Actions Ubuntu Runner (Ubuntu 24.04 LTS, GCC)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, SHA-NI, PCLMUL CRC` | 28.6 GB/s | 16.0 MB/s | 32.8 MB/s (2.05x) | 566.0 MB/s | **25,196** |
| 6 | **Intel(R) Core(TM) i7-7500U CPU @ 2.70GHz**<br>_Workstation (Windows 11 x64)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, PCLMUL CRC` | 5.7 GB/s | 10.7 MB/s | 20.7 MB/s (1.94x) | 351.1 MB/s | **15,652** |
| 7 | **Intel(R) Core(TM) i7-7500U CPU @ 2.70GHz**<br>_Dell XPS 13 (i7-7500U)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, PCLMUL CRC` | 3.0 GB/s | 6.6 MB/s | 14.6 MB/s (2.21x) | 318.1 MB/s | **13,930** |

---

## Official CI Hardware Baselines

Standardized GitHub Actions runner environments measured during CI builds:

| Rank | Processor / System | Arch | Threads | Vector SIMD | Memory Bandwidth | Single-Thread | Multi-Thread | Parallel Decompress | RARMark Score |
|:---:|---|:---:|:---:|---|:---:|:---:|:---:|:---:|:---:|
| 1 | **Apple Silicon (M-Series ARM64)**<br>_GitHub Actions macOS Runner (macOS 15 Sequoia)_ | ARM64 | 3T | `NEON LZ, NEON BLAKE2sp, AES, SHA, CRC` | 68.4 GB/s | 25.7 MB/s | 69.5 MB/s (2.70x) | 1001.4 MB/s | **45,290** |
| 2 | **AMD EPYC 7763 64-Core Processor**<br>_GitHub Actions Windows Runner (Windows Server 2022)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, SHA-NI, PCLMUL CRC` | 29.8 GB/s | 18.1 MB/s | 42.3 MB/s (2.34x) | 685.5 MB/s | **30,680** |
| 3 | **AMD EPYC 7763 64-Core Processor**<br>_GitHub Actions Ubuntu Runner (Ubuntu 24.04 LTS, GCC)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, SHA-NI, PCLMUL CRC` | 28.6 GB/s | 16.0 MB/s | 32.8 MB/s (2.05x) | 566.0 MB/s | **25,196** |

---

## Community Hardware Submissions

Benchmarks submitted by community members across diverse architectures (Ryzen, Core Ultra, Apple Silicon, ARM64 servers, Raspberry Pi):

| Rank | Processor / System | Arch | Threads | Vector SIMD | Memory Bandwidth | Single-Thread | Multi-Thread | Parallel Decompress | RARMark Score |
|:---:|---|:---:|:---:|---|:---:|:---:|:---:|:---:|:---:|
| 1 | **Apple Silicon (M-Series ARM64)**<br>_GitHub Actions macOS Runner (macOS 15 Sequoia)_ | ARM64 | 3T | `NEON LZ, NEON BLAKE2sp, AES, SHA, CRC` | 68.4 GB/s | 25.7 MB/s | 69.5 MB/s (2.70x) | 1001.4 MB/s | **45,290** |
| 2 | **AMD EPYC 7763 64-Core Processor**<br>_GitHub Actions Windows Runner (Windows Server 2022)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, SHA-NI, PCLMUL CRC` | 29.8 GB/s | 18.1 MB/s | 42.3 MB/s (2.34x) | 685.5 MB/s | **30,680** |
| 3 | **Intel(R) Core(TM) i7-7500U CPU @ 2.70GHz**<br>_Workstation (Windows 11 x64)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, PCLMUL CRC` | 5.7 GB/s | 10.7 MB/s | 20.7 MB/s (1.94x) | 351.1 MB/s | **15,652** |
| 4 | **Intel(R) Core(TM) i7-7500U CPU @ 2.70GHz**<br>_Dell XPS 13 (i7-7500U)_ | x86_64 | 4T | `AVX2 LZ, AVX2 BLAKE2sp, AES-NI, PCLMUL CRC` | 3.0 GB/s | 6.6 MB/s | 14.6 MB/s (2.21x) | 318.1 MB/s | **13,930** |

---

## How to Run & Submit Your Benchmark

You can benchmark your machine directly using the OpenRAR CLI:

### 1. Run the Benchmark
```bash
# Standard benchmark (all logical cores, 5s per phase)
openrar b

# Quick benchmark with plain log output
openrar b --seconds=1 -plain

# Emit RFC 8259 structured JSON with hardware metrics
openrar b --json > my_hardware.json
```

### 2. Submit Your Results
There are three ways to contribute your benchmark to this leaderboard:

- **Option A (Instant Cloud Ingestion - Recommended):** Pipe directly to our Cloudflare Workers edge ingestion API:
  ```bash
  openrar b --submit
  # Or with custom endpoint / piping:
  openrar b --json | curl -s -X POST -H "Content-Type: application/json" -d @- https://benchmark.openrar.workers.dev/api/submit
  ```
  Submissions are verified cryptographically in real-time, saved to Cloudflare D1, and automatically synced into this table. View live stats at [Cloud Leaderboard](https://benchmark.openrar.workers.dev/) or the [openrar-benchmark-submission](https://github.com/geometric-dev/openrar-benchmark-submission) service.

- **Option B (GitHub Issue Form):** Open an issue using the [Benchmark Submission Template](https://github.com/geometric-dev/openrar/issues/new?template=benchmark_submission.yml) and paste your JSON output.

- **Option C (Pull Request):** Place `my_hardware.json` in `benchmarks/submissions/<your-cpu-name>.json` and open a PR. The aggregation workflow will automatically validate your results and update the leaderboard!
