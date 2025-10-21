#pragma once

#include <cstdint>
#include <format>
#include <type_traits>
#include <memory>
#include <cstdlib>
#include <vector>

#define macro static inline

namespace fox::supper {

using uint = uint32_t;
using Area = float   ;
using Edge = float   ;
using Time = uint    ;
using Sign = uint    ;
using word = uint64_t;


constexpr Time kMaxTime = 123456;

class Lit {
    uint _val;
public:
    explicit Lit(uint v, uint c = 0) : _val((v << 1) | (c & 1)) {}
             Lit()                   : _val(0)                  {}

    ~Lit() = default;

    [[always_inline]] bool operator==(const Lit &b) const { return _val == b._val; }
    [[always_inline]] bool operator!=(const Lit &b) const { return _val != b._val; }
    [[always_inline]] bool operator< (const Lit &b) const { return _val <  b._val; }
    [[always_inline]] bool operator<=(const Lit &b) const { return _val <= b._val; }
    [[always_inline]] bool operator> (const Lit &b) const { return _val >  b._val; }
    [[always_inline]] bool operator>=(const Lit &b) const { return _val >= b._val; }

    [[always_inline]] Lit operator~() const { return Lit(id(), !is_complement()); }
    Lit &operator=(const Lit &b) = default;

    [[always_inline]] bool is_complement() const { return _val & 1;  }
    [[always_inline]] uint id()            const { return _val >> 1; }
    [[always_inline]] uint val()           const { return _val;      }
};

template<typename T>
class SigMap : public std::vector<T> {
public:
    enum class flag_t : uint8_t {
        RESERVE,
        ALLOCATE
    };

    template <typename... Args>
    SigMap(Args&&... args) : std::vector<T>(std::forward<Args>(args)...) {}

    ~SigMap() = default;

    const T &operator[](Lit lit)           const { return std::vector<T>::operator[](lit.id()); }
    const T &operator[](std::size_t index) const { return std::vector<T>::operator[](index);    }

    T &operator[](Lit lit)           { return std::vector<T>::operator[](lit.id()); }
    T &operator[](std::size_t index) { return std::vector<T>::operator[](index);    }
};

// mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Concepts 
template<typename T>
concept Indexable = std::integral<T> || std::same_as<T, Lit>;

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
macro T* allocate(uint size, Args&&... args) {
    constexpr uint preserved_size = T::R;
    const     uint extra_size     = preserved_size >= size ? 0 : (size - preserved_size);
    return new (std::malloc(sizeof(T) + sizeof(typename T::elem_type) * extra_size)) T(std::forward<Args>(args)...);
}

template <typename T>
macro void deallocate(T* item) noexcept {
    if (item) {
        item->~T();
        std::free(item);
    }
}

static std::string format_time(double time, const int width) {
    std::string str = std::format("{:{}.3f} ", time, width - 4);
    if (time >= 1.0)
        str += "s";
    else
        str += "ms";
    return str;
}

}
