# Third Party Notices

This document contains licenses and notices for third party software used in the OpenRAR project.

## BLAKE2sp
OpenRAR uses the BLAKE2sp cryptographic hash function. The BLAKE2 reference implementation is released into the public domain (CC0 1.0 Universal) and is also available under the Apache 2.0 license.
More information: https://blake2.net/

## Fast CRC32 Computation
The SIMD-accelerated CRC32 implementation using PCLMULQDQ/carry-less multiplication is based on the algorithm described in the Intel White Paper: "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction" (Gopal, Ozturk, et al., 2009). Some implementation techniques draw inspiration from the Zlib library (Zlib license) and Chromium project (BSD 3-Clause).

## Acknowledgements
We would like to acknowledge the invaluable work of the open-source community in documenting the RAR format, which made this implementation possible. You can find out more at https://github.com/bitplane/rar-research
