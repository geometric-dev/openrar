# cmake/openrar_cpack.cmake — CPack configuration for DEB generation
# Generates Debian .deb binary packages via `cpack -G DEB`

if(NOT EMSCRIPTEN)
    set(CPACK_PACKAGE_NAME "openrar")
    set(CPACK_PACKAGE_VENDOR "OpenRAR Project")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Modern C++17 RAR 5.0 archiver and frozen C ABI library")
    set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
    set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
    set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

    # Debian generator specific metadata
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "OpenRAR Project <geometric-dev@users.noreply.github.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/geometric-dev/openrar")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

    include(CPack)
endif()
