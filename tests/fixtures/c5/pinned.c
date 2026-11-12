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
 * on for x86-64 while the other targets produce genuine non-x86 code.
 *
 * This file is pinned by SHA-256 in tools/build_c5_real_corpus.py and the
 * README; changing it changes the corpus and voids previous measurements.
 */

typedef unsigned int u32;
typedef unsigned long long u64;

static u32 mix(u32 x) {
    return x * 2654435761u + 0x9e3779b9u;
}

static u32 step(u32 a, u32 b) {
    return mix(a ^ (b << 3)) + (b >> 5);
}

u32 c5_table[64];

u64 c5_kernel(u32 seed, const u32* in, u32 n) {
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

void c5_fill(u32 seed) {
    u32 i = 0;
    for (i = 0; i < 64; ++i) {
        c5_table[i] = step(seed, i) + mix(i);
    }
}

u32 c5_fold(const u32* in, u32 n) {
    u32 a = 0;
    u32 i = 0;
    for (i = 0; i < n; ++i) {
        a = step(a, in[i]) + mix(a + i);
    }
    return a;
}

u32 c5_entry(u32 seed) {
    c5_fill(seed);
    return c5_fold(c5_table, 64) ^ seed;
}
