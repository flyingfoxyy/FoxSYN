
// --> Toggle performance analyze
#define PERF_ANALYZE 1

// --> Toggle debug info
#define DEBUG_INFO 1

// --> Time measurement macros
#define TIME_START(name)                                        \
    std::clock_t cpu_time_begin_##name = std::clock();          \
    auto wall_time_begin_##name = std::chrono::high_resolution_clock::now();

#define TIME_STOP(name)                                                                       \
    std::clock_t cpu_time_end_##name = std::clock();                                          \
    auto wall_time_end_##name = std::chrono::high_resolution_clock::now();                    \
    double cpu_##name = double(cpu_time_end_##name - cpu_time_begin_##name) / CLOCKS_PER_SEC; \
    double wall_##name = std::chrono::duration<double>(wall_time_end_##name - wall_time_begin_##name).count();

// --> Signature macro
#define SIGNATURE(x, BITNUM) (1 << ((x) % ((BITNUM) - 1)))

#define MAX_NODE_SIZE 16

// --> Compiler related
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

#define INFO1 "P{} LUT {:>6}   Edge {:>6}   Time {}   Cut {:>6} {:>6} {:>6}"
#define INFO2 "P{} LUT {:>6}   Edge {:>6}   Time {}"
#define INFO3 "Ex LUT {:>6}   Edge {:>6}   Time {}"

