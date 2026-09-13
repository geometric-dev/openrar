#ifndef OPENRAR_CLI_THREAD_POOL_HPP
#define OPENRAR_CLI_THREAD_POOL_HPP

// The pool moved to core so the recovery layer can parallelize RR parity
// too; this forwarding header keeps the CLI's includes stable.
#include "../core/thread_pool.hpp"

namespace openrar::cli {
using core::ByteBudget;
using core::ThreadPool;
using core::hardware_thread_hint;
} // namespace openrar::cli

#endif // OPENRAR_CLI_THREAD_POOL_HPP
