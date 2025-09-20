cmake_minimum_required(VERSION 3.10)

project(FoxSYN)

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
