add_library(supper
    map.cpp
    cut.cpp
    agdmap.cpp
)

add_compile_options(-fexceptions)

target_link_libraries(supper PRIVATE libabc)

target_include_directories(supper PUBLIC ${CMAKE_SOURCE_DIR}/abc/src)
