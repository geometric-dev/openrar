#ifndef OPENRAR_IO_WIN32_META_HPP
#define OPENRAR_IO_WIN32_META_HPP

#include "../core/types.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace openrar::io {

// Alternate Data Streams (ADS, -os)
struct StreamEntry {
    std::string name; // Stream name including leading ':', e.g. ":Zone.Identifier"
    std::vector<core::byte> data;
};

// Security Descriptor ACLs (-ow)
struct AclEntry {
    std::vector<core::byte> descriptor; // Self-relative SECURITY_DESCRIPTOR blob
};

// Reparse Points & Symlinks (-ol)
enum class RedirType : core::uint8 {
    None = 0,
    UnixSymlink = 1,
    WinSymlink = 2,
    Junction = 3,
    Hardlink = 4,
    FileCopy = 5
};

struct RedirEntry {
    RedirType type;
    std::string target;
    bool is_directory;
};

// Read all Alternate Data Streams for a file (Windows NTFS only)
bool read_alternate_streams(const std::filesystem::path& path,
                            std::vector<StreamEntry>& out_streams);

// Restore an Alternate Data Stream (Windows NTFS only)
bool write_alternate_stream(const std::filesystem::path& host_file, const std::string& stream_name,
                            const void* data, size_t size);

// Read Security Descriptor (Windows only)
bool read_security_descriptor(const std::filesystem::path& path, std::vector<core::byte>& out_sd);

// Write Security Descriptor (Windows only)
bool write_security_descriptor(const std::filesystem::path& path, const void* sd_data,
                               size_t sd_size);

// Read Reparse Point / Symlink target
bool read_reparse_info(const std::filesystem::path& path, RedirEntry& out_redir);

// Create Reparse Point / Symlink link
bool create_reparse_link(const std::filesystem::path& link_path, const RedirEntry& redir);

} // namespace openrar::io

#endif // OPENRAR_IO_WIN32_META_HPP
