// fuzz_target.cpp - audit-driven libFuzzer target for the RAR5 codec.
//
// Built from the Phase 2 review findings (docs/review-master-plan.md). Each
// input is routed to one of four lanes by its first byte; the LAST bytes of
// the input are configuration (window, method, filters, chunking), so the
// data prefix stays friendly to libFuzzer's mutators.
//
//   Lane 0  hostile decode   arbitrary bytes into every decode entry point:
//                            Decompressor50::decompress (bounded output),
//                            StreamDecoder with random chunking (H-17, M-08),
//                            sequential vs two-phase parallel decode
//                            (H-06: the engines must agree), Inflate.
//                            Oracle: no crash / UB / OOB (sanitizers), output
//                            never exceeds the requested bound.
//   Lane 1  round trip       compress -> decompress -> assert byte-identical,
//                            across compress_buffer, compress_buffer_parallel
//                            (C-03 window clamp), and StreamEncoder chunked
//                            feeds; with forced Delta/E8 filters (H-08/H-09)
//                            and filter-region boundaries (C-10, M-06).
//                            Then two corruption oracles on the valid stream:
//                            truncation (H-07: an accepted truncated stream
//                            may only emit a PREFIX of the original) and bit
//                            flips (no crash; both decoders agree).
//   Lane 2  bit reader model BitReader peek/get/consume (32- and 64-bit,
//                            buffer and chunked-callback sources) checked
//                            against a reference MSB-first bit extractor
//                            (H-05: 58..64-bit peeks; H-07: truncated()).
//   Lane 3  all of the above at reduced cost.
//
// Build: -DOPENRAR_FUZZ=ON. With clang (non-Windows) this links libFuzzer
// (OPENRAR_USE_LIBFUZZER); elsewhere a standalone main() runs a deterministic
// sweep, or replays the files given on the command line.
//
// A failed oracle prints the lane, dumps the input to
// fuzz_target_failure.bin, and aborts (libFuzzer then saves the crash unit).

#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"
#include "../../src/compress/filters50.hpp"
#include "../../src/compress/parallel_decode.hpp"
#include "../../src/compress/stream_decoder.hpp"
#include "../../src/compress/stream_encoder.hpp"
#if defined(OPENRAR_FUZZ_HAVE_INFLATE)
#include "../../src/compress/inflate.hpp"
#endif
#include "../../src/core/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace openrar;
using namespace openrar::compress;

namespace {

// ---------------------------------------------------------------------------
// Input plumbing
// ---------------------------------------------------------------------------

const uint8_t* g_input = nullptr; // current unit, for failure dumps
size_t g_input_size = 0;

[[noreturn]] void oracle_fail(const char* lane, const char* what, size_t a = 0, size_t b = 0) {
    std::fprintf(stderr, "fuzz_target ORACLE FAILURE [%s]: %s (a=%zu b=%zu, input=%zu bytes)\n",
                 lane, what, a, b, g_input_size);
    if (std::FILE* f = std::fopen("fuzz_target_failure.bin", "wb")) {
        if (g_input_size) std::fwrite(g_input, 1, g_input_size, f);
        std::fclose(f);
    }
    std::fflush(stderr);
    std::abort();
}

// Configuration is consumed from the END of the input; data is the prefix.
struct Config {
    const uint8_t* p;
    size_t n;
    size_t pos{0};
    uint8_t u8() { return pos < n ? p[n - 1 - pos++] : 0; }
    uint32_t u16() { return static_cast<uint32_t>(u8()) | (static_cast<uint32_t>(u8()) << 8); }
};

// Small deterministic PRNG seeded from config, for chunk splits.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<uint32_t>(s >> 11);
    }
    size_t below(size_t n) { return n ? next() % n : 0; }
};

// Windows the decoder supports in-process (<= ALLOC_LIMIT), all powers of
// two so the encoder's normalisation leaves them unchanged.
constexpr size_t kWindows[] = {0x10000, 0x20000, 0x100000, 0x200000, 0x400000};
size_t pick_window(uint8_t b) {
    return kWindows[b % (sizeof(kWindows) / sizeof(kWindows[0]))];
}

