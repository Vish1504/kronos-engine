#pragma once

#include <kronos/engine.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace kronos {

enum class WorkloadType { WRITE, READ };

struct BenchmarkConfig {
  size_t operation_count = 100'000;
  size_t value_size = 256;
};

struct BenchmarkResult {
  std::chrono::nanoseconds elapsed_time{0};
  double throughput_ops_per_sec = 0.0;

  std::chrono::nanoseconds mean_latency{0};
  std::chrono::nanoseconds p50_latency{0};
  std::chrono::nanoseconds p95_latency{0};
  std::chrono::nanoseconds p99_latency{0};
  std::chrono::nanoseconds max_latency{0};
};

std::vector<std::pair<std::string, std::string>>
generateWorkload(size_t operation_count, size_t value_size);

BenchmarkResult runWriteBenchmark(KronosEngine &engine,
                                  const BenchmarkConfig &config);

} // namespace kronos