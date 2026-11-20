// SEC-INTEROP-UNDEF-FILTER (docs/experimentation-roadmap.md §4.21).
//
// The RAR5 filter `Type` field is 3 bits. Types 0..3 are defined (DELTA, E8,
// E8E9, ARM); 4..7 are reserved. The two decoders disagree about a reserved
// type, and this test pins both halves of that disagreement so neither can
// drift silently:
//
//   OpenRAR (decompressor50.cpp:853-882): an unknown type falls through the
//     if/else chain and the RAW region is emitted. The member survives.
//
//   UnRAR 7.23 (unpack50.cpp:422-501): ApplyFilter has no case for 4..7 and
//     returns NULL; the caller writes only non-NULL, so the region is DROPPED.
//
// OpenRAR's behaviour is the safer one (it preserves bytes), and this project
// must never EMIT such a token: its own decoder would read it back correctly,
// but every reference decoder would drop the region and the unpack would fail.
//
// Three properties, in order of importance:
//
//   1. The encoder can never emit type > 3. FilterType only declares 0..3, so
//      this is structural - but put_bits(ft.type, 3) would silently truncate a
//      future 8+ value, so the enum and the write width are pinned together.
//   2. Every legal filter round-trips at m1/m3/m5. A type written with the
//      wrong width decodes as something else and fails the CRC.
//   3. Legal filters are APPLIED, not passed through raw. The reserved-type
//      raw-emission branch is source-level documented but not reachable from a
//      unit test without a bitstream writer; see the note at
//      test_legal_filters_are_applied_not_raw.

#include "../src/compress/compressor50.hpp"
#include "../src/compress/decompressor50.hpp"
#include "../src/compress/filters50.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../support/test_runner.hpp"

using namespace openrar;
using namespace openrar::compress;

namespace openrar::test {
struct Case;
}

namespace {

// A synthetic blob with enough E8 density to trigger detection.
std::vector<core::byte> make_x86_like(size_t n, uint32_t seed) {
    std::vector<core::byte> v(n);
    uint32_t x = seed ? seed : 1;
    for (size_t i = 0; i < n; ++i) {
        x ^= (x << 13) & 0xFFFFFFFFu;
        x ^= x >> 17;
        x ^= (x << 5) & 0xFFFFFFFFu;
        v[i] = static_cast<core::byte>(x & 0xFF);
        if (i % 5 == 0) {
            v[i] = 0xE8; // rel32 CALL
            if (i + 4 < n) {
                const core::uint32 rel = (x >> 8) & 0x7FFFFFFF;
                v[i + 1] = static_cast<core::byte>(rel);
                v[i + 2] = static_cast<core::byte>(rel >> 8);
                v[i + 3] = static_cast<core::byte>(rel >> 16);
                v[i + 4] = static_cast<core::byte>(rel >> 24);
            }
        }
    }
    return v;
}

// A 16-bit stereo WAV: RIFF header plus fmt/data chunks, then seeded payload.
std::vector<core::byte> make_wav(size_t frames) {
    std::vector<core::byte> pcm(frames * 4);
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < pcm.size(); ++i) {
        x = x * 1664525u + 1013904223u;
        pcm[i] = static_cast<core::byte>(x >> 24);
    }
    std::vector<core::byte> v;
    auto push32 = [&](core::uint32 x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<core::byte>(x >> (8 * i)));
    };
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    push32(static_cast<core::uint32>(36 + pcm.size()));
    v.insert(v.end(), {'W', 'A', 'V', 'E'});
    v.insert(v.end(), {'f', 'm', 't', ' '});
    push32(16);
    v.push_back(1);
    v.push_back(0); // PCM
    v.push_back(2);
    v.push_back(0); // 2 channels
    push32(44100);
    push32(44100 * 4);
    v.push_back(4);
    v.push_back(0); // block align
    v.push_back(16);
    v.push_back(0); // bits per sample
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    push32(static_cast<core::uint32>(pcm.size()));
    v.insert(v.end(), pcm.begin(), pcm.end());
    return v;
}

// Pack `src` at the given method. Uses the streaming entry point, which is what
// the CLI and SolidPacker use and which is byte-identical to compress_buffer().
std::vector<core::byte> pack(const std::vector<core::byte>& src, int method) {
    Compressor50 enc;
    if (!enc.begin_stream(method, 0x200000)) return {};
    std::vector<core::byte> out;
    enc.set_memory_dest(&out);
    size_t off = 0;
    const size_t kFeed = 0x80000;
    while (off < src.size()) {
        const size_t n = std::min(kFeed, src.size() - off);
        if (enc.feed(src.data() + off, n) != 0) return {};
        off += n;
    }
    if (enc.finish_stream() != 0) return {};
    return out;
}