constexpr size_t kMaxDecodeOut = 4u << 20;    // bound for hostile decodes
constexpr size_t kMaxRoundTrip = 1536u << 10; // tiled round-trip ceiling

// ---------------------------------------------------------------------------
// Lane 0 - hostile decode
// ---------------------------------------------------------------------------

// Bounded sink: collects (optionally) and enforces the requested ceiling.
struct BoundedSink {
    const char* lane;
    size_t cap;
    std::vector<core::byte>* keep;
    size_t total{0};
    Decompressor50::OutputCallback fn() {
        return [this](const core::byte* d, size_t n) -> bool {
            if (n != 0 && d == nullptr) oracle_fail(lane, "null output chunk");
            total += n;
            if (total > cap) oracle_fail(lane, "decoder emitted past dest_size", total, cap);
            if (keep) keep->insert(keep->end(), d, d + n);
            return true;
        };
    }
};

void decode_sequential(const uint8_t* d, size_t n, size_t win, size_t cap, bool xd = false) {
    Decompressor50 dec(win, xd);
    BoundedSink sink{"seq", cap, nullptr};
    size_t written = 0;
    bool finished = false;
    const bool ok = dec.decompress(d, n, cap, false, sink.fn(), &written, &finished);
    if (ok && written > cap) oracle_fail("seq", "written > dest_size", written, cap);
    if (sink.total > written && ok)
        oracle_fail("seq", "flushed more than written", sink.total, written);
}

int count_flush(void* user, const core::byte*, size_t n) {
    auto* total = static_cast<size_t*>(user);
    *total += n;
    return *total > kMaxDecodeOut ? 1 : 0; // a non-zero return aborts the stream
}

void decode_stream_chunked(const uint8_t* d, size_t n, size_t win, Rng& rng, bool use_sink) {
    // Raw streams carry no header: StreamDecoder is always the RAR5 flavor.
    StreamDecoder sd(win, 3);
    if (!sd.is_valid()) return;
    size_t flushed = 0;
    if (use_sink) sd.set_flush(count_flush, &flushed);
    size_t pos = 0;
    while (pos < n) {
        const size_t take = std::min(n - pos, 1 + rng.below(4096));
        if (!sd.feed(d + pos, take)) return; // fail-closed is a valid verdict
        pos += take;
        if (!use_sink) {
            std::vector<core::byte> part;
            sd.take_output(part); // drain so retention stays bounded
            if (part.size() > kMaxDecodeOut * 4) oracle_fail("stream", "unbounded retention");
        }
    }
    std::vector<core::byte> out;
    sd.finish(out);
}

// H-06: the two-phase pipeline restarts every block at its byte boundary;
// the sequential decoder streams continuously. When both accept a stream
// they must emit identical bytes. With an exact dest_size (`exact`), the R5
// contract also applies: the parallel path may only refuse a stream the
// sequential decoder accepts through its pre-emission fallback.
void decode_differential(const char* lane, const uint8_t* d, size_t n, size_t win, size_t cap,
                         bool exact = false, bool xd = false) {
    std::vector<core::byte> seq_out, par_out;
    bool seq_ok;
    {
        Decompressor50 dec(win, xd);
        BoundedSink sink{lane, cap, &seq_out};
        seq_ok = dec.decompress(d, n, cap, false, sink.fn());
    }
    ParallelDecodeDiag diag;
    BoundedSink psink{lane, cap, &par_out};
    const bool par_ok = decode_entry(d, n, cap, win, psink.fn(), &diag, 2, xd);
    if (seq_ok && par_ok && seq_out != par_out)
        oracle_fail(lane, "sequential and two-phase decoders disagree", seq_out.size(),
                    par_out.size());
    if (exact && !par_ok && seq_ok && !diag.fell_back) {
        oracle_fail(lane, "two-phase refused after emission a stream sequential accepts");
    }
}

