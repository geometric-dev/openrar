#ifndef OPENRAR_ARCHIVE_ARCHIVE_READER_HPP
#define OPENRAR_ARCHIVE_ARCHIVE_READER_HPP

#include "../core/types.hpp"
#include "../io/file_stream.hpp"
#include "../format/headers.hpp"
#include "archive_entry.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace openrar::compress {
class Decompressor50;
}

namespace openrar::archive {

class ArchiveReader {
public:
    ArchiveReader();
    ~ArchiveReader();

    bool open(const std::filesystem::path& arc_path, const std::string& password = "");
    void close();

    void set_password(const std::string& password) { password_ = password; }
    const std::string& password() const { return password_; }
    bool has_bad_password() const { return bad_password_; }

    bool is_open() const;
    bool is_locked() const;
    bool is_volume() const;
    bool is_solid() const;
    bool is_header_encrypted() const { return header_encrypted_; }
    const format::CryptBlock& header_crypt() const { return header_crypt_; }
    core::uint64 sfx_offset() const { return sfx_offset_; }

    // For testing: get the window size of the active solid chain (returns 0 if none)
    size_t test_get_solid_window_size() const;

    const format::MainBlock& main_block() const { return main_block_; }
    const std::vector<ArchiveEntry>& entries() const { return entries_; }

    // Test CRC32 of stored uncompressed or compressed payload
    bool test_entry(const ArchiveEntry& entry);

    // Extract entry (stored or compressed) directly to disk
    bool extract_entry(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                       const std::string& password = "");

    // Extract stored (method 0) entry directly to disk
    bool extract_store_entry(const ArchiveEntry& entry, const std::filesystem::path& dest_path,
                             const std::string& password = "");

    io::FileStream& stream() { return stream_; }
    bool read_packed_data(const ArchiveEntry& entry, std::vector<core::byte>& out) const;

private:
    bool scan_archive();
    static std::filesystem::path derive_next_volume_name(const std::filesystem::path& cur,
                                                         bool old_numbering);
    static std::filesystem::path derive_first_volume_name(const std::filesystem::path& cur,
                                                          bool old_numbering);

    // Decode a compressed (method != 0) entry payload, continuing the shared
    // LZ dictionary/huffman state across solid entries when required.

    // Shared solid/window/unpacker-selection logic used by both
    // decode_compressed overloads. `sel.unpacker` survives only until
    // `sel.local` is destroyed (the caller holds sel through the call).
    struct UnpackerSelection {
        compress::Decompressor50* unpacker = nullptr;
        bool solid = false;
        std::unique_ptr<compress::Decompressor50> local;
    };
    bool select_unpacker(const ArchiveEntry& entry, UnpackerSelection& sel);

    bool decode_compressed(const ArchiveEntry& entry, const core::byte* src, size_t src_size,
                           std::function<bool(const core::byte*, size_t)> flush_cb);

    bool decode_compressed(const ArchiveEntry& entry,
                           std::function<size_t(core::byte*, size_t)> src_cb, size_t src_size,
                           std::function<bool(const core::byte*, size_t)> flush_cb);

    io::FileStream stream_;
    std::filesystem::path path_;
    std::string password_;
    bool bad_password_{false};
    core::uint64 sfx_offset_{0};
    format::MainBlock main_block_;
    std::vector<ArchiveEntry> entries_;
    bool header_encrypted_{false};
    format::CryptBlock header_crypt_{};

    // Persistent LZ state for solid archives: solid entries decode against
    // the window contents, Huffman tables and repeat distances left behind by
    // the previous entry, so one Decompressor50 must survive across per-entry
    // calls. Null until the first compressed entry of a solid archive is
    // decoded. solid_chain_ok_ tracks whether the shared state actually
    // reflects the archive position (any skip/failure invalidates it).
    std::unique_ptr<compress::Decompressor50> solid_unpacker_;
    bool solid_chain_ok_{false};

    // M9: symlinks/junctions this reader created during the current
    // extraction session. convert_self_links() may replace only these with
    // real directories; pre-existing user links on the machine are never
    // touched (safe links-to-directories conversion semantics).
    std::vector<std::filesystem::path> links_created_;
    void convert_self_links(const std::filesystem::path& dest_path);
};

} // namespace openrar::archive

#endif // OPENRAR_ARCHIVE_ARCHIVE_READER_HPP
