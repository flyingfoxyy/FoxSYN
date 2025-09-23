#pragma once

#include <cstdint>
#include <type_traits>

#define macro static inline

namespace fox::supper {

using uint = uint32_t;
using Area = float   ;
using Edge = float   ;
using Time = uint    ;
using Sign = uint    ;
using word = uint64_t;

constexpr uint kMaxId      = 0x0FFFFFFF; // FIX ME
constexpr uint kMaxTime    = 0xFFFFFFFF;

struct Lit {
    uint p  :  1;
    uint id : 31;
    Lit()                : p(0), id(kMaxId) {}
    Lit(uint id, uint p) : p(p), id(id)     {}
    operator bool() const { return id != kMaxId; }
};

template <typename T>
macro T regular(T var) {
    static_assert(std::is_pointer_v<T> || std::is_same_v<T, Lit>);
    if constexpr (std::is_pointer_v<T>)
        return (T)((std::uint64_t)var & ~(std::uint64_t)1);
    else
        return (T)var.id;
}

template <typename T>
macro bool is_compl(T var) {
    static_assert(std::is_pointer_v<T> || std::is_same_v<T, Lit>);
    if constexpr (std::is_pointer_v<T>)
        return (std::uint64_t)var & (std::uint64_t)1;
    else
        return var.p;
}

template <typename T>
macro T compl_cond(T var, uint cond) {
    static_assert(std::is_pointer_v<T> || std::is_same_v<T, Lit>);
    if constexpr (std::is_pointer_v<T>)
        return (T)((std::uint64_t)var ^ (std::uint64_t)(cond != 0));
    else
        return T(var.id, var.p ^ cond);
}

}
