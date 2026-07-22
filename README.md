# Kronos Engine

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
9. Multi-node Benchmarking
