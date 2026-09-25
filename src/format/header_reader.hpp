#ifndef OPENRAR_FORMAT_HEADER_READER_HPP
#define OPENRAR_FORMAT_HEADER_READER_HPP

#include "headers.hpp"
#include "../io/file_stream.hpp"
#include "../crypto/pbkdf2.hpp"
#include <string>
#include <vector>

namespace openrar::format {

enum class HeaderResult { Ok, Eof, Error, HeaderCrcMismatch };

// AES-256-CBC decryptor state for RAR5 encrypted headers (-hp). Each
// encrypted header on disk is a fresh 16-byte clear IV followed by the
// ciphertext of the plaintext [CRC32 | size vint | body] zero-padded to the
// 16-byte cipher block boundary; the key comes from the archive password
// plus the salt carried by the (never encrypted) HEAD_CRYPT block.
struct HeaderCryptReader {
    bool active{false};
    bool bad_password{false};
    crypto::Rar5Keys keys{};

    // Derive keys from the HEAD_CRYPT parameters. Fails (without setting
    // bad_password) on unknown versions. When the optional PswCheck value is
    // present and self-consistent, a mismatch with the derived value sets
    // bad_password and fails.
    bool init(const std::string& password, const CryptBlock& crypt);
    ~HeaderCryptReader() noexcept { keys.wipe(); }
};

class HeaderReader {
public:
    // Read and verify 8-byte RAR 5.0 archive signature
    static bool read_signature(io::FileStream& src);

    // Read next raw block from stream with CRC validation. When `crypt` is
    // non-null and active, the block is decrypted first (see
    // HeaderCryptReader); HEAD_CRYPT itself is stored in clear and is read
    // with an inactive crypt.
    // v1.25: accepts any ReadSource (FileStream buffered, MappedFile
    // mapped) — one scan code path for both engines.
    static HeaderResult read_block_raw(io::ReadSource& src, core::uint64& out_type,
                                       core::uint64& out_flags, std::vector<core::byte>& out_body,
                                       core::uint64& out_data_size,
                                       HeaderCryptReader* crypt = nullptr);

    // Read next raw block from memory buffer with CRC validation
    static HeaderResult read_block_raw_mem(const core::byte* data, size_t size, size_t& offset,
                                           core::uint64& out_type, core::uint64& out_flags,
                                           std::vector<core::byte>& out_body,
                                           core::uint64& out_data_size);

    // Parse strongly typed header representations
    static bool parse_main_header(const core::byte* body, size_t body_size, MainBlock& out_block);
    static bool parse_file_header(const core::byte* body, size_t body_size, FileBlock& out_block);
    static bool parse_crypt_header(const core::byte* body, size_t body_size, CryptBlock& out_block);
    static bool parse_end_header(const core::byte* body, size_t body_size, EndArcBlock& out_block);
};

} // namespace openrar::format

#endif // OPENRAR_FORMAT_HEADER_READER_HPP
