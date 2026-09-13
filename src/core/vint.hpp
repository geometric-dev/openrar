#ifndef OPENRAR_CORE_VINT_HPP
#define OPENRAR_CORE_VINT_HPP

#include "types.hpp"
#include <vector>

namespace openrar::core {

// Maximum bytes needed to encode any 64-bit vint (ceil(64/7) = 10 bytes)
constexpr size_t MAX_VINT_SIZE = 10;

// Appends variable-length integer Value to Vect
void push_vint(std::vector<byte>& vect, uint64 value);

// Appends variable-length integer Value padded to exactly width bytes
void push_vint_fixed(std::vector<byte>& vect, uint64 value, size_t width);

// Reads variable-length integer from buffer [data, data + size].
// Returns true on success, populating value and bytes_read.
// Returns false if buffer ends prematurely or integer overflows 64-bit range.
bool read_vint(const byte* data, size_t size, uint64& value, size_t& bytes_read);

} // namespace openrar::core

#endif // OPENRAR_CORE_VINT_HPP