#if defined(OPENRAR_FUZZ_HAVE_INFLATE)
void decode_inflate(const uint8_t* d, size_t n) {
    Inflate inf(kMaxDecodeOut);
    size_t pos = 0;
    size_t emitted = 0;
    inf.decode(
        [&](core::byte* buf, size_t max) -> size_t {
            const size_t take = std::min(max, n - pos);
            if (take) std::memcpy(buf, d + pos, take);
            pos += take;
            return take;
        },
        [&](const core::byte* p, size_t m) -> bool {
            if (m != 0 && p == nullptr) oracle_fail("inflate", "null output chunk");
            emitted += m;
            if (emitted > kMaxDecodeOut) oracle_fail("inflate", "output cap not enforced", emitted);
            return true;
        });
}
#endif

void lane_hostile(const uint8_t* d, size_t n, Config& cfg, bool reduced) {
    const size_t win = pick_window(cfg.u8());
    const uint8_t flags = cfg.u8();
    const size_t cap = std::min<size_t>(kMaxDecodeOut, std::max<size_t>(1, n * 64));
    Rng rng(cfg.u16());

    // flags bit 2: RAR7 algorithm flavor (446-entry ExtraDist tables).
    const bool xd = (flags & 4) != 0;
    decode_sequential(d, n, win, cap, xd);
    decode_stream_chunked(d, n, win, rng, (flags & 1) != 0);
    if (!reduced || (flags & 2)) decode_differential("hostile-diff", d, n, win, cap, false, xd);
#if defined(OPENRAR_FUZZ_HAVE_INFLATE)
    decode_inflate(d, n);
#endif
}

// ---------------------------------------------------------------------------
// Lane 1 - round trip (+ truncation / bit-flip oracles)
// ---------------------------------------------------------------------------

[[noreturn]] void roundtrip_fail(const char* what, const std::vector<core::byte>& want,
                                 const std::vector<core::byte>& got) {
    size_t first = std::min(want.size(), got.size());
    for (size_t i = 0; i < std::min(want.size(), got.size()); ++i) {
        if (want[i] != got[i]) {
            first = i;
            break;
        }
    }
    oracle_fail("roundtrip", what, first, got.size());
}

std::string g_last_decode_error; // decoder diagnostic for failure reports

std::vector<core::byte> decode_all(const std::vector<core::byte>& comp, size_t win, size_t dest,
                                   bool& ok) {
    std::vector<core::byte> out;
    out.reserve(dest);
    Decompressor50 dec(win);
    BoundedSink sink{"roundtrip-decode", dest, &out};
    ok = dec.decompress(comp.data(), comp.size(), dest, false, sink.fn());
    g_last_decode_error = dec.last_error_message();
    return out;
}

