#pragma once
// Sanitizer-build detection. GCC defines __SANITIZE_*__; clang only exposes
// __has_feature(*_sanitizer), so both spellings are needed.
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define OPENRAR_ASAN_BUILD 1
#else
#define OPENRAR_ASAN_BUILD 0
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#define OPENRAR_TSAN_BUILD 1
#else
#define OPENRAR_TSAN_BUILD 0
#endif
#if defined(__SANITIZE_MEMORY__) || __has_feature(memory_sanitizer)
#define OPENRAR_MSAN_BUILD 1
#else
#define OPENRAR_MSAN_BUILD 0
#endif
// ASan/TSan/MSan reserve terabytes of shadow address space, so an address-space
// rlimit cannot coexist with them.
#define OPENRAR_SANITIZER_SHADOW_BUILD                                                             \
    (OPENRAR_ASAN_BUILD || OPENRAR_TSAN_BUILD || OPENRAR_MSAN_BUILD)
// Any sanitizer at all (UBSan has no __has_feature spelling; sanitizers.cmake
// defines OPENRAR_SANITIZED_BUILD whenever a mode is active). Runtimes read
// /proc and probe memory via syscalls a seccomp sandbox denies, so sandboxed
// child-process suites cannot run under them.
#if OPENRAR_SANITIZER_SHADOW_BUILD || defined(OPENRAR_SANITIZED_BUILD)
#define OPENRAR_ANY_SANITIZER_BUILD 1
#else
#define OPENRAR_ANY_SANITIZER_BUILD 0
#endif