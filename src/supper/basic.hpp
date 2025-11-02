#pragma once

#include <bit>
#include <cstdint>
#include <ctime>
#include <format>
#include <ostream>
#include <type_traits>
#include <memory>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <cassert>

#include "macros.hpp"

namespace fox::supper {
// ====================================================================
// Type definitions
// ====================================================================
using uint = uint32_t;
using Area = float   ;
using Edge = float   ;
using Time = uint    ;
using Sign = uint    ;
using word = uint64_t;

// ====================================================================
// Constants
// ====================================================================
constexpr Time kMaxTime = 123456;

// ====================================================================
// Literals
// ====================================================================
class Lit {
    uint _val;
public:
    explicit Lit(uint v, uint c = 0) : _val((v << 1) | (c & 1)) {}
             Lit()                   : _val(0)                  {}

    ~Lit() = default;

    Inline bool operator==(const Lit &b) const { return _val == b._val; }
    Inline bool operator!=(const Lit &b) const { return _val != b._val; }
    Inline bool operator< (const Lit &b) const { return _val <  b._val; }
    Inline bool operator<=(const Lit &b) const { return _val <= b._val; }
    Inline bool operator> (const Lit &b) const { return _val >  b._val; }
    Inline bool operator>=(const Lit &b) const { return _val >= b._val; }
    Inline bool operator==(uint b)       const { return id() == b;      }
    Inline bool operator!=(uint b)       const { return id() != b;      }
    Inline bool operator< (uint b)       const { return id() <  b;      }
    Inline bool operator<=(uint b)       const { return id() <= b;      }
    Inline bool operator> (uint b)       const { return id() >  b;      }
    Inline bool operator>=(uint b)       const { return id() >= b;      }

    Inline Lit  operator~() const { return Lit(id(), !sign()); }
    Inline Lit &operator=(const Lit &b) = default;

    Inline uint operator-(int diff) { return id() - diff; }
    Inline uint operator+(int diff) { return id() + diff; }

    Inline bool sign() const { return _val & 1;  }
    Inline uint id  () const { return _val >> 1; }
    Inline uint val () const { return _val;      }

    Inline friend std::ostream &operator<<(std::ostream &os, Lit l) {
        return os << l.sign() << "," << l.id();
    }
};

template<typename T>
class Array : public std::vector<T> {
    uint _offset {0};
public:
    enum class flag_t : uint8_t {
        RESERVE,
        ALLOC
    };

    template <typename... Args>
    Array(Args&&... args) : std::vector<T>(std::forward<Args>(args)...) {}

    ~Array() = default;

    Inline const T &operator[](Lit lit)           const { return std::vector<T>::operator[](lit.id() - _offset); }
    Inline const T &operator[](std::size_t index) const { return std::vector<T>::operator[](index - _offset);    }

    Inline T &operator[](Lit lit)           { return std::vector<T>::operator[](lit.id() - _offset); }
    Inline T &operator[](std::size_t index) { return std::vector<T>::operator[](index - _offset);    }

    Inline void push_unique(const T &item) {
        if (std::find(this->begin(), this->end(), item) == this->end()) {
            this->push_back(item);
        }
    }

    Inline void set_offset(uint offset) { _offset = offset; }
};

// ====================================================================
// Concepts 
// ====================================================================
template<typename T>
concept Indexable = std::integral<T> || std::same_as<T, Lit>;

// ====================================================================
// Pointer manipulation
// ====================================================================
template <typename T>
Inline static T regular(T var) {
    static_assert(std::is_pointer_v<T>);
    return (T)((std::uint64_t)var & ~(std::uint64_t)1);
}

template <typename T>
Inline static bool is_signed(T var) {
    static_assert(std::is_pointer_v<T>);
    return (std::uint64_t)var & (std::uint64_t)1;
}

template <typename T>
Inline static T compl_cond(T var, uint cond) {
    static_assert(std::is_pointer_v<T>);
    return (T)((std::uint64_t)var ^ (std::uint64_t)(cond != 0));
}

// ====================================================================
// For class with dynamic array
// ====================================================================
// template <typename T, typename... Args>
// Inline static T* allocate(uint size, Args&&... args) {
//     return new (std::malloc(sizeof(T) + sizeof(typename T::elem_type) * size)) T(std::forward<Args>(args)...);
// }

// template <typename T>
// Inline static void deallocate(T* item) noexcept {
//     if (item) {
//         item->~T();
//         std::free(item);
//     }
// }


// ====================================================================
// Timer
// ====================================================================
class Timer {
    std::unordered_map<std::string, decltype(std::chrono::high_resolution_clock::now())> _wall_points;
    std::unordered_map<std::string, decltype(std::clock())> _cpu_points;
    std::unordered_map<std::string, double> _cpu_durations;
    std::unordered_map<std::string, double> _wall_durations;
    std::unordered_set<std::string> _names;

