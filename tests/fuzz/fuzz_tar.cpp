// TAR fuzzer (v1.47.0 M2) — foreign TAR parser robustness, PAX, LongLink, and bounds.
// Targets TarReader: ustar header parsing, oversized/hostile PAX attributes,
// GNU LongLink / LongName prefixes, sparse member refusal, and octal integer conversions.
// Crash-, hang-, and leak-free is the contract: any ForeignStatus return is fine.

#include "../../src/archive/foreign_reader.hpp"
#include "../../src/archive/foreign_tar.hpp"
#include "../../src/archive/extraction_limits.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace openrar;
using namespace openrar::archive;
using namespace openrar::archive::foreign;

namespace {

unsigned long current_pid() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;

    static unsigned long counter = 0;
    const unsigned long id = counter++;
    fs::path dir = fs::temp_directory_path() /
                   ("openrar_fuzz_tar_" + std::to_string(current_pid()) + "_" + std::to_string(id));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return 0;

    fs::path arc_path = dir / "test.tar";
    {
        std::ofstream out(arc_path, std::ios::binary);
        if (!out) {
            fs::remove_all(dir, ec);
            return 0;
        }
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    {
        TarReader reader;
        ExtractionLimits limits;
        limits.max_member_output_bytes = 2ULL * 1024 * 1024;
        limits.max_total_output_bytes = 8ULL * 1024 * 1024;
        limits.max_header_count = 1000;
        limits.max_header_bytes = 1024 * 1024;

        ReaderHooks hooks;
        std::string detail;
        ForeignStatus st = reader.open(arc_path, limits, hooks, detail);
        if (st == ForeignStatus::Ok) {
            LimitState state;
            const size_t count = reader.entry_count();
            for (size_t i = 0; i < count; ++i) {
                const ForeignEntry& ent = reader.entry(i);
                (void)ent;
                auto sink = [](const core::byte*, size_t) -> bool {
                    return true;
                };
                (void)reader.decode(i, sink, limits, state, hooks);
            }
        }
    }

    fs::remove_all(dir, ec);
    return 0;
}

#ifndef OPENRAR_USE_LIBFUZZER
int main() {
    // Deterministic seeds:
    // 1. Two 512-byte zero blocks (end-of-archive marker in tar)
    std::vector<uint8_t> kZeroBlocks(1024, 0);

    // 2. Minimal ustar entry for "a.txt" with 5 bytes "hello"
    std::vector<uint8_t> kUstarTar(1536, 0);
    // name = "a.txt"
    std::memcpy(kUstarTar.data(), "a.txt", 5);
    // mode = 0000644
    std::memcpy(kUstarTar.data() + 100, "0000644\0", 8);
    // uid = 0001750
    std::memcpy(kUstarTar.data() + 108, "0001750\0", 8);
    // gid = 0001750
    std::memcpy(kUstarTar.data() + 116, "0001750\0", 8);
    // size = 5 bytes -> "00000000005\0"
    std::memcpy(kUstarTar.data() + 124, "00000000005 ", 12);
    // mtime = 0
    std::memcpy(kUstarTar.data() + 136, "00000000000 ", 12);
    // typeflag = '0' (regular file)
    kUstarTar[156] = '0';
    // magic = "ustar\0"
    std::memcpy(kUstarTar.data() + 257, "ustar\0", 6);
    // version = "00"
    std::memcpy(kUstarTar.data() + 263, "00", 2);
    // Calculate simple octal checksum over header block
    // chksum field at [148..156] treated as spaces (8 spaces) during calculation
    std::memset(kUstarTar.data() + 148, ' ', 8);
    uint32_t csum = 0;
    for (size_t i = 0; i < 512; ++i) csum += kUstarTar[i];
    std::snprintf(reinterpret_cast<char*>(kUstarTar.data() + 148), 8, "%06o", csum);
    // Payload "hello" at offset 512
    std::memcpy(kUstarTar.data() + 512, "hello", 5);

    // 3. Truncated record (< 512 bytes)
    static const uint8_t kTruncated[] = "ustar header partial test...";

    // 4. Garbage stream
    static const uint8_t kGarbage[] = {0x7F, 0x45, 0x4C, 0x46, 0x02, 0x01, 0x01, 0x00};

    LLVMFuzzerTestOneInput(kZeroBlocks.data(), kZeroBlocks.size());
    LLVMFuzzerTestOneInput(kUstarTar.data(), kUstarTar.size());
    LLVMFuzzerTestOneInput(kTruncated, sizeof(kTruncated));
    LLVMFuzzerTestOneInput(kGarbage, sizeof(kGarbage));

    // Prefix sweep on the ustar tar
    for (size_t step = 64; step <= kUstarTar.size(); step += 64) {
        LLVMFuzzerTestOneInput(kUstarTar.data(), step);
    }

    std::printf("[PASS] fuzz_tar standalone seed sweep completed successfully\n");
    return 0;
}
#endif // !OPENRAR_USE_LIBFUZZER
