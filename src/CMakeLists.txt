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
add_subdirectory(cmfs)
add_subdirectory(csr)
add_subdirectory(csr2)
add_subdirectory(csr3)
add_subdirectory(csr4)
add_subdirectory(pdecomp)
add_subdirectory(pst)
add_subdirectory(agdmap)
add_subdirectory(curvemap)
add_subdirectory(fmpart)
add_subdirectory(hive)
add_subdirectory(../test test_build)

add_compile_options(-fexceptions)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")

add_executable(FoxSYN "main.cpp")
target_link_libraries(FoxSYN PRIVATE
    libabc
    fox
    supper
    partsyn
    hpart
    fmpart
    hive
    timer
    cpr
    cmfs
    csr
    csr2
    csr3
    csr4
    pdecomp
    pst
    agdmap
    curvemap
)
