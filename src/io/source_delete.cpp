#include "source_delete.hpp"

#include <chrono>
#include <cstdio>
#if !defined(_WIN32)
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace openrar::io {

namespace {

// -dw's documented sequence: zero overwrite → truncate → temp-name → delete.
// The rename defeats name-based recovery tools before the unlink; the zero
// pass defeats in-place recovery of the content. Best-effort by contract.
bool wipe_delete(const std::filesystem::path& path, std::error_code& ec) {
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!f) {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }
        f.seekg(0, std::ios::end);
        const std::streamoff len = f.tellg();
        f.seekp(0, std::ios::beg);
        std::vector<char> zeros(64 * 1024, 0);
        std::streamoff done = 0;
        while (done < len) {
            const std::streamoff take = (len - done) < (std::streamoff)zeros.size()
                                            ? (len - done)
                                            : (std::streamoff)zeros.size();
            f.write(zeros.data(), take);
            if (!f) {
                ec = std::make_error_code(std::errc::io_error);
                return false;
            }
            done += take;
        }
        f.flush();
        f.close();
    }
    std::filesystem::resize_file(path, 0, ec);
    if (ec) return false;

    // Temp-name: random hex suffix in the SAME directory (same volume), the
    // journal-safe shape is irrelevant here — the file is deleted next.
    // Uniqueness only (not secrecy): pid + a tick counter — the crypto RNG
    // lives two layers UP from io (layer_check: downward-only includes).
    const unsigned seed =
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        static_cast<unsigned>(getpid());
    std::filesystem::path renamed = path;
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::string suffix;
        unsigned x = seed + static_cast<unsigned>(attempt) * 2654435761u;
        for (int i = 0; i < 8; ++i) {
            x = x * 1103515245u + 12345u;
            char hb[3];
            std::snprintf(hb, sizeof(hb), "%02x", (x >> 16) & 0xFF);
            suffix += hb;
        }
        std::filesystem::path candidate =
            path.parent_path() / ("." + path.filename().string() + ".wipe." + suffix);
        std::error_code rec;
        std::filesystem::rename(path, candidate, rec);
        if (!rec) {
            renamed = candidate;
            break;
        }
    }
    std::filesystem::remove(renamed, ec);
    return !ec;
}

#if defined(_WIN32)
bool recycle_delete_windows(const std::filesystem::path& path, std::error_code& ec) {
    // SHFileOperationW wants a DOUBLE-NUL-terminated list.
    std::wstring from = path.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = nullptr;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int r = SHFileOperationW(&op);
    if (r != 0 || op.fAnyOperationsAborted) {
        ec = std::make_error_code(std::errc::operation_not_permitted);
        return false;
    }
    return true;
}
#endif

} // namespace

bool delete_source_securely(const std::filesystem::path& path, SourceDeleteMode mode,
                            std::error_code& ec) {
    if (mode == SourceDeleteMode::Wipe) return wipe_delete(path, ec);
#if defined(_WIN32)
    if (mode == SourceDeleteMode::Recycle) return recycle_delete_windows(path, ec);
#else
        // POSIX has no recycle bin: degrade to Plain (the CLI surfaces the note).
#endif
    std::filesystem::remove(path, ec);
    return !ec;
}

} // namespace openrar::io
