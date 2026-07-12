add_library(csr2
    csr2.cpp
    csr2_hop.cpp
    csr2_state.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(csr2 PRIVATE libabc timer cpr)
target_include_directories(csr2 PUBLIC ${CMAKE_SOURCE_DIR}/abc/src ${CMAKE_SOURCE_DIR})
