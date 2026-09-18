# cmake/wasm.cmake — Emscripten / WASM target (kept separate from native flags).
# Included only when CMAKE_SYSTEM_NAME STREQUAL "Emscripten" (i.e. emcmake).
# Defines: openrar_wasm (MODULARIZE JS+WASM) and openrar_wasm_cli (optional full CLI with MEMFS).
# Native builds never include this file, so no flag leakage.

if(NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    return()
endif()

message(STATUS "OpenRAR: configuring WASM targets (Emscripten ${EMSCRIPTEN_VERSION})")

# ── WASM compile settings (do not propagate to native) ──────────────────────
# NOTE: unquoted list — quoted, CMake passes "-O3 -fwasm-exceptions" as one
# argument and em++ rejects it ("invalid optimization level").
set(WASM_COMMON_FLAGS -O3 -fwasm-exceptions)

# Core library for WASM (re-use OPENRAR_CORE_SOURCES / OPENRAR_INCLUDES defined in main CMakeLists)
add_library(openrar_wasm_core STATIC ${OPENRAR_CORE_SOURCES})
target_include_directories(openrar_wasm_core PUBLIC ${OPENRAR_INCLUDES})
target_compile_definitions(openrar_wasm_core PUBLIC __EMSCRIPTEN__=1)
target_compile_options(openrar_wasm_core PRIVATE ${WASM_COMMON_FLAGS})
set_target_properties(openrar_wasm_core PROPERTIES OUTPUT_NAME "openrar_wasm_core")

# ── Block codec JS library (src/wasm/wasm_api.cpp) ──────────────────────────
# openrar_stream_* exports are backed by stream_encoder.cpp, which is part
# of OPENRAR_CORE_SOURCES.
add_executable(openrar_wasm src/wasm/wasm_api.cpp)
target_include_directories(openrar_wasm PRIVATE ${OPENRAR_INCLUDES} src/wasm)
target_link_libraries(openrar_wasm PRIVATE openrar_wasm_core)
target_compile_options(openrar_wasm PRIVATE ${WASM_COMMON_FLAGS})

# Emscripten link options — MODULARIZE so Node + bundlers can `import createOpenRAR from './openrar.js'`
# ALLOW_MEMORY_GROWTH: RAR windows up to 4 GiB need growth; FILESYSTEM=0 for block codec (no FS).
# No -lembind/--bind: the JS wrapper drives the C ABI via ccall (v2 dropped embind).
# Each flag is its own list element: a joined LINK_FLAGS string reaches em++
# as one semicolon-joined argument ("setting WASM expects int but got str").
# Heap views (HEAPU8/HEAPU32) are auto-exposed on the Module and must NOT be
# listed in EXPORTED_RUNTIME_METHODS (em++ 3.1.x warns "invalid item").
target_link_options(openrar_wasm PRIVATE
    "-sWASM=1"
    "-fwasm-exceptions"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createOpenRAR"
    "-sEXPORT_ES6=1"
    "-sEXPORTED_FUNCTIONS=['_openrar_version','_openrar_alloc','_openrar_free','_openrar_compress','_openrar_compress2','_openrar_decompress','_openrar_decompress2','_openrar_stream_create','_openrar_stream_feed','_openrar_stream_finish','_openrar_stream_free','_malloc','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
    "-sENVIRONMENT=web,node"
    "-sFILESYSTEM=0"
)

# Output to wasm/dist/ (repo-relative) so wasm/js package can reference it
set_target_properties(openrar_wasm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/wasm/dist"
    OUTPUT_NAME "openrar"
    SUFFIX ".js"
)

# ── Optional full CLI with MEMFS (for archive-level round-trips in wasm) ───
# Enable with -DOPENRAR_WASM_CLI=ON . Keeps block codec as default to avoid FS overhead.
option(OPENRAR_WASM_CLI "Build openrar_cli.js with MEMFS (full archive a/x/t)" OFF)
if(OPENRAR_WASM_CLI)
    add_executable(openrar_cli src/cli/main.cpp)
    target_include_directories(openrar_cli PRIVATE ${OPENRAR_INCLUDES})
    target_link_libraries(openrar_cli PRIVATE openrar_wasm_core)
    target_compile_options(openrar_cli PRIVATE ${WASM_COMMON_FLAGS})
    target_link_options(openrar_cli PRIVATE
        "-sWASM=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINVOKE_RUN_DIRECTLY=0"
        "-sEXPORTED_RUNTIME_METHODS=['FS','callMain','UTF8ToString']"
        "-sFORCE_FILESYSTEM=1"
        "-sNODERAWFS=0"
    )
    set_target_properties(openrar_cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/wasm/dist"
        OUTPUT_NAME "openrar_cli"
        SUFFIX ".js"
    )
endif()

# ── Optional in-memory archive API (wasm-archive preset) ────────────────────
# Enable with -DOPENRAR_INMEM_ARCHIVE=ON. Builds a separate artifact
# `wasm/dist/openrar_archive.js` that wraps `BufferArchive` with C exports
# (`openrar_archive_*`). Kept OFF by default so the block codec stays lean.
# This target wires `buffer_archive.cpp` (from OPENRAR_INMEM_ARCHIVE_SOURCES)
# directly — it does NOT include src/cli/main.cpp, so no MEMFS.
if(OPENRAR_INMEM_ARCHIVE)
    # 1) Tiny static lib that wraps buffer_archive.cpp with the WASM compile
    #    defs so __EMSCRIPTEN__ guards activate.
    add_library(openrar_wasm_archive_core STATIC ${OPENRAR_INMEM_ARCHIVE_SOURCES})
    target_include_directories(openrar_wasm_archive_core PUBLIC ${OPENRAR_INCLUDES})
    target_compile_definitions(openrar_wasm_archive_core PUBLIC __EMSCRIPTEN__=1)
    target_compile_options(openrar_wasm_archive_core PRIVATE ${WASM_COMMON_FLAGS})
    target_link_libraries(openrar_wasm_archive_core PUBLIC openrar_wasm_core)
    # 2) The MODULARIZE wrapper.
    add_executable(openrar_wasm_archive src/wasm/archive_api.cpp)
    target_include_directories(openrar_wasm_archive PRIVATE ${OPENRAR_INCLUDES} src/wasm)
    target_link_libraries(openrar_wasm_archive PRIVATE
        openrar_wasm_core
        openrar_wasm_archive_core)
    target_compile_options(openrar_wasm_archive PRIVATE ${WASM_COMMON_FLAGS})
    # 3) Output path.
    set_target_properties(openrar_wasm_archive PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/wasm/dist"
        OUTPUT_NAME "openrar_archive"
        SUFFIX ".js")
    # 4) Link options — distinct EXPORT_NAME so the archive module doesn't
    #    collide with the block-codec factory (see spec §4.4).
    #    ALLOW_TABLE_GROWTH + addFunction: JS registers progress/cancel
    #    callbacks after instantiation (Phase 2 of the API revision).
    target_link_options(openrar_wasm_archive PRIVATE
        "-sWASM=1"
        "-fwasm-exceptions"
        "-sALLOW_MEMORY_GROWTH=1"
        # i64 at the JS boundary is BigInt. Required for the progress hook
        # signature (void* user, uint64_t done, uint64_t total) via addFunction.
        # No archive C-ABI entry point takes an i64 parameter directly (sizes
        # are size_t = u32), so this changes no call-site marshalling.
        "-sWASM_BIGINT=1"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createOpenRARArchive"
        "-sEXPORT_ES6=1"
        "-sENVIRONMENT=web,node"
        "-sFILESYSTEM=0"
        "-sALLOW_TABLE_GROWTH=1"
        "-sEXPORTED_FUNCTIONS=['_openrar_archive_version','_openrar_archive_open','_openrar_archive_close','_openrar_archive_handle_list','_openrar_archive_handle_extract','_openrar_archive_handle_extract_all','_openrar_archive_handle_extract_all2','_openrar_archive_handle_set_limits','_openrar_archive_list','_openrar_archive_list_free','_openrar_archive_extract','_openrar_archive_extract_all','_openrar_archive_extract_all2','_openrar_archive_create','_openrar_archive_create2','_openrar_archive_get_error','_openrar_archive_last_error_code','_openrar_archive_alloc','_openrar_archive_free','_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','addFunction','removeFunction']")
endif()
