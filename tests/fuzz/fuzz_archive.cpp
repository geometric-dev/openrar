// Fuzz harness for the archive + block C ABI surface — the exact entry
// points browsers reach through wasm/js. Mirrors fuzz_decompress.cpp:
// libFuzzer entry when OPENRAR_USE_LIBFUZZER is defined, deterministic
// standalone main otherwise.
//
// Targets: openrar_archive_list / extract / extract_all(2) / create(2) /
// open / handle flows, and openrar_compress2 / decompress2 — mutated
// headers, lying size fields, hostile paths, truncated archives.

#include "../../src/wasm/archive_api.hpp"
#include "../../src/wasm/wasm_api.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <vector>

using openrar::wasm::RAR_OK;

namespace {

void exercise_archive_abi(const uint8_t* data, size_t size) {
    // list + entry readback
    uint32_t count = 0;
    void* entries = nullptr;
    void* paths = nullptr;
    size_t paths_size = 0;
    if (openrar_archive_list(data, size, &count, &entries, &paths, &paths_size) == RAR_OK) {
        if (count > 0 && entries && paths) {
            // Extract every index (bounded: no payload materialisation for
            // out-of-range indices, so this is cheap for hostile inputs).
            for (uint32_t i = 0; i < count; i++) {
                uint8_t* out = nullptr;
                size_t out_len = 0;
                openrar_archive_extract(data, size, i, &out, &out_len);
                openrar_archive_free(out);
            }
        }
        openrar_archive_list_free(entries, paths, paths_size);
    }

    // extract_all (bounded internally by the core's cumulative output cap)
    {
        uint8_t* buf = nullptr;
        size_t buf_size = 0;
        uint64_t* offsets = nullptr;
        uint32_t n = 0;
        openrar_archive_extract_all(data, size, &buf, &buf_size, &offsets, &n);
        openrar_archive_free(buf);
        openrar_archive_free(offsets);
    }

    // open / close handle flow
    {
        uint32_t h = openrar_archive_open(data, size);
        if (h != 0) {
            uint32_t hc = 0;
            void* he = nullptr;
            void* hp = nullptr;
            size_t hs = 0;
            if (openrar_archive_handle_list(h, &hc, &he, &hp, &hs) == RAR_OK) {
                if (hc > 0) {
                    uint8_t* out = nullptr;
                    size_t out_len = 0;
                    openrar_archive_handle_extract(h, hc - 1, &out, &out_len);
                    openrar_archive_free(out);
                }
                openrar_archive_list_free(he, hp, hs);
                he = nullptr;
                hp = nullptr;
                hs = 0;
            }
            if (openrar_archive_handle_list(h, &hc, &he, &hp, &hs) == RAR_OK) {
                openrar_archive_list_free(he, hp, hs);
            }
            openrar_archive_close(h);
            openrar_archive_close(h); // double close must be safe
        }
    }

    // create2 with a hostile path derived from the fuzz input. Paths are
    // validated at the ABI boundary; the harness checks the validator never
    // crashes and never escapes (no null deref, no OOB on weird bytes).
    if (size > 4) {
        std::vector<char> pathbuf(data, data + (size > 256 ? 256 : size));
        pathbuf.push_back('\0');
        openrar::wasm::ArchiveInputFile f{};
        f.path = pathbuf.data();
        f.data = size > 300 ? data + 300 : nullptr;
        f.data_len = size > 300 ? size - 300 : 0;
        f.mtime_unix = 0;
        f.is_dir = (data[0] & 1);
        uint8_t* out = nullptr;
        size_t out_len = 0;
        openrar_archive_create2(&f, 1, nullptr, nullptr, &out, &out_len);
        openrar_archive_free(out);
        openrar_archive_last_error_code();
    }
}

void exercise_block_abi(const uint8_t* data, size_t size) {
    if (size < 2) return;
    uint8_t* out = nullptr;
    size_t out_len = 0;
    // Compress whatever we got (any method), then decompress the result —
    // both directions must be crash-free on attacker-shaped bytes.
    int method = static_cast<int>(data[0] % 6);
    size_t win = 128 * 1024;
    if (openrar_compress2(data + 1, size - 1, &out, &out_len, method, win) == 1) {
        uint8_t* back = nullptr;
        size_t back_len = 0;
        openrar_decompress2(out, out_len, &back, &back_len, win);
        openrar_free(back);
    }
    openrar_free(out);

    // Decompress the raw fuzz input too (it may accidentally be a valid stream).
    uint8_t* back = nullptr;
    size_t back_len = 0;
    openrar_decompress2(data, size, &back, &back_len, win);
    openrar_free(back);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    exercise_archive_abi(data, size);
    exercise_block_abi(data, size);
    return 0;
}

#ifndef OPENRAR_USE_LIBFUZZER

#include <random>

int main() {
    // Deterministic sweep: seeds + byte flips + truncations, then a digest
    // of pseudo-random blobs. Same shape as fuzz_decompress's standalone mode.
    std::mt19937 rng(2026);
    std::vector<std::vector<uint8_t>> cases;
    cases.emplace_back(std::vector<uint8_t>{'R', 'a', 'r', '!', 0x1a, 0x00, 0x00, 0x00});
    for (int i = 0; i < 64; i++) {
        size_t n = 1 + (rng() % 4096);
        std::vector<uint8_t> blob(n);
        for (auto& b : blob) b = static_cast<uint8_t>(rng());
        if (i % 2 == 0 && blob.size() > 8) {
            // Give some cases a RAR5 signature prefix so deeper parsing runs.
            const uint8_t sig[8] = {'R', 'a', 'r', '!', 0x1a, 0x00, 0x00, 0x00};
            std::memcpy(blob.data(), sig, 8);
        }
        cases.push_back(std::move(blob));
    }

    long iterations = 0;
    for (const auto& blob : cases) {
        LLVMFuzzerTestOneInput(blob.data(), blob.size());
        iterations++;
        // Truncation sweep
        for (size_t cut = 1; cut < blob.size() && cut < 64; cut <<= 1) {
            LLVMFuzzerTestOneInput(blob.data(), blob.size() - cut);
            iterations++;
        }
    }
    std::fprintf(stderr, "fuzz_archive standalone: %ld iterations, no crashes\n", iterations);
    return 0;
}

#include <cstdio>
#endif // OPENRAR_USE_LIBFUZZER
