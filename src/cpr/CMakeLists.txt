add_library(cpr
    cpr.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(cpr PRIVATE libabc timer)

target_include_directories(cpr PUBLIC ${CMAKE_SOURCE_DIR}/abc/src)
