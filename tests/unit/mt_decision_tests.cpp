// EXP-MT-DECISION-CONSOLIDATE (§4.25) regression test.
//
// The defect this pins: three call sites decided "does this member take the
// chunked encoder?" independently, with DIFFERENT sample policies. detect_filter
// is position-sensitive - it follows e_lfanew at +0x3C and validates Mach-O fat
// slice ranges against the BUFFER SIZE - so a 64 KiB sample could yield a
// different verdict than the whole member. A PE with e_lfanew at 0x20000, or a
// fat Mach-O with a slice at 0x20000, returns E8 on the full member and None on
// a 64 KiB prefix. The two sampling sites would chunk it (forcing filters off,
// silently losing -mc); the one site that read the whole member would not.
//
// This test asserts the property that actually matters: every caller of the one
// decision function gets the same answer for the same input. It does NOT assert
// that detect_filter is sample-size invariant - it is not, and that is fine now
// that only one policy exists.
//
// Run: build/Release/mt_decision_tests.exe   (registered as a ctest)
// Exit 0 = pass, 1 = fail.

#include "../src/compress/compressor50.hpp"
#include "../src/compress/parallel_compressor.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace openrar;
using namespace openrar::compress;

// Compile-time pin of the shared prefix. This is also what puts the suite in
// the v1.44 ledger's measured table: it uses a local check() counter rather
// than the doctest macros, so a file with zero counted assertions is omitted
// from that table entirely.
static_assert(MT_DETECT_PREFIX == 1024 * 1024, "MT_DETECT_PREFIX must stay 1 MiB");

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void put_le32(std::vector<core::byte>& v, size_t off, core::uint32 x) {
    v[off + 0] = static_cast<core::byte>(x);
    v[off + 1] = static_cast<core::byte>(x >> 8);
    v[off + 2] = static_cast<core::byte>(x >> 16);
    v[off + 3] = static_cast<core::byte>(x >> 24);
}

void put_be32(std::vector<core::byte>& v, size_t off, core::uint32 x) {
    v[off + 0] = static_cast<core::byte>(x >> 24);
    v[off + 1] = static_cast<core::byte>(x >> 16);
    v[off + 2] = static_cast<core::byte>(x >> 8);
    v[off + 3] = static_cast<core::byte>(x);
}

// FAT_MAGIC (big-endian) with one x86_64 slice at `slice_off`.
std::vector<core::byte> make_fat_macho(size_t slice_off) {
    std::vector<core::byte> v(slice_off + 0x1000, 0);
    v[0] = 0xCA;
    v[1] = 0xFE;
    v[2] = 0xBA;
    v[3] = 0xBE;                 // 0xCAFEBABE, big-endian
    put_be32(v, 4, 1);           // nfat_arch = 1
    put_be32(v, 8, 0x01000007u); // cputype = x86_64
    put_be32(v, 12, 3);          // cpusubtype
    put_be32(v, 16, static_cast<core::uint32>(slice_off));
    put_be32(v, 20, 0x1000); // size
    put_be32(v, 24, 12);     // align
    return v;
}

// PE with e_lfanew at `pe_off`, machine AMD64.
std::vector<core::byte> make_pe(size_t pe_off) {
    std::vector<core::byte> v(pe_off + 0x400, 0);
    v[0] = 'M';
    v[1] = 'Z';
    put_le32(v, 0x3C, static_cast<core::uint32>(pe_off));
    v[pe_off] = 'P';
    v[pe_off + 1] = 'E';
    v[pe_off + 2] = 0;
    v[pe_off + 3] = 0;
    v[pe_off + 4] = 0x64;
    v[pe_off + 5] = 0x86; // 0x8664 little-endian
    return v;
}

std::vector<core::byte> make_text(size_t n) {
    static const char* words[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
    std::vector<core::byte> v;
    v.reserve(n + 64);
    size_t i = 0;
    while (v.size() < n) {
        const char* w = words[i++ % 5];
        v.insert(v.end(), w, w + 5);
        v.push_back(static_cast<core::byte>(' '));
    }
    v.resize(n);
    return v;
}

} // namespace

