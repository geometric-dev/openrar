# cmake/openrar_flags.cmake — native compiler flags, warnings-as-errors, ISA kernels.
# Extracted verbatim from CMakeLists.txt (v1.45 M2 modularization).
# Included BEFORE cmake/sanitizers.cmake; both must precede the first target.

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

# ── Hardened Security Mode (compiler, standard library, and linker) ────────────
option(OPENRAR_HARDENED "Enable hardened compiler, standard library, and linker security modes" ON)
if(OPENRAR_HARDENED AND NOT EMSCRIPTEN)
    if(MSVC)
        # MSVC: Standard C++ exception unwinding, Stack Canaries, Control Flow Guard
        add_compile_options(/EHsc /GS /guard:cf)
        add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            add_link_options(/HIGHENTROPYVA)
            # Intel CET shadow stack is supported on x64 MSVC targets only (incompatible with ARM64 / x86).
            if(NOT (CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64" OR
                    CMAKE_VS_PLATFORM_NAME MATCHES "ARM64|arm64" OR
                    (DEFINED CMAKE_SYSTEM_PROCESSOR AND CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64") OR
                    (DEFINED CMAKE_CXX_COMPILER_ARCHITECTURE_ID AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64")))
                add_link_options(/CETCOMPAT)
            endif()
        endif()
    else()
        # Standard library assertions (libstdc++ and libc++)
        add_compile_definitions(_GLIBCXX_ASSERTIONS)
        add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
        # Buffer overflow checking for libc calls under Release optimization
        add_compile_options("$<$<CONFIG:Release>:-D_FORTIFY_SOURCE=3>")

        # Stack canary protection
        add_compile_options(-fstack-protector-strong)
        # Format string security
        add_compile_options(-Wformat -Wformat-security)

        # Linux-specific stack clash & control flow protection + RELRO
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_compile_options(-fstack-clash-protection)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64)|(AMD64)")
                add_compile_options(-fcf-protection=full)
            elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64)|(arm64)")
                add_compile_options(-mbranch-protection=standard)
            endif()
            add_link_options(-Wl,-z,relro,-z,now -Wl,-z,noexecstack)
        endif()
    endif()
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
        set_source_files_properties(src/compress/arch/match_avx512.cpp src/recovery/rs16_gfni.cpp src/crypto/arch/blake2sp_avx512.cpp
            PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Enable when the build has an x86-64 slice. Checking only the host
        # processor was wrong for universal2: that build is configured on an
        # Apple Silicon runner, so CMAKE_SYSTEM_PROCESSOR says arm64 while one
        # slice is Intel, and the Intel half silently shipped without the
        # kernels. cmake/isa_kernel_gate.hpp (force-included below) drops the
        # capability defines for whichever slices are not x86.
        set(_openrar_x86_slice FALSE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64)|(AMD64)")
            set(_openrar_x86_slice TRUE)
        endif()
        if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
            set(_openrar_x86_slice TRUE)
        endif()
        if(_openrar_x86_slice)
            set(OPENRAR_AVX512_KERNEL ON)
        endif()
    endif()
endif()
if(OPENRAR_AVX512_KERNEL)
    add_compile_definitions(OPENRAR_HAS_AVX512_KERNEL=1 OPENRAR_HAS_GFNI_KERNEL=1)
    # The capability defines are target-wide, so a universal build would hand
    # them to the arm64 slice as well. Force-include the gate: it #undefs them
    # when the compiler is targeting a non-x86 architecture, and is a no-op on
    # every x86-64 build (including the Intel slice of a fat binary).
    if(MSVC)
        add_compile_options("/FI${CMAKE_SOURCE_DIR}/cmake/isa_kernel_gate.hpp")
    else()
        add_compile_options(-include ${CMAKE_SOURCE_DIR}/cmake/isa_kernel_gate.hpp)
    endif()
endif()