void lane_roundtrip(const uint8_t* d, size_t n, Config& cfg, bool reduced) {
    const uint8_t c0 = cfg.u8(), c1 = cfg.u8(), c2 = cfg.u8(), c3 = cfg.u8();
    const int method = 1 + (c0 % 5);
    const size_t win = pick_window(c0 >> 3);
    Rng rng(cfg.u16());

    // Input shaping. Tiling makes the data compressible (long matches, many
    // blocks) and, on 1-in-16 inputs, crosses the 1 MiB parallel threshold.
    std::vector<core::byte> src(d, d + n);
    if ((c1 & 0x0F) == 0 && !src.empty() && !reduced) {
        const size_t target = std::min(kMaxRoundTrip, (1u << 20) + 4096 + n);
        while (src.size() < target) src.insert(src.end(), d, d + std::min(n, target - src.size()));
    } else if ((c1 & 0x03) == 1 && !src.empty()) {
        const size_t target = std::min<size_t>(256u << 10, n * 32);
        while (src.size() < target) src.insert(src.end(), d, d + std::min(n, target - src.size()));
    }
    if (src.empty()) return;

    // Filters: auto, off, or forced Delta (1..32 channels) / E8.
    FilterConfig fc;
    switch (c2 & 3) {
    case 0:
        break;
    case 1:
        fc.mode = FilterMode::DisableAll;
        break;
    case 2:
        fc.delta_override = 1;
        fc.delta_channels = static_cast<core::uint8>(1 + ((c2 >> 2) % 32));
        break;
    default:
        fc.e8_override = 1;
        break;
    }

    // Encoder path: one-shot, chunk-parallel, or streaming with random feeds.
    std::vector<core::byte> comp;
    bool comp_ok = false;
    const int path = c3 % 3;
    if (path == 0) {
        comp_ok = Compressor50::compress_buffer(src.data(), src.size(), comp, method, win, fc);
    } else if (path == 1) {
        comp_ok = Compressor50::compress_buffer_parallel(src.data(), src.size(), comp, method, win,
                                                         fc, 2);
    } else {
        StreamEncoder enc(method, win, fc);
        comp_ok = true;
        size_t pos = 0;
        while (comp_ok && pos < src.size()) {
            const size_t take = std::min(src.size() - pos, 1 + rng.below(65536));
            comp_ok = enc.feed(src.data() + pos, take);
            pos += take;
        }
        if (comp_ok) comp_ok = enc.finish(comp);
    }
    if (!comp_ok) oracle_fail("roundtrip", "encoder rejected valid input", src.size(), path);
    if (comp.empty()) oracle_fail("roundtrip", "encoder produced no output", src.size(), path);

    bool ok = false;
    const std::vector<core::byte> back = decode_all(comp, win, src.size(), ok);
    if (!ok || back != src) {
        std::fprintf(stderr,
                     "roundtrip params: method=%d win=%zu path=%d filter_sel=%d delta_ch=%u "
                     "src=%zu packed=%zu decoded=%zu decoder_error='%s'\n",
                     method, win, path, c2 & 3, static_cast<unsigned>(fc.delta_channels),
                     src.size(), comp.size(), back.size(), g_last_decode_error.c_str());
    }
    if (!ok) roundtrip_fail("decoder rejected the encoder's own stream", src, back);
    if (back != src) roundtrip_fail("decompressed != original", src, back);

    // Two-phase parallel decode of the clean stream with exact dest_size must agree.
    if (!reduced)
        decode_differential("roundtrip-diff", comp.data(), comp.size(), win, src.size(), true);

    // Streaming decode of the same stream with random chunking must agree.
    if (!reduced || (c3 & 0x10)) {
        StreamDecoder sd(win, method);
        std::vector<core::byte> sout, part;
        size_t pos = 0;
        bool sok = sd.is_valid();
        while (sok && pos < comp.size()) {
            const size_t take = std::min(comp.size() - pos, 1 + rng.below(8192));
            sok = sd.feed(comp.data() + pos, take);
            pos += take;
            sd.take_output(part);
            sout.insert(sout.end(), part.begin(), part.end());
        }
        if (sok) sok = sd.finish(part);
        sout.insert(sout.end(), part.begin(), part.end());
        if (!sok || sout != src)
            roundtrip_fail("StreamDecoder disagrees with one-shot decode", src, sout);
    }

    // Truncation oracle (H-07): a truncated stream may be rejected, or - if
    // every token it needed survived - accepted, but it may only ever emit a
    // prefix of the original. Bytes fabricated from zero padding fail this.
    if (comp.size() > 1) {
        const size_t cut = 1 + rng.below(comp.size() - 1);
        std::vector<core::byte> trunc(comp.begin(),
                                      comp.begin() + static_cast<std::ptrdiff_t>(cut));
        bool tok = false;
        const std::vector<core::byte> t = decode_all(trunc, win, src.size(), tok);
        if (t.size() > src.size() || !std::equal(t.begin(), t.end(), src.begin()))
            roundtrip_fail("truncated stream emitted bytes that are not a prefix", src, t);
        (void)tok;
    }

    // Bit-flip oracle: no crash, bounded output, and the two decoders agree.
    // On a corrupted stream, divergence in early vs late block boundary detection
    // is expected; exact=false enforces identical output only when both accept.
    {
        std::vector<core::byte> bad = comp;
        const int flips = 1 + static_cast<int>(rng.below(4));
        for (int i = 0; i < flips; ++i)
            bad[rng.below(bad.size())] ^= static_cast<core::byte>(1u << rng.below(8));
        if (!reduced)
            decode_differential("bitflip-diff", bad.data(), bad.size(), win, src.size(), false);
        else
            decode_sequential(bad.data(), bad.size(), win, src.size());
    }
}

