# Third Party Notices

This document contains licenses and notices for third party software used in the OpenRAR project.

## BLAKE2sp
OpenRAR uses the BLAKE2sp cryptographic hash function. The BLAKE2 reference implementation is released into the public domain (CC0 1.0 Universal) and is also available under the Apache 2.0 license.
More information: https://blake2.net/

## Fast CRC32 Computation
The SIMD-accelerated CRC32 implementation using PCLMULQDQ/carry-less multiplication is based on the algorithm described in the Intel White Paper: "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction" (Gopal, Ozturk, et al., 2009). Some implementation techniques draw inspiration from the Zlib library (Zlib license) and Chromium project (BSD 3-Clause).

## doctest (test-only)
OpenRAR's unit test suites (v1.44 Verification Substrate) use the doctest
framework, vendored as a single header in `third_party/doctest/doctest.h`.
Pinned to release v2.4.12 (2025-04-28), SHA-256
94029A7D32DA24A56249658147DBD2B33FF0B9ED665295CBBAF19AAFFF5B0CED.
License text is reproduced alongside the header (`third_party/doctest/LICENSE.txt`).
Referenced from the tests only; it is not linked into any shipped binary.

The MIT License (MIT)
Copyright (c) 2016-2023 Viktor Kirilov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Acknowledgements
We would like to acknowledge the invaluable work of the open-source community in documenting the RAR format, which made this implementation possible. You can find out more at https://github.com/bitplane/rar-research
