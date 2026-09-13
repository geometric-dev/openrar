#ifndef OPENRAR_FORMAT_HEADER_WRITER_HPP
#define OPENRAR_FORMAT_HEADER_WRITER_HPP

#include "headers.hpp"
#include "../io/file_stream.hpp"
#include "../crypto/pbkdf2.hpp"
#include <string>
#include <vector>

namespace openrar::format {

// AES-256-CBC encryptor for RAR5 archive headers (-hp). Each encrypted
// header is stored as a fresh 16-byte clear IV followed by the ciphertext
// of the plaintext [CRC32 | size vint | body] zero-padded to the 16-byte
// cipher block boundary. The key is derived once from the archive password
// and the salt carried by the (never encrypted) HEAD_CRYPT block; every
// header encrypted afterwards shares that key.
struct HeaderCryptWriter {
    bool active{false};
    crypto::Rar5Keys keys_{};

    // Fresh archive: generate a random salt, derive keys at 2^15 PBKDF2
    // rounds (standard default) and fill `out_crypt` for HEAD_CRYPT emission.
    bool init_new(const std::string& password, CryptBlock& out_crypt);

    // Existing archive: re-derive keys from the stored HEAD_CRYPT params so
    // appended headers decrypt with the same key as the copied old ones.
    bool init_existing(const std::string& password, const CryptBlock& crypt);

    // Encrypt and write one wrapped plaintext block ([CRC32|size vint|body]).
    bool write_block(io::FileStream& dest, const std::vector<core::byte>& wrapped);
};

class HeaderWriter {
public:
    // Write 8-byte RAR 5.0 archive signature
    static bool write_signature(io::FileStream& dest);

    // Format & write archive blocks. When `crypt` is active each block is
    // AES-256-CBC encrypted (see HeaderCryptWriter); HEAD_CRYPT itself is
    // always written in clear.
    static bool write_main_block(io::FileStream& dest, const MainBlock& block,
                                 HeaderCryptWriter* crypt = nullptr);
    static bool write_file_block(io::FileStream& dest, const FileBlock& block);
    static bool write_file_block(io::FileStream& dest, const FileBlock& block,
                                 core::uint64 extra_head_flags, HeaderCryptWriter* crypt = nullptr);
    static bool write_crypt_block(io::FileStream& dest, const CryptBlock& block);
    static bool write_end_block(io::FileStream& dest, const EndArcBlock& block,
                                HeaderCryptWriter* crypt = nullptr);

    // Serialize body and wrap in [CRC32:4][vint(body_size)][body]
    static std::vector<core::byte> wrap_block(const std::vector<core::byte>& body);

    // Write an already-wrapped block, encrypting it first when requested.
    static bool emit_block(io::FileStream& dest, const std::vector<core::byte>& wrapped,
                           HeaderCryptWriter* crypt);
};

} // namespace openrar::format

#endif // OPENRAR_FORMAT_HEADER_WRITER_HPP