// Decode `packed`, writing output into `out`. Returns true on success.
//
// Decompressor50::decompress streams through an OutputCallback; filters are
// applied inside the decoder before the callback fires, so `out` holds
// post-filter bytes - which is what this test asserts about.
bool unpack_to(const std::vector<core::byte>& packed, size_t dest_size,
               std::vector<core::byte>& out) {
    out.clear();
    Decompressor50 dec(0x200000, false);
    FilterCensus census;
    dec.set_filter_census(&census);
    size_t written = 0;
    const bool ok = dec.decompress(
        packed.data(), packed.size(), dest_size,
        /*solid=*/false,
        [&](const core::byte* d, size_t n) -> bool {
            out.insert(out.end(), d, d + n);
            return true;
        },
        &written);
    return ok && out.size() == dest_size;
}

void fail(const char* what) {
    std::printf("  [FAIL] %s\n", what);
    std::exit(1);
}

// ── Property 1: the enum declares exactly the four legal wire values ───────
void test_filter_type_enum_is_pinned() {
    // If a future type is added, this fails at compile time rather than shipping
    // a token that every reference decoder drops.
    static_assert(static_cast<unsigned>(FilterType::Delta) == 0, "Delta must be 0");
    static_assert(static_cast<unsigned>(FilterType::E8) == 1, "E8 must be 1");
    static_assert(static_cast<unsigned>(FilterType::E8E9) == 2, "E8E9 must be 2");
    static_assert(static_cast<unsigned>(FilterType::Arm) == 3, "Arm must be 3");
    // None is a sentinel, never a wire value. If it were ever written as a
    // 3-bit field it would truncate to 7 (reserved) - so it must stay out.
    static_assert(static_cast<unsigned>(FilterType::None) == 255, "None must stay a sentinel");

    // The static_asserts above catch a new ENUM VALUE. This runtime check
    // catches the other half: a value that fits the enum but not the 3-bit
    // field. If a future type were numbered 8+, put_bits(ft.type, 3) would
    // truncate it silently and this would catch it at run time.
    const FilterType legal[] = {FilterType::Delta, FilterType::E8, FilterType::E8E9,
                                FilterType::Arm};
    for (const FilterType t : legal) {
        const unsigned v = static_cast<unsigned>(t);
        if (v > 7) {
            std::printf("  [FAIL] FilterType value %u does not fit the 3-bit wire field\n", v);
            std::exit(1);
        }
    }
    // And the sentinel must never be encodable.
    if (static_cast<unsigned>(FilterType::None) <= 7) {
        std::printf("  [FAIL] None must stay a sentinel outside the 3-bit space\n");
        std::exit(1);
    }
    std::printf("  [PASS] FilterType declares exactly the four legal wire values 0..3\n");
}

// ── Property 2: every legal filter round-trips at m1/m3/m5 ─────────────────
void test_legal_filters_round_trip() {
    struct Case {
        const char* name;
        std::vector<core::byte> data;
        FilterType expect;
    };
    std::vector<Case> cases;
    cases.push_back({"wav", make_wav(4096), FilterType::Delta});
    cases.push_back({"x86", make_x86_like(64 * 1024, 7), FilterType::E8});

    for (const auto& c : cases) {
        core::uint8 ch = 0;
        FilterConfig cfg;
        const FilterType got = Filters50::detect_filter(c.data.data(), c.data.size(), ch, cfg);
        if (got != c.expect) {
            // Fail closed: a fixture that stops tripping its detector is no
            // longer evidence that legal filters round-trip, so skipping it
            // would turn a broken fixture into a green run.
            std::printf("  [FAIL] %s fixture gave %d, expected %d - fixture no longer "
                        "exercises a legal filter\n",
                        c.name, static_cast<int>(got), static_cast<int>(c.expect));
            std::exit(1);
        }
        for (int method : {1, 3, 5}) {
            const std::vector<core::byte> packed = pack(c.data, method);
            if (packed.empty()) fail("packing produced no output");
            std::vector<core::byte> out;
            if (!unpack_to(packed, c.data.size(), out)) {
                std::printf("  [FAIL] %s -m%d decode failed\n", c.name, method);
                std::exit(1);
            }
            if (out != c.data) fail("round trip is not byte-exact");
        }
        std::printf("  [PASS] %s filter (type %d) round-trips at m1/m3/m5\n", c.name,
                    static_cast<int>(c.expect));
    }
}

