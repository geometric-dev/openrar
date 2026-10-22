# cmake/openrar_dll.cmake — the SHARED openrar_dll target (Hybrid D).
# Included after cmake/openrar_natives.cmake; native-only.

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

