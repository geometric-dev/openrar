#include "solid_packer.hpp"

namespace openrar::compress {

bool SolidPacker::pack_begin(bool chain_member) {
    // A carry is only possible when the window actually holds previously
    // packed bytes (fresh creates, appends onto an existing solid archive's
    // chain, and post-break entries all start fresh — a self-contained
    // encode is carry-neutral for the decoder, so this is always safe).
    const bool carry = chain_member && has_state_;
    return enc_.pack_solid_begin(carry);
}

bool SolidPacker::pack_feed(const core::byte* data, size_t n) {
    return enc_.feed(data, n);
}

bool SolidPacker::pack_end(std::vector<core::byte>* out) {
    if (!enc_.pack_solid_finish()) return false;
    if (out) {
        if (!enc_.take_output(*out)) return false;
    }
    has_state_ = true;
    return true;
}

} // namespace openrar::compress
