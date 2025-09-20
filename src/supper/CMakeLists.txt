add_library(supper
    foxmap.cpp
    cut.cpp
    node.cpp
)

add_compile_options(-fexceptions)

target_link_libraries(supper PRIVATE libabc)

target_include_directories(supper PUBLIC ${CMAKE_SOURCE_DIR}/abc/src)
