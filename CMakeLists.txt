cmake_minimum_required(VERSION 3.16)
# Release version - single source of truth (SemVer; PATCH is a monotonic
# commit counter since the current MAJOR.MINOR line's anchor tag - see
# docs/versioning.md).
# wasm/js/package.json and CHANGELOG.md are synced at release time.
# C is enabled for tests/unit/abi_header_c_compat.c — the C-mode compile proof
# of the frozen public C ABI (docs/abi-freeze.md); no production source is C.
project(openrar VERSION 1.37.0 LANGUAGES C CXX)

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
    src/io/posix_xattr.cpp
    src/io/motw.cpp
    src/io/containment.cpp
    src/io/source_delete.cpp
    src/io/extraction_journal.cpp
    src/io/mapped_file.cpp
    src/format/header_reader.cpp
    src/format/header_writer.cpp
    src/compress/filters50.cpp
    src/compress/cdc_chunker.cpp
    src/compress/cdc_planner.cpp
    src/compress/solid_packer.cpp
    src/compress/compressor50.cpp
    src/compress/decompressor50.cpp
    src/compress/parallel_decode.cpp
    src/compress/stream_encoder.cpp
    src/compress/stream_decoder.cpp
    src/compress/parallel_compressor.cpp
    src/compress/arch/match_avx512.cpp
    src/archive/archive_reader.cpp
    src/archive/archive_mutator.cpp
    src/archive/collision_detector.cpp
    src/archive/extraction_report.cpp
    src/unicode/unicode_nfc.cpp
    src/unicode/unicode_tables.cpp
    src/sfx/sfx_config.cpp
    src/sfx/sfx_consent.cpp
    src/sfx/prompt_console.cpp
    src/sfx/process_exec.cpp
    src/sfx/sfx_pipeline.cpp
    src/sandbox/ipc.cpp
    src/sandbox/channel.cpp
    src/sandbox/sandbox_mode.cpp
    src/sandbox/spawn.cpp
    src/sandbox/worker_protocol.cpp
    src/sandbox/worker_broker.cpp
    src/archive/volume.cpp
    src/recovery/rs16.cpp
    src/recovery/rs16_gfni.cpp
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

# ── Foreign-format readers + cv transcoder (v1.29.0) ─────────────────────────
# ZIP/TAR/GZIP/legacy-RAR readers (docs/spec/11, docs/spec/12) and the `cv`
# CLI pipeline. Native-only: the lean wasm builds exclude them by default
# (the 500 KiB budget, docs/wasm-limitations.md §5.3); a wasm build that
# wants them passes -DOPENRAR_FOREIGN_FORMATS=ON explicitly.
if(EMSCRIPTEN)
    set(OPENRAR_FOREIGN_FORMATS_DEFAULT OFF)
else()
    set(OPENRAR_FOREIGN_FORMATS_DEFAULT ON)
endif()
option(OPENRAR_FOREIGN_FORMATS "Foreign-format readers (ZIP/TAR/GZIP/legacy RAR), inflate, and the cv transcoder. Excluded from the lean wasm block-codec build." ${OPENRAR_FOREIGN_FORMATS_DEFAULT})
set(OPENRAR_FOREIGN_FORMATS_SOURCES
    src/compress/inflate.cpp
    src/archive/foreign_reader.cpp
    src/archive/foreign_zip.cpp
    src/archive/foreign_tar.cpp
    src/archive/foreign_gzip.cpp
    src/archive/foreign_transcode.cpp
)

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

