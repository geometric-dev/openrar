cmake_minimum_required(VERSION 3.16)
# Release version - single source of truth (SemVer; PATCH is a monotonic
# commit counter since the current MAJOR.MINOR line's anchor tag - see
# docs/versioning.md).
# wasm/js/package.json and CHANGELOG.md are synced at release time.
# C is enabled for tests/unit/abi_header_c_compat.c — the C-mode compile proof
# of the frozen public C ABI (docs/abi-freeze.md); no production source is C.
project(openrar VERSION 1.48.0 LANGUAGES C CXX)

# Where the Windows CLI/SFX/worker binaries are written.
#
# CMAKE_SOURCE_DIR is the TOP-LEVEL source dir, so it resolves to a
# FetchContent/add_subdirectory CONSUMER's checkout, not ours: a subproject
# build used to drop openrar.exe (plus PDBs) into the consumer's source tree.
# See issue #2.
#
# The split is deliberate. Under a subproject we use CMAKE_BINARY_DIR, which
# is always a binary tree. Standalone we keep the historical
# <source>/build/openrar64, because that path is baked into the repo's own
# tooling — tools/gate.ps1, tools/interop_gate.py, tools/perf_matrix.py and
# ~10 more scripts locate the binary there, and CI configures with
# `cmake -B build` so CMAKE_BINARY_DIR would happen to match. A bare swap to
# CMAKE_BINARY_DIR would therefore be a silent regression for anyone building
# via a preset: the windows-msvc preset's binaryDir is build/msvc and the
# release preset's is build/release, so the exe would move out from under
# every one of those scripts. Overridable for anyone who wants the binary-tree
# layout unconditionally.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(OPENRAR_OUTPUT_ROOT "${CMAKE_SOURCE_DIR}/build/openrar64"
        CACHE PATH "Root directory for the Windows openrar binaries")
else()
    set(OPENRAR_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/openrar64"
        CACHE PATH "Root directory for the Windows openrar binaries")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The AES-256 and SHA-256 kernels take the ARMv8 crypto-intrinsic path
# whenever __aarch64__ is defined (see src/crypto/aes256.cpp / sha256.cpp).
# Generic GNU/Clang aarch64 targets don't enable the crypto feature by
# default (Apple clang does), so request it explicitly or vaeseq_u8 and
# friends fail to inline with "target specific option mismatch".
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64" AND NOT APPLE AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # C++ only: the armv8+crypto value is rejected for plain-C translation
    # units (abi_header_c_compat.c) and the crypto intrinsics are all C++.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-march=armv8-a+crypto>)
endif()

include(cmake/openrar_sources.cmake)

include(cmake/openrar_flags.cmake)

include(cmake/sanitizers.cmake)

include(cmake/openrar_natives.cmake)

# ── WASM targets (kept separate; only when emcmake) ─────────────────────────
if(EMSCRIPTEN)
    include(cmake/wasm.cmake)
endif()


include(cmake/openrar_dll.cmake)

include(cmake/openrar_tests.cmake)

include(cmake/openrar_install.cmake)


