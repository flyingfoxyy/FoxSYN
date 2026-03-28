add_library(partsyn
    partsyn.cpp
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fexceptions)

target_link_libraries(partsyn PRIVATE libabc)

target_include_directories(partsyn PUBLIC ${CMAKE_SOURCE_DIR}/abc/src)

# Link pthread for std::thread support
find_package(Threads REQUIRED)
target_link_libraries(partsyn PRIVATE Threads::Threads)
