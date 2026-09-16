#include "rng.hpp"

#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)
#include <sys/random.h>
#endif
#endif

namespace openrar::crypto {

#if defined(_WIN32)

bool secure_random_bytes(core::byte* buf, size_t len) {
    if (len == 0) return true;
    if (buf == nullptr) return false;
    NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buf),
                                      static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0; // STATUS_SUCCESS
}

#else

// getentropy() caps at 256 bytes per call; loop for larger requests.
static bool try_getentropy(core::byte* buf, size_t len) {
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)
    while (len > 0) {
        size_t chunk = len > 256 ? 256 : len;
        if (getentropy(buf, chunk) != 0) return false;
        buf += chunk;
        len -= chunk;
    }
    return true;
#else
    (void)buf;
    (void)len;
    return false;
#endif
}

static bool try_urandom(core::byte* buf, size_t len) {
// O_CLOEXEC: the fd is short-lived, but a concurrent exec in the host
// process would otherwise inherit it. Older platforms that predate
// POSIX-2008 may not define the flag; fall back to plain open.
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (len > 0) {
        ssize_t n = ::read(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) {
            ::close(fd);
            return false;
        }
        buf += static_cast<size_t>(n);
        len -= static_cast<size_t>(n);
    }
    ::close(fd);
    return true;
}

bool secure_random_bytes(core::byte* buf, size_t len) {
    if (len == 0) return true;
    if (buf == nullptr) return false;
    if (try_getentropy(buf, len)) return true;
    return try_urandom(buf, len);
}

#endif

} // namespace openrar::crypto
