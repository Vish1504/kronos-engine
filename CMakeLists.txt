# Minimum CMake version needed
cmake_minimum_required(VERSION 3.20)

# Gives the project a name
project(KronosEngine)

# Use C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build an executable named kronos from these source files
add_executable(
    kronos
    src/main.cpp
)