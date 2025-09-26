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

class Lit {
    uint _val;
public:
    Lit(uint v, uint c = 0) : _val((v << 1) | (c & 1)) {}
    Lit()                   : _val(0)                  {}

    ~Lit() = default;

    bool operator==(const Lit &b) const { return _val == b._val; }
    bool operator!=(const Lit &b) const { return _val != b._val; }
    bool operator< (const Lit &b) const { return _val <  b._val; }
    bool operator<=(const Lit &b) const { return _val <= b._val; }
    bool operator> (const Lit &b) const { return _val >  b._val; }
    bool operator>=(const Lit &b) const { return _val >= b._val; }

    Lit operator~() const { return Lit(id(), !is_complement()); }
    Lit operator=(const Lit &b) = default;

    bool is_complement() const { return _val & 1;  }
    uint id()            const { return _val >> 1; }
};

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
