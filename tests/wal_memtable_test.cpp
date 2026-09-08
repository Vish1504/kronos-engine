#include "kronos/memtable.hpp"
#include "kronos/wal.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
  const std::filesystem::path wal_path = "tests/integration_test.wal";

  // Start with a clean WAL.
  std::filesystem::remove(wal_path);

  // --------------------------------------------------
  // 1. Write some data to the WAL
  // --------------------------------------------------
  {
    kronos::Wal wal(wal_path);

    wal.put("name", "Krishna");
    wal.put("language", "C++");
    wal.put("name", "Vishnu");
    wal.remove("language");
  }

  // At this point imagine Kronos crashed.
  // The old Memtable is gone.

  // --------------------------------------------------
  // 2. Start again with a fresh Memtable
  // --------------------------------------------------
  kronos::Memtable memtable(4096);

  {
    kronos::Wal wal(wal_path);

    auto records = wal.recover();

    // ------------------------------------------------
    // 3. Replay WAL records into Memtable
    // ------------------------------------------------
    for (const auto &record : records) {

      if (record.operation == kronos::Wal::Operation::PUT) {
        memtable.put(record.key, record.value, record.sequence);
      } else if (record.operation == kronos::Wal::Operation::DELETE) {
        memtable.remove(record.key, record.sequence);
      }
    }
  }

  // --------------------------------------------------
  // 4. Verify reconstructed state
  // --------------------------------------------------

  auto name = memtable.get("name");

  assert(name.status == kronos::Memtable::GetStatus::FOUND);
  assert(name.value == "Vishnu");

  auto language = memtable.get("language");

  assert(language.status == kronos::Memtable::GetStatus::DELETED);

  std::cout << "PASS: WAL successfully rebuilt Memtable\n";

  std::filesystem::remove(wal_path);

  return 0;
}