# ── AVX-512 match kernel + GFNI RS16 fold kernel (v1.22.0) ───────────────────
# The kernel TUs are the ONLY translation units compiled with /arch:AVX512, so
# 512-bit intrinsics never leak into baseline codegen (a global /arch would
# emit AVX-512 outside guarded paths and #UD on older CPUs). The runtime
# dispatchers still require the CPU feature (CPUID + OS XSTATE) before
# selecting them. GCC/Clang need no per-TU flag (function-level target
# attribute inside the TUs); the defines only exist where the kernels are
# actually compiled.
set(OPENRAR_AVX512_KERNEL OFF)
if(NOT EMSCRIPTEN)
    if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_GENERATOR_PLATFORM MATCHES "x64|X64|AMD64")
        # Generator-platform check keeps the kernels off the MSVC ARM64
        # cross legs: /arch:AVX512 there is meaningless and immintrin.h is
        # x86-family only (v1.22.0 CI fix).
        set(OPENRAR_AVX512_KERNEL ON)
        set_source_files_properties(src/compress/arch/match_avx512.cpp src/recovery/rs16_gfni.cpp
            PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    elseif((CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64)|(AMD64)")
        set(OPENRAR_AVX512_KERNEL ON)
    endif()
endif()
if(OPENRAR_AVX512_KERNEL)
    add_compile_definitions(OPENRAR_HAS_AVX512_KERNEL=1 OPENRAR_HAS_GFNI_KERNEL=1)
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

# Foreign-format readers + cv transcoder (v1.29.0): attached to openrar_core
# behind OPENRAR_FOREIGN_FORMATS (default ON native, OFF under EMSCRIPTEN).
if(OPENRAR_FOREIGN_FORMATS)
    target_sources(openrar_core PRIVATE ${OPENRAR_FOREIGN_FORMATS_SOURCES})
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

    # WinGUI.SFX: native-dialog SFX stub (v1.23.0). Windows-only; a windowed
    # subsystem binary (no console) sharing the directive/consent/containment
    # engine with Default.SFX.
    if(WIN32)
        add_executable(WinGUI.SFX src/cli/sfx_gui_main.cpp)
        target_link_libraries(WinGUI.SFX PRIVATE openrar_core)
        set_target_properties(WinGUI.SFX PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(Default.SFX PRIVATE ${OPENRAR_THREAD_LIB})
    endif()

    if(WIN32)
        set_target_properties(openrar Default.SFX PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${OPENRAR_OUTPUT_ROOT}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OPENRAR_OUTPUT_ROOT}/Debug"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OPENRAR_OUTPUT_ROOT}/Release"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OPENRAR_OUTPUT_ROOT}/RelWithDebInfo"
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
    add_openrar_test(two_phase_tests tests/unit/two_phase_tests.cpp)
    add_openrar_test(bitreader_stream_tests tests/unit/bitreader_stream_tests.cpp)
    add_openrar_test(parallel_compress_tests tests/unit/parallel_compress_tests.cpp)

    # Streaming encoder gates: byte-identical vs one-shot, the historical
    # iteration-3 crash regression, cancel/progress, flush sink.
    add_openrar_test(stream_encoder_tests tests/unit/stream_encoder_tests.cpp)
    add_openrar_test(crypto_tests tests/unit/crypto_tests.cpp)
    add_openrar_test(format_tests tests/unit/format_tests.cpp)
    add_openrar_test(io_tests tests/unit/io_tests.cpp)
    add_openrar_test(extraction_atomic_tests tests/unit/extraction_atomic_tests.cpp)
    add_openrar_test(extraction_containment_tests tests/unit/extraction_containment_tests.cpp)
    add_openrar_test(collision_matrix_tests tests/unit/collision_matrix_tests.cpp)
    add_openrar_test(extraction_report_tests tests/unit/extraction_report_tests.cpp)
    add_openrar_test(extraction_fidelity_tests tests/unit/extraction_fidelity_tests.cpp)
    add_openrar_test(unknown_extras_tests tests/unit/unknown_extras_tests.cpp)
    add_openrar_test(mapped_file_tests tests/unit/mapped_file_tests.cpp)
    add_openrar_test(mapped_scan_tests tests/unit/mapped_scan_tests.cpp)
    add_openrar_test(cdc_chunker_tests tests/unit/cdc_chunker_tests.cpp)
    add_openrar_test(cdc_planner_tests tests/unit/cdc_planner_tests.cpp)
    add_openrar_test(solid_packer_tests tests/unit/solid_packer_tests.cpp)
    add_openrar_test(volume_tests tests/unit/volume_tests.cpp)
    add_openrar_test(recovery_tests tests/unit/recovery_tests.cpp)
    add_openrar_test(archive_tests tests/unit/archive_tests.cpp)
    add_openrar_test(sfx_tests tests/unit/sfx_tests.cpp)
    add_openrar_test(sfx_pipeline_tests tests/unit/sfx_pipeline_tests.cpp)
    add_openrar_test(sfx_e2e_tests tests/unit/sfx_e2e_tests.cpp)
    if(TARGET Default.SFX)
        set_tests_properties(sfx_e2e_tests PROPERTIES
            ENVIRONMENT "OPENRAR_SFX_STUB=$<TARGET_FILE:Default.SFX>")
    endif()
    # Cross-compiled legs (QEMU): child spawns in the SFX tests must go
    # through the emulator or execv(ENOEXEC)s on the host kernel. Space-
    # joined so the test can tokenize it into an argv prefix.
    if(CMAKE_CROSSCOMPILING_EMULATOR)
        string(JOIN " " SFX_EMU ${CMAKE_CROSSCOMPILING_EMULATOR})
        target_compile_definitions(sfx_tests PRIVATE
            OPENRAR_SFX_EMULATOR="${SFX_EMU}")
        target_compile_definitions(sfx_pipeline_tests PRIVATE
            OPENRAR_SFX_EMULATOR="${SFX_EMU}")
        target_compile_definitions(sfx_e2e_tests PRIVATE
            OPENRAR_SFX_EMULATOR="${SFX_EMU}")
    endif()
    # Golden fixtures: reader coverage against checked-in archives not
    # produced by this codebase (see tests/unit/golden_fixtures_tests.cpp).
    add_openrar_test(golden_fixtures_tests tests/unit/golden_fixtures_tests.cpp)
    target_compile_definitions(golden_fixtures_tests PRIVATE
        OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
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
    # pinned to a fixed <source>/build/openrar64/<cfg>), then fell through
    # to a hard-coded Debug path and failed. Hand it the real path instead.
    target_compile_definitions(cli_tests PRIVATE
        OPENRAR_CLI_EXE="$<TARGET_FILE:openrar>")
    # v1.28 M4: the bench-engine tests shell out to the openrar_bench tool.
    target_compile_definitions(cli_tests PRIVATE
        OPENRAR_BENCH_EXE="$<TARGET_FILE:openrar_bench>")
    add_dependencies(cli_tests openrar openrar_bench)

    # collision_matrix_tests (gate 2) also shells out to the built CLI for
    # the end-to-end abort assertions (plan §10 tests 7-10).
    target_compile_definitions(collision_matrix_tests PRIVATE
        OPENRAR_CLI_EXE="$<TARGET_FILE:openrar>")
    add_dependencies(collision_matrix_tests openrar)

    # extraction_report_tests (M4) shells out for the stdout-purity and
    # displayed≡extracted invariant assertions (plan §10 tests 15-17).
    target_compile_definitions(extraction_report_tests PRIVATE
        OPENRAR_CLI_EXE="$<TARGET_FILE:openrar>")
    add_dependencies(extraction_report_tests openrar)

    # buffer_archive_tests only registers when the source is built into the
    # native library (gated by OPENRAR_INMEM_ARCHIVE above). Keeps the default
    # native test suite lean and avoids linker errors when the feature is off.
    if(OPENRAR_INMEM_ARCHIVE)
        add_openrar_test(buffer_archive_tests tests/unit/buffer_archive_tests.cpp)
        # The suite reads the checked-in fixtures under tests/ (hello5_hp.rar,
        # hello5_p.rar) for the encrypted-listing coverage.
        target_compile_definitions(buffer_archive_tests PRIVATE
            OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    endif()

    # Foreign-format reader tests (v1.29): gated by OPENRAR_FOREIGN_FORMATS
    # exactly like the sources they exercise.
    if(OPENRAR_FOREIGN_FORMATS)
        add_openrar_test(inflate_tests tests/unit/inflate_tests.cpp)
        add_openrar_test(foreign_format_tests tests/unit/foreign_format_tests.cpp)
        add_openrar_test(transcode_tests tests/unit/transcode_tests.cpp)
        target_compile_definitions(transcode_tests PRIVATE
            OPENRAR_CLI_EXE="$<TARGET_FILE:openrar>")
        add_dependencies(transcode_tests openrar)
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
    target_compile_definitions(dll_tests PRIVATE OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
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
    target_compile_definitions(file_handle_tests PRIVATE OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
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
    target_compile_definitions(mutation_tests PRIVATE OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
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
    target_compile_definitions(metadata_tests PRIVATE OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    openrar_enable_test_asserts(metadata_tests)
    add_test(NAME metadata_tests COMMAND metadata_tests)

    add_executable(dll_cpp_tests tests/unit/dll_cpp_tests.cpp)
    target_link_libraries(dll_cpp_tests PRIVATE openrar_dll)
    target_include_directories(dll_cpp_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    # Password-listing coverage reads the fixtures under tests/.
    target_compile_definitions(dll_cpp_tests PRIVATE OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    openrar_enable_test_asserts(dll_cpp_tests)
    add_test(NAME dll_cpp_tests COMMAND dll_cpp_tests)

    # ── ABI freeze gates (v1.30.0; docs/abi-freeze.md) ────────────────────────
    # Layout golden + C-mode compile proof + feature-mask parity, exercised
    # against the real shared library. The .c TU is compiled by the C compiler
    # inside this target: if the public header stops compiling as plain C the
    # build itself fails.
    add_executable(abi_layout_tests
        tests/unit/abi_layout_tests.cpp
        tests/unit/abi_header_c_compat.c)
    target_link_libraries(abi_layout_tests PRIVATE openrar_dll)
    target_include_directories(abi_layout_tests PRIVATE
        ${OPENRAR_INCLUDES} src/dll include)
    target_compile_definitions(abi_layout_tests PRIVATE
        OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    openrar_enable_test_asserts(abi_layout_tests)
    add_test(NAME abi_layout_tests COMMAND abi_layout_tests)

    # Library-mode containment gates (v1.30.0 M2): the non-disableable
    # floors vs the tunable caller-budget layer, and containment parity
    # under the OPENRAR_NO_MMAP kill-switch.
    add_executable(library_mode_tests tests/unit/library_mode_tests.cpp)
    target_link_libraries(library_mode_tests PRIVATE openrar_dll openrar_core)
    target_include_directories(library_mode_tests PRIVATE
        ${OPENRAR_INCLUDES} src/dll include)
    openrar_enable_test_asserts(library_mode_tests)
    add_test(NAME library_mode_tests COMMAND library_mode_tests)

    # Sandbox IPC gates (v1.30.0 M3 foundation): the frame codec's hostile-
    # input semantics, bounded-memory guarantees, and the real-pipe channel
    # (EOF / poison propagation / threaded large-frame flow) — independent
    # of any per-OS sandbox model.
    add_openrar_test(sandbox_ipc_tests tests/unit/sandbox_ipc_tests.cpp)

    # Reader handle seam (v1.30.0 M3 harness): open_read_handle scan/extract
    # parity and the single-volume contract, independent of the worker.
    add_openrar_test(reader_handle_seam_tests tests/unit/reader_handle_seam_tests.cpp)

    # Attack regression corpus (v1.30 M5): the historical CVE classes
    # re-run in CI with per-case guarantee assertions.
    add_openrar_test(attack_corpus_tests tests/unit/attack_corpus_tests.cpp)

    # QO partial-cache fallback regression (v1.30.0): a QuickOpen record that
    # omits middle entries must fall back to the linear scan.
    add_openrar_test(qo_gap_fallback_tests tests/unit/qo_gap_fallback_tests.cpp)

    # Walk diagnostic probe (not a test): prints read_block_raw results.
    add_executable(walk_probe tools/walk_probe.cpp)
    target_link_libraries(walk_probe PRIVATE openrar_core)
    target_include_directories(walk_probe PRIVATE ${OPENRAR_INCLUDES})

    # v1.37.0 two-phase Gate 0 probe (not a test): token census + alpha.
    add_executable(tokencensus tools/tokencensus.cpp)
    target_link_libraries(tokencensus PRIVATE openrar_core)
    target_include_directories(tokencensus PRIVATE ${OPENRAR_INCLUDES})

    # The sandboxed worker process (v1.30.0 M3): drives the SAME
    # ArchiveReader engine over the IPC protocol, holds no paths, writes
    # nothing. The per-OS privilege models (AppContainer / seccomp /
    # Seatbelt) wrap this exact spawn contract.
    add_executable(openrar_worker src/sandbox/worker_main.cpp)
    target_link_libraries(openrar_worker PRIVATE openrar_core)
    target_include_directories(openrar_worker PRIVATE ${OPENRAR_INCLUDES})
    # The sandboxed worker ships BESIDE the CLI (worker_exe_path() looks
    # there) — same output dirs as the openrar target, set AFTER it exists.
    if(WIN32)
        set_target_properties(openrar_worker PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${OPENRAR_OUTPUT_ROOT}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OPENRAR_OUTPUT_ROOT}/Debug"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OPENRAR_OUTPUT_ROOT}/Release"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OPENRAR_OUTPUT_ROOT}/RelWithDebInfo"
        )
    endif()
    if(WIN32)
        target_link_libraries(openrar_worker PRIVATE ws2_32) # selftest network probe
    endif()
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(openrar_worker PRIVATE ${OPENRAR_THREAD_LIB})
    endif()

    # Broker/worker e2e over a real (unsandboxed) spawn: protocol parity,
    # broker-side cap enforcement, crash isolation.
    add_executable(sandbox_worker_e2e_tests tests/unit/sandbox_worker_e2e_tests.cpp)
    target_link_libraries(sandbox_worker_e2e_tests PRIVATE openrar_core)
    target_include_directories(sandbox_worker_e2e_tests PRIVATE ${OPENRAR_INCLUDES})
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(sandbox_worker_e2e_tests PRIVATE ${OPENRAR_THREAD_LIB})
    endif()
    target_compile_definitions(sandbox_worker_e2e_tests PRIVATE
        OPENRAR_WORKER_EXE="$<TARGET_FILE:openrar_worker>")
    openrar_enable_test_asserts(sandbox_worker_e2e_tests)
    add_test(NAME sandbox_worker_e2e_tests COMMAND sandbox_worker_e2e_tests)

    # Sandbox model e2e (M3a/b): the OBSERVED-denial matrix per shipped
    # platform model (AppContainer / seccomp) + extraction parity through the
    # sandboxed worker. Skips loudly where no model exists (macOS/other —
    # the honest-cut follow-up).
    add_executable(sandbox_sandboxed_e2e_tests tests/unit/sandbox_sandboxed_e2e_tests.cpp)
    target_link_libraries(sandbox_sandboxed_e2e_tests PRIVATE openrar_core)
    target_include_directories(sandbox_sandboxed_e2e_tests PRIVATE ${OPENRAR_INCLUDES})
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(sandbox_sandboxed_e2e_tests PRIVATE ${OPENRAR_THREAD_LIB})
    endif()
    target_compile_definitions(sandbox_sandboxed_e2e_tests PRIVATE
        OPENRAR_WORKER_EXE="$<TARGET_FILE:openrar_worker>")
    openrar_enable_test_asserts(sandbox_sandboxed_e2e_tests)
    add_test(NAME sandbox_sandboxed_e2e_tests COMMAND sandbox_sandboxed_e2e_tests)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        # Export-list parity: the built library's C-ABI symbol surface must
        # equal tools/abi_exports_canonical.txt. Skips loudly (exit 0 +
        # [SKIP]) on hosts with no symbol tool (dumpbin/llvm-nm/nm); CI
        # wiring lands with the post-rebase workflow changes.
        add_test(NAME abi_export_parity
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/abi_exports.py
                    --check $<TARGET_FILE:openrar_dll>)

        # JS mirror parity: wasm JS CODE_MAP + RarErrorCode union must equal
        # the C RarError enum (the wasm binding defect class, gated).
        # Registered only where the wasm JS surface exists (the lean CLI
        # build tree may not carry wasm/js).
        if(EXISTS "${PROJECT_SOURCE_DIR}/wasm/js/openrar-archive.js"
           AND EXISTS "${PROJECT_SOURCE_DIR}/wasm/js/openrar-archive.d.ts")
            add_test(NAME js_error_mirror_parity
                COMMAND ${Python3_EXECUTABLE}
                        ${PROJECT_SOURCE_DIR}/tools/check_js_error_mirror.py)
        endif()

        # Python SDK conformance gate (v1.30 M4, freeze prereq 3): the
        # normative cross-binding case list against the freshly built DLL.
        add_test(NAME python_conformance
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/bindings/python/tests/conformance_test.py
                    $<TARGET_FILE:openrar_dll>)
    endif()

    # ── Kernel throughput benchmark (informational, never a gate) ───────────
    if(NOT EMSCRIPTEN)
        add_executable(openrar_bench tests/bench/openrar_bench.cpp)
        target_link_libraries(openrar_bench PRIVATE openrar_core)
        target_include_directories(openrar_bench PRIVATE ${OPENRAR_INCLUDES})
    endif()

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

        # Inflate fuzzer (v1.29): only meaningful when the foreign-format
        # sources are built (it exercises src/compress/inflate).
        if(OPENRAR_FOREIGN_FORMATS)
            add_executable(fuzz_inflate tests/fuzz/fuzz_inflate.cpp)
            target_link_libraries(fuzz_inflate PRIVATE openrar_core)
            target_include_directories(fuzz_inflate PRIVATE ${OPENRAR_INCLUDES})
            openrar_enable_test_asserts(fuzz_inflate)
        endif()

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
            # fuzz_inflate was missing from this block, so it linked with
            # neither the libFuzzer runtime nor a standalone main() and failed
            # with "undefined reference to main". It now has both (see the
            # #ifndef in the source), so it must be wired like the others.
            if(TARGET fuzz_inflate)
                target_compile_definitions(fuzz_inflate PRIVATE OPENRAR_USE_LIBFUZZER)
                target_compile_options(fuzz_inflate PRIVATE -fsanitize=fuzzer-no-link)
                target_link_options(fuzz_inflate PRIVATE -fsanitize=fuzzer)
            endif()
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
