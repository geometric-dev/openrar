# cmake/openrar_natives.cmake — thread lib selection, core STATIC library,
# CLI/SFX/worker executables, and the native WASM-API syntax-check targets.
# Included after cmake/sanitizers.cmake (first target creation is in here).

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

# v1.45 M2 fail-closed canary (FMM-45): sanitizers requested without the
# module having run BEFORE this target — meaning a refactored orchestrator
# included cmake/sanitizers.cmake too late — would silently compile an
# unsanitized/core-without-flags build (LNK2038 on MSVC, silent no-op on
# clang). OPENRAR_SANITIZE_ACTIVE_MODES is the module's applied-modes
# publication; requested modes without it = late include.
if(OPENRAR_SANITIZERS OR OPENRAR_SANITIZE)
    if(NOT DEFINED OPENRAR_SANITIZE_ACTIVE_MODES OR NOT OPENRAR_SANITIZE_ACTIVE_MODES)
        message(FATAL_ERROR
            "Sanitizers requested (OPENRAR_SANITIZERS/OPENRAR_SANITIZE) but "
            "cmake/sanitizers.cmake did not run before openrar_core was "
            "created - the library would be silently unsanitized.")
    endif()
endif()

# Buffer-archive code is isolated behind OPENRAR_INMEM_ARCHIVE so the CLI
# binary and the lean block-codec wasm build stay free of it. The DLL is the
# exception: its own DLL_SOURCES list above compiles buffer_archive.cpp
# directly (v1.45 M1: the stream TUs were dropped from that list instead of
# core gaining the option, because they are already unconditional in core).
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
    add_executable(openrar src/cli/main.cpp src/cli/benchmark.cpp src/cli/inspect.cpp)
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

