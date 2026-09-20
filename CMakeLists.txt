cmake_minimum_required(VERSION 3.16)
# Release version - single source of truth (SemVer; PATCH is a monotonic
# commit counter since the current MAJOR.MINOR line's anchor tag - see
# docs/versioning.md).
# wasm/js/package.json and CHANGELOG.md are synced at release time.
project(openrar VERSION 1.17.0 LANGUAGES CXX)

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
    add_compile_options(-march=armv8-a+crypto)
endif()

# ── Core sources (shared by native + wasm) — keep separate so wasm can reuse without CLI ──
# These are linked into every native target AND every wasm target.
# The in-memory archive layer (used by the DLL and the WASM archive API, not
# by the CLI) lives behind the OPENRAR_INMEM_ARCHIVE option instead.
set(OPENRAR_CORE_SOURCES
    src/core/cpu.cpp
    src/core/vint.cpp
    src/crypto/aes256.cpp
    src/crypto/blake2sp.cpp
    src/crypto/crc32.cpp
    src/crypto/crc64.cpp
    src/crypto/pbkdf2.cpp
    src/crypto/rng.cpp
    src/crypto/sha256.cpp
    src/io/file_stream.cpp
    src/io/path_util.cpp
    src/io/win32_meta.cpp
    src/format/header_reader.cpp
    src/format/header_writer.cpp
    src/compress/filters50.cpp
    src/compress/compressor50.cpp
    src/compress/decompressor50.cpp
    src/compress/stream_encoder.cpp
    src/compress/stream_decoder.cpp
    src/archive/archive_reader.cpp
    src/archive/archive_mutator.cpp
    src/archive/volume.cpp
    src/recovery/rs16.cpp
    src/recovery/recovery_record.cpp
    src/recovery/recovery_writer.cpp
)

# ── WASM-archive source list (excluded from default builds) ──────────────────
# `buffer_archive.cpp` is the in-memory RAR5 reader/writer used by the WASM
# archive API. It is NOT built into:
#   - the standard native CLI (which never calls it — it has the file-based
#     archive reader/mutator for that),
#   - the default WASM block-codec build (which must stay <= 500 KiB per
#     `docs/wasm-limitations.md` §5.3).
# Enable with `-DOPENRAR_INMEM_ARCHIVE=ON` to wire it into either target.
set(OPENRAR_INMEM_ARCHIVE_SOURCES
    src/archive/buffer_archive.cpp
)

option(OPENRAR_INMEM_ARCHIVE "Include the in-memory archive layer (buffer_archive): used by the DLL and the WASM archive API, and registers their native tests. Excluded from the CLI binary and the lean block-codec wasm build." OFF)

# Warning policy: MSVC /W4, GCC/Clang -Wall -Wextra everywhere. Warnings are
# not errors by default; CI turns them on for one gate leg via this option
# (see .github/workflows/build.yml and CONTRIBUTING.md §Warnings).
option(OPENRAR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (/WX, -Werror)" OFF)

# ── DLL sources (always built for SHARED target) ─────────────────────────────
set(OPENRAR_DLL_SOURCES
    src/archive/buffer_archive.cpp
    src/compress/stream_encoder.cpp
    src/compress/stream_decoder.cpp
    src/dll/dll_api.cpp
)

# Generate openrar/version.h from template
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/openrar/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/openrar/version.h"
    @ONLY)

set(OPENRAR_INCLUDES
    src
    src/archive
    src/cli
    src/compress
    src/core
    src/crypto
    src/format
    src/io
    src/recovery
    src/dll
    include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include/openrar
)

