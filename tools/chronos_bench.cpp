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

    kronos::Config config("config/kronos.config");

    // Uses defaults from BenchmarkConfig in bench.hpp.
    kronos::BenchmarkConfig benchmark_config;
    // benchmark_config.operation_count = 25000;
    // benchmark_config.value_size = 256;

    const auto to_us = [](std::chrono::nanoseconds duration) {
      return std::chrono::duration<double, std::micro>(duration).count();
    };

    // ============================================================
    // START FROM A FRESH DATABASE
    // ============================================================

    std::filesystem::remove_all(db_path);

    // ============================================================
    // WRITE BENCHMARK
    // ============================================================

    {
      std::cout << "Running WRITE benchmark..." << std::endl;

      kronos::KronosEngine write_engine(config, db_path);

      const auto write_result =
          kronos::runWriteBenchmark(write_engine, benchmark_config);

      // Timing has already stopped.
      // Drain outstanding maintenance and persist remaining data.
      write_engine.shutdown();

      const auto write_metrics = write_engine.getMetrics();

      const double write_elapsed_seconds =
          std::chrono::duration<double>(write_result.elapsed_time).count();

      std::cout << "\n=== Kronos WRITE Benchmark ===\n\n";

      std::cout << "Operations:      " << benchmark_config.operation_count
                << '\n';

      std::cout << "Value size:      " << benchmark_config.value_size
                << " bytes\n";

      std::cout << std::fixed << std::setprecision(2);

      std::cout << "Elapsed:         " << write_elapsed_seconds << " s\n";

      std::cout << "Throughput:      " << write_result.throughput_ops_per_sec
                << " ops/sec\n\n";

      std::cout << "Mean latency:    " << to_us(write_result.mean_latency)
                << " us\n";

      std::cout << "P50 latency:     " << to_us(write_result.p50_latency)
                << " us\n";

      std::cout << "P95 latency:     " << to_us(write_result.p95_latency)
                << " us\n";

      std::cout << "P99 latency:     " << to_us(write_result.p99_latency)
                << " us\n";

      std::cout << "Max latency:     " << to_us(write_result.max_latency)
                << " us\n\n";

      std::cout << "Flushes:         " << write_metrics.flush_count << '\n';

      std::cout << "Compactions:     " << write_metrics.compaction_count
                << '\n';

      std::cout << "SSTables:        " << write_metrics.sstable_count << '\n';

      std::cout << "Compaction time: "
                << to_us(write_metrics.total_compaction_time) << " us\n";

    } // write_engine destroyed here

    // ============================================================
    // READ BENCHMARK
    // ============================================================

    // IMPORTANT:
    // Do NOT delete the database here.
    //
    // The WRITE benchmark has already populated and persisted
    // exactly the dataset that the READ benchmark expects.

    std::cout << "\nReopening database for READ benchmark..." << std::endl;

    {
      kronos::KronosEngine read_engine(config, db_path);

      std::cout << "Running READ benchmark..." << std::endl;

      const auto read_result =
          kronos::runReadBenchmark(read_engine, benchmark_config);

      read_engine.shutdown();

      const auto read_metrics = read_engine.getMetrics();

      const double read_elapsed_seconds =
          std::chrono::duration<double>(read_result.elapsed_time).count();

      std::cout << "\n=== Kronos READ Benchmark ===\n\n";

      std::cout << "Operations:      " << benchmark_config.operation_count
                << '\n';

      std::cout << "Value size:      " << benchmark_config.value_size
                << " bytes\n";

      std::cout << std::fixed << std::setprecision(2);

      std::cout << "Elapsed:         " << read_elapsed_seconds << " s\n";

      std::cout << "Throughput:      " << read_result.throughput_ops_per_sec
                << " ops/sec\n\n";

      std::cout << "Mean latency:    " << to_us(read_result.mean_latency)
                << " us\n";

      std::cout << "P50 latency:     " << to_us(read_result.p50_latency)
                << " us\n";

      std::cout << "P95 latency:     " << to_us(read_result.p95_latency)
                << " us\n";

      std::cout << "P99 latency:     " << to_us(read_result.p99_latency)
                << " us\n";

      std::cout << "Max latency:     " << to_us(read_result.max_latency)
                << " us\n\n";

      std::cout << "Flushes:         " << read_metrics.flush_count << '\n';

      std::cout << "Compactions:     " << read_metrics.compaction_count << '\n';

      std::cout << "SSTables:        " << read_metrics.sstable_count << '\n';

      std::cout << "Compaction time: "
                << to_us(read_metrics.total_compaction_time) << " us\n";

    } // read_engine destroyed here

  } catch (const std::exception &error) {

    std::cerr << "Benchmark failed: " << error.what() << '\n';

    return 1;
  }

  return 0;
}