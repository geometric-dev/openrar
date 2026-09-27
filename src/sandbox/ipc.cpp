#include "ipc.hpp"

#include <cstring>

namespace openrar::sandbox {

namespace {

bool type_registered(core::uint8 t) {
    return t >= static_cast<core::uint8>(FrameType::Ping) &&
           t <= static_cast<core::uint8>(FrameType::SelftestResult);
}

} // namespace

bool encode_frame(const Frame& f, std::vector<core::byte>& out) {
    const core::uint8 t = static_cast<core::uint8>(f.type);
    if (!type_registered(t)) return false;
    if (f.payload.size() > kMaxFramePayload) return false;
    const core::uint32 len = static_cast<core::uint32>(f.payload.size());
    core::byte header[kFrameHeaderSize];
    std::memcpy(header, &len, sizeof(len)); // little-endian hosts only: the
    // protocol runs over the worker's local process pair on x86-64/aarch64
    // (both little-endian); a big-endian target would need explicit LE
    // serialization here and in feed() — pinned by the wasm32 layout rules
    // of the shared contract (see abi-freeze.md §3 note on endianness).
    header[4] = t;
    out.insert(out.end(), header, header + kFrameHeaderSize);
    out.insert(out.end(), f.payload.begin(), f.payload.end());
    return true;
}

FeedResult FrameReader::feed(const core::byte* data, size_t len, std::vector<Frame>& out) {
    if (poisoned_) return FeedResult::Poisoned;

    size_t pos = 0;
    while (true) {
        if (!header_done_) {
            const size_t need = kFrameHeaderSize - pending_.size();
            const size_t take = (len - pos) < need ? (len - pos) : need;
            pending_.insert(pending_.end(), data + pos, data + pos + take);
            pos += take;
            if (pending_.size() < kFrameHeaderSize) {
                return pos == len ? FeedResult::Incomplete : FeedResult::Ok;
            }
            core::uint32 len32;
            std::memcpy(&len32, pending_.data(), sizeof(len32));
            const core::uint8 t = static_cast<core::uint8>(pending_[4]);
            if (!type_registered(t)) {
                poisoned_ = true;
                pending_.clear();
                return FeedResult::Poisoned;
            }
            if (len32 > kMaxFramePayload) {
                poisoned_ = true;
                pending_.clear();
                return FeedResult::Poisoned;
            }
            frame_len_ = len32;
            frame_type_ = static_cast<FrameType>(t);
            pending_.clear();
            header_done_ = true;
            if (frame_len_ == 0) {
                out.push_back(Frame{frame_type_, {}});
                header_done_ = false;
                if (pos == len) return FeedResult::Ok;
                continue;
            }
            pending_.reserve(frame_len_); // ≤ 4 MiB, only after the length passed the check
        }

        const size_t need = frame_len_ - pending_.size();
        const size_t take = (len - pos) < need ? (len - pos) : need;
        pending_.insert(pending_.end(), data + pos, data + pos + take);
        pos += take;
        if (pending_.size() < frame_len_) {
            return pos == len ? FeedResult::Incomplete : FeedResult::Ok;
        }
        out.push_back(Frame{frame_type_, std::move(pending_)});
        // pending_ stays moved-from with capacity 0 here; small header
        // frames re-fill it cheaply and DataChunk frames reserve exactly
        // what the (validated ≤ 4 MiB) length claims — one allocation per
        // received frame, none before the length check passes.
        header_done_ = false;
        if (pos == len) return FeedResult::Ok;
    }
}

} // namespace openrar::sandbox
