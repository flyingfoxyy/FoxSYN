#pragma once

#include <cstdint>
#include <format>
#include <type_traits>
#include <memory>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <chrono>

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

// ====================================================================
// Pointer manipulation
// ====================================================================
template <typename T>
macro T regular(T var) {
    static_assert(std::is_pointer_v<T>);
    return (T)((std::uint64_t)var & ~(std::uint64_t)1);
}

template <typename T>
macro bool is_signed(T var) {
    static_assert(std::is_pointer_v<T>);
    return (std::uint64_t)var & (std::uint64_t)1;
}

template <typename T>
macro T compl_cond(T var, uint cond) {
    static_assert(std::is_pointer_v<T>);
    return (T)((std::uint64_t)var ^ (std::uint64_t)(cond != 0));
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


// mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Timer
class Timer {
    std::unordered_map<std::string, decltype(std::chrono::high_resolution_clock::now())> _wall_points[2];
    std::unordered_map<std::string, decltype(std::clock())> _cpu_points[2];

    int  _width     {5};
    int  _precision {1};
    char _alignment {'r'};

public:
     Timer() = default;
    ~Timer() = default;

    static std::string formatted_time(double time_sec, int width, int precision = 1, char alignment = 'r') {
        std::string unit = " s";
        if (time_sec < 1.0) {
            time_sec  = time_sec * 1000.0;
            precision = 2;
            unit      = " ms";
        }
        std::string formatted;
        switch (alignment) {
        case 'l':
            formatted = std::format("{:<{}.{}f}", time_sec, width, precision);
            break;
        case 'c':
            formatted = std::format("{:^{}.{}f}", time_sec, width, precision);
            break;
        case 'r':
        default:
            formatted = std::format("{:>{}.{}f}", time_sec, width, precision);
            break;
        }
        if (formatted.length() > static_cast<size_t>(width)) {
            formatted = formatted.substr(0, width);
        }
        return formatted + unit;
    }

    void time_point_start(const std::string &name) {
        _cpu_points[0][name]  = std::clock();
        _wall_points[0][name] = std::chrono::high_resolution_clock::now();
    }

    void time_point_end(const std::string &name) {
        _cpu_points[1][name]  = std::clock();
        _wall_points[1][name] = std::chrono::high_resolution_clock::now();
    }

    std::string time_duration_cpu (const std::string &name) const {
        auto start = _cpu_points[0].find(name);
        auto end   = _cpu_points[1].find(name);
        if (start == _cpu_points[0].end() || end == _cpu_points[1].end())
            return "None";
        return Timer::formatted_time(double(end->second - start->second) / CLOCKS_PER_SEC, _width, _precision, _alignment);
    }

    std::string time_duration_wall(const std::string &name) const {
        auto start = _wall_points[0].find(name);
        auto end   = _wall_points[1].find(name);
        if (start == _wall_points[0].end() || end == _wall_points[1].end())
            return "None";
        return formatted_time(std::chrono::duration<double>(end->second - start->second).count(), _width, _precision, _alignment);
    }

};

} // namespace fox::supper
