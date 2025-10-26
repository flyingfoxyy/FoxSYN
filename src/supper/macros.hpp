
// --> Toggle performance analyze
#define PERF_ANALYZE 1

// --> Toggle debug info
#define DEBUG_INFO 1

// --> Time measurement macros
#define TIME_POINT(name)                                        \
    std::clock_t cpu_time_point_##name = std::clock();          \
    auto wall_time_point_##name = std::chrono::high_resolution_clock::now();

#define TIME_DURATION(name, start, end)                                                                  \
    double cpu_##name = double(cpu_time_point_##end - cpu_time_point_##start) / CLOCKS_PER_SEC;          \
    double wall_##name = std::chrono::duration<double>(wall_time_point_##end - wall_time_point_##start).count();

#define TIME_BEGIN(name)                                        \
    std::clock_t cpu_time_begin_##name = std::clock();          \
    auto wall_time_begin_##name = std::chrono::high_resolution_clock::now();

#define TIME_END(name)                                                                        \
    std::clock_t cpu_time_end_##name = std::clock();                                          \
    auto wall_time_end_##name = std::chrono::high_resolution_clock::now();                    \
    double cpu_##name = double(cpu_time_end_##name - cpu_time_begin_##name) / CLOCKS_PER_SEC; \
    double wall_##name = std::chrono::duration<double>(wall_time_end_##name - wall_time_begin_##name).count();

#define TIME_END_MS(name)                                                                            \
    std::clock_t cpu_time_end_##name = std::clock();                                                 \
    auto wall_time_end_##name = std::chrono::high_resolution_clock::now();                           \
    double cpu_##name = 1000 * double(cpu_time_end_##name - cpu_time_begin_##name) / CLOCKS_PER_SEC; \
    double wall_##name = 1000 * std::chrono::duration<double>(wall_time_end_##name - wall_time_begin_##name).count();

// --> Signature macro
#define SIGNATURE(x, BITNUM) (1 << ((x) % ((BITNUM) - 1)))

#define MAX_NODE_SIZE 16
