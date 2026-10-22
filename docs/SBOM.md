# SBOM — Software Bill of Materials (v1.44)

Minimal inventory of third-party components shipped with or used by OpenRAR.
Maintained per v1.44 gate: `SBOM + THIRD_PARTY_NOTICES.md same commit`.

| Component | Version | License | Location | Used by | URL |
|---|---|---|---|---|---|
| doctest | 2.4.12 | MIT | `third_party/doctest/doctest.h` (+ `LICENSE.txt`) | unit tests only; not linked into any shipped binary | https://github.com/doctest/doctest |

No runtime dependency is added by v1.44: doctest is consumed through
`#include` by unit-test translation units only (I-8 `/UNDEBUG` and the
`openrar_enable_test_asserts` CMake property remain untouched for validects
still on `assert()`).

Not inventory-worthy (documentation-level acknowledgements only): the RAR
format research community (see `THIRD_PARTY_NOTICES.md`), referenced algorithm
papers (Intel PCLMULQDQ CRC white paper, BLAKE2 family).