// ── Property 3: legal filters are applied, never passed through raw ────────
//
// decompressor50.cpp:853-882 dispatches on f.type and, for anything that is not
// 0/1/2/3, emits filter_scratch_ unchanged. That reserved-type branch is a
// *source-level* property: the dispatch is inside the decompression loop and
// the 3-bit type is Huffman-coded, so reaching it from a unit test would need a
// bitstream writer. It is document-pinned at the dispatch site, not exercised
// here - and claiming otherwise would be a green check on something never run.
// (The reference engine returns NULL there and drops the region,
// unpack50.cpp:505-506; docs/spec/04-filters.md records the divergence.)
//
// What IS testable, and what actually matters for interop: on content that
// legitimately triggers a filter, the decoder must APPLY it (not fall through
// to raw) and the round trip must be byte-exact. If a future refactor made the
// dispatch fall through for types 0..3, the output would silently differ and
// this would catch it.
void test_legal_filters_are_applied_not_raw() {
    struct Case {
        const char* name;
        std::vector<core::byte> data;
        FilterType expect;
        unsigned min_entries;
    };
    std::vector<Case> cases;
    cases.push_back({"wav", make_wav(4096), FilterType::Delta, 1});
    cases.push_back({"x86", make_x86_like(64 * 1024, 7), FilterType::E8, 1});

    for (const auto& c : cases) {
        core::uint8 ch = 0;
        FilterConfig cfg;
        const FilterType got = Filters50::detect_filter(c.data.data(), c.data.size(), ch, cfg);
        if (got != c.expect) {
            // Fail closed - see test_legal_filters_round_trip. A skipped case
            // here would let the whole "applied, not raw" property evaporate.
            std::printf("  [FAIL] %s fixture gave %d, expected %d - fixture no longer "
                        "triggers the filter this property asserts\n",
                        c.name, static_cast<int>(got), static_cast<int>(c.expect));
            std::exit(1);
        }

        const std::vector<core::byte> packed = pack(c.data, 3);
        if (packed.empty()) fail("packing produced no output");

        std::vector<core::byte> out;
        Decompressor50 dec(0x200000, false);
        FilterCensus census;
        dec.set_filter_census(&census);
        size_t written = 0;
        const bool ok = dec.decompress(
            packed.data(), packed.size(), c.data.size(), /*solid=*/false,
            [&](const core::byte* d, size_t n) -> bool {
                out.insert(out.end(), d, d + n);
                return true;
            },
            &written);
        if (!ok) {
            std::printf("  [FAIL] %s decode failed\n", c.name);
            std::exit(1);
        }

        // The census must show the filter was APPLIED. A raw passthrough would
        // leave the census empty for filtered content, which is the failure
        // mode this asserts against.
        if (census.entries.size() < c.min_entries) {
            std::printf("  [FAIL] %s: filter was NOT applied (census=%zu, expected >=%u)\n", c.name,
                        census.entries.size(), c.min_entries);
            std::printf("         a raw passthrough would look exactly like this\n");
            std::exit(1);
        }
        if (out != c.data) fail("round trip is not byte-exact");

        // And the applied type must be the expected one - not a reserved value.
        for (const auto& fe : census.entries) {
            if (fe.type > 3) {
                std::printf("  [FAIL] %s: census recorded reserved type %u\n", c.name,
                            static_cast<unsigned>(fe.type));
                std::exit(1);
            }
        }
        std::printf("  [PASS] %s: filter type %d APPLIED (%zu region), round-trip exact\n", c.name,
                    static_cast<int>(c.expect), census.entries.size());
    }
}

} // namespace

static void run_suite_body() {
    std::printf("SEC-INTEROP-UNDEF-FILTER\n");
    test_filter_type_enum_is_pinned();
    test_legal_filters_round_trip();
    test_legal_filters_are_applied_not_raw();
    std::printf("SEC-INTEROP-UNDEF-FILTER: OK\n");
}

static const openrar::test::Case TEST_CASES[] = {
    {"sec_interop_undef_filter", run_suite_body, false},
};

int main(int argc, char* argv[]) {
    openrar::test::route_crt_asserts_to_stderr();
    return openrar::test::openrar_test_main(TEST_CASES, sizeof(TEST_CASES) / sizeof(TEST_CASES[0]),
                                            argc, argv);
}
