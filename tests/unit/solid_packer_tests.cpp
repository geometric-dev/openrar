// M2b (v1.26 plan §1): encoder-side solid-window session. Pins that
// SolidPacker (a) round-trips byte-identically through the carried-window
// decoder and (b) actually REALIZES cross-file compression — the write side
// mirroring the reader's solid carry (until v1.26 the encoder started every
// file from an empty window, making solid archives header-solid only).

#include "../../src/compress/solid_packer.hpp"
#include "../../src/compress/compressor50.hpp"
#include "../../src/compress/decompressor50.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace openrar;

namespace {

std::vector<core::byte> make_noise(size_t n, unsigned seed) {
    std::vector<core::byte> v(n);
    core::uint32 x = seed * 2654435761u + 1u;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        v[i] = static_cast<core::byte>((x >> 16) & 0xFF);
    }
    return v;
}

// Deterministic LZ-friendly corpus: a shared "base" half plus per-file noise
// halves. With the window carried, the shared halves of files 2..N compress
// against file 1's copy; without carry they cannot.
std::vector<core::byte> make_member(size_t n, unsigned seed,
                                    const std::vector<core::byte>& shared) {
    std::vector<core::byte> v = make_noise(n / 2, seed);
    v.insert(v.end(), shared.begin(), shared.end());
    v.resize(n);
    return v;
}

} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif

    const size_t member_size = 512 * 1024;
    const std::vector<core::byte> shared = make_noise(member_size / 2, 7);
    std::vector<std::vector<core::byte>> members;
    for (unsigned s : {11u, 12u, 13u, 14u}) {
        members.push_back(make_member(member_size, s, shared));
    }

    // Pack a 4-file run: head, member, FILECOPY-gap stand-in (chain head
    // again), member. The gap exercises the chain-break reset path.
    compress::SolidPacker packer(3, 1 << 20); // -m3, 1 MiB window
    std::vector<std::vector<core::byte>> packed(4);
    assert(packer.pack_begin(false)); // run head: fresh window
    assert(packer.pack_feed(members[0].data(), members[0].size()));
    assert(packer.pack_end(&packed[0]));
    assert(packer.pack_begin(true)); // chain member: carry
    assert(packer.pack_feed(members[1].data(), members[1].size()));
    assert(packer.pack_end(&packed[1]));
    assert(packer.pack_begin(false)); // chain break: reset window
    assert(packer.pack_feed(members[2].data(), members[2].size()));
    assert(packer.pack_end(&packed[2]));
    assert(packer.pack_begin(true)); // carry again
    assert(packer.pack_feed(members[3].data(), members[3].size()));
    assert(packer.pack_end(&packed[3]));
    assert(packer.has_state());

    const auto total_packed = [](const std::vector<std::vector<core::byte>>& v) {
        size_t t = 0;
        for (const auto& p : v) t += p.size();
        return t;
    };
    // (b) Cross-file compression actually materialized: each carried member
    // matches its predecessor's copy of the shared half down to ~nothing and
    // keeps only its incompressible noise half (plus stream overhead), while
    // the head and the post-break entry have no carried source and stay near
    // the raw size.
    assert(packed[1].size() < member_size / 2 + 8192);
    assert(packed[3].size() < member_size / 2 + 8192);
    assert(packed[0].size() > member_size - 8192);
    assert(packed[2].size() > member_size - 8192);
    std::cout << "[PASS] carried members realize cross-file compression (" << total_packed(packed)
              << " packed vs " << 4 * member_size << " raw)\n";

    // (a) Round-trip: decode with the carried-window decoder in the same
    // chain structure (head resets, members continue) and require identity.
    compress::Decompressor50 dec(1 << 20);
    for (size_t i = 0; i < 4; ++i) {
        const bool solid_member = (i == 1 || i == 3);
        std::vector<core::byte> out;
        out.resize(member_size);
        size_t written = 0;
        bool finished = false;
        auto sink = [&](const core::byte* data, size_t n) -> bool {
            if (written + n > out.size()) return false;
            std::memcpy(out.data() + written, data, n);
            written += n;
            return true;
        };
        assert(dec.decompress(packed[i].data(), packed[i].size(), member_size, solid_member, sink,
                              &written, &finished));
        assert(written == member_size);
        assert(out == members[i]);
    }
    std::cout << "[PASS] carried-window decode reproduces every byte\n";

    // v1.36.x regression: the E8/E8E9/ARM transform base resets PER MEMBER
    // even on solid chains (oracle-verified against a Rar 7.20-made solid
    // multi-member archive). The decoder used to seed the transform base
    // with the accumulated solid position, corrupting every non-first
    // member whose stream carries filter tokens (CRC-rejected end to end).
    // Member 1 primes the carried window (no filter); members 2..3 are
    // PE-like so compress_buffer emits E8-filtered streams whose filter
    // tokens hold member-local offsets — the decoder must apply them with
    // the per-member base.
    {
        const size_t prime_size = 256 * 1024;
        const size_t exe_size = 256 * 1024;
        auto pe_like = [](size_t n, unsigned seed) {
            std::vector<core::byte> v = make_noise(n, seed);
            v[0] = 'M';
            v[1] = 'Z';
            v[0x3C] = 0x80;
            v[0x3C + 1] = 0;
            v[0x3C + 2] = 0;
            v[0x3C + 3] = 0;
            v[0x80] = 'P';
            v[0x81] = 'E';
            v[0x82] = 0;
            v[0x83] = 0;
            v[0x84] = 0x64; // machine 0x8664 (x64) — E8/E8E9 class
            v[0x85] = 0x86;
            // Dense call pattern so the pretransform has real work.
            for (size_t i = 0x100; i + 5 <= n; i += 24) {
                v[i] = 0xE8;
                v[i + 1] = static_cast<core::byte>((i * 13) & 0xFF);
                v[i + 2] = static_cast<core::byte>((i >> 8) & 0xFF);
                v[i + 3] = 0x01;
                v[i + 4] = 0x00;
            }
            return v;
        };
        const std::vector<core::byte> member1 = make_noise(prime_size, 21);
        const std::vector<core::byte> member2 = pe_like(exe_size, 22);
        const std::vector<core::byte> member3 = pe_like(exe_size, 23);

        std::vector<core::byte> packed1, packed2, packed3;
        assert(compress::Compressor50::compress_buffer(member1.data(), member1.size(), packed1, 3,
                                                       1 << 20));
        assert(compress::Compressor50::compress_buffer(member2.data(), member2.size(), packed2, 3,
                                                       1 << 20));
        assert(compress::Compressor50::compress_buffer(member3.data(), member3.size(), packed3, 3,
                                                       1 << 20));

        compress::Decompressor50 sdec(1 << 20);
        const std::vector<core::byte>* plain[] = {&member1, &member2, &member3};
        const std::vector<core::byte>* pk[] = {&packed1, &packed2, &packed3};
        const size_t szs[] = {prime_size, exe_size, exe_size};
        for (size_t i = 0; i < 3; ++i) {
            std::vector<core::byte> out(szs[i]);
            size_t written = 0;
            bool finished = false;
            auto sink = [&](const core::byte* data, size_t n) -> bool {
                if (written + n > out.size()) return false;
                std::memcpy(out.data() + written, data, n);
                written += n;
                return true;
            };
            // Member 1 is the chain head; 2..3 ride the carried window.
            assert(sdec.decompress(pk[i]->data(), pk[i]->size(), szs[i], i > 0, sink, &written,
                                   &finished));
            assert(written == szs[i]);
            assert(out == *plain[i]);
        }
        std::cout << "[PASS] solid-chain E8 transform base resets per member\n";
    }

    // A fresh session's pack_begin(true) has nothing to carry and silently
    // packs fresh — the append-onto-existing-chain case (chain membership
    // without window state is always carry-neutral and safe).
    {
        compress::SolidPacker fresh(3, 1 << 20);
        std::vector<core::byte> out;
        assert(fresh.pack_begin(true));
        assert(fresh.pack_feed(members[0].data(), members[0].size()));
        assert(fresh.pack_end(&out));
        assert(out.size() > member_size - 8192); // head-like: nothing matched
    }

    // v1.31 M2: the binary-tree finder under the solid carry — m5 packs a
    // two-member chain through the tree (the child arrays ride the KEEP set
    // across pack_begin(true) exactly like head_/prev_); cross-file
    // compression must still materialize and every byte must roundtrip.
    {
        const size_t m5_size = 256 * 1024;
        const std::vector<core::byte> shared5 = make_noise(m5_size / 2, 21);
        std::vector<core::byte> a = make_noise(m5_size / 2, 31);
        a.insert(a.end(), shared5.begin(), shared5.end());
        std::vector<core::byte> b = make_noise(m5_size / 2, 32);
        b.insert(b.end(), shared5.begin(), shared5.end());

        compress::SolidPacker packer5(5, 1 << 19);
        std::vector<core::byte> pa, pb;
        assert(packer5.pack_begin(false));
        assert(packer5.pack_feed(a.data(), a.size()));
        assert(packer5.pack_end(&pa));
        assert(packer5.pack_begin(true));
        assert(packer5.pack_feed(b.data(), b.size()));
        assert(packer5.pack_end(&pb));
        // carried member realizes cross-file compression through the tree
        assert(pb.size() < m5_size / 2 + 8192);

        compress::Decompressor50 dec5(1 << 19);
        for (int which = 0; which < 2; ++which) {
            const auto& src = which == 0 ? a : b;
            const auto& pk = which == 0 ? pa : pb;
            std::vector<core::byte> out(m5_size);
            size_t written = 0;
            bool finished = false;
            auto sink = [&](const core::byte* data, size_t n) -> bool {
                if (written + n > out.size()) return false;
                std::memcpy(out.data() + written, data, n);
                written += n;
                return true;
            };
            assert(dec5.decompress(pk.data(), pk.size(), m5_size, which == 1, sink, &written,
                                   &finished));
            assert(written == m5_size);
            assert(out == src);
        }
    }

    std::cout << "All solid_packer_tests passed.\n";
    return 0;
}
