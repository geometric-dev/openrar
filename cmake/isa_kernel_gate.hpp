#ifndef OPENRAR_ISA_KERNEL_GATE_HPP
#define OPENRAR_ISA_KERNEL_GATE_HPP

// Force-included in every translation unit by cmake/openrar_flags.cmake when
// the AVX-512 match kernel and the GFNI RS16 fold kernel are enabled.
//
// Those kernels are switched on with add_compile_definitions, which applies to
// the whole target. A universal2 macOS build compiles every TU twice (arm64 and
// x86_64) from that single definition set, while the kernel bodies are gated on
// the capability define alone and use x86 intrinsics. Undefining the capability
// on a non-x86 target keeps the arm64 slice compiling and leaves the x86_64
// slice untouched, so the Intel half of a fat binary keeps the same kernels as
// a native x86-64 build. Runtime selection is unchanged: the dispatchers still
// require CPU support and OS-enabled ZMM state before using them.

#if !defined(__x86_64__) && !defined(_M_X64)
#undef OPENRAR_HAS_AVX512_KERNEL
#undef OPENRAR_HAS_GFNI_KERNEL
#endif

#endif // OPENRAR_ISA_KERNEL_GATE_HPP
