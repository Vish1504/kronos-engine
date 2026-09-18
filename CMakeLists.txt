# ---------------------------------------------------------
# Minimum CMake version required to build Kronos
# ---------------------------------------------------------
cmake_minimum_required(VERSION 3.20)


# ---------------------------------------------------------
# Project name
# ---------------------------------------------------------
project(KronosEngine)


# ---------------------------------------------------------
# C++ Standard
#
# Compile the entire project using C++20.
# ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# =========================================================
# EXTERNAL DEPENDENCIES
# =========================================================

# ---------------------------------------------------------
# ZLIB
#
# We use zlib's crc32() implementation in:
#   - WAL
#   - SSTable
#
# ZLIB::ZLIB is the CMake target representing the
# installed zlib library.
# ---------------------------------------------------------
find_package(ZLIB REQUIRED)


# =========================================================
# MAIN KRONOS EXECUTABLE
# =========================================================

# ---------------------------------------------------------
# Main Storage Engine
#
# This builds the actual "kronos" executable.
#
# Every production source file that currently belongs to
# the storage engine is listed here.
# ---------------------------------------------------------
add_executable(
    kronos

    src/main.cpp

    # Core utilities
    src/logger.cpp
    src/config.cpp

    # Storage engine components
    src/wal.cpp
    src/memtable.cpp
    src/sstable.cpp
    src/bloom_filter.cpp
    src/compactor.cpp

    # Third-party hashing implementation used by BloomFilter
    src/third_party/murmurhash3/MurmurHash3.cpp
)


# ---------------------------------------------------------
# Header search paths for the main executable
#
# "include" contains our public/project headers.
# "src" is also included because some implementation-level
# headers / third-party headers currently live there.
# ---------------------------------------------------------
target_include_directories(
    kronos
    PRIVATE

    include
    src
)


# ---------------------------------------------------------
# External libraries used by the main executable
#
# WAL and SSTable both use crc32(), so Kronos must link
# against zlib.
# ---------------------------------------------------------
target_link_libraries(
    kronos
    PRIVATE

    ZLIB::ZLIB
)


# =========================================================
# TESTS
# =========================================================
#
# IMPORTANT:
#
# Each test executable compiles only the source files it
# actually needs.
#
# We currently DO NOT link tests against the "kronos"
# executable because CMake does not allow one normal
# executable to be linked into another executable.
#
# A future cleanup could extract engine code into a
# "kronos_core" library and let both the main executable
# and tests link against it.
#
# We are intentionally NOT doing that refactor now.
# =========================================================


# =========================================================
# MEMTABLE UNIT TEST
# =========================================================

# ---------------------------------------------------------
# Tests only the Memtable.
#
# Required:
#   - memtable_test.cpp
#   - memtable.cpp
#
# No SSTable/WAL/zlib dependency is required.
# ---------------------------------------------------------
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


# =========================================================
# WAL -> MEMTABLE INTEGRATION TEST
# =========================================================

# ---------------------------------------------------------
# Tests WAL recovery into a fresh Memtable.
#
# Required:
#   - wal.cpp
#   - memtable.cpp
#
# WAL uses CRC32, so this target must link zlib.
# ---------------------------------------------------------
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


target_link_libraries(
    wal_memtable_test
    PRIVATE

    ZLIB::ZLIB
)


# =========================================================
# SSTABLE TEST
# =========================================================

# ---------------------------------------------------------
# Tests SSTable writing, reading, iteration, tombstones,
# corruption detection, BloomFilter integration, etc.
#
# Required:
#   - sstable.cpp
#   - memtable.cpp
#   - bloom_filter.cpp
#   - MurmurHash3.cpp
#
# SSTable uses CRC32, so this target links zlib.
# ---------------------------------------------------------
add_executable(
    sstable_test

    tests/sstable_test.cpp

    src/sstable.cpp
    src/memtable.cpp
    src/bloom_filter.cpp

    src/third_party/murmurhash3/MurmurHash3.cpp
)


target_include_directories(
    sstable_test
    PRIVATE

    include
    src
)


target_link_libraries(
    sstable_test
    PRIVATE

    ZLIB::ZLIB
)


# =========================================================
# BLOOM FILTER UNIT TEST
# =========================================================

# ---------------------------------------------------------
# Tests the BloomFilter independently.
#
# Required:
#   - bloom_filter.cpp
#   - MurmurHash3.cpp
#
# No zlib dependency here.
# ---------------------------------------------------------
add_executable(
    bloom_filter_test

    tests/bloom_filter_test.cpp

    src/bloom_filter.cpp
    src/third_party/murmurhash3/MurmurHash3.cpp
)


target_include_directories(
    bloom_filter_test
    PRIVATE

    include
    src
)


# =========================================================
# COMPACTOR TEST
# =========================================================

# ---------------------------------------------------------
# Tests K-way SSTable compaction.
#
# The Compactor:
#   1. Opens multiple SSTables
#   2. Creates one iterator per SSTable
#   3. Uses a min-heap to merge sorted records
#   4. Resolves duplicate keys using sequence numbers
#   5. Preserves winning tombstones
#   6. Writes the merged result into a new SSTable
#
# Required:
#   - compactor.cpp
#   - sstable.cpp
#   - bloom_filter.cpp
#   - MurmurHash3.cpp
#
# We DO NOT link against the "kronos" executable.
#
# Instead, this test builds the required engine source files
# directly into its own executable.
#
# SSTable uses CRC32, therefore this target also needs zlib.
# ---------------------------------------------------------
add_executable(
    compactor_test

    tests/compactor_test.cpp

    src/compactor.cpp
    src/sstable.cpp
    src/bloom_filter.cpp

    src/third_party/murmurhash3/MurmurHash3.cpp
)


target_include_directories(
    compactor_test
    PRIVATE

    include
    src
)


target_link_libraries(
    compactor_test
    PRIVATE

    ZLIB::ZLIB
)