#ifndef OPENRAR_ISA_KERNEL_GATE_HPP
#define OPENRAR_ISA_KERNEL_GATE_HPP

// Force-included by cmake/openrar_flags.cmake for Apple builds when the AVX-512
// match kernel and the GFNI RS16 fold kernel are enabled.
//
// Those kernels are switched on with add_compile_definitions, which applies to
// the whole target, and their bodies are gated on the capability define alone.
// A universal2 build compiles every TU for both slices from that single
// definition set, so the defines would reach the arm64 slice and its TUs would
// try to compile x86 intrinsics. Dropping them there keeps the arm64 slice
// building while the x86_64 slice keeps the same kernels as a native x86-64
// build. Runtime selection is unchanged: the dispatchers still require CPU
// support and OS-enabled ZMM state before using them.

#if !defined(__x86_64__)
#undef OPENRAR_HAS_AVX512_KERNEL
#undef OPENRAR_HAS_GFNI_KERNEL
#endif

#endif // OPENRAR_ISA_KERNEL_GATE_HPP
