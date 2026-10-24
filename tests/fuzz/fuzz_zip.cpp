// ZIP fuzzer (v1.47.0 M1) — foreign ZIP parser robustness and CD-vs-LFH oracle.
// Targets ZipReader: EOCD scanning, central directory parsing, ZIP64 extensions,
// local file header pre-flight consistency checks, and DEFLATE/stored payload decoding.
// Crash-, hang-, and leak-free is the contract: any ForeignStatus return is fine.

#include "../../src/archive/foreign_reader.hpp"
#include "../../src/archive/foreign_zip.hpp"
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
                   ("openrar_fuzz_zip_" + std::to_string(current_pid()) + "_" + std::to_string(id));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return 0;

    fs::path arc_path = dir / "test.zip";
    {
        std::ofstream out(arc_path, std::ios::binary);
        if (!out) {
            fs::remove_all(dir, ec);
            return 0;
        }
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    {
        ZipReader reader;
        ExtractionLimits limits;
        // Cap uncompressed outputs and headers to prevent zip bombs from exhausting memory
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
    // 1. Minimal empty ZIP (22-byte EOCD)
    static const uint8_t kEmptyZip[] = {0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    // 2. Single-file stored ZIP ("a.txt" -> "hello")
    static const uint8_t kStoredZip[] = {
        0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x36, 0x5e, 0x0b, 0x36, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00,
        0x00, 0x00, 0x61, 0x2e, 0x74, 0x78, 0x74, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x50, 0x4b,
        0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x36, 0x5e, 0x0b, 0x36, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x61, 0x2e, 0x74, 0x78, 0x74, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x33, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x00};

    // 3. Truncated header
    static const uint8_t kTruncated[] = {0x50, 0x4b, 0x03, 0x04};

    // 4. Random garbage
    static const uint8_t kGarbage[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD};

    LLVMFuzzerTestOneInput(kEmptyZip, sizeof(kEmptyZip));
    LLVMFuzzerTestOneInput(kStoredZip, sizeof(kStoredZip));
    LLVMFuzzerTestOneInput(kTruncated, sizeof(kTruncated));
    LLVMFuzzerTestOneInput(kGarbage, sizeof(kGarbage));

    // Prefix sweep on the stored zip
    for (size_t i = 1; i < sizeof(kStoredZip); ++i) {
        LLVMFuzzerTestOneInput(kStoredZip, i);
    }

    std::printf("[PASS] fuzz_zip standalone seed sweep completed successfully\n");
    return 0;
}
#endif // !OPENRAR_USE_LIBFUZZER
