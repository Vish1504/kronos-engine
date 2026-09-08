# Minimum CMake version needed
cmake_minimum_required(VERSION 3.20)

# Gives the project a name
project(KronosEngine)

# Use C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)

# Build an executable named kronos from these source files
add_executable(
    kronos
    src/main.cpp
    src/logger.cpp
    src/config.cpp
    src/wal.cpp
    src/memtable.cpp
)

target_include_directories(
    kronos
    PRIVATE
    include
    
)

target_link_libraries(
    kronos
    PRIVATE
    ZLIB::ZLIB
)

add_executable(
    memtable_test
    tests/memtable_test.cpp
    src/memtable.cpp
)

target_include_directories(
    memtable_test
    PRIVATE
    include
)