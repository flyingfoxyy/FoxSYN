/* 
 @ mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
 @
 @                      Supermap for LUT-based FPGAs
 @
 @                                Author
 @                              Longfei Fan
 @
 @                           Licence      MIT
 @                          Copyright     Longfei Fan
 @
 @ mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
 */

#pragma once

#include <cstddef>
#include <cstdint>

using uint = uint32_t;

// ========================================================================
// Attributes
// ========================================================================
#ifdef NDEBUG
    #ifdef __GNUC__
        #define Inline inline __attribute__((always_inline))
    #elif __clang__
        #define Inline inline __attribute__((always_inline))
    #else
        #define Inline inline
    #endif
#else
    #define Inline
#endif

// ========================================================================
// Enable performance analysis code
// ========================================================================
#define PERF_ANALYZE 1

// ========================================================================
// Enable more debug info
// ========================================================================
#define DEBUG_INFO 1

// ========================================================================
// Max simple gate size for agdmap
// ========================================================================
#define MAX_GATE_SIZE 16

// ========================================================================
// Max LUT size for agdmap
// ========================================================================
#define MAX_LUT_SIZE 6

// ========================================================================
// Max node size in graph_t
// ========================================================================
#define MAX_NODE_SIZE 16

// ========================================================================
// Signature
// ========================================================================
// #define SIGNATURE(x, BITNUM) (1 << ((x) % ((BITNUM) - 1)))
template <std::size_t BITNUM>
Inline uint get_signature(uint x) { if constexpr (BITNUM & (BITNUM - 1)) return 1 << (x % (BITNUM - 1)); else return 1 << (x & (BITNUM - 1)); }

#define SIGNATURE(x) get_signature<Cut::BIT_NUM_SIGN>(x)

// ========================================================================
// Signature
// ========================================================================
#define Assert assert

// ========================================================================
// Shorthand format string
// ========================================================================
#define INFO1 "P{} LUT {:>6}   Edge {:>6}   Time {}   Cut {:>6} {:>6} {:>6}"
#define INFO2 "P{} LUT {:>6}   Edge {:>6}   Time {}"
#define INFO3  "Ex LUT {:>6}   Edge {:>6}   Time {}"

// ========================================================================
// Shorthands
// ========================================================================
#define LOG    std::println
#define CREF   const auto &
#define REF    auto &

// ========================================================================
// Shorthands for time measurement
// ========================================================================
#define TIME_START(name)                                        \
    std::clock_t cpu_time_begin_##name = std::clock();          \
    auto wall_time_begin_##name = std::chrono::high_resolution_clock::now();

#define TIME_STOP(name)                                                                       \
    std::clock_t cpu_time_end_##name = std::clock();                                          \
    auto wall_time_end_##name = std::chrono::high_resolution_clock::now();                    \
    double cpu_##name = double(cpu_time_end_##name - cpu_time_begin_##name) / CLOCKS_PER_SEC; \
    double wall_##name = std::chrono::duration<double>(wall_time_end_##name - wall_time_begin_##name).count();



