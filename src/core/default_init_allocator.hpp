#ifndef OPENRAR_CORE_DEFAULT_INIT_ALLOCATOR_HPP
#define OPENRAR_CORE_DEFAULT_INIT_ALLOCATOR_HPP

#include <memory>
#include <type_traits>
#include <utility>

namespace openrar::core {

// C++17 default-initialising allocator: leaves primitive types uninitialised
// on resize/allocation (OPT-D01 / OPT-P01), eliminating redundant page zeroing.
template <typename T, typename A = std::allocator<T>> struct default_init_allocator : A {
    using A::A;
    template <typename U> struct rebind {
        using other =
            default_init_allocator<U, typename std::allocator_traits<A>::template rebind_alloc<U>>;
    };
    template <typename U>
    void construct(U* ptr) noexcept(std::is_nothrow_default_constructible<U>::value) {
        ::new (static_cast<void*>(ptr)) U;
    }
    template <typename U, typename... Args> void construct(U* ptr, Args&&... args) {
        std::allocator_traits<A>::construct(static_cast<A&>(*this), ptr,
                                            std::forward<Args>(args)...);
    }
};

} // namespace openrar::core

#endif // OPENRAR_CORE_DEFAULT_INIT_ALLOCATOR_HPP
