add_library(hpart
    hpart.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(hpart PRIVATE libabc)

target_include_directories(hpart PUBLIC ${CMAKE_SOURCE_DIR}/abc/src)
