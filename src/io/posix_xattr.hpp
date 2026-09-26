#ifndef OPENRAR_IO_POSIX_XATTR_HPP
#define OPENRAR_IO_POSIX_XATTR_HPP

#include "../core/types.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace openrar::io {

// POSIX/macOS extended attributes (v1.27, -ox). All access uses the
// no-follow l-variants — capture and restore never traverse symlinks,
// mirroring the lstat/lchown metadata pattern (v1.27 plan §1.2).
//
// Windows/WASM builds compile to empty stubs: the FORMAT layer
// (FHEXTRA_XATTR) parses and re-emits records on every OS; capture and
// restore are platform-gated.
//
// Contract (fail-soft by design — plan §3 FMM row C):
//   list_xattrs -> false  = xattrs do not exist here (ENOTSUP/ENOSYS) or
//                           the name list could not be read completely
//                           (EPERM, ERANGE beyond the probe cap); the
//                           caller treats this as "no attributes".
//   true + possibly-empty list = the complete attribute list.
//   get_xattr / set_xattr -> false on ANY failure; the caller skips and
//                           reports, the operation never fails the add.

// Lists attribute names for path (no-follow). Filesystems without xattr
// support yield false; callers treat it as "no attributes".
bool list_xattrs(const std::filesystem::path& path, std::vector<std::string>& out_names);

// Reads one attribute's value (no-follow).
bool get_xattr(const std::filesystem::path& path, const std::string& name,
               std::vector<core::byte>& out_value);

// Writes one attribute, create-or-replace (no-follow).
bool set_xattr(const std::filesystem::path& path, const std::string& name, const core::byte* data,
               size_t size);

} // namespace openrar::io

#endif // OPENRAR_IO_POSIX_XATTR_HPP
