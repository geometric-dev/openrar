// Internal forwarding shim: the C ABI declarations live in the public header
// include/openrar/openrar_dll.h (installed with the package). src/ translation
// units keep including "openrar_dll.h" from this directory.
#ifndef OPENRAR_DLL_SRC_FORWARDING_SHIM_H
#define OPENRAR_DLL_SRC_FORWARDING_SHIM_H

#include "../../include/openrar/openrar_dll.h"

#endif // OPENRAR_DLL_SRC_FORWARDING_SHIM_H
