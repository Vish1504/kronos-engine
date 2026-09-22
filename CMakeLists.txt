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

find_package(Threads REQUIRED)


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
    src/manifest.cpp
    src/compaction_policy.cpp
    src/engine.cpp

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

# =========================================================
# MANIFEST TEST
# =========================================================

# Verifies authoritative SSTable state transitions.
#
# Important cases:
#   - state persists across reload
#   - merely-created SSTable is not automatically live
#   - compaction edit removes old files and adds output
#   - compaction may legitimately produce no output file

add_executable(
    manifest_test

    tests/manifest_test.cpp
    src/manifest.cpp
)

target_include_directories(
    manifest_test
    PRIVATE

    include
)


# =========================================================
# COMPACTION POLICY TEST
# =========================================================

# Verifies the v1 leveled-compaction policy:
#
#   - L0 file-count threshold
#   - all L0 files selected
#   - only overlapping L1 files selected
#   - tombstone GC allowed only when L1 is bottom level
add_executable(
    compaction_policy_test

    tests/compaction_policy_test.cpp
    src/compaction_policy.cpp
)

target_include_directories(
    compaction_policy_test
    PRIVATE

    include
)

# =========================================================
# THREAD-SAFE QUEUE TEST
# =========================================================
# Verifies the producer-consumer queue used by the
# background maintenance worker.
#
# Important cases:
#   - push/pop
#   - FIFO ordering
#   - blocked consumer wakes when work arrives
#   - blocked consumer wakes during shutdown
#   - accepted tasks drain before shutdown completes
#   - pushes are rejected after shutdown begins
#
# thread_safe_queue is a template and is implemented
# entirely in its header, so no .cpp source is required.
# =========================================================

add_executable(
    thread_safe_queue_test
    tests/thread_safe_queue_test.cpp
)

target_include_directories(
    thread_safe_queue_test
    PRIVATE
    include
)

target_link_libraries(
    thread_safe_queue_test
    PRIVATE
    Threads::Threads
)

# =========================================================
# KRONOS ENGINE / MODULE 8 CONCURRENCY TEST
# =========================================================
# Verifies the integrated foreground/background engine path:
#
#   - put/get/remove through KronosEngine
#   - active -> immutable Memtable rotation
#   - background L0 flush
#   - graceful shutdown queue draining
#   - L0 -> L1 background compaction
#   - concurrent foreground reads while maintenance runs
#
# This target intentionally compiles the required engine source
# files directly, matching the existing per-test CMake style.
# =========================================================

add_executable(
    engine_test

    tests/engine_test.cpp

    src/engine.cpp
    src/config.cpp
    src/wal.cpp
    src/memtable.cpp
    src/sstable.cpp
    src/bloom_filter.cpp
    src/manifest.cpp
    src/compactor.cpp
    src/compaction_policy.cpp

    src/third_party/murmurhash3/MurmurHash3.cpp
)

target_include_directories(
    engine_test
    PRIVATE

    include
    src
)

target_link_libraries(
    engine_test
    PRIVATE

    ZLIB::ZLIB
    Threads::Threads
)

# =========================================================
# THREADSANITIZER
# =========================================================
#
# Optional race-condition detection for the Module 8
# concurrency/integration tests.
#
# Enable with:
#
#   cmake -S . -B build-tsan -DKRONOS_ENABLE_TSAN=ON
#
# This is kept off for normal builds because sanitizer
# instrumentation adds substantial runtime overhead.
# =========================================================

option(
    KRONOS_ENABLE_TSAN
    "Enable ThreadSanitizer for Kronos concurrency tests"
    OFF
)

if(KRONOS_ENABLE_TSAN)

    message(STATUS "ThreadSanitizer enabled for engine_test")

    target_compile_options(
        engine_test
        PRIVATE

        -fsanitize=thread
        -fno-omit-frame-pointer
        -g
    )

    target_link_options(
        engine_test
        PRIVATE

        -fsanitize=thread
    )

endif()

# =========================================================
# MODULE 9 RECOVERY TEST
# =========================================================
#
# Simulates an ungraceful process exit after WAL-backed writes,
# then reopens the same database directory and verifies that
# Kronos reconstructs the lost active Memtable from the WAL.
#
# Like engine_test, this target compiles the required engine
# sources directly because "kronos" is an executable, not a
# reusable library target.
# =========================================================

add_executable(
    recovery_test

    tests/recovery_test.cpp

    src/engine.cpp
    src/config.cpp
    src/wal.cpp
    src/memtable.cpp
    src/sstable.cpp
    src/bloom_filter.cpp
    src/manifest.cpp
    src/compactor.cpp
    src/compaction_policy.cpp
    src/third_party/murmurhash3/MurmurHash3.cpp
)

target_include_directories(
    recovery_test

    PRIVATE

    include
    src
)

target_link_libraries(
    recovery_test

    PRIVATE

    ZLIB::ZLIB
    Threads::Threads
)