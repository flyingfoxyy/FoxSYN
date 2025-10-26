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

    bool operator==(const Lit &b) const { return _val == b._val; }
    bool operator!=(const Lit &b) const { return _val != b._val; }
    bool operator< (const Lit &b) const { return _val <  b._val; }
    bool operator<=(const Lit &b) const { return _val <= b._val; }
    bool operator> (const Lit &b) const { return _val >  b._val; }
    bool operator>=(const Lit &b) const { return _val >= b._val; }

    Lit  operator~() const { return Lit(id(), !sign()); }
    Lit &operator=(const Lit &b) = default;

    bool sign() const { return _val & 1;  }
    uint id  () const { return _val >> 1; }
    uint val () const { return _val;      }
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
    return new (std::malloc(sizeof(T) + sizeof(typename T::elem_type) * size)) T(std::forward<Args>(args)...);
}

template <typename T>
macro void deallocate(T* item) noexcept {
    if (item) {
        item->~T();
        std::free(item);
    }
}

static std::string formatted_time(double time_sec, int w, int precision = 1, char alignment = 'r') {
    std::string unit = " s";
    if (time_sec < 1.0) {
        time_sec  = time_sec * 1000.0;
        precision = 2;
        unit      = " ms";
    }
    std::string formatted;
    switch (alignment) {
    case 'l':
        formatted = std::format("{:<{}.{}f}", time_sec, w, precision);
        break;
    case 'c':
        formatted = std::format("{:^{}.{}f}", time_sec, w, precision);
        break;
    case 'r':
    default:
        formatted = std::format("{:>{}.{}f}", time_sec, w, precision);
        break;
    }
    if (formatted.length() > static_cast<size_t>(w)) {
        formatted = formatted.substr(0, w);
    }
    return formatted + unit;
}

}
