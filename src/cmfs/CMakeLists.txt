add_library(cmfs cmfs.cpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(cmfs PRIVATE libabc timer)
target_include_directories(cmfs PUBLIC ${CMAKE_SOURCE_DIR}/abc/src ${CMAKE_SOURCE_DIR})
