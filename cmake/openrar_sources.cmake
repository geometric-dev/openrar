# cmake/openrar_sources.cmake — source lists, include dirs, related options.
# Extracted verbatim from CMakeLists.txt (v1.45 M2 modularization). Included
# right after the project boilerplate, before any flags or target blocks.

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
    src/crypto/arch/blake2sp_avx512.cpp
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
# Only dll_api.cpp (the C ABI implementation) and buffer_archive.cpp compile
# directly here; the stream codec TUs live in openrar_core unconditionally
# and arrive through target_link_libraries(openrar_core) below (v1.45 M1
# dedup: each TU compiles exactly once). buffer_archive.cpp stays a direct
# compile because the default config's openrar_core does not contain it.
set(OPENRAR_DLL_SOURCES
    src/archive/buffer_archive.cpp
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

