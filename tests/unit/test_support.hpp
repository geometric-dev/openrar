#ifndef OPENRAR_TESTS_TEST_SUPPORT_HPP
#define OPENRAR_TESTS_TEST_SUPPORT_HPP

// Shared helpers for the assert-based unit suites.

// 1. Assert routing.
// Under ctest (piped stdio) the MSVC default for _CRT_ASSERT is a modal
// dialog, which silently hangs the test process forever while ctest moves
// on, leaving file locks behind. Route assert failures to stderr instead.
#ifdef _MSC_VER
#include <crtdbg.h>
#define OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR()                                                       \
    do {                                                                                           \
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);                                         \
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);                                       \
    } while (0)
#else
#define OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR()                                                       \
    do {                                                                                           \
    } while (0)
#endif

// 2. Per-suite scratch sandbox.
// Suites used to write CWD-relative "build/..." paths. Under ctest the
// working directory is the build dir, so those paths landed in
// <build>/build/... and every suite silently depended on compress_tests
// (first in ctest order) creating that directory — single-suite ctest -R
// and parallel ctest -j were broken. All file artifacts must now live in
// a per-suite sandbox under the system temp directory: independent of the
// working directory, of other suites, and safe under -j.
#include <filesystem>
#include <string>

namespace openrar::test {

inline std::filesystem::path scratch_root() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "openrar_tests";
    std::filesystem::create_directories(root);
    return root;
}

// Per-suite sandbox directory, created on demand. Any stale artifacts
// from a previous (crashed) run are wiped so tests never see leftovers.
inline std::filesystem::path scratch_dir(const std::string& suite) {
    std::filesystem::path dir = scratch_root() / suite;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace openrar::test

#endif // OPENRAR_TESTS_TEST_SUPPORT_HPP
