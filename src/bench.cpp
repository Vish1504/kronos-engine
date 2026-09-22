#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <kronos/bench.hpp>
#include <kronos/engine.hpp>

namespace kronos {
std::vector<std::pair<std::string, std::string>>
generateWorkload(size_t operation_count, size_t value_size) {
  std::vector<std::pair<std::string, std::string>> dummy_records;
  dummy_records.reserve(operation_count);
  const std::string value(value_size, 'x');
  // Generate N deterministic key/value pairs sequentially
  for (size_t i = 0; i < operation_count; ++i) {
    std::string key = "key_" + std::to_string(i);
    dummy_records.emplace_back(std::move(key), value);
  }

  // Shuffle them with a fixed seed for reproducible shuffled workloads
  constexpr uint32_t fixed_seed = 12345;
  std::mt19937 prng(fixed_seed);
  std::shuffle(dummy_records.begin(), dummy_records.end(), prng);
  return dummy_records;
}

std::chrono::nanoseconds
percentile(const std::vector<std::chrono::nanoseconds> &sorted_latencies,
           double p) {

  const size_t index =
      static_cast<size_t>(std::ceil(p * sorted_latencies.size())) - 1;

  return sorted_latencies[index];
}

BenchmarkResult runWriteBenchmark(KronosEngine &engine,
                                  const BenchmarkConfig &config) {

  if (config.operation_count == 0) {
    throw std::invalid_argument(
        "Benchmark operation_count must be greater than zero");
  }

  // Generate the workload before starting the benchmark timer.
  const auto workload =
      generateWorkload(config.operation_count, config.value_size);

  std::vector<std::chrono::nanoseconds> latencies;
  latencies.reserve(config.operation_count);

  // Measure the complete WRITE workload.
  const auto workload_start = std::chrono::steady_clock::now();

  for (const auto &[key, value] : workload) {
    const auto operation_start = std::chrono::steady_clock::now();

    engine.put(key, value);

    const auto operation_end = std::chrono::steady_clock::now();

    latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
        operation_end - operation_start));
  }

  const auto workload_end = std::chrono::steady_clock::now();

  BenchmarkResult result;

  result.elapsed_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
      workload_end - workload_start);

  const double elapsed_seconds =
      std::chrono::duration<double>(result.elapsed_time).count();

  result.throughput_ops_per_sec =
      static_cast<double>(config.operation_count) / elapsed_seconds;

  // Everything below this point happens outside the measured workload.
  std::sort(latencies.begin(), latencies.end());

  int64_t total_latency_ns = 0;

  for (const auto latency : latencies) {
    total_latency_ns += latency.count();
  }

  result.mean_latency = std::chrono::nanoseconds(
      total_latency_ns / static_cast<int64_t>(latencies.size()));

  result.p50_latency = percentile(latencies, 0.50);
  result.p95_latency = percentile(latencies, 0.95);
  result.p99_latency = percentile(latencies, 0.99);
  result.max_latency = latencies.back();

  return result;
}

} // namespace kronos
