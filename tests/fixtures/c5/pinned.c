/* C5 real-object source.
 *
 * Freestanding and deterministic: no headers, no libc calls, no external
 * state. It exists to be cross-compiled by clang for the C5 ISA corpus:
 *
 *   clang --target=aarch64-linux-gnu -O2 -c pinned.c -o elf_aarch64.o
 *   clang --target=riscv64-linux-gnu -O2 -c pinned.c -o elf_riscv64.o
 *   clang --target=x86_64-linux-gnu  -O2 -c pinned.c -o elf_x86_64.o
 *
 * The mix/call/switch structure is deliberate: it emits real calls, branches
 * and a jump table, so the E8 density heuristic has something honest to chew
 * on for x86-64 while the other targets produce genuine non-x86 code. The
 * c5_fn sequence exists because -O2 otherwise collapses a small translation
 * unit into one function with no call sites at all, which would make the
 * x86-64 control useless to the filter and to its gate.
 *
 * This file is pinned by SHA-256 in tools/build_c5_real_corpus.py and the
 * README; changing it changes the corpus and voids previous measurements.
 */

typedef unsigned int u32;
typedef unsigned long long u64;

/* Keep real call instructions in the object: at -O2 clang would inline this
 * whole translation unit and leave no call sites. */
#if defined(__clang__) || defined(__GNUC__)
#define C5_NOINLINE __attribute__((noinline))
#else
#define C5_NOINLINE
#endif

static C5_NOINLINE u32 mix(u32 x) {
    return x * 2654435761u + 0x9e3779b9u;
}

static C5_NOINLINE u32 step(u32 a, u32 b) {
    return mix(a ^ (b << 3)) + (b >> 5);
}

#define C5_DEF(n)                                                                                  \
    static C5_NOINLINE u32 c5_fn##n(u32 x) {                                                       \
        return mix(x ^ (0x9e3779b9u * (n)));                                                       \
    }
C5_DEF(1)
C5_DEF(2)
C5_DEF(3)
C5_DEF(4)
C5_DEF(5)
C5_DEF(6)
C5_DEF(7)
C5_DEF(8)
C5_DEF(9)
C5_DEF(10)
C5_DEF(11)
C5_DEF(12)
C5_DEF(13)
C5_DEF(14)
C5_DEF(15)
C5_DEF(16)
C5_DEF(17)
C5_DEF(18)
C5_DEF(19)
C5_DEF(20)
C5_DEF(21)
C5_DEF(22)
C5_DEF(23)
C5_DEF(24)
C5_DEF(25)
C5_DEF(26)
C5_DEF(27)
C5_DEF(28)
C5_DEF(29)
C5_DEF(30)
C5_DEF(31)
C5_DEF(32)
C5_DEF(33)
C5_DEF(34)
C5_DEF(35)
C5_DEF(36)
C5_DEF(37)
C5_DEF(38)
C5_DEF(39)
C5_DEF(40)
#undef C5_DEF

#define C5_CALL(n) a = c5_fn##n(a) + (n);
C5_NOINLINE u32 c5_wide(u32 seed) {
    u32 a = seed;
    C5_CALL(1);
    C5_CALL(2);
    C5_CALL(3);
    C5_CALL(4);
    C5_CALL(5);
    C5_CALL(6);
    C5_CALL(7);
    C5_CALL(8);
    C5_CALL(9);
    C5_CALL(10);
    C5_CALL(11);
    C5_CALL(12);
    C5_CALL(13);
    C5_CALL(14);
    C5_CALL(15);
    C5_CALL(16);
    C5_CALL(17);
    C5_CALL(18);
    C5_CALL(19);
    C5_CALL(20);
    C5_CALL(21);
    C5_CALL(22);
    C5_CALL(23);
    C5_CALL(24);
    C5_CALL(25);
    C5_CALL(26);
    C5_CALL(27);
    C5_CALL(28);
    C5_CALL(29);
    C5_CALL(30);
    C5_CALL(31);
    C5_CALL(32);
    C5_CALL(33);
    C5_CALL(34);
    C5_CALL(35);
    C5_CALL(36);
    C5_CALL(37);
    C5_CALL(38);
    C5_CALL(39);
    C5_CALL(40);
    return a;
}
#undef C5_CALL

u32 c5_table[64];

C5_NOINLINE u64 c5_kernel(u32 seed, const u32* in, u32 n) {
    u32 acc = seed ^ 0x5bf03635u;
    u32 i = 0;
    for (i = 0; i < n; ++i) {
        acc = step(acc, in[i & 15]) + mix(acc >> 7);
        if (acc & 1u) {
            acc ^= step(in[i & 15], acc);
        } else {
            acc += mix(in[(i + 1) & 15]);
        }
        switch (acc & 3u) {
        case 0:
            acc = mix(acc);
            break;
        case 1:
            acc += 0x9e3779b9u;
            break;
        case 2:
            acc ^= acc >> 11;
            break;
        default:
            acc = step(acc, in[i & 15]);
            break;
        }
    }
    return ((u64)acc << 32) | mix(acc);
}

C5_NOINLINE void c5_fill(u32 seed) {
    u32 i = 0;
    for (i = 0; i < 64; ++i) {
        c5_table[i] = step(seed, i) + mix(i);
    }
}

C5_NOINLINE u32 c5_fold(const u32* in, u32 n) {
    u32 a = 0;
    u32 i = 0;
    for (i = 0; i < n; ++i) {
        a = step(a, in[i]) + mix(a + i);
    }
    return a;
}

C5_NOINLINE u32 c5_entry(u32 seed) {
    c5_fill(seed);
    return c5_fold(c5_table, 64) ^ c5_wide(seed);
}
