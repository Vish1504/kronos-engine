# Kronos Engine

Kronos is a durable LSM-tree key-value storage engine written from scratch in modern C++.

It began as a systems-engineering project to understand what actually happens beneath a database API: how writes become durable, how immutable files are organized, how deleted data is represented, how background compaction preserves correctness, how a storage engine recovers after failure, and how architectural decisions affect real performance.

The public interface is intentionally small:

```cpp
KronosEngine db(config, "./data");

db.put("user:42", "Vishnu");

auto result = db.get("user:42");

db.remove("user:42");
```

The interesting work happens underneath.

## Architecture

Kronos follows a log-structured merge-tree design.

### Write path

```text
PUT / DELETE
      │
      ▼
     WAL
      │
      ▼
Active Memtable
      │
      │ size threshold
      ▼
Immutable Memtable
      │
      │ background flush
      ▼
   L0 SSTable
      │
      │ compaction policy
      ▼
   L1 SSTable
```

Writes are recorded in the Write-Ahead Log before becoming visible in memory. When the active Memtable reaches its configured size threshold, it becomes immutable and is flushed to an SSTable by a background maintenance worker.

### Read path

```text
GET
 │
 ├── Active Memtable
 │
 ├── Immutable Memtable
 │
 └── MANIFEST live SSTables
          │
          ├── key-range check
          ├── Bloom filter
          ├── sparse index
          └── block read via pread()
```

Kronos resolves competing versions using monotonically increasing sequence numbers. Tombstones represent deletes until compaction can safely discard them.

## Core Components

### Write-Ahead Log

The WAL provides durable write history and supports recovery after an ungraceful process exit.

Recovery validates record structure, CRC32 checksums and sequence ordering. An incomplete final WAL record can be safely discarded and the WAL repaired to its last valid boundary.

### Memtables

Writes first enter an in-memory ordered Memtable.

Kronos uses active and immutable Memtables so foreground operations can continue while previously accumulated data is flushed in the background.

### SSTables

Persistent data is stored in immutable sorted-string tables.

Kronos SSTables include:

- deterministic binary serialization
- data blocks
- sparse indexing
- per-SSTable Bloom filters
- CRC32 integrity checking
- explicit metadata
- ordered iteration for compaction

### Bloom Filters

Each SSTable contains a Bloom filter used to reject many absent-key lookups before performing unnecessary data-block reads.

The implementation uses configurable bits-per-key and MurmurHash3-based probe generation.

### Versions and Tombstones

Every write receives a monotonically increasing sequence number.

When multiple representations of a key exist, the newest sequence wins. Deletes are represented by tombstones rather than immediately removing immutable historical data.

### Compaction

Kronos performs leveled L0 → L1 compaction.

The compactor uses a K-way merge over sorted SSTable iterators, resolves duplicate keys by sequence number, preserves required tombstones and can discard tombstones when they are provably safe to remove.

### MANIFEST

The MANIFEST is the authoritative description of the logical SSTable state.

Compaction commits logical state through the MANIFEST before obsolete physical files are considered removable. This separates logical correctness from filesystem cleanup and provides a clear recovery boundary.

### Concurrency

Kronos uses:

- a serialized foreground write path
- concurrent foreground reads
- active and immutable Memtables
- a thread-safe maintenance queue
- one background flush/compaction worker
- backpressure when the immutable Memtable slot is occupied

Immutable SSTable readers can be shared across reads.

### Crash Recovery

On startup Kronos reconstructs persistent state from the MANIFEST and WAL.

Recovery is designed around the principle that the MANIFEST defines logical truth. Files not referenced by it do not automatically become part of the database.

## Performance Engineering

Kronos includes a reproducible benchmark harness that records:

- throughput
- mean latency
- P50 latency
- P95 latency
- P99 latency
- maximum latency
- flush count
- compaction count
- SSTable count
- cumulative compaction time

Benchmark workloads are generated before timing and use deterministic shuffled keys so workload generation itself is not included in operation latency.

### Persistent-read investigation

An early persistent-read benchmark exposed unexpectedly poor GET performance.

For every candidate SSTable, the read path constructed a new `SstableReader`. Reader construction repeatedly reopened the SSTable and reloaded immutable metadata including the Bloom filter and sparse index.

Kronos was changed to cache immutable SSTable readers.

That optimization changed reader lifetime and introduced a new ownership constraint: multiple reads could now share the same file descriptor. Block access therefore moved from shared-offset `lseek()` + `read()` operations to offset-explicit `pread()`.

In a controlled 25,000-operation persistent-read benchmark against a single SSTable:

