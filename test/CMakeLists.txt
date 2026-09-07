cmake_minimum_required(VERSION 3.10)

# Test executables
add_executable(test_dot "test_dot.cpp")
target_link_libraries(test_dot PRIVATE
    supper
    libabc
    timer
)

add_executable(test_verilog "test_verilog.cpp")
target_link_libraries(test_verilog PRIVATE
    supper
    libabc
    timer
)

add_executable(test_hop "test_hop.cpp")
target_link_libraries(test_hop PRIVATE
    libabc
    timer
)

add_executable(test_cpr "test_cpr.cpp")
target_link_libraries(test_cpr PRIVATE
    libabc
    timer
    cpr
)

add_executable(test_csr2 "test_csr2.cpp")
target_link_libraries(test_csr2 PRIVATE
    libabc
    timer
    cpr
    csr2
)

add_executable(test_csr3 "test_csr3.cpp")
target_link_libraries(test_csr3 PRIVATE
    libabc
    timer
    csr3
)

add_executable(test_csr4 "test_csr4.cpp")
target_link_libraries(test_csr4 PRIVATE
    libabc
    timer
    csr4
)

add_executable(test_fmpart "test_fmpart.cpp")
target_link_libraries(test_fmpart PRIVATE fmpart libabc timer)

add_executable(test_hive "test_hive.cpp")
target_link_libraries(test_hive PRIVATE hive libabc timer)