    uint  _width     {5};
    uint  _precision {1};
    char  _alignment {'r'};

    std::clock_t _cpu_start;
public:

    Timer() {
        _cpu_start = std::clock();
    }

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

    void start(const std::string &name) {
        _cpu_points[name]  = std::clock();
        _wall_points[name] = std::chrono::high_resolution_clock::now();
        _names.insert(name);
    }

    void stop(const std::string &name) {
        if (_names.find(name) == _names.end()) [[unlikely]] {
            assert(0 && "Timer stop called without start");
            return;
        }
        auto   cpu_end       = std::clock();
        double cpu_duration  = double(cpu_end - _cpu_points[name]) / CLOCKS_PER_SEC;
        auto   wall_end      = std::chrono::high_resolution_clock::now();
        double wall_duration = std::chrono::duration<double>(wall_end - _wall_points[name]).count();
        _cpu_durations[name]  += cpu_duration;
        _wall_durations[name] += wall_duration;
    }

    std::string time_duration_cpu(const std::string &name, bool fuzzy = false) const {
        if (fuzzy) {
            double total = 0.0;
            for (const auto &[key, duration] : _cpu_durations) {
                if (key.find(name) != std::string::npos) {
                    total += duration;
                }
            }
            return Timer::formatted_time(total, _width, _precision, _alignment);
        }
        if (_cpu_durations.find(name) == _cpu_durations.end())
            return "None";
        return Timer::formatted_time(_cpu_durations.at(name), _width, _precision, _alignment);
    }

    std::string time_duration_wall(const std::string &name, bool fuzzy = false) const {
        if (fuzzy) {
            double total = 0.0;
            for (const auto &[key, duration] : _wall_durations) {
                if (key.find(name) != std::string::npos) {
                    total += duration;
                }
            }
            return Timer::formatted_time(total, _width, _precision, _alignment);
        }
        if (_wall_durations.find(name) == _wall_durations.end())
            return "None";
        return Timer::formatted_time(_wall_durations.at(name), _width, _precision, _alignment);
    }

    void report(std::ostream &os) const {
        const double cpu_time = double(std::clock() - _cpu_start) / CLOCKS_PER_SEC;
        os << std::format(" {:<20} {:>15} {:>15} {:>12}\n", "Action", "CPU Time", "Wall Time", "Ratio");
        #define PRINT_TIME(Stage)                                         \
        os << std::format(" {:<20} {:>15} {:>15} {:>10.1f} %\n", Stage,   \
            time_duration_cpu(Stage),                                     \
            time_duration_wall(Stage),                                    \
            _cpu_durations. at(Stage) * 100.0 / cpu_time                  \
        );
        PRINT_TIME("create_graph"  )
        PRINT_TIME("lut_mapping"   )
        PRINT_TIME("create_gate"   )
        PRINT_TIME("forward"       )
        PRINT_TIME("backward"      )
        PRINT_TIME("exact_imp"     )
        PRINT_TIME("create_abc_ntk")
    }
};

// ====================================================================
// Cut related utility functions
// ====================================================================
static Inline uint *set_union(uint *dest,
                              const uint *begin1, const uint *end1,
                              const uint *begin2, const uint *end2,
                              uint max_size)
{
  int size1 = end1 - begin1;
  int size2 = end2 - begin2;
  int i, k, c, s;

  // both cuts are the largest
  if (size1 == max_size && size2 == max_size) [[unlikely]] {
    for (i = 0; i < size1; i++) {
      if (begin1[i] != begin2[i])
        return nullptr;
      dest[i] = begin1[i];
    }
    return dest + max_size;
  }

  // compare two cuts with different numbers
  i = k = c = s = 0;
  while (1) {
    if (c == max_size)
      return nullptr;
    if (begin1[i] < begin2[k]) {
      dest[c++] = begin1[i++];
      if (i == size1)
        goto FlushCut1;
    } else if (begin1[i] > begin2[k]) {
      dest[c++] = begin2[k++];
      if (k == size2)
        goto FlushCut0;
    } else {
      dest[c++] = begin1[i++];
      k++;
      if (i == size1)
        goto FlushCut1;
      if (k == size2)
        goto FlushCut0;
    }
  }

FlushCut0:
  if (c + size1 > max_size + i)
    return nullptr;
  while (i < size1)
    dest[c++] = begin1[i++];
  return dest + c;

FlushCut1:
  if (c + size2 > max_size + k)
    return nullptr;
  while (k < size2)
    dest[c++] = begin2[k++];
  return dest + c;
}

// ====================================================================
// popcount
// ====================================================================
template <typename T>
static Inline int popcount(T var) {
    static_assert(std::is_integral_v<T>);
    return std::popcount(var);
}

} // namespace fox::supper
