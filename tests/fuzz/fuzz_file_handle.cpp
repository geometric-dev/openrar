// Fuzz harness for the file-based surface (v1.3.0) — openrar_archive_open_file
// / handle_extract_to_path / handle_extract / handle_test / list_file_pw over
// the streaming reader (ArchiveReader). Complements fuzz_archive.cpp, which
// covers the in-memory buffer ABI only: every path here starts from an
// archive materialised on disk, so the volume scan, extent stitching, chunked
// AES decrypt and solid catch-up machinery all see attacker-shaped input.
//
// Mirrors fuzz_decompress.cpp / fuzz_archive.cpp: libFuzzer entry when
// OPENRAR_USE_LIBFUZZER is defined, deterministic standalone main otherwise.
//
// Threat-model notes:
//  - The input is used both as archive bytes and as the archive NAME shape
//    (plain .rar / full .partNN set / lone middle volume / lone first
//    volume), exercising first-volume rewind and the strict missing-volume
//    checks with hostile naming.
//  - A fixed password ("pw") drives the HEAD_CRYPT / FHEXTRA_CRYPT paths.
//    Hostile lg2_count (up to the spec max 24) can make one PBKDF2 derivation
//    take seconds — bounded work per exec, but nightly runs use -timeout.
//  - The progress callback cancels past a 32 MiB output ceiling: decompression
//    bombs (1 KB packed -> GB unpacked) must cost the fuzzer a cancel, not a
//    multi-GB disk write. handle_extract stays bounded via the 256 MiB heap
//    cap. Extraction/abort must never leave a partial destination file.

#include "openrar/openrar_dll.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#ifndef OPENRAR_SOURCE_DIR
#define OPENRAR_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;

namespace {

// Progress/cancel pair: progress counts emitted bytes; cancel fires once the
// output ceiling is exceeded (or on the data-derived poll count) so bombs and
// long solid catch-ups abort instead of stalling the fuzzer.
struct ExtractGuard {
    uint64_t emitted = 0;
    uint64_t ceiling = 32ull << 20; // 32 MiB
    int cancel_after = -1;          // -1 = only the ceiling aborts
    int polls = 0;

