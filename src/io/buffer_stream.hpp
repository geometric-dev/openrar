#ifndef OPENRAR_IO_BUFFER_STREAM_HPP
#define OPENRAR_IO_BUFFER_STREAM_HPP

#include "../core/types.hpp"
#include "file_stream.hpp" // SeekOrigin, resolve_seek
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openrar::io {

class BufferStream {
public:
    BufferStream() = default;
    explicit BufferStream(const core::byte* data, size_t size) { set(data, size); }
    explicit BufferStream(const std::vector<core::byte>& buf) { set(buf.data(), buf.size()); }

    void set(const core::byte* data, size_t size) {
        data_ = data;
        size_ = size;
        pos_ = 0;
    }
    void set(const char* data, size_t size) {
        data_ = reinterpret_cast<const core::byte*>(data);
        size_ = size;
        pos_ = 0;
    }

    bool is_open() const { return data_ != nullptr; }
    size_t size() const { return size_; }
    core::uint64 tell() const { return pos_; }
    size_t remaining() const { return pos_ <= size_ ? size_ - static_cast<size_t>(pos_) : 0; }

    size_t read(void* dst, size_t n) {
        if (!data_ || n == 0) return 0;
        size_t avail = remaining();
        if (n > avail) n = avail;
        if (n == 0) return 0;
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return n;
    }

    bool seek(int64_t off, SeekOrigin origin) {
        // M-05: unsigned resolution - pos_ + off can no longer overflow int64.
        core::uint64 next = 0;
        if (!resolve_seek(pos_, size_, off, origin, next)) return false;
        pos_ = next;
        return true;
    }

    void reset() { pos_ = 0; }
    const core::byte* data() const { return data_; }

private:
    const core::byte* data_{nullptr};
    size_t size_{0};
    core::uint64 pos_{0};
};

} // namespace openrar::io

#endif // OPENRAR_IO_BUFFER_STREAM_HPP
