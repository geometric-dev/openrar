# cmake/openrar_tests.cmake — enable_testing, all unit/probe test targets,
# ctest registrations, and the fuzz option block (OPENRAR_FUZZ,
# OPENRAR_CROSS_VALIDATE). The OPENRAR_SANITIZERS option lives in
# cmake/sanitizers.cmake since its flags must be consumed pre-target.

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

    # v1.44 M5: the verification substrate migrates the unit tests onto the
    # vendored doctest (third_party/doctest, pinned v2.4.12 — see
    # docs/SBOM.md). The runner owns DOCTEST_CONFIG_IMPLEMENT; suites only
    # include <doctest.h>, so every test target needs the include path.
    add_library(openrar_test_doctest INTERFACE)
    target_include_directories(openrar_test_doctest INTERFACE
        ${PROJECT_SOURCE_DIR}/third_party/doctest)

    function(add_openrar_test target_name)
        add_executable(${target_name} ${ARGN} tests/support/test_runner.cpp)
        target_link_libraries(${target_name} PRIVATE openrar_core openrar_test_doctest)
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
    add_openrar_test(benchmark_tests tests/unit/benchmark_tests.cpp src/cli/benchmark.cpp)
    add_openrar_test(json_output_tests tests/unit/json_output_tests.cpp)
    add_openrar_test(inspect_tests tests/unit/inspect_tests.cpp src/cli/inspect.cpp)
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
    add_executable(wasm_api_tests tests/unit/wasm_api_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(wasm_api_tests PRIVATE openrar_core openrar_wasm_api_check
    openrar_test_doctest)
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(wasm_api_tests PRIVATE ${OPENRAR_THREAD_LIB})
    endif()
    target_include_directories(wasm_api_tests PRIVATE ${OPENRAR_INCLUDES} src/wasm)
    openrar_enable_test_asserts(wasm_api_tests)
    add_test(NAME wasm_api_tests COMMAND wasm_api_tests)

    # Same idea for the archive API. Only registered when the source is
    # built (mirrors the buffer_archive_tests gating above).
    if(OPENRAR_INMEM_ARCHIVE)
        add_executable(archive_api_tests tests/unit/archive_api_tests.cpp tests/support/test_runner.cpp)
target_link_libraries(archive_api_tests PRIVATE
    openrar_test_doctest
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
    add_executable(dll_tests tests/unit/dll_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(dll_tests PRIVATE openrar_dll openrar_test_doctest)
    target_include_directories(dll_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    # Encrypted-listing coverage reads the fixtures under tests/.
    target_compile_definitions(dll_tests PRIVATE OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    openrar_enable_test_asserts(dll_tests)
    add_test(NAME dll_tests COMMAND dll_tests)

    # File-mode handle tests (v1.3.0): the C ABI surface plus engine-side
    # fixture building (solid / encrypted / multi-volume sets via
    # ArchiveMutator, which lives in openrar_core on every configuration).
    add_executable(file_handle_tests tests/unit/file_handle_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(file_handle_tests PRIVATE openrar_dll openrar_core
    openrar_test_doctest)
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
    add_executable(mutation_tests tests/unit/mutation_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(mutation_tests PRIVATE openrar_dll openrar_core
    openrar_test_doctest)
    target_include_directories(mutation_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    target_compile_definitions(mutation_tests PRIVATE OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    openrar_enable_test_asserts(mutation_tests)
    add_test(NAME mutation_tests COMMAND mutation_tests)

    # Metadata tests (v1.5.0): entry_ex / handle_info over the file-mode
    # surface — FILETIME timestamps from both htime encodings, solid/
    # encrypted/redir/directory flags, lazily-read CMT payload, RR size,
    # volume provenance, buffer-handle refusals. Needs openrar_core for
    # engine-side fixtures (ArchiveMutator / RecoveryWriter / HeaderWriter).
    add_executable(metadata_tests tests/unit/metadata_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(metadata_tests PRIVATE openrar_dll openrar_core
    openrar_test_doctest)
    target_include_directories(metadata_tests PRIVATE ${OPENRAR_INCLUDES} src/dll include)
    target_compile_definitions(metadata_tests PRIVATE OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    openrar_enable_test_asserts(metadata_tests)
    add_test(NAME metadata_tests COMMAND metadata_tests)

    add_executable(dll_cpp_tests tests/unit/dll_cpp_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(dll_cpp_tests PRIVATE openrar_dll openrar_test_doctest)
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
        tests/unit/abi_header_c_compat.c
        tests/support/test_runner.cpp)
    target_link_libraries(abi_layout_tests PRIVATE openrar_dll openrar_test_doctest)
    target_include_directories(abi_layout_tests PRIVATE
        ${OPENRAR_INCLUDES} src/dll include)
    target_compile_definitions(abi_layout_tests PRIVATE
        OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    openrar_enable_test_asserts(abi_layout_tests)
    add_test(NAME abi_layout_tests COMMAND abi_layout_tests)

    # Library-mode containment gates (v1.30.0 M2): the non-disableable
    # floors vs the tunable caller-budget layer, and containment parity
    # under the OPENRAR_NO_MMAP kill-switch.
    add_executable(library_mode_tests tests/unit/library_mode_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(library_mode_tests PRIVATE openrar_dll openrar_core
    openrar_test_doctest)
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

    # The verification substrate's own negative tests (v1.44 M0): runner CLI
    # contract, G-9 rules, JUnit atomic-rename, crash partial-report protocol.
    add_openrar_test(runner_tests tests/unit/runner_tests.cpp)

    # Walk diagnostic probe (not a test): prints read_block_raw results.
    add_executable(walk_probe tools/walk_probe.cpp)
    target_link_libraries(walk_probe PRIVATE openrar_core)
    target_include_directories(walk_probe PRIVATE ${OPENRAR_INCLUDES})

    # v1.37.0 two-phase Gate 0 probe (not a test): token census + alpha.
    add_executable(tokencensus tools/tokencensus.cpp)
    target_link_libraries(tokencensus PRIVATE openrar_core)
    target_include_directories(tokencensus PRIVATE ${OPENRAR_INCLUDES})

    # v1.38.0 Gate 0 probe (not a test): decode-kernel variant A/B + rebuild
    # trap reproduction (docs/v1.38.0-pre-analysis.md).
    add_executable(decode_kernel_probe tools/decode_kernel_probe.cpp)
    target_link_libraries(decode_kernel_probe PRIVATE openrar_core)
    target_include_directories(decode_kernel_probe PRIVATE ${OPENRAR_INCLUDES})

    # v1.43 M3 probe (not a test): RAT-F H1 chain-budget reachability,
    # classified by chain-walk TERMINATION REASON (log-only).
    add_executable(h1probe tools/h1probe.cpp)
    target_link_libraries(h1probe PRIVATE openrar_core)
    target_include_directories(h1probe PRIVATE ${OPENRAR_INCLUDES})

    # v1.43 M2 axis-5 probe (not a test): RAT-A arbiter cost-pricing
    # disagreement rate (log-only).
    add_executable(arbprobe tools/arbprobe.cpp)
    target_link_libraries(arbprobe PRIVATE openrar_core)
    target_include_directories(arbprobe PRIVATE ${OPENRAR_INCLUDES})

    # v1.39.0 Gate 0 probe (not a test): extraction durability cost
    # decomposition (docs/v1.39.0-pre-analysis.md).
    add_executable(durability_probe tools/durability_probe.cpp)
    target_link_libraries(durability_probe PRIVATE openrar_core)
    target_include_directories(durability_probe PRIVATE ${OPENRAR_INCLUDES})

    # v1.40 quick-win microprobe (not a test): per-entry extraction
    # operation costs for the quick-win scorecard.
    add_executable(perf_microprobe tools/perf_microprobe.cpp)
    target_link_libraries(perf_microprobe PRIVATE openrar_core)
    target_include_directories(perf_microprobe PRIVATE ${OPENRAR_INCLUDES})

    # v1.40 benchmark bundle driver (not a test): the Setup= payload of
    # openrar-bench.exe (tools/make_bench_bundle.ps1) — generates its own
    # corpora, runs the full matrix, writes the JSON report.
    # Windows-only: bench_run.cpp spawns the three engines and is a
    # #error on other platforms (same shape as ren_iso below), so the
    # target must not be declared off-Windows or the Linux/macOS CI legs
    # fail to compile.
    if(WIN32)
        add_executable(runbench tools/bench_run.cpp)
        target_link_libraries(runbench PRIVATE openrar_core)
        # advapi32 supplies the registry read of the Power Mode overlay (the
        # value PowerSetActiveOverlayScheme updates; reading needs no
        # privilege, and is done via RegGetValueW).
        target_link_libraries(runbench PRIVATE advapi32)
        target_include_directories(runbench PRIVATE ${OPENRAR_INCLUDES})
    endif()

    if(WIN32)
        add_executable(ren_iso tools/ren_iso.cpp)
        target_link_libraries(ren_iso PRIVATE openrar_core)
        target_include_directories(ren_iso PRIVATE ${OPENRAR_INCLUDES})
    endif()

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
    add_executable(sandbox_worker_e2e_tests tests/unit/sandbox_worker_e2e_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(sandbox_worker_e2e_tests PRIVATE openrar_core openrar_test_doctest)
    target_include_directories(sandbox_worker_e2e_tests PRIVATE ${OPENRAR_INCLUDES})
    if(OPENRAR_THREAD_LIB)
        target_link_libraries(sandbox_worker_e2e_tests PRIVATE ${OPENRAR_THREAD_LIB})
    endif()
    target_compile_definitions(sandbox_worker_e2e_tests PRIVATE
        OPENRAR_WORKER_EXE="$<TARGET_FILE:openrar_worker>"
        OPENRAR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    openrar_enable_test_asserts(sandbox_worker_e2e_tests)
    add_test(NAME sandbox_worker_e2e_tests COMMAND sandbox_worker_e2e_tests)

    # Sandbox model e2e (M3a/b): the OBSERVED-denial matrix per shipped
    # platform model (AppContainer / seccomp) + extraction parity through the
    # sandboxed worker. Skips loudly where no model exists (macOS/other —
    # the honest-cut follow-up).
    add_executable(sandbox_sandboxed_e2e_tests tests/unit/sandbox_sandboxed_e2e_tests.cpp tests/support/test_runner.cpp)
    target_link_libraries(sandbox_sandboxed_e2e_tests PRIVATE openrar_core openrar_test_doctest)
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
        # A sanitized libopenrar.so loaded by an unsanitized python needs the
        # sanitizer runtime preloaded (otherwise: undefined symbol
        # __ubsan_vptr_type_cache / asan_* at dlopen).
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND OPENRAR_SANITIZE_ACTIVE_MODES
           AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            set(_openrar_preload "")
            foreach(_rt IN ITEMS asan ubsan_standalone)
                if(_rt STREQUAL "asan" AND NOT OPENRAR_SANITIZE_ACTIVE_MODES MATCHES "address")
                    continue()
                endif()
                if(_rt STREQUAL "ubsan_standalone" AND NOT OPENRAR_SANITIZE_ACTIVE_MODES MATCHES "undefined")
                    continue()
                endif()
                if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                    set(_rtname "libclang_rt.${_rt}-${CMAKE_SYSTEM_PROCESSOR}.so")
                elseif(_rt STREQUAL "asan")
                    set(_rtname "libasan.so")
                else()
                    set(_rtname "libubsan.so")
                endif()
                execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_rtname}
                                OUTPUT_VARIABLE _rtpath OUTPUT_STRIP_TRAILING_WHITESPACE)
                if(IS_ABSOLUTE "${_rtpath}" AND EXISTS "${_rtpath}")
                    list(APPEND _openrar_preload "${_rtpath}")
                endif()
            endforeach()
            if(_openrar_preload)
                list(JOIN _openrar_preload ":" _openrar_preload_str)
                set_tests_properties(python_conformance PROPERTIES
                    ENVIRONMENT "LD_PRELOAD=${_openrar_preload_str};ASAN_OPTIONS=detect_leaks=0")
            endif()
        endif()

        # Aggregator fail-closed contract (v1.44 M0): in-process negative
        # tests for tools/ctest_to_junit.py.
        add_test(NAME aggregator_selftest
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tools/ctest_to_junit_selftest.py)
    endif()

    # ── Kernel throughput benchmark (informational, never a gate) ───────────
    if(NOT EMSCRIPTEN)
        add_executable(openrar_bench tests/bench/openrar_bench.cpp)
        target_link_libraries(openrar_bench PRIVATE openrar_core)
        target_include_directories(openrar_bench PRIVATE ${OPENRAR_INCLUDES})

        # Codec hot-path micro-benchmarks (compress / decompress / Delta MB/s
        # across 64 B .. 1 MiB blocks, zero-allocation timing loops with a
        # per-iteration allocation counter). Uses Google Benchmark when the
        # package is installed (find_package(benchmark)); otherwise the
        # API-compatible runner built into the source. Informational only:
        # never registered with ctest.
        add_executable(bench_target tests/bench/bench_target.cpp)
        target_link_libraries(bench_target PRIVATE openrar_core)
        target_include_directories(bench_target PRIVATE ${OPENRAR_INCLUDES})
        option(OPENRAR_GOOGLE_BENCHMARK "Link bench_target against Google Benchmark if found" ON)
        if(OPENRAR_GOOGLE_BENCHMARK)
            find_package(benchmark CONFIG QUIET)
            if(benchmark_FOUND)
                target_link_libraries(bench_target PRIVATE benchmark::benchmark)
                target_compile_definitions(bench_target PRIVATE OPENRAR_HAVE_GOOGLE_BENCHMARK=1)
                message(STATUS "bench_target: using Google Benchmark ${benchmark_VERSION}")
            else()
                message(STATUS "bench_target: Google Benchmark not found, using built-in runner")
            endif()
        endif()
    endif()

    # ── Fuzzing Infrastructure ──────────────────────────────────────────────
    option(OPENRAR_FUZZ "Enable fuzzer targets" OFF)
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

        add_executable(fuzz_rs16 tests/fuzz/fuzz_rs16.cpp)
        target_link_libraries(fuzz_rs16 PRIVATE openrar_core)
        target_include_directories(fuzz_rs16 PRIVATE ${OPENRAR_INCLUDES})
        openrar_enable_test_asserts(fuzz_rs16)
        if(OPENRAR_THREAD_LIB)
            target_link_libraries(fuzz_rs16 PRIVATE ${OPENRAR_THREAD_LIB})
        endif()

        # Audit-driven codec target (docs/review-master-plan.md): hostile
        # decode, compress->decompress round trip with truncation/bit-flip
        # oracles, sequential-vs-two-phase differential, BitReader model.
        add_executable(fuzz_target tests/fuzz/fuzz_target.cpp)
        target_link_libraries(fuzz_target PRIVATE openrar_core)
        target_include_directories(fuzz_target PRIVATE ${OPENRAR_INCLUDES})
        openrar_enable_test_asserts(fuzz_target)
        if(OPENRAR_FOREIGN_FORMATS)
            target_compile_definitions(fuzz_target PRIVATE OPENRAR_FUZZ_HAVE_INFLATE)
        endif()
        if(OPENRAR_THREAD_LIB)
            target_link_libraries(fuzz_target PRIVATE ${OPENRAR_THREAD_LIB})
        endif()

        # Inflate fuzzer (v1.29): only meaningful when the foreign-format
        # sources are built (it exercises src/compress/inflate).
        if(OPENRAR_FOREIGN_FORMATS)
            add_executable(fuzz_inflate tests/fuzz/fuzz_inflate.cpp)
            target_link_libraries(fuzz_inflate PRIVATE openrar_core)
            target_include_directories(fuzz_inflate PRIVATE ${OPENRAR_INCLUDES})
            openrar_enable_test_asserts(fuzz_inflate)

            add_executable(fuzz_zip tests/fuzz/fuzz_zip.cpp)
            target_link_libraries(fuzz_zip PRIVATE openrar_core)
            target_include_directories(fuzz_zip PRIVATE ${OPENRAR_INCLUDES})
            openrar_enable_test_asserts(fuzz_zip)
            if(OPENRAR_THREAD_LIB)
                target_link_libraries(fuzz_zip PRIVATE ${OPENRAR_THREAD_LIB})
            endif()

            add_executable(fuzz_tar tests/fuzz/fuzz_tar.cpp)
            target_link_libraries(fuzz_tar PRIVATE openrar_core)
            target_include_directories(fuzz_tar PRIVATE ${OPENRAR_INCLUDES})
            openrar_enable_test_asserts(fuzz_tar)
            if(OPENRAR_THREAD_LIB)
                target_link_libraries(fuzz_tar PRIVATE ${OPENRAR_THREAD_LIB})
            endif()
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
            target_compile_definitions(fuzz_target PRIVATE OPENRAR_USE_LIBFUZZER)
            target_compile_options(fuzz_target PRIVATE -fsanitize=fuzzer-no-link)
            target_link_options(fuzz_target PRIVATE -fsanitize=fuzzer)
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
            if(TARGET fuzz_zip)
                target_compile_definitions(fuzz_zip PRIVATE OPENRAR_USE_LIBFUZZER)
                target_compile_options(fuzz_zip PRIVATE -fsanitize=fuzzer-no-link)
                target_link_options(fuzz_zip PRIVATE -fsanitize=fuzzer)
            endif()
            if(TARGET fuzz_tar)
                target_compile_definitions(fuzz_tar PRIVATE OPENRAR_USE_LIBFUZZER)
                target_compile_options(fuzz_tar PRIVATE -fsanitize=fuzzer-no-link)
                target_link_options(fuzz_tar PRIVATE -fsanitize=fuzzer)
            endif()
            if(TARGET fuzz_rs16)
                target_compile_definitions(fuzz_rs16 PRIVATE OPENRAR_USE_LIBFUZZER)
                target_compile_options(fuzz_rs16 PRIVATE -fsanitize=fuzzer-no-link)
                target_link_options(fuzz_rs16 PRIVATE -fsanitize=fuzzer)
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
            add_test(NAME fuzz_inflate COMMAND fuzz_inflate -runs=2000 -max_len=1048576)
            add_test(NAME fuzz_target COMMAND fuzz_target -runs=2000 -max_len=262144)
            # libFuzzer mode runs until killed without -runs; an unbounded ctest entry hung
            # the sanitizer leg for an hour.
            add_test(NAME fuzz_file_handle COMMAND fuzz_file_handle -runs=500 -max_len=1048576)
            if(TARGET fuzz_zip)
                add_test(NAME fuzz_zip COMMAND fuzz_zip -runs=2000 -max_len=1048576)
            endif()
            if(TARGET fuzz_tar)
                add_test(NAME fuzz_tar COMMAND fuzz_tar -runs=2000 -max_len=1048576)
            endif()
            if(TARGET fuzz_rs16)
                add_test(NAME fuzz_rs16 COMMAND fuzz_rs16 -runs=2000 -max_len=65536)
            endif()
        else()
            add_test(NAME fuzz_decompress COMMAND fuzz_decompress)
            add_test(NAME fuzz_archive COMMAND fuzz_archive)
            add_test(NAME fuzz_inflate COMMAND fuzz_inflate)
            add_test(NAME fuzz_target COMMAND fuzz_target)
            add_test(NAME fuzz_file_handle COMMAND fuzz_file_handle)
            if(TARGET fuzz_zip)
                add_test(NAME fuzz_zip COMMAND fuzz_zip)
            endif()
            if(TARGET fuzz_tar)
                add_test(NAME fuzz_tar COMMAND fuzz_tar)
            endif()
            if(TARGET fuzz_rs16)
                add_test(NAME fuzz_rs16 COMMAND fuzz_rs16)
            endif()
        endif()
    endif()
endif()


