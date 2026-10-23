# cmake/sanitizers.cmake - sanitizer configuration, consolidated (v1.45 M2).
#
# *MUST* be included BEFORE the first add_library()/add_executable():
# flags declared after openrar_core would instrument only the harnesses -
# on MSVC that is a hard LNK2038 mismatch, on gcc/clang it silently leaves
# the library unsanitized (which is how the nightly job once fuzzed
# uninstrumented core code). /GL (+/LTCG) must be disabled under ASan.
#
# Modes (comma list in OPENRAR_SANITIZE): address, undefined, thread, memory.
#   * address, undefined: the primary sanitizer leg (ASan + UBSan).
#   * thread:             the TSan leg (clang, Linux).
#   * memory:             the best-effort MSan leg (clang, Linux; requires
#                         an instrumented libc++ installed by the caller;
#                         this module adds the matching -stdlib=libc++).
# Legacy flag: OPENRAR_SANITIZERS=ON behaves as OPENRAR_SANITIZE=address,undefined.
#
# Compiler support:
#   * MSVC: address only (/fsanitize=address). thread/memory -> FATAL_ERROR
#     (fail-closed; MSVC has no -fsanitize=thread|memory equivalent).
#   * GNU/Clang: any combination of the four modes.
#
# Position canary contract (FMM-45): the module publishes
# OPENRAR_SANITIZE_ACTIVE_MODES when flags are applied. The natives module
# asserts at openrar_core creation that requested sanitize modes were
# configured BEFORE the first target; a late include leaves the variable
# empty while the request cache-vars are still set, which is a fatal error.

option(OPENRAR_SANITIZERS
    "Enable ASan+UBSan for all targets (legacy alias for OPENRAR_SANITIZE=address,undefined)"
    OFF)
set(OPENRAR_SANITIZE "" CACHE STRING
    "Comma list of sanitizer modes: address, undefined, thread, memory")

if(OPENRAR_SANITIZE STREQUAL "" AND OPENRAR_SANITIZERS)
    set(_openrar_sani_modes "address;undefined")
    set(_openrar_sani_legacy TRUE)
else()
    string(REPLACE "," ";" _openrar_sani_modes "${OPENRAR_SANITIZE}")
    list(FILTER _openrar_sani_modes EXCLUDE REGEX "^[ \t]*$")
    set(_openrar_sani_legacy FALSE)
endif()

get_property(_openrar_sani_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
if(_openrar_sani_targets)
    message(FATAL_ERROR
        "cmake/sanitizers.cmake included AFTER targets were defined "
        "(${_openrar_sani_targets}). Sanitizer flags must precede the first "
        "target or the library is unsanitized (hazard documented above).")
endif()

if(NOT _openrar_sani_modes)
    set(OPENRAR_SANITIZE_ACTIVE_MODES "")
    return()
endif()

foreach(_mode IN LISTS _openrar_sani_modes)
    if(NOT _mode MATCHES "^(address|undefined|thread|memory)$")
        message(FATAL_ERROR
            "OPENRAR_SANITIZE contains unknown mode '${_mode}' "
            "(known: address, undefined, thread, memory)")
    endif()
    if(MSVC AND NOT _mode STREQUAL "address")
        if(_openrar_sani_legacy)
            # Legacy OPENRAR_SANITIZERS=ON semantics (pre-M2 block): MSVC
            # silently ignored everything but address. Preserve exactly that.
            list(REMOVE_ITEM _openrar_sani_modes "${_mode}")
            continue()
        endif()
        message(FATAL_ERROR
            "OPENRAR_SANITIZE mode '${_mode}' has no MSVC equivalent; "
            "MSVC supports 'address' only (/fsanitize=address).")
    endif()
endforeach()

list(LENGTH _openrar_sani_modes _openrar_sani_count)
if(_openrar_sani_count EQUAL 0)
    set(OPENRAR_SANITIZE_ACTIVE_MODES "")
    return()
endif()

if(MSVC)
    add_compile_options(/fsanitize=address)
    add_compile_definitions(OPENRAR_SANITIZED_BUILD=1)
    add_compile_options("$<$<CONFIG:Release>:/GL->")
    add_link_options("$<$<CONFIG:Release>:/LTCG:OFF>")
    set(OPENRAR_SANITIZE_ACTIVE_MODES "address")
else()
    list(JOIN _openrar_sani_modes "," _openrar_sani_flags)
    add_compile_options(-fsanitize=${_openrar_sani_flags})
    add_compile_definitions(OPENRAR_SANITIZED_BUILD=1)
    add_link_options(-fsanitize=${_openrar_sani_flags})
    if(_openrar_sani_flags MATCHES "address|undefined")
        add_compile_options(-fno-omit-frame-pointer)
    endif()
    if(_openrar_sani_flags MATCHES "memory")
        add_compile_options(-fsanitize-memory-track-origins -stdlib=libc++)
        add_link_options(-stdlib=libc++)
    endif()
    set(OPENRAR_SANITIZE_ACTIVE_MODES "${_openrar_sani_flags}")
endif()

unset(_openrar_sani_modes)
unset(_openrar_sani_flags)
unset(_openrar_sani_targets)
