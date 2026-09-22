#include <kronos/bench.hpp>
#include <kronos/config.hpp>
#include <kronos/engine.hpp>

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>

int main() {
  try {
    const std::filesystem::path db_path = "benchmark_db";

    // Every benchmark run starts from an identical empty database.
    std::filesystem::remove_all(db_path);

    kronos::Config config("config/kronos.config");

    kronos::BenchmarkConfig benchmark_config;
    benchmark_config.operation_count = 100'000;
    benchmark_config.value_size = 256;

    // Construct this using the same constructor signature your
    // KronosEngine currently exposes.
    kronos::KronosEngine engine(config, db_path);

    const auto result = kronos::runWriteBenchmark(engine, benchmark_config);

    // Timing has already stopped. Now drain all outstanding
    // background maintenance so final engine metrics are stable.
    engine.shutdown();

    const auto metrics = engine.getMetrics();

    const auto to_us = [](std::chrono::nanoseconds duration) {
      return std::chrono::duration<double, std::micro>(duration).count();
    };

    const double elapsed_seconds =
        std::chrono::duration<double>(result.elapsed_time).count();

    std::cout << "\n=== Kronos WRITE Benchmark ===\n\n";

    std::cout << "Operations:      " << benchmark_config.operation_count
              << '\n';

    std::cout << "Value size:      " << benchmark_config.value_size
              << " bytes\n";

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "Elapsed:         " << elapsed_seconds << " s\n";

    std::cout << "Throughput:      " << result.throughput_ops_per_sec
              << " ops/sec\n\n";

    std::cout << "Mean latency:    " << to_us(result.mean_latency) << " us\n";

    std::cout << "P50 latency:     " << to_us(result.p50_latency) << " us\n";

    std::cout << "P95 latency:     " << to_us(result.p95_latency) << " us\n";

    std::cout << "P99 latency:     " << to_us(result.p99_latency) << " us\n";

    std::cout << "Max latency:     " << to_us(result.max_latency) << " us\n\n";

    std::cout << "Flushes:         " << metrics.flush_count << '\n';

    std::cout << "Compactions:     " << metrics.compaction_count << '\n';

    std::cout << "SSTables:        " << metrics.sstable_count << '\n';

    std::cout << "Compaction time: " << to_us(metrics.total_compaction_time)
              << " us\n";
  } catch (const std::exception &error) {
    std::cerr << "Benchmark failed: " << error.what() << '\n';

    return 1;
  }

  return 0;
}