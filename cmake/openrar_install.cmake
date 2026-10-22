# cmake/openrar_install.cmake — install rules + CMake package export (R3).
# Native-only; Emscripten builds produce no install rules.

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