// ---------------------------------------------------------------------------
// Lane 2 - BitReader model check
// ---------------------------------------------------------------------------

// Reference: MSB-first bit string over buf, zero-padded past the end.
uint64_t ref_bits(const uint8_t* buf, size_t n, size_t bitpos, unsigned count) {
    uint64_t v = 0;
    for (unsigned i = 0; i < count; ++i) {
        const size_t b = bitpos + i;
        const unsigned bit = (b / 8 < n) ? (buf[b / 8] >> (7 - (b % 8))) & 1u : 0u;
        v = (v << 1) | bit;
    }
    return v;
}

void bitreader_model(const uint8_t* d, size_t n, Config& cfg, bool callback_source) {
    Rng rng(cfg.u16());
    size_t fed = 0;
    std::unique_ptr<BitReader> br;
    if (callback_source) {
        // Random, sometimes tiny, chunks exercise fetch_chunk() and the
        // H-05 splice across a chunk boundary.
        br = std::make_unique<BitReader>(
            [&, d, n](core::byte* out, size_t max) -> size_t {
                const size_t take = std::min({max, n - fed, 1 + rng.below(9)});
                if (take) std::memcpy(out, d + fed, take);
                fed += take;
                return take;
            },
            n);
    } else {
        br = std::make_unique<BitReader>(d, n);
    }
    const size_t total_bits = n * 8;
    size_t pos = 0;
    bool want_truncated = false;
    Rng ops_rng(cfg.u16() ^ (callback_source ? 0xA5A5u : 0u));
    const int ops = 16 + static_cast<int>(ops_rng.below(240));
    for (int i = 0; i < ops; ++i) {
        const uint32_t op = ops_rng.next();
        // Bias towards the wide (57..64-bit) requests H-05 is about.
        const unsigned k = (op & 0x100) ? 57 + static_cast<unsigned>(ops_rng.below(8))
                                        : 1 + static_cast<unsigned>(ops_rng.below(64));
        const char* lane = callback_source ? "bitreader-cb" : "bitreader";
        switch (op % 5) {
        case 0: { // peek_bits (clamps to 32)
            const unsigned c = std::min(k, 32u);
            const uint64_t want = ref_bits(d, n, pos, c);
            if (br->peek_bits(c) != want) oracle_fail(lane, "peek_bits mismatch", pos, c);
            break;
        }
        case 1: { // peek_bits64 (H-05: 58..64-bit peeks with input remaining)
            const uint64_t want = ref_bits(d, n, pos, k);
            if (br->peek_bits64(k) != want) oracle_fail(lane, "peek_bits64 mismatch", pos, k);
            break;
        }
        case 2: { // get_bits (clamps to 32)
            const unsigned c = std::min(k, 32u);
            const uint64_t want = ref_bits(d, n, pos, c);
            if (br->get_bits(c) != want) oracle_fail(lane, "get_bits mismatch", pos, c);
            if (pos + c > total_bits) want_truncated = true;
            pos = std::min(total_bits, pos + c);
            break;
        }
        case 3: { // get_bits64
            const uint64_t want = ref_bits(d, n, pos, k);
            if (br->get_bits64(k) != want) oracle_fail(lane, "get_bits64 mismatch", pos, k);
            if (pos + k > total_bits) want_truncated = true;
            pos = std::min(total_bits, pos + k);
            break;
        }
        default: { // consume (capped at the accumulator width by contract)
            const unsigned c = std::min(k, 57u);
            br->consume_bits(c);
            if (pos + c > total_bits) want_truncated = true;
            pos = std::min(total_bits, pos + c);
            break;
        }
        }
        if (br->bit_pos() != pos) oracle_fail(lane, "bit_pos drift", br->bit_pos(), pos);
        if (br->bits_remaining() != total_bits - pos)
            oracle_fail(lane, "bits_remaining drift", br->bits_remaining(), total_bits - pos);
        // H-07: truncated() is sticky and set exactly when a get/consume
        // asked for more bits than remained (peeks never set it).
        if (br->truncated() != want_truncated)
            oracle_fail(lane, "truncated() flag mismatch", br->truncated(), want_truncated);
    }
}