# Same dirs wrapped for export: build-interface keeps source-tree paths out of
# the installed CMake package (R3), which only exposes include/ to consumers.
set(OPENRAR_EXPORT_INCLUDES "")
foreach(_inc ${OPENRAR_INCLUDES})
    if(IS_ABSOLUTE "${_inc}")
        list(APPEND OPENRAR_EXPORT_INCLUDES
            "$<BUILD_INTERFACE:${_inc}>")
    else()
        list(APPEND OPENRAR_EXPORT_INCLUDES
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${_inc}>")
    endif()
endforeach()

# ── Native platform flags (NOT applied when targeting Emscripten) ─────────────
if(EMSCRIPTEN)
    # Emscripten defines __EMSCRIPTEN__; keep native flags off this path.
    # RAR_SMP / pthreads are disabled for wasm by default (SharedArrayBuffer needs COOP/COEP).
    add_compile_definitions(_FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
    add_compile_options(-Wall -Wextra)
    # Optional wasm pthreads: pass -DOPENRAR_WASM_THREADS=ON + -pthread link flags.
    option(OPENRAR_WASM_THREADS "Enable pthreads for WASM (requires COOP/COEP)" OFF)
    if(OPENRAR_WASM_THREADS)
        add_compile_options(-pthread)
        add_link_options(-pthread -sUSE_PTHREADS=1 -sPTHREAD_POOL_SIZE=4)
        add_compile_definitions(RAR_SMP)
    endif()
    # Optional wasm SIMD128: pass -DOPENRAR_WASM_SIMD=ON. Opt-in so the
    # baseline module keeps older-browser compatibility and its size budget;
    # enables -msimd128 globally and the SIMD match finder in match_simd.hpp.
    option(OPENRAR_WASM_SIMD "Enable WASM SIMD128 (-msimd128) kernels" OFF)
    if(OPENRAR_WASM_SIMD)
        add_compile_options(-msimd128)
        add_compile_definitions(OPENRAR_WASM_SIMD=1)
    endif()
elseif(WIN32)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    if(MSVC)
        add_compile_options(/utf-8 /W4 "$<$<CONFIG:Release>:/O2>" "$<$<CONFIG:Release>:/Oi>" "$<$<CONFIG:Release>:/Ot>" "$<$<CONFIG:Release>:/GL>")
        add_link_options("$<$<CONFIG:Release>:/LTCG>")
    endif()
else()
    add_compile_definitions(_FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE RAR_SMP)
    add_compile_options(-Wall -Wextra "$<$<CONFIG:Release>:-O3>" "$<$<CONFIG:Release>:-fomit-frame-pointer>")
endif()

if(OPENRAR_WARNINGS_AS_ERRORS)
    if(MSVC)
        add_compile_options(/WX)
    else()
        add_compile_options(-Werror)
    endif()
endif()

# ── Sanitizers (must be set before any target is defined) ────────────────────
# openrar_core is created further down; sanitizer flags declared after it
# would instrument only the harnesses — on MSVC that is a hard LNK2038
# mismatch, on gcc/clang it silently leaves the library unsanitized (which is
# how the nightly job fuzzed uninstrumented core code). /GL (+/LTCG) must be
# disabled under ASan.
if(OPENRAR_SANITIZERS)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_compile_options("$<$<CONFIG:Release>:/GL->")
        add_link_options("$<$<CONFIG:Release>:/LTCG:OFF>")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ── Threads (native only; wasm opt-in via OPENRAR_WASM_THREADS) ───────────────
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    set(OPENRAR_THREAD_LIB Threads::Threads)
else()
    if(OPENRAR_WASM_THREADS)
        find_package(Threads REQUIRED)
        set(OPENRAR_THREAD_LIB Threads::Threads)
    else()
        set(OPENRAR_THREAD_LIB "")
    endif()
endif()

# ── Native library + executables ─────────────────────────────────────────────
# Static core for reuse (tests + exe) — keeps wasm include separate.
add_library(openrar_core STATIC ${OPENRAR_CORE_SOURCES})
target_include_directories(openrar_core PUBLIC
    ${OPENRAR_EXPORT_INCLUDES}
    $<INSTALL_INTERFACE:include>)
if(OPENRAR_THREAD_LIB)
    target_link_libraries(openrar_core PUBLIC ${OPENRAR_THREAD_LIB})
endif()

# Buffer-archive code is isolated behind OPENRAR_INMEM_ARCHIVE so the CLI
# binary and the lean block-codec wasm build stay free of it. The DLL is the
# exception: OPENRAR_DLL_SOURCES always compiles buffer_archive.cpp directly.
# When the option is OFF (default), openrar_core does not contain it and the
# buffer_archive / archive_api test targets are not registered.
if(OPENRAR_INMEM_ARCHIVE)
    target_sources(openrar_core PRIVATE ${OPENRAR_INMEM_ARCHIVE_SOURCES})
endif()

# Main CLI executable (native). When EMSCRIPTEN, this target is skipped — wasm uses src/wasm/wasm_api.cpp instead.
if(NOT EMSCRIPTEN)
    add_executable(openrar src/cli/main.cpp)
    target_link_libraries(openrar PRIVATE openrar_core)
    # Single source of truth for the CLI banner version (Q2): the CMake
    # project version. main.cpp has an OFF fallback for bare manual builds.
    target_compile_definitions(openrar PRIVATE
        OPENRAR_CLI_VERSION="${PROJECT_VERSION}")
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(openrar PRIVATE ${OPENRAR_THREAD_LIB})
    endif()

    # Default.SFX module
    add_executable(Default.SFX src/cli/sfx_main.cpp)
    target_link_libraries(Default.SFX PRIVATE openrar_core)
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(Default.SFX PRIVATE ${OPENRAR_THREAD_LIB})
    endif()

    if(WIN32)
        set_target_properties(openrar Default.SFX PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/openrar64"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/build/openrar64/Debug"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/build/openrar64/Release"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/build/openrar64/RelWithDebInfo"
        )
    else()
        set_target_properties(openrar Default.SFX PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
    endif()
endif()

# ── WASM targets (kept separate; only when emcmake) ─────────────────────────
if(EMSCRIPTEN)
    include(cmake/wasm.cmake)
endif()

# ── Standalone WASM API syntax check (native, no embind — only C ABI compiles) ──
# Useful for CI on non-emscripten runners: ensures wasm_api.cpp is portable C++.
# Real WASM build requires `emcmake cmake --preset wasm`.
if(NOT EMSCRIPTEN)
    add_library(openrar_wasm_api_check STATIC src/wasm/wasm_api.cpp)
    target_include_directories(openrar_wasm_api_check PRIVATE ${OPENRAR_INCLUDES} src/wasm)
    target_link_libraries(openrar_wasm_api_check PUBLIC openrar_core)
    set_target_properties(openrar_wasm_api_check PROPERTIES OUTPUT_NAME "openrar_wasm_api_check")

    # Same idea for the archive API. Only registered when the source is
    # built (mirrors the buffer_archive_tests gating above).
    if(OPENRAR_INMEM_ARCHIVE)
        add_library(openrar_wasm_archive_api_check STATIC src/wasm/archive_api.cpp)
        target_include_directories(openrar_wasm_archive_api_check PRIVATE
            ${OPENRAR_INCLUDES} src/wasm)
        target_link_libraries(openrar_wasm_archive_api_check PUBLIC openrar_core)
        set_target_properties(openrar_wasm_archive_api_check PROPERTIES
            OUTPUT_NAME "openrar_wasm_archive_api_check")
    endif()
endif()

# ── DLL shared library (Hybrid D) ────────────────────────────────────────────
# Arch-specific suffix: pass -DOPENRAR_DLL_SUFFIX=_x64 / _arm64 / _armv7 etc.
# to produce side-by-side openrar_x64.dll + openrar_arm64.dll. Also auto-detected
# from VSCMD_ARG_TGT_ARCH / CMAKE_SYSTEM_PROCESSOR when OFF.
set(OPENRAR_DLL_SUFFIX "" CACHE STRING "Append arch suffix to DLL name (e.g. _x64, _arm64). Empty = plain openrar")
if(NOT EMSCRIPTEN)
    add_library(openrar_dll SHARED ${OPENRAR_DLL_SOURCES})
    target_include_directories(openrar_dll PUBLIC
        ${OPENRAR_EXPORT_INCLUDES}
        $<INSTALL_INTERFACE:include>
    )
    target_include_directories(openrar_dll PRIVATE src/wasm)
    target_link_libraries(openrar_dll PUBLIC openrar_core)
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(openrar_dll PUBLIC ${OPENRAR_THREAD_LIB})
    endif()
    target_compile_definitions(openrar_dll PRIVATE OPENRAR_DLL_EXPORTS)
    set(_dll_suffix "")
    # Auto-detect suffix when not explicitly set but building for known Windows ARM
    if((OPENRAR_DLL_SUFFIX STREQUAL "" OR OPENRAR_DLL_SUFFIX STREQUAL "OFF") AND WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_dll_suffix "_arm64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM$")
            set(_dll_suffix "_arm")
        endif()
    endif()
    if(NOT OPENRAR_DLL_SUFFIX STREQUAL "" AND NOT OPENRAR_DLL_SUFFIX STREQUAL "OFF")
        set(_dll_suffix "${OPENRAR_DLL_SUFFIX}")
    endif()
    set_target_properties(openrar_dll PROPERTIES
        OUTPUT_NAME "openrar${_dll_suffix}"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        INTERPROCEDURAL_OPTIMIZATION OFF
    )
    if(WIN32)
        set_target_properties(openrar_dll PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
    else()
        set_target_properties(openrar_dll PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
    endif()
endif()

enable_testing()

# ── Unit tests (native only — Emscripten has its own node-based harness) ────
if(NOT EMSCRIPTEN)
    # The whole unit-test suite expresses its expectations with assert().
    # Release builds define NDEBUG, which makes assert() a no-op -- so every
    # test silently "passed" regardless of whether its assertions held, and
    # ctest only caught crashes. tools/gate.ps1 builds and runs Release, so
    # this masked real failures.
    #
    # Undefine NDEBUG for test targets only. This affects assert() alone;
    # _ITERATOR_DEBUG_LEVEL is keyed off _DEBUG (set by /MDd), not NDEBUG, so
    # the tests keep the same STL ABI as the release openrar_core they link
    # against and no mismatch is introduced.
    function(openrar_enable_test_asserts target_name)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${target_name} PRIVATE -UNDEBUG)
        endif()
    endfunction()

    function(add_openrar_test target_name)
        add_executable(${target_name} ${ARGN})
        target_link_libraries(${target_name} PRIVATE openrar_core)
        if(OPENRAR_THREAD_LIB)
            target_link_libraries(${target_name} PRIVATE ${OPENRAR_THREAD_LIB})
        endif()
        target_include_directories(${target_name} PRIVATE ${OPENRAR_INCLUDES})
        openrar_enable_test_asserts(${target_name})
        add_test(NAME ${target_name} COMMAND ${target_name})
    endfunction()

    add_openrar_test(compress_tests tests/unit/compress_tests.cpp)

    # Streaming encoder gates: byte-identical vs one-shot, the historical
    # iteration-3 crash regression, cancel/progress, flush sink.
    add_openrar_test(stream_encoder_tests tests/unit/stream_encoder_tests.cpp)
    add_openrar_test(crypto_tests tests/unit/crypto_tests.cpp)
    add_openrar_test(format_tests tests/unit/format_tests.cpp)
    add_openrar_test(io_tests tests/unit/io_tests.cpp)
    add_openrar_test(volume_tests tests/unit/volume_tests.cpp)
    add_openrar_test(recovery_tests tests/unit/recovery_tests.cpp)
    add_openrar_test(archive_tests tests/unit/archive_tests.cpp)
    # Golden fixtures: reader coverage against checked-in archives not
    # produced by this codebase (see tests/unit/golden_fixtures_tests.cpp).
    add_openrar_test(golden_fixtures_tests tests/unit/golden_fixtures_tests.cpp)
    target_compile_definitions(golden_fixtures_tests PRIVATE
        OPENRAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # BufferArchive symbols are available in both configurations — compiled
    # directly into this test when the lean build excludes them from core, or
    # provided by openrar_core when ON. The macro gates the golden test's own
    # BufferArchive verification block, so it is always defined here.
    target_compile_definitions(golden_fixtures_tests PRIVATE OPENRAR_INMEM_ARCHIVE)
    if(NOT OPENRAR_INMEM_ARCHIVE)
        # Lean default build: buffer_archive.cpp is not in openrar_core, so
        # compile it directly into this test. With OPENRAR_INMEM_ARCHIVE=ON
        # the core provides the symbols and adding it here would be a
        # duplicate-definition link error, hence the conditional.
        target_sources(golden_fixtures_tests PRIVATE src/archive/buffer_archive.cpp)
    endif()
    add_openrar_test(cli_tests tests/unit/cli_tests.cpp)
    # cli_tests shells out to the built CLI. It used to guess the location from
    # a list of CWD-relative candidates, which silently missed whenever the
    # build directory wasn't the default `build/` (the exe output path is
    # pinned to ${CMAKE_SOURCE_DIR}/build/openrar64/<cfg>), then fell through
    # to a hard-coded Debug path and failed. Hand it the real path instead.
    target_compile_definitions(cli_tests PRIVATE
        OPENRAR_CLI_EXE="$<TARGET_FILE:openrar>")
    add_dependencies(cli_tests openrar)

    # buffer_archive_tests only registers when the source is built into the
    # native library (gated by OPENRAR_INMEM_ARCHIVE above). Keeps the default
    # native test suite lean and avoids linker errors when the feature is off.
    if(OPENRAR_INMEM_ARCHIVE)
        add_openrar_test(buffer_archive_tests tests/unit/buffer_archive_tests.cpp)
        # The suite reads the checked-in fixtures under tests/ (hello5_hp.rar,
        # hello5_p.rar) for the encrypted-listing coverage.
        target_compile_definitions(buffer_archive_tests PRIVATE
            OPENRAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    endif()

    # WASM ABI surface test (C exports are linkable on every platform).
    add_executable(wasm_api_tests tests/unit/wasm_api_tests.cpp)
    target_link_libraries(wasm_api_tests PRIVATE openrar_core openrar_wasm_api_check)
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(wasm_api_tests PRIVATE ${OPENRAR_THREAD_LIB})
    endif()
    target_include_directories(wasm_api_tests PRIVATE ${OPENRAR_INCLUDES} src/wasm)
    openrar_enable_test_asserts(wasm_api_tests)
    add_test(NAME wasm_api_tests COMMAND wasm_api_tests)

    # Same idea for the archive API. Only registered when the source is
    # built (mirrors the buffer_archive_tests gating above).
    if(OPENRAR_INMEM_ARCHIVE)
        add_executable(archive_api_tests tests/unit/archive_api_tests.cpp)
        target_link_libraries(archive_api_tests PRIVATE
            openrar_core
            openrar_wasm_archive_api_check)
        if(OPENRAR_THREAD_LIB)
            target_link_libraries(archive_api_tests PRIVATE ${OPENRAR_THREAD_LIB})
        endif()
        target_include_directories(archive_api_tests PRIVATE ${OPENRAR_INCLUDES} src/wasm)
        openrar_enable_test_asserts(archive_api_tests)
        add_test(NAME archive_api_tests COMMAND archive_api_tests)
    endif()

    # DLL tests — always (dll target is always built)
    add_executable(dll_tests tests/unit/dll_tests.cpp)
    target_link_libraries(dll_tests PRIVATE openrar_dll)
    target_include_directories(dll_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    # Encrypted-listing coverage reads the fixtures under tests/.
    target_compile_definitions(dll_tests PRIVATE OPENRAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    openrar_enable_test_asserts(dll_tests)
    add_test(NAME dll_tests COMMAND dll_tests)

    # File-mode handle tests (v1.3.0): the C ABI surface plus engine-side
    # fixture building (solid / encrypted / multi-volume sets via
    # ArchiveMutator, which lives in openrar_core on every configuration).
    add_executable(file_handle_tests tests/unit/file_handle_tests.cpp)
    target_link_libraries(file_handle_tests PRIVATE openrar_dll openrar_core)
    if(WIN32)
        target_link_libraries(file_handle_tests PRIVATE psapi) # GetProcessMemoryInfo
    endif()
    target_include_directories(file_handle_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    target_compile_definitions(file_handle_tests PRIVATE OPENRAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    openrar_enable_test_asserts(file_handle_tests)
    add_test(NAME file_handle_tests COMMAND file_handle_tests)

    # Mutation tests (v1.4.0): delete/add exports over the atomic mutator
    # engine — file-handle listing index space, suffix-only solid delete,
    # 'u' replacement semantics, atomicity, RAR_ERR_BUSY handle collision,
    # CMT preservation and QO stripping. Engine-side fixtures need
    # openrar_core (ArchiveMutator / HeaderWriter).
    add_executable(mutation_tests tests/unit/mutation_tests.cpp)
    target_link_libraries(mutation_tests PRIVATE openrar_dll openrar_core)
    target_include_directories(mutation_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    target_compile_definitions(mutation_tests PRIVATE OPENRAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    openrar_enable_test_asserts(mutation_tests)
    add_test(NAME mutation_tests COMMAND mutation_tests)

    # Metadata tests (v1.5.0): entry_ex / handle_info over the file-mode
    # surface — FILETIME timestamps from both htime encodings, solid/
    # encrypted/redir/directory flags, lazily-read CMT payload, RR size,
    # volume provenance, buffer-handle refusals. Needs openrar_core for
    # engine-side fixtures (ArchiveMutator / RecoveryWriter / HeaderWriter).
    add_executable(metadata_tests tests/unit/metadata_tests.cpp)
    target_link_libraries(metadata_tests PRIVATE openrar_dll openrar_core)
    target_include_directories(metadata_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    target_compile_definitions(metadata_tests PRIVATE OPENRAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    openrar_enable_test_asserts(metadata_tests)
    add_test(NAME metadata_tests COMMAND metadata_tests)

    add_executable(dll_cpp_tests tests/unit/dll_cpp_tests.cpp)
    target_link_libraries(dll_cpp_tests PRIVATE openrar_dll)
    target_include_directories(dll_cpp_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    # Password-listing coverage reads the fixtures under tests/.
    target_compile_definitions(dll_cpp_tests PRIVATE OPENRAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    openrar_enable_test_asserts(dll_cpp_tests)
    add_test(NAME dll_cpp_tests COMMAND dll_cpp_tests)

    # ── Fuzzing Infrastructure ──────────────────────────────────────────────
    option(OPENRAR_FUZZ "Enable fuzzer targets" OFF)
    option(OPENRAR_SANITIZERS "Enable ASan/UBSan for fuzzer builds" OFF)
    option(OPENRAR_CROSS_VALIDATE "Enable decompression cross-validation engine" OFF)

    if(OPENRAR_CROSS_VALIDATE)
        add_compile_definitions(OPENRAR_CROSS_VALIDATE)
    endif()

    if(OPENRAR_FUZZ)
        # Sanitizer flags are applied globally above (before openrar_core is
        # defined) so the library itself is instrumented.
        add_executable(fuzz_roundtrip tests/fuzz/fuzz_roundtrip.cpp)
        target_link_libraries(fuzz_roundtrip PRIVATE openrar_core)
        target_include_directories(fuzz_roundtrip PRIVATE ${OPENRAR_INCLUDES})
        openrar_enable_test_asserts(fuzz_roundtrip)

        add_executable(fuzz_decompress tests/fuzz/fuzz_decompress.cpp)
        target_link_libraries(fuzz_decompress PRIVATE openrar_core)
        target_include_directories(fuzz_decompress PRIVATE ${OPENRAR_INCLUDES})
        openrar_enable_test_asserts(fuzz_decompress)

        # Archive + block C ABI harness (the exact entry points browsers
        # reach through wasm/js). Needs archive_api.cpp + buffer_archive.cpp.
        add_executable(fuzz_archive tests/fuzz/fuzz_archive.cpp
                                    ${OPENRAR_INMEM_ARCHIVE_SOURCES}
                                    src/wasm/archive_api.cpp
                                    src/wasm/wasm_api.cpp)
        target_link_libraries(fuzz_archive PRIVATE openrar_core)
        target_include_directories(fuzz_archive PRIVATE ${OPENRAR_INCLUDES} src/wasm)
        openrar_enable_test_asserts(fuzz_archive)
        if(OPENRAR_THREAD_LIB)
            target_link_libraries(fuzz_archive PRIVATE ${OPENRAR_THREAD_LIB})
        endif()

        # Real libFuzzer on clang: define the harness switch and link the
        # fuzzing engine (nightly job then actually fuzzes for max_total_time).
        # MSVC has no libFuzzer driver and keeps the standalone sweep.
        # File-based surface (v1.3.0): open_file / handle_extract_to_path /
        # handle_extract / handle_test / list_file_pw over the streaming
        # reader. dll_api.cpp is compiled directly into the harness (like the
        # wasm sources above) with OPENRAR_DLL_STATIC; requires
        # OPENRAR_INMEM_ARCHIVE=OFF (the default) so buffer_archive.cpp is not
        # also in openrar_core.
        add_executable(fuzz_file_handle tests/fuzz/fuzz_file_handle.cpp
                                        src/dll/dll_api.cpp
                                        ${OPENRAR_INMEM_ARCHIVE_SOURCES})
        target_compile_definitions(fuzz_file_handle PRIVATE OPENRAR_DLL_STATIC)
        target_link_libraries(fuzz_file_handle PRIVATE openrar_core)
        target_include_directories(fuzz_file_handle PRIVATE ${OPENRAR_INCLUDES} src/dll include)
        openrar_enable_test_asserts(fuzz_file_handle)
        if(OPENRAR_THREAD_LIB)
            target_link_libraries(fuzz_file_handle PRIVATE ${OPENRAR_THREAD_LIB})
        endif()

        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT WIN32)
            target_compile_definitions(fuzz_decompress PRIVATE OPENRAR_USE_LIBFUZZER)
            target_compile_options(fuzz_decompress PRIVATE -fsanitize=fuzzer-no-link)
            target_link_options(fuzz_decompress PRIVATE -fsanitize=fuzzer)
            target_compile_definitions(fuzz_archive PRIVATE OPENRAR_USE_LIBFUZZER)
            target_compile_options(fuzz_archive PRIVATE -fsanitize=fuzzer-no-link)
            target_link_options(fuzz_archive PRIVATE -fsanitize=fuzzer)
            target_compile_definitions(fuzz_file_handle PRIVATE OPENRAR_USE_LIBFUZZER)
            target_compile_options(fuzz_file_handle PRIVATE -fsanitize=fuzzer-no-link)
            target_link_options(fuzz_file_handle PRIVATE -fsanitize=fuzzer)
        endif()

        # The sweeps are cheap deterministic loops. They are only built and
        # registered with -DOPENRAR_FUZZ=ON (build.yml does not set it — the
        # nightly fuzz.yml job owns fuzzing; add the flag to build.yml if
        # per-PR sweeps are wanted). libFuzzer mode (clang) caps the run so
        # ctest stays fast; the nightly job owns the long -max_total_time run.
        add_test(NAME fuzz_roundtrip COMMAND fuzz_roundtrip)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT WIN32)
            add_test(NAME fuzz_decompress COMMAND fuzz_decompress -runs=2000 -max_len=1048576)
            add_test(NAME fuzz_archive COMMAND fuzz_archive -runs=2000 -max_len=1048576)
        else()
            add_test(NAME fuzz_decompress COMMAND fuzz_decompress)
            add_test(NAME fuzz_archive COMMAND fuzz_archive)
        endif()
        add_test(NAME fuzz_file_handle COMMAND fuzz_file_handle)
    endif()
endif()

# ── Install & package export (R3) ────────────────────────────────────────────
# Package managers / system installs get the shared library, the CLI, the
# public headers, and a CMake package config so find_package(openrar) provides
# openrar::openrar_dll. Emscripten builds produce no native targets and no
# install rules.
if(NOT EMSCRIPTEN)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS openrar_dll openrar openrar_core
            EXPORT openrar_targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES
                include/openrar/openrar_dll.h
                include/openrar/openrar.hpp
                "${CMAKE_CURRENT_BINARY_DIR}/include/openrar/version.h"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/openrar)

    set(OPENRAR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/openrar")
    configure_package_config_file(
        cmake/openrarConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/openrarConfig.cmake"
        INSTALL_DESTINATION "${OPENRAR_INSTALL_CMAKEDIR}")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/openrarConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
                "${CMAKE_CURRENT_BINARY_DIR}/openrarConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/openrarConfigVersion.cmake"
            DESTINATION "${OPENRAR_INSTALL_CMAKEDIR}")
    install(EXPORT openrar_targets
            FILE openrarTargets.cmake
            NAMESPACE openrar::
            DESTINATION "${OPENRAR_INSTALL_CMAKEDIR}")
endif()
