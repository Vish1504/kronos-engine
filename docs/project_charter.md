# Kronos Engine Project Charter

## Objective

Build a production-quality, configurable LSM-tree storage engine from scratch in modern C++, expose it through a network service, and deploy it using modern infrastructure practices including Docker, Kubernetes, observability, and automated CI/CD.

## Version 1 Scope

Kronos V1 will support:

### Storage Engine

- Put operations
- Get operations
- Delete operations
- Write-Ahead Logging (WAL)
- Crash Recovery
- In-memory MemTables
- SSTable generation
- Multi-level SSTable storage
- Compaction
- Bloom Filters
- Configurable Storage Policies

### Interfaces

- Command Line Interface (CLI)
- Network API Service

### Infrastructure

- Docker containerization
- Docker Compose deployment
- Kubernetes deployment

### Observability

- Prometheus metrics
- Grafana dashboards

### Engineering

- Automated CI/CD pipeline
- Benchmark suite
- Single-node and multi-node performance benchmarks

## Non-Goals for Version 1

The following are intentionally out of scope:

- SQL support
- ACID transactions comparable to relational databases
- Distributed replication
- Consensus protocols (Raft/Paxos)
- Authentication and authorization
- Multi-region deployments
- A graphical user interface
- Distributed clustering
- Query language
- AI-memory functionality
- KronosDB

## Engineering Principles

- Understand every major component before implementing it.
- Prefer correctness before optimization.
- Keep the storage engine independent of networking and infrastructure.
- Design modular components with clear responsibilities.
- Test components independently before integration.
- Measure performance instead of guessing.
- Minimize external dependencies.
- Write clean, maintainable, and well-documented code.
- Keep commits small, focused, and meaningful.
