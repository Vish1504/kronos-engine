#include <kronos/config.hpp>
#include <kronos/engine.hpp>
#include <kronos/manifest.hpp>
#include <kronos/types.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

void expect(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct TempDb {
  fs::path root;
  fs::path config;

  explicit TempDb(const std::string &name, size_t compaction_trigger = 100) {
    root = fs::temp_directory_path() /
           ("kronos_module8_" + name + "_" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()));
    fs::create_directories(root);
    config = root / "kronos.config";

    std::ofstream out(config);
    out << "memtable_size_mb=1\n";
    out << "bloom_filter=true\n";
    out << "log_path=" << (root / "kronos.log").string() << "\n";
    out << "l0_compaction_trigger=" << compaction_trigger << "\n";
  }

  ~TempDb() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

void test_basic_api() {
  TempDb temp("basic");
  kronos::Config config(temp.config);
  kronos::KronosEngine engine(config, temp.root / "db");

  engine.put("name", "Vishnu");
  auto found = engine.get("name");
  expect(found.status == kronos::GetStatus::FOUND, "PUT should be readable");
  expect(found.value == "Vishnu", "PUT returned wrong value");

  engine.remove("name");
  auto deleted = engine.get("name");
  expect(deleted.status == kronos::GetStatus::DELETED,
         "DELETE tombstone should win");

  engine.shutdown();
  std::cout << "PASS: basic engine put/get/remove\n";
}

void test_background_flush_and_shutdown_drain() {
  TempDb temp("flush", 100);
  kronos::Config config(temp.config);
  const fs::path db = temp.root / "db";

  {
    kronos::KronosEngine engine(config, db);
    const std::string big(700 * 1024, 'a');
    engine.put("a", big);
    engine.put("b", big); // rotates and schedules a

    auto a = engine.get("a");
    expect(a.status == kronos::GetStatus::FOUND,
           "read must survive active->immutable->SSTable transition");

    engine.shutdown(); // also flushes final active b
  }

  kronos::Manifest manifest(db / "MANIFEST");
  expect(manifest.filesAtLevel(0).size() == 2,
         "shutdown should drain both L0 flushes");

  std::cout << "PASS: background flush and graceful drain\n";
}

void test_background_compaction() {
  TempDb temp("compaction", 2);
  kronos::Config config(temp.config);
  const fs::path db = temp.root / "db";

  {
    kronos::KronosEngine engine(config, db);
    const std::string big_a(700 * 1024, 'a');
    const std::string big_b(700 * 1024, 'b');
    const std::string big_c(700 * 1024, 'c');

    engine.put("a", big_a);
    engine.put("b", big_b);
    engine.put("c", big_c);
    engine.shutdown();
  }

  kronos::Manifest manifest(db / "MANIFEST");
  expect(!manifest.filesAtLevel(1).empty(),
         "L0 threshold should trigger background compaction into L1");

  std::cout << "PASS: background compaction\n";
}

void test_concurrent_reader_with_background_maintenance() {
  TempDb temp("concurrency", 4);
  kronos::Config config(temp.config);
  kronos::KronosEngine engine(config, temp.root / "db");

  constexpr int count = 80;
  std::atomic<int> published{0};
  std::atomic<bool> failed{false};

  std::thread writer([&] {
    try {
      const std::string payload(32 * 1024, 'x');
      for (int i = 0; i < count; ++i) {
        engine.put("k" + std::to_string(i), payload + std::to_string(i));
        published.store(i + 1, std::memory_order_release);
      }
    } catch (...) {
      failed.store(true, std::memory_order_release);
    }
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    readers.emplace_back([&] {
      try {
        while (published.load(std::memory_order_acquire) < count &&
               !failed.load(std::memory_order_acquire)) {
          int available = published.load(std::memory_order_acquire);
          if (available == 0) {
            std::this_thread::yield();
            continue;
          }

          const int index = (available - 1) / 2;
          auto result = engine.get("k" + std::to_string(index));
          if (result.status != kronos::GetStatus::FOUND) {
            failed.store(true, std::memory_order_release);
            break;
          }
        }
      } catch (...) {
        failed.store(true, std::memory_order_release);
      }
    });
  }

  writer.join();
  for (auto &reader : readers) {
    reader.join();
  }

  expect(!failed.load(), "concurrent reader/writer maintenance test failed");
  engine.shutdown();

  std::cout << "PASS: concurrent reads during background maintenance\n";
}

int main() {
  try {
    std::cout << "Running KronosEngine Module 8 tests...\n\n";
    test_basic_api();
    test_background_flush_and_shutdown_drain();
    test_background_compaction();
    test_concurrent_reader_with_background_maintenance();
    std::cout << "\nAll KronosEngine Module 8 tests passed.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "\nFAIL: " << e.what() << '\n';
    return 1;
  }
}
