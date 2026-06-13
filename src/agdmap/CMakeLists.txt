add_library(agdmap agdmap.cpp AgdmapCommand.cpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(agdmap PRIVATE libabc)
target_include_directories(agdmap PUBLIC ${CMAKE_SOURCE_DIR}/abc/src ${CMAKE_SOURCE_DIR}/agdmap)
