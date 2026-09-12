# ---------------------------------------------------------
# Minimum CMake version required to build Kronos
# ---------------------------------------------------------
cmake_minimum_required(VERSION 3.20)


# ---------------------------------------------------------
# Project name
# ---------------------------------------------------------
project(KronosEngine)


# ---------------------------------------------------------
# Compile everything using C++20
# ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# ---------------------------------------------------------
# Find zlib installed on the system.
#
# We use zlib's crc32() function in:
#   - WAL
#   - SSTable
#
# ZLIB::ZLIB is the CMake target representing that library.
# ---------------------------------------------------------
find_package(ZLIB REQUIRED)


# =========================================================
# MAIN KRONOS EXECUTABLE
# =========================================================

# Build the actual Kronos executable.
#
# These are the source files that currently make up
# the storage engine.
add_executable(
    kronos

    src/main.cpp
    src/logger.cpp
    src/config.cpp
    src/wal.cpp
    src/memtable.cpp
    src/sstable.cpp
)


# Tell the compiler where our own header files live.
#
# This allows things like:
#
#   #include "kronos/memtable.hpp"
#   #include "kronos/sstable.hpp"
#
# to work.
target_include_directories(
    kronos

    PRIVATE
    include
)


# WAL and SSTable use crc32(), whose implementation
# lives inside zlib.
#
# Including <zlib.h> only gives the compiler the
# declaration of crc32().
#
# This line gives the linker the actual implementation.
target_link_libraries(
    kronos

    PRIVATE
    ZLIB::ZLIB
)


# =========================================================
# MEMTABLE UNIT TEST
# =========================================================

# This test is only testing the Memtable.
#
# Therefore we compile:
#   - the test itself
#   - memtable.cpp
#
# We DO NOT include sstable.cpp here because this test
# has nothing to do with SSTables.
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


# No ZLIB linkage is required here because Memtable
# does not use crc32().


# =========================================================
# WAL -> MEMTABLE INTEGRATION TEST
# =========================================================

# This test verifies that WAL records can be recovered
# and replayed into a fresh Memtable.
#
# Therefore it needs:
#   - wal.cpp
#   - memtable.cpp
#
# It does NOT need sstable.cpp.
add_executable(
    wal_memtable_test

    tests/wal_memtable_test.cpp
    src/wal.cpp
    src/memtable.cpp
)


target_include_directories(
    wal_memtable_test

    PRIVATE
    include
)


# WAL uses CRC32, so this executable must also
# link against zlib.
target_link_libraries(
    wal_memtable_test

    PRIVATE
    ZLIB::ZLIB
)

# =========================================================
# SSTABLE WRITER TEST
# =========================================================

# Tests creation/finalization of an SSTable.
#
# SSTable uses CRC32, so this target must link zlib.
add_executable(
    sstable_test

    tests/sstable_test.cpp
    src/sstable.cpp
)

target_include_directories(
    sstable_test

    PRIVATE
    include
)

target_link_libraries(
    sstable_test

    PRIVATE
    ZLIB::ZLIB
)