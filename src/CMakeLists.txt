cmake_minimum_required(VERSION 3.10)

# Add option to enable clang-20 compiler
option(USE_CLANG20 "Use clang-20 compiler" OFF)

# Set up compiler based on option
if(USE_CLANG20)
    # Check if clang-20 is available
    find_program(CLANG20_COMPILER "clang-20")
    find_program(CLANGXX20_COMPILER "clang++-20")
    
    if(CLANG20_COMPILER AND CLANGXX20_COMPILER)
        message(STATUS "Using clang-20 compiler: ${CLANG20_COMPILER}")
        message(STATUS "Using clang++-20 compiler: ${CLANGXX20_COMPILER}")
        
        set(CMAKE_C_COMPILER "${CLANG20_COMPILER}")
        set(CMAKE_CXX_COMPILER "${CLANGXX20_COMPILER}")
    else()
        message(FATAL_ERROR "clang-20 and/or clang++-20 not found, but USE_CLANG20 is ON")
    endif()
endif()

project(FoxSYN)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add clang-specific options when using clang-20
if(USE_CLANG20)
    add_compile_options(-fexperimental-library)
    message(STATUS "Adding clang-20 specific compile options")
endif()

add_compile_options(-Wno-unused-variable)

include_directories(/home/longfei/taskflow)

add_subdirectory(abc)
add_subdirectory(fox)
add_subdirectory(supper)
add_subdirectory(partsyn)
add_subdirectory(hpart)
add_subdirectory(timer)
add_subdirectory(cpr)

add_compile_options(-fexceptions)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")

add_executable(FoxSYN "main.cpp")
target_link_libraries(FoxSYN PRIVATE
    libabc
    fox
    supper
    partsyn
    hpart
    timer
    cpr
)

# Add test_dot executable to demonstrate the DOT file generation
add_executable(test_dot "test_dot.cpp")
target_link_libraries(test_dot PRIVATE
    supper
    libabc
    timer
)

# Add test_verilog executable to demonstrate the Verilog netlist generation
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
