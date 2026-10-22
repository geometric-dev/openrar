#ifndef OPENRAR_CRYPTO_BLAKE2SP_HPP
#define OPENRAR_CRYPTO_BLAKE2SP_HPP

#include "../core/types.hpp"
#include <array>
#include <cstddef>

namespace openrar::crypto {

// Clean-room C++17 implementation of BLAKE2sp based on RFC 7693
class Blake2sp {
public:
    static constexpr size_t DIGEST_SIZE = 32;
    static constexpr size_t BLOCK_SIZE = 64;
    static constexpr size_t PARALLELISM = 8;

    Blake2sp();
    ~Blake2sp() = default;

    void reset();
    void update(const void* data, size_t size);
    void finish(void* out_digest);

    // Convenience one-shot function
    static void compute(const void* data, size_t size, void* out_digest);

private:
    struct Blake2sState {
        core::uint32 h_[8];
        core::uint32 t_[2];
        core::uint32 f_[2];
        core::byte buf_[2 * BLOCK_SIZE];
        size_t buflen_{0};
        bool last_node_{false};

        void init(core::uint32 node_offset, core::uint32 node_depth);
        void update(const core::byte* in, size_t inlen);
        void compress(const core::byte* block);
        void finish(core::byte* out);
    };

    // H-14: 8-way AVX2 bulk leaf update (x86-64 only; defined and called
    // only when the build has the kernel and the CPU reports AVX2).
    void update_bulk_avx2(const core::byte* in, size_t chunks);

    Blake2sState states_[PARALLELISM];
    Blake2sState root_state_;
    core::byte buf_[PARALLELISM * BLOCK_SIZE];
    size_t buflen_{0};
};

} // namespace openrar::crypto

#endif // OPENRAR_CRYPTO_BLAKE2SP_HPP