void lane_bitreader(const uint8_t* d, size_t n, Config& cfg) {
    bitreader_model(d, n, cfg, false);
    bitreader_model(d, n, cfg, true);
}

void run_one(const uint8_t* data, size_t size) {
    if (size < 2) return;
    g_input = data;
    g_input_size = size;
    const uint8_t lane = data[0] & 3;
    // Up to 16 trailing bytes are configuration; the rest is payload.
    const size_t cfg_len = std::min<size_t>(16, (size - 1) / 2);
    const uint8_t* payload = data + 1;
    const size_t payload_len = size - 1 - cfg_len;
    Config cfg{data + 1 + payload_len, cfg_len};
    switch (lane) {
    case 0:
        lane_hostile(payload, payload_len, cfg, false);
        break;
    case 1:
        lane_roundtrip(payload, payload_len, cfg, false);
        break;
    case 2:
        lane_bitreader(payload, payload_len, cfg);
        break;
    default:
        lane_hostile(payload, payload_len, cfg, true);
        lane_roundtrip(payload, payload_len, cfg, true);
        lane_bitreader(payload, payload_len, cfg);
        break;
    }
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void init_once() {
    static bool done = false;
    if (done) return;
    done = true;
    // Let the two-phase decoder engage on fuzz-sized inputs so the H-06
    // differential compares real span workers, not just the fallback.
    set_env("OPENRAR_PARALLEL_DECODE_SPAN_FLOOR", "4096");
    set_env("OPENRAR_PARALLEL_DECODE_THREADS", "2");
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    init_once();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    init_once();
    run_one(data, size);
    return 0;
}

#ifndef OPENRAR_USE_LIBFUZZER
#include <fstream>
#include <iterator>
#include <random>

// Standalone driver: replay files named on the command line, or run a
// deterministic sweep over every lane (the MSVC / ctest path).
int main(int argc, char** argv) {
    init_once();
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::ifstream f(argv[i], std::ios::binary);
            std::vector<uint8_t> unit((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
            LLVMFuzzerTestOneInput(unit.data(), unit.size());
            std::fprintf(stderr, "replayed %s (%zu bytes)\n", argv[i], unit.size());
        }
        return 0;
    }
    std::mt19937 rng(20261021);
    const int iterations = 600;
    for (int it = 0; it < iterations; ++it) {
        const size_t n = 2 + rng() % 6000;
        std::vector<uint8_t> unit(n);
        const int shape = static_cast<int>(rng() % 4);
        for (size_t i = 0; i < n; ++i) {
            switch (shape) {
            case 0:
                unit[i] = static_cast<uint8_t>(rng());
                break; // noise
            case 1:
                unit[i] = static_cast<uint8_t>((i * 7) ^ (i >> 3));
                break; // structured
            case 2:
                unit[i] = static_cast<uint8_t>(rng() % 4);
                break; // small alphabet
            default:
                unit[i] = static_cast<uint8_t>(i % 2 ? i >> 8 : i);
                break; // 16-bit PCM-like
            }
        }
        unit[0] = static_cast<uint8_t>(it & 3); // rotate lanes
        // Hostile lane: seed half the units with a real stream, then mutate.
        if ((it & 3) == 0 && (it & 4)) {
            std::vector<core::byte> comp;
            if (Compressor50::compress_buffer(unit.data() + 1, n - 1, comp) && !comp.empty()) {
                for (int k = 0; k < 4; ++k)
                    comp[rng() % comp.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
                unit.assign(1, 0);
                unit.insert(unit.end(), comp.begin(), comp.end());
            }
        }
        LLVMFuzzerTestOneInput(unit.data(), unit.size());
        if ((it + 1) % 100 == 0)
            std::fprintf(stderr, "  fuzz_target sweep %d/%d\n", it + 1, iterations);
    }
    std::fprintf(stderr, "fuzz_target standalone sweep PASSED (%d units)\n", iterations);
    return 0;
}
#endif // OPENRAR_USE_LIBFUZZER
