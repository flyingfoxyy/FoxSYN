#pragma once

#include <cstdint>
#include <type_traits>
#include <memory>
#include <cstdlib>

#define macro static inline

namespace fox::supper {

using uint = uint32_t;
using Area = float   ;
using Edge = float   ;
using Time = uint    ;
using Sign = uint    ;
using word = uint64_t;
using Lit  = uint    ;

#define Lit2Var(lit)    ((lit) >> 1)
#define Var2Lit(var, c) ((var) + (var) + c)

template <typename T>
macro T regular(T var) {
    static_assert(std::is_pointer_v<T> || std::is_unsigned_v<T>);
    if constexpr (std::is_pointer_v<T>)
        return (T)((std::uint64_t)var & ~(std::uint64_t)1);
    else
        return (T)var.id;
}

template <typename T>
macro bool is_complement(T var) {
    static_assert(std::is_pointer_v<T> || std::is_unsigned_v<T>);
    if constexpr (std::is_pointer_v<T>)
        return (std::uint64_t)var & (std::uint64_t)1;
    else
        return var.p;
}

template <typename T>
macro T compl_cond(T var, uint cond) {
    static_assert(std::is_pointer_v<T> || std::is_unsigned_v<T>);
    if constexpr (std::is_pointer_v<T>)
        return (T)((std::uint64_t)var ^ (std::uint64_t)(cond != 0));
    else
        return T(var.id, var.p ^ cond);
}

// mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// For class with dynamic array

template <typename T, typename... Args>
static inline T* allocate(uint size, Args&&... args) {
    constexpr uint preserved_size = T::R;
    const     uint extra_size     = preserved_size >= size ? 0 : (size - preserved_size);
    return new (std::malloc(sizeof(T) + sizeof(typename T::elem_type) * extra_size)) T(std::forward<Args>(args)...);
}

template <typename T>
static inline void deallocate(T* elem) noexcept {
    assert(elem);
    elem->~T();
    std::free(elem);
}

}