| Metric | Before | Post-change median |
|---|---:|---:|
| Throughput | ~316 GET/s | ~4,837 GET/s |
| Mean latency | ~3.16 ms | ~0.206 ms |
| P50 latency | ~2.95 ms | ~0.200 ms |
| P95 latency | ~4.04 ms | ~0.300 ms |
| P99 latency | ~6.42 ms | ~0.360 ms |

This is a result from one controlled Kronos configuration and workload, not a universal performance claim.

The more important architectural consequence was:

```text
Reader caching
      │
      ▼
Reader lifetime changes
      │
      ▼
Shared file descriptors
      │
      ├── Concurrency correctness ──► pread()
      │
      └── Resource retention ──────► future cache eviction/pruning
```

The investigation reinforced a central project rule:

> Measure first. Optimize the demonstrated bottleneck. Re-measure afterward.

## Testing

Kronos contains tests for:

- Memtable semantics
- WAL recovery
- truncated WAL repair
- WAL corruption detection
- sequence validation
- SSTable serialization and lookup
- SSTable corruption detection
- Bloom filters
- MANIFEST persistence
- compaction policy
- K-way compaction
- tombstone behavior
- thread-safe queue semantics
- integrated engine behavior
- foreground/background concurrency
- shutdown behavior
- crash recovery

The normal test suite is registered with CTest.

```bash
ctest --test-dir build --output-on-failure
```

A separate recovery harness exercises crash/restart scenarios.

## Building

### Requirements

- C++20-compatible compiler
- CMake 3.20+
- zlib
- POSIX-compatible environment

Kronos currently uses POSIX filesystem APIs such as `pread()` and is primarily developed and tested on macOS/Linux-style environments.

### Configure

```bash
cmake -S . -B build
```

### Build

```bash
cmake --build build
```

### Run tests

```bash
ctest --test-dir build --output-on-failure
```

### Run benchmarks

```bash
./build/kronos_bench
```

The default benchmark configuration can be adjusted through the benchmark/configuration code.

## Project Structure

```text
kronos-engine/
├── include/kronos/       Public engine/component headers
├── src/                  Storage-engine implementation
├── tests/                Unit and integration tests
├── tools/                Benchmark tooling
├── config/               Runtime configuration
├── docs/                 Architecture/design documentation
└── CMakeLists.txt
```

## Design Principles

Kronos deliberately favors systems understanding and correctness over feature count.

The project follows a few rules:

- durability before visibility
- immutable persistent files
- MANIFEST-defined logical truth
- sequence numbers define version authority
- deletion is represented explicitly through tombstones
- expensive maintenance belongs off the foreground path
- performance claims require measurements
- optimizations must account for ownership and concurrency changes
- correctness comes before benchmark numbers

## Scope and Limitations

Kronos v1 is a single-node educational storage engine, not a replacement for mature production databases such as RocksDB.

It intentionally does not implement:

- SQL
- transactions
- replication
- distributed consensus
- sharding
- authentication
- networking
- distributed deployment
- a graphical interface

Some lifecycle optimizations are also intentionally deferred. For example, cached readers for obsolete SSTables are not yet aggressively evicted.

The purpose of v1 is narrower: implement and reason about the core mechanics of a durable concurrent LSM storage engine from first principles.

## Status

Kronos v1 implements the core storage-engine architecture and is in final hardening/release preparation.


<!-- # Kronos Engine

Kronos v1 is a configurable LSM-tree storage engine written in modern C++, exposed through a network service, containerized with Docker, orchestrated using Kubernetes, instrumented with Prometheus metrics, and validated through automated CI/CD and benchmarking.

## Features (Planned)

### Storage Engine

- Write-Ahead Logging (WAL)
- In-memory MemTables
- Immutable SSTables
- Bloom Filters
- Compaction
- Crash Recovery
- Configurable Storage Policies

### Interfaces

- Command Line Interface (CLI)
- Network API Service

### Infrastructure

- Docker
- Docker Compose
- Kubernetes

### Observability

- Prometheus Metrics
- Grafana Dashboards

### Engineering

- Automated CI/CD Pipeline
- Performance Benchmark Suite

## Current Status

Project setup and development environment completed.

## Build

### Configure the project

cmake -S . -B build

### Build the executable

cmake --build build

### Run Kronos

./build/kronos

## Requirements

- C++20-compatible compiler
- CMake 3.20 or newer

## Project Structure

kronos-engine/

├── src/  
├── include/  
├── tests/  
├── docs/  
├── CMakeLists.txt  
├── README.md  
└── .gitignore

## Roadmap

The project will be developed incrementally in the following order:

1. C++ Storage Engine
2. Command Line Interface (CLI)
3. Network API Service
4. Docker
5. Prometheus Metrics
6. Docker Compose
7. CI/CD Pipeline
8. Kubernetes Deployment
9. Multi-node Benchmarking -->
