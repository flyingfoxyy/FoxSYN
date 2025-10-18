add_library(fox
    foxmap.cpp
    cut.cpp
    node.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(fox PRIVATE libabc)

target_include_directories(fox PUBLIC ${CMAKE_SOURCE_DIR}/abc/src)
