/*
 * abi_header_c_compat.c — C-mode standalone compile + layout proof for the
 * frozen public C ABI (docs/abi-freeze.md).
 *
 * Compiled as a C translation unit inside the abi_layout_tests executable:
 * if the public header stops compiling as plain C, or its packed structs
 * change size/offset, this file fails the build (compile) or the test
 * (runtime checks below). The C++ side of the same surface is pinned by
 * src/dll/dll_api.cpp static_asserts and abi_layout_tests.cpp.
 *
 * Deliberately C89-safe in constructs and dependency-free: stdint/stddef
 * only, no static_assert (dialect differences across MSVC/GCC/Clang C
 * modes) — everything is checked at runtime by abi_c_mode_layout_check().
 */
#include <openrar/openrar_dll.h>

#include <stddef.h>
#include <stdint.h>

int abi_c_mode_version_probe(void) {
    /* The host-side contract from docs/dll-integration-spec.md §3. */
    return openrar_version() == OPENRAR_DLL_API_VERSION && openrar_archive_version() == 1;
}

int abi_c_mode_layout_check(void) {
    int failures = 0;

    if (sizeof(openrar_archive_entry_t) != 64) ++failures;
    if (sizeof(openrar_entry_ex_t) != 48) ++failures;
    if (sizeof(openrar_entry_owner_t) != 20) ++failures;
    if (sizeof(openrar_archive_info_t) != 24) ++failures;

    if (offsetof(openrar_archive_entry_t, path_offset) != 0) ++failures;
    if (offsetof(openrar_archive_entry_t, path_len) != 4) ++failures;
    if (offsetof(openrar_archive_entry_t, is_dir) != 8) ++failures;
    if (offsetof(openrar_archive_entry_t, method) != 12) ++failures;
    if (offsetof(openrar_archive_entry_t, is_encrypted) != 16) ++failures;
    if (offsetof(openrar_archive_entry_t, crc32) != 20) ++failures;
    if (offsetof(openrar_archive_entry_t, size) != 24) ++failures;
    if (offsetof(openrar_archive_entry_t, packed_size) != 32) ++failures;
    if (offsetof(openrar_archive_entry_t, mtime) != 40) ++failures;
    if (offsetof(openrar_archive_entry_t, _pad) != 48) ++failures;

    if (offsetof(openrar_entry_ex_t, attrs) != 0) ++failures;
    if (offsetof(openrar_entry_ex_t, host_os) != 4) ++failures;
    if (offsetof(openrar_entry_ex_t, mtime_ft) != 8) ++failures;
    if (offsetof(openrar_entry_ex_t, ctime_ft) != 16) ++failures;
    if (offsetof(openrar_entry_ex_t, atime_ft) != 24) ++failures;
    if (offsetof(openrar_entry_ex_t, flags) != 32) ++failures;
    if (offsetof(openrar_entry_ex_t, win_size) != 36) ++failures;
    if (offsetof(openrar_entry_ex_t, redir_type) != 40) ++failures;
    if (offsetof(openrar_entry_ex_t, version_needed) != 44) ++failures;

    if (offsetof(openrar_entry_owner_t, uid) != 0) ++failures;
    if (offsetof(openrar_entry_owner_t, gid) != 8) ++failures;
    if (offsetof(openrar_entry_owner_t, flags) != 16) ++failures;

    if (offsetof(openrar_archive_info_t, flags) != 0) ++failures;
    if (offsetof(openrar_archive_info_t, volume_index) != 4) ++failures;
    if (offsetof(openrar_archive_info_t, volume_count) != 8) ++failures;
    if (offsetof(openrar_archive_info_t, recovery_size) != 12) ++failures;
    if (offsetof(openrar_archive_info_t, comment_len) != 20) ++failures;

    /* Enum values are part of the frozen surface (docs/abi-freeze.md); the
       reserved -8/-10 slots and the next slot -16 are documented. */
    if (RAR_OK != 0) ++failures;
    if (RAR_ERR_PARTIAL_OK != 1) ++failures;
    if (RAR_ERR_NOT_RAR != -1) ++failures;
    if (RAR_ERR_UNSUPPORTED_FEATURE != -2) ++failures;
    if (RAR_ERR_TRUNCATED != -3) ++failures;
    if (RAR_ERR_CRC_MISMATCH != -4) ++failures;
    if (RAR_ERR_NOMEM != -5) ++failures;
    if (RAR_ERR_IO != -6) ++failures;
    if (RAR_ERR_BAD_PASSWORD != -7) ++failures;
    if (RAR_ERR_INVALID_ARG != -9) ++failures;
    if (RAR_ERR_ABORTED != -11) ++failures;
    if (RAR_ERR_ENCRYPTED != -12) ++failures;
    if (RAR_ERR_MISSING_VOLUME != -13) ++failures;
    if (RAR_ERR_BUSY != -14) ++failures;
    if (RAR_ERR_LIMIT_EXCEEDED != -15) ++failures;

    return failures;
}
