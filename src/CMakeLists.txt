cmake_minimum_required(VERSION 3.10)

project(FoxSYN)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wno-unused-variable)

include_directories(/home/longfei/taskflow)

add_subdirectory(abc)
add_subdirectory(fox)
add_subdirectory(supper)

add_compile_options(-fexceptions)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")

add_executable(FoxSYN "main.cpp")
target_link_libraries(FoxSYN PRIVATE
    libabc
    fox
    supper
)

# Add test_dot executable to demonstrate the DOT file generation
add_executable(test_dot "test_dot.cpp")
target_link_libraries(test_dot PRIVATE
    supper
)

# Add test_verilog executable to demonstrate the Verilog netlist generation
add_executable(test_verilog "test_verilog.cpp")
target_link_libraries(test_verilog PRIVATE
    supper
)
