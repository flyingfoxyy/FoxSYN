add_library(fox foxmap.cpp)

target_link_libraries(fox PRIVATE libabc)

target_include_directories(fox PUBLIC ${CMAKE_SOURCE_DIR}/abc/src)
