cmake_minimum_required(VERSION 3.10)

project(FoxSYN)

add_compile_options(-Wno-unused-variable)

add_subdirectory(abc)
add_subdirectory(fox)

add_executable(FoxSYN "main.cpp")
target_link_libraries(FoxSYN PRIVATE
    libabc
    fox
)
