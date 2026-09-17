#include "win32_meta.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <aclapi.h>
#pragma comment(lib, "advapi32.lib")

namespace openrar::io {

namespace {

std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &result[0],
                        size_needed, nullptr, nullptr);
    return result;
}

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed =
        MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &result[0],
                        size_needed);
    return result;
}

bool is_prohibited_stream(const std::string& stream_name) {
    // ColonCount > 1 rule (leading ':' is expected, second colon is prohibited)
    int colons = 0;
    for (char c : stream_name) {
        if (c == ':') colons++;
        if (c == '/' || c == '\\') return true;
    }
    return colons > 1;
}

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

// Reparse buffer structures
typedef struct _REPARSE_DATA_BUFFER {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    };
} REPARSE_DATA_BUFFER;

} // namespace

bool read_alternate_streams(const std::filesystem::path& path,
                            std::vector<StreamEntry>& out_streams) {
    out_streams.clear();
    std::wstring host_w = path.wstring();

    WIN32_FIND_STREAM_DATA fsd;
    HANDLE hFind = FindFirstStreamW(host_w.c_str(), FindStreamInfoStandard, &fsd, 0);
    if (hFind == INVALID_HANDLE_VALUE) {
        return false;
    }

    do {
        std::wstring raw_name = fsd.cStreamName;
        // Skip default stream ::$DATA
        if (_wcsicmp(raw_name.c_str(), L"::$DATA") == 0) {
            continue;
        }

        // Strip trailing ":$DATA"
        std::string stream_name = wide_to_utf8(raw_name);
        const std::string data_suffix = ":$DATA";
        if (stream_name.size() >= data_suffix.size()) {
            size_t pos = stream_name.size() - data_suffix.size();
            std::string end_part = stream_name.substr(pos);
            if (_stricmp(end_part.c_str(), data_suffix.c_str()) == 0) {
                stream_name = stream_name.substr(0, pos);
            }
        }

        if (stream_name.empty() || stream_name == ":" || is_prohibited_stream(stream_name)) {
            continue;
        }

        // 1 GiB sanity limit (0x40000000)
        if (static_cast<core::uint64>(fsd.StreamSize.QuadPart) > 0x40000000ULL) {
            continue;
        }

        // Read stream data
        std::wstring full_stream_path = host_w + utf8_to_wide(stream_name);
        HANDLE stream_handle =
            CreateFileW(full_stream_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (stream_handle != INVALID_HANDLE_VALUE) {
            size_t size = static_cast<size_t>(fsd.StreamSize.QuadPart);
            StreamEntry entry;
            entry.name = stream_name;
            entry.data.resize(size);

            DWORD bytes_read = 0;
            if (size == 0 || ReadFile(stream_handle, entry.data.data(), static_cast<DWORD>(size),
                                      &bytes_read, nullptr)) {
                entry.data.resize(bytes_read);
                out_streams.push_back(std::move(entry));
            }
            CloseHandle(stream_handle);
        }
    } while (FindNextStreamW(hFind, &fsd));

    FindClose(hFind);
    return true;
}

bool write_alternate_stream(const std::filesystem::path& host_file, const std::string& stream_name,
                            const void* data, size_t size) {
    if (stream_name.empty() || stream_name[0] != ':' || is_prohibited_stream(stream_name)) {
        return false;
    }

    std::wstring full_path = host_file.wstring() + utf8_to_wide(stream_name);
    DWORD orig_attrs = GetFileAttributesW(host_file.wstring().c_str());
    bool was_readonly = (orig_attrs != INVALID_FILE_ATTRIBUTES && (orig_attrs & FILE_ATTRIBUTE_READONLY));
    if (was_readonly) {
        SetFileAttributesW(host_file.wstring().c_str(), orig_attrs & ~FILE_ATTRIBUTE_READONLY);
    }

    HANDLE stream_handle = CreateFileW(full_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (was_readonly) {
        SetFileAttributesW(host_file.wstring().c_str(), orig_attrs);
    }

    if (stream_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes_written = 0;
    BOOL ok = TRUE;
    if (size > 0) {
        ok = WriteFile(stream_handle, data, static_cast<DWORD>(size), &bytes_written, nullptr);
    }
    CloseHandle(stream_handle);
    return ok && (bytes_written == size);
}

bool read_security_descriptor(const std::filesystem::path& path, std::vector<core::byte>& out_sd) {
    out_sd.clear();
    SECURITY_INFORMATION si =
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION;

    DWORD needed = 0;
    GetFileSecurityW(path.wstring().c_str(), si, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0 || needed > (1 << 20)) {
        return false;
    }

    out_sd.resize(needed);
    if (!GetFileSecurityW(path.wstring().c_str(), si,
                          reinterpret_cast<PSECURITY_DESCRIPTOR>(out_sd.data()), needed, &needed)) {
        out_sd.clear();
        return false;
    }
    out_sd.resize(needed);
    return true;
}

bool write_security_descriptor(const std::filesystem::path& path, const void* sd_data,
                               size_t sd_size) {
    if (sd_data == nullptr || sd_size == 0) return false;
    SECURITY_INFORMATION si =
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION;
    return SetFileSecurityW(path.wstring().c_str(), si,
                            const_cast<PSECURITY_DESCRIPTOR>(sd_data)) != FALSE;
}

bool read_reparse_info(const std::filesystem::path& path, RedirEntry& out_redir) {
    // INTENTIONALLY UNWIRED: this parser has no callers (reserved for future
    // symlink metadata display). Do NOT wire it up without bounds hardening
    // first (report INFO 13): PrintNameOffset/PrintNameLength and the
    // Substitute* pair are unvalidated u16 offsets from the reparse data,
    // used here against a 16 KiB stack buffer.
    out_redir.type = RedirType::None;
    out_redir.target.clear();
    out_redir.is_directory = false;

    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    core::byte buf[16384];
    DWORD bytes_returned = 0;
    BOOL res = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf, sizeof(buf),
                               &bytes_returned, nullptr);
    CloseHandle(h);

    if (!res || bytes_returned < sizeof(ULONG)) {
        return false;
    }

    const auto* rdb = reinterpret_cast<const REPARSE_DATA_BUFFER*>(buf);
    if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        out_redir.type = RedirType::Junction;
        out_redir.is_directory = true;
        const auto& mp = rdb->MountPointReparseBuffer;
        const wchar_t* p = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const char*>(mp.PathBuffer) + mp.PrintNameOffset);
        size_t len = mp.PrintNameLength / sizeof(wchar_t);
        if (len == 0) {
            p = reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(mp.PathBuffer) +
                                                 mp.SubstituteNameOffset);
            len = mp.SubstituteNameLength / sizeof(wchar_t);
        }
        out_redir.target = wide_to_utf8(std::wstring(p, len));
        return true;
    } else if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        out_redir.type = RedirType::WinSymlink;
        out_redir.is_directory = std::filesystem::is_directory(path);
        const auto& sl = rdb->SymbolicLinkReparseBuffer;
        const wchar_t* p = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const char*>(sl.PathBuffer) + sl.PrintNameOffset);
        size_t len = sl.PrintNameLength / sizeof(wchar_t);
        if (len == 0) {
            p = reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(sl.PathBuffer) +
                                                 sl.SubstituteNameOffset);
            len = sl.SubstituteNameLength / sizeof(wchar_t);
        }
        out_redir.target = wide_to_utf8(std::wstring(p, len));
        return true;
    }

    return false;
}

bool create_reparse_link(const std::filesystem::path& link_path, const RedirEntry& redir) {
    std::wstring link_w = link_path.wstring();
    std::wstring target_w = utf8_to_wide(redir.target);

    if (redir.type == RedirType::WinSymlink || redir.type == RedirType::UnixSymlink) {
        DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (redir.is_directory) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
        return CreateSymbolicLinkW(link_w.c_str(), target_w.c_str(), flags) != FALSE;
    }
    return false;
}

} // namespace openrar::io

#else

namespace openrar::io {

bool read_alternate_streams(const std::filesystem::path&, std::vector<StreamEntry>&) {
    return false;
}
bool write_alternate_stream(const std::filesystem::path&, const std::string&, const void*, size_t) {
    return false;
}
bool read_security_descriptor(const std::filesystem::path&, std::vector<core::byte>&) {
    return false;
}
bool write_security_descriptor(const std::filesystem::path&, const void*, size_t) {
    return false;
}
bool read_reparse_info(const std::filesystem::path&, RedirEntry&) {
    return false;
}
bool create_reparse_link(const std::filesystem::path&, const RedirEntry&) {
    return false;
}

} // namespace openrar::io

#endif