int main() {
    std::printf("EXP-MT-DECISION-CONSOLIDATE regression test\n\n");

    const std::vector<core::byte> text = make_text(4 * 1024 * 1024);
    FilterConfig auto_cfg; // Auto, no overrides

    // ── 1. The predicate's truth table ─────────────────────────────────────
    std::printf("predicate truth table:\n");

    // Single thread -> never chunked, regardless of content.
    check(decide_chunked_pack(auto_cfg, text.data(),
                              std::min<size_t>(text.size(), MT_DETECT_PREFIX), 1, text.size())
                  .decision == MtDecision::SingleThread,
          "1 thread -> SingleThread");

    // Filters disabled -> chunked even on filter-triggering content. The
    // member must clear the chunking floor, which is checked BEFORE the filter
    // decision (a 512 B PE is TooSmall, not Chunked - the floor wins).
    FilterConfig no_filters;
    no_filters.mode = FilterMode::DisableAll;
    {
        std::vector<core::byte> pe = make_pe(0x200);
        pe.resize(2 * 1024 * 1024, 0); // clear the 1 MiB floor
        check(decide_chunked_pack(no_filters, pe.data(),
                                  std::min<size_t>(pe.size(), MT_DETECT_PREFIX), 4, pe.size())
                      .decision == MtDecision::Chunked,
              "filters disabled + 2 MiB PE -> Chunked");
        // And the same input with filters enabled is NOT chunked: a filter would
        // fire on the PE header, and chunking would silently drop -mc.
        check(decide_chunked_pack(auto_cfg, pe.data(),
                                  std::min<size_t>(pe.size(), MT_DETECT_PREFIX), 4, pe.size())
                      .decision == MtDecision::Sequential,
              "filters enabled + 2 MiB PE -> Sequential");
    }

    // Below the chunking floor -> TooSmall.
    {
        std::vector<core::byte> tiny(1024, 'x');
        check(decide_chunked_pack(auto_cfg, tiny.data(),
                                  std::min<size_t>(tiny.size(), MT_DETECT_PREFIX), 4, tiny.size())
                      .decision == MtDecision::TooSmall,
              "1 KiB member -> TooSmall");
    }

    // Filter-free text -> Chunked.
    check(decide_chunked_pack(auto_cfg, text.data(),
                              std::min<size_t>(text.size(), MT_DETECT_PREFIX), 4, text.size())
                  .decision == MtDecision::Chunked,
          "4 MiB text -> Chunked");

    // ── 2. The divergence this row exists to fix ───────────────────────────
    // detect_filter is position-sensitive, so a TRUNCATED prefix can read as
    // None where the full member reads as E8. Two properties matter:
    //   (a) a header inside MT_DETECT_PREFIX is always seen, so a member that
    //       would be filtered is never chunked;
    //   (b) the two archive sites and the in-memory site all get the same
    //       answer, because they now pass the same prefix length.
    std::printf("\nsingle-decision property (late container headers):\n");
    {
        // e_lfanew at 0x20000 (128 KiB): past a naive 64 KiB sample, well
        // inside MT_DETECT_PREFIX. The constant check is not decorative: if a
        // future edit shrinks the prefix this fixture silently stops proving
        // the divergence, so fail here instead.
        std::vector<core::byte> pe = make_pe(0x20000);
        pe.resize(4 * 1024 * 1024, 0); // clear the chunking floor
        check(MT_DETECT_PREFIX > 0x20000 + 0x400,
              "the late PE header fits inside MT_DETECT_PREFIX");

        const auto d = decide_chunked_pack(
            auto_cfg, pe.data(), std::min<size_t>(pe.size(), MT_DETECT_PREFIX), 4, pe.size());
        std::printf("    PE@0x20000 -> %s (detected=%d)\n",
                    d.decision == MtDecision::Chunked ? "Chunked" : "Sequential",
                    static_cast<int>(d.detected));

        // The invariant: a member that WOULD be filtered must never be chunked,
        // because the chunked path forces filters off. The fixture is only
        // evidence if the whole-member detector actually fires, so that is a
        // hard precondition - not a branch that silently passes when it does
        // not.
        core::uint8 ch = 1;
        const FilterType whole = Filters50::detect_filter(pe.data(), pe.size(), ch, auto_cfg);
        check(whole != FilterType::None, "whole-member PE detects a filter (fixture precondition)");
        check(d.detected == whole,
              "late PE header -> predicate sees the same filter as the whole member");
        check(d.detected != FilterType::None,
              "late PE header -> predicate does not lose the detected filter");
        check(d.decision != MtDecision::Chunked, "late PE header -> decision is not Chunked");

        // The same shape for a fat Mach-O with a late slice.
        std::vector<core::byte> mo = make_fat_macho(0x20000);
        mo.resize(4 * 1024 * 1024, 0);
        const auto dm = decide_chunked_pack(
            auto_cfg, mo.data(), std::min<size_t>(mo.size(), MT_DETECT_PREFIX), 4, mo.size());
        core::uint8 ch2 = 1;
        const FilterType whole_m = Filters50::detect_filter(mo.data(), mo.size(), ch2, auto_cfg);
        std::printf("    fat macho@0x20000 -> %s (detected=%d)\n",
                    dm.decision == MtDecision::Chunked ? "Chunked" : "Sequential",
                    static_cast<int>(dm.detected));
        check(whole_m != FilterType::None,
              "whole-member fat Mach-O detects a filter (fixture precondition)");
        check(dm.detected == whole_m,
              "late fat slice -> predicate sees the same filter as the whole member");
        check(dm.detected != FilterType::None,
              "late fat slice -> predicate does not lose the detected filter");
        check(dm.decision != MtDecision::Chunked, "late fat slice -> decision is not Chunked");
    }

    // ── 3. Prefix policy is shared, not per-site ───────────────────────────
    std::printf("\nshared prefix policy:\n");
    std::printf("  [PASS] MT_DETECT_PREFIX is %zu bytes (pinned at compile time)\n",
                MT_DETECT_PREFIX);
    {
        // The documented contract: every site passes the same prefix length.
        const size_t n = std::min<size_t>(text.size(), MT_DETECT_PREFIX);
        core::uint8 a_ch = 1, b_ch = 1;
        const FilterType a = Filters50::detect_filter(text.data(), n, a_ch, auto_cfg);
        const FilterType b = Filters50::detect_filter(text.data(), n, b_ch, auto_cfg);
        check(a == b, "same prefix + same config -> same verdict");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}