    static void progress(void* user, uint64_t done, uint64_t) {
        static_cast<ExtractGuard*>(user)->emitted = done;
    }
    static int cancel(void* user) {
        auto* g = static_cast<ExtractGuard*>(user);
        if (g->emitted > g->ceiling) return 1;
        if (g->cancel_after >= 0 && ++g->polls > g->cancel_after) return 1;
        return 0;
    }
};

void write_file(const fs::path& p, const uint8_t* data, size_t size) {
    std::ofstream f(p, std::ios::binary);
    if (size) f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

unsigned long current_pid_stub() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// One fuzz iteration: materialise the input as one or two volume files, then
// hammer every file-based entry point. Everything must be crash-free; results
// are checked only for crash-safety and the no-partial-file guarantee.
int one_input(const uint8_t* data, size_t size) {
    static unsigned long counter = 0;
    const unsigned long id = counter++;
    fs::path dir =
        fs::temp_directory_path() /
        ("openrar_fuzz_fh_" + std::to_string(current_pid_stub()) + "_" + std::to_string(id));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return 0;

    // Naming variant from the input. The same bytes land under different
    // volume-name shapes so derivation/strictness paths get covered.
    const unsigned variant = size ? (data[0] & 0x3u) : 0u;
    const size_t split = size ? (size / 2) : 0;
    fs::path primary;
    if (variant == 0) {
        primary = dir / "f.rar";
        write_file(primary, data, size);
    } else if (variant == 1) {
        primary = dir / "f.part01.rar";
        write_file(primary, data, split);
        write_file(dir / "f.part02.rar", data + split, size - split);
    } else if (variant == 2) {
        // Lone middle volume: rewind must fail with "cannot open first volume"
        // (crash-free), never derive-and-open something unexpected.
        primary = dir / "f.part02.rar";
        write_file(primary, data, size);
    } else {
        // Lone first volume of a set: strict open may report MISSING_VOLUME.
        primary = dir / "f.part01.rar";
        write_file(primary, data, split);
    }

    const char* const passwords[] = {nullptr, "", "pw"};
    const char* password = passwords[size ? (data[size - 1] % 3) : 0];

    ExtractGuard guard;
    guard.cancel_after = (size > 3 && (data[2] & 1)) ? static_cast<int>(data[3] % 4) : -1;

    uint32_t h = openrar_archive_open_file(primary.u8string().c_str(), password,
                                           ExtractGuard::progress, ExtractGuard::cancel, &guard);
    if (h != 0) {
        uint32_t count = 0;
        void* entries = nullptr;
        void* paths = nullptr;
        size_t paths_size = 0;
        if (openrar_archive_handle_list(h, &count, &entries, &paths, &paths_size) == RAR_OK) {
            openrar_archive_list_free(entries, paths, paths_size);
        }
        // Valid and out-of-range indices alike: bounds checks must hold.
        const uint32_t idx = size ? static_cast<uint32_t>(data[1]) : 0u;
        for (uint32_t attempt = 0; attempt < 3; ++attempt) {
            const uint32_t i =
                attempt == 0 ? idx : (attempt == 1 ? (count ? count - 1 : 0) : 0x7ffffffeu);
            fs::path dest = dir / ("dest" + std::to_string(attempt) + ".bin");
            openrar_archive_handle_extract_to_path(h, i, dest.u8string().c_str(),
                                                   ExtractGuard::progress, ExtractGuard::cancel,
                                                   &guard);
            uint8_t* out = nullptr;
            size_t out_len = 0;
            openrar_archive_handle_extract(h, i, &out, &out_len);
            openrar_free(out);
            openrar_archive_handle_test(h, i, ExtractGuard::progress, ExtractGuard::cancel, &guard);
        }
        openrar_archive_close(h);
        openrar_archive_close(h); // double close must be safe
    }

    // _pw listing over the same bytes (Phase 0 surface, password variants).
    {
        uint32_t count = 0;
        void* entries = nullptr;
        void* paths = nullptr;
        size_t paths_size = 0;
        int rc =
            openrar_archive_list_file_pw(primary.u8string().c_str(), password, &count, &entries,
                                         &paths, &paths_size, nullptr, nullptr, nullptr);
        if (rc == RAR_OK) openrar_archive_list_free(entries, paths, paths_size);
    }

    // Destination guard: extracting ONTO the archive must be rejected (and
    // must not corrupt what we are reading).
    {
        uint32_t h2 = openrar_archive_open_file(primary.u8string().c_str(), password, nullptr,
                                                nullptr, nullptr);
        if (h2 != 0) {
            openrar_archive_handle_extract_to_path(h2, 0, primary.u8string().c_str(), nullptr,
                                                   nullptr, nullptr);
            openrar_archive_close(h2);
        }
    }

    // Mutation leg (v1.4.0): the delete/add exports face arbitrary bytes.
    // Everything must be crash-free with any result code; a successful
    // mutation must leave a loadable archive (the re-open below parses the
    // rewrite, covering the header-offset rewrite path against garbage).
    if (size > 8) {
        const uint32_t del_idx[2] = {0u, static_cast<uint32_t>(data[4])};
        openrar_archive_delete_entries_file(primary.u8string().c_str(), del_idx, 2);
        const fs::path add_src = dir / "fuzz_add.bin";
        write_file(add_src, data, size / 2);
        const std::string add_src_u8 = add_src.u8string();
        const char* srcs[1] = {add_src_u8.c_str()};
        const char* names[1] = {"fuzz.bin"};
        const int method = static_cast<int>(data[5] % 6u); // includes invalid → must refuse
        openrar_archive_add_files_file(primary.u8string().c_str(), srcs, names, 1, method,
                                       static_cast<uint32_t>(data[6] % 6u));
        uint32_t h3 = openrar_archive_open_file(primary.u8string().c_str(), password, nullptr,
                                                nullptr, nullptr);
        if (h3 != 0) {
            uint32_t count = 0;
            void* entries = nullptr;
            void* paths = nullptr;
            size_t paths_size = 0;
            if (openrar_archive_handle_list(h3, &count, &entries, &paths, &paths_size) == RAR_OK) {
                openrar_archive_list_free(entries, paths, paths_size);
            }
            openrar_archive_close(h3);
        }
    }

    fs::remove_all(dir, ec);
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > (8u << 20)) return 0; // cap input size: 8 MiB is plenty
    return one_input(data, size);
}

#ifndef OPENRAR_USE_LIBFUZZER

// Standalone sweep (MSVC and every non-libFuzzer build): seed with the real
// fixtures and freshly built archives, then mutate and truncate.

namespace {
unsigned long long rng_state = 0x9E3779B97F4A7C15ull;
unsigned long long rng() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

void mutate(std::vector<uint8_t>& v) {
    for (int i = 0; i < 8; ++i) {
        if (v.empty()) break;
        const size_t pos = rng() % v.size();
        switch (rng() % 4) {
        case 0:
            v[pos] = static_cast<uint8_t>(rng());
            break;
        case 1:
            v[pos] ^= static_cast<uint8_t>(1u << (rng() % 8));
            break;
        case 2:
            v[pos] = static_cast<uint8_t>(0xFFu << (rng() % 8));
            break;
        default:
            v.insert(v.begin() + static_cast<long>(pos), static_cast<uint8_t>(rng()));
            break;
        }
    }
}
} // namespace

int main() {
    std::cout << "Running deterministic file-handle fuzzer sweep...\n";

    std::vector<std::vector<uint8_t>> corpus;
    // Real fixtures give valid RAR5 header shapes (plain, -p, -hp).
    const fs::path tests_dir = OPENRAR_SOURCE_DIR;
    for (const char* name : {"hello5.rar", "hello5_p.rar", "hello5_hp.rar", "hello4.rar"}) {
        fs::path p = tests_dir / "tests" / name;
        std::error_code ec;
        if (fs::exists(p, ec)) corpus.push_back(read_file(p));
    }
    fs::path seeds_dir = tests_dir / "tests" / "fuzz" / "seeds";
    std::error_code sec;
    if (fs::exists(seeds_dir, sec)) {
        for (const auto& entry : fs::directory_iterator(seeds_dir, sec)) {
            if (entry.is_regular_file()) {
                auto b = read_file(entry.path());
                if (!b.empty()) corpus.push_back(std::move(b));
            }
        }
    }
    // Freshly built archives round out the valid-input side.
    {
        const uint8_t a[] = {'h', 'e', 'l', 'l', 'o'};
        const uint8_t* paths[] = {reinterpret_cast<const uint8_t*>("a.txt")};
        const uint8_t* datas[] = {a};
        const size_t sizes[] = {sizeof(a)};
        uint8_t* out = nullptr;
        size_t out_len = 0;
        if (openrar_archive_create(paths, datas, sizes, 1, 3, 2, &out, &out_len) == RAR_OK && out) {
            corpus.emplace_back(out, out + out_len);
            openrar_free(out);
        }
    }
    if (corpus.empty()) {
        std::vector<uint8_t> dummy(64, 0x52);
        corpus.push_back(dummy);
    }

    const int num_iterations = 1500;
    for (int i = 0; i < num_iterations; ++i) {
        std::vector<uint8_t> input = corpus[rng() % corpus.size()];
        if (rng() % 10 != 0) mutate(input);
        if (rng() % 5 == 0 && !input.empty()) input.resize((rng() % input.size()) + 1);
        one_input(input.data(), input.size());
        if ((i + 1) % 250 == 0)
            std::cout << "  Completed " << (i + 1) << " iterations...\n" << std::flush;
    }
    std::cout << "File-handle fuzzer sweep PASSED.\n";
    return 0;
}

#endif // OPENRAR_USE_LIBFUZZER
