#include "kronos/memtable.hpp"
#include "kronos/sstable.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error("TEST FAILED: " + message);
  }
}

} // namespace

int main() {
  using namespace kronos;

  const std::filesystem::path path = "sstable_roundtrip_test.sst";

  auto temp_path = path;
  temp_path.replace_extension(".tmp");

  // Clean leftovers from an earlier failed test run.
  std::filesystem::remove(path);
  std::filesystem::remove(temp_path);

  try {

    // ============================================================
    // 1. BUILD AN SSTABLE
    // ============================================================

    {
      // Small target deliberately forces multiple data blocks.
      SstableBuilder builder(path, 64, 10, 5);

      builder.add("apple", InternalEntry{.value = "red",
                                         .sequence = 1,
                                         .operation = OperationType::PUT});

      builder.add("banana", InternalEntry{.value = "yellow",
                                          .sequence = 2,
                                          .operation = OperationType::PUT});

      builder.add("cat", InternalEntry{.value = "",
                                       .sequence = 3,
                                       .operation = OperationType::DELETE});

      builder.add("dog", InternalEntry{.value = "brown",
                                       .sequence = 4,
                                       .operation = OperationType::PUT});

      builder.add("mango", InternalEntry{.value = "orange",
                                         .sequence = 5,
                                         .operation = OperationType::PUT});

      builder.finish();
    }

    require(std::filesystem::exists(path), "final .sst file was not created");

    require(!std::filesystem::exists(temp_path),
            ".tmp file still exists after finish()");

    // ============================================================
    // 2. READ IT BACK
    // ============================================================

    {
      SstableReader reader(path);

      // Existing PUT in the first block.
      auto apple = reader.get("apple");

      require(apple.status == GetStatus::FOUND, "apple should be FOUND");

      require(apple.value == "red", "apple should contain value 'red'");

      // Another key from the early part of the SSTable.
      auto banana = reader.get("banana");

      require(banana.status == GetStatus::FOUND, "banana should be FOUND");

      require(banana.value == "yellow", "banana should contain value 'yellow'");

      // Tombstone must remain distinguishable from absence.
      auto cat = reader.get("cat");

      require(cat.status == GetStatus::DELETED, "cat should return DELETED");

      // Later block lookup proves sparse-index block selection works.
      auto dog = reader.get("dog");

      require(dog.status == GetStatus::FOUND, "dog should be FOUND");

      require(dog.value == "brown", "dog should contain value 'brown'");

      // Final block.
      auto mango = reader.get("mango");

      require(mango.status == GetStatus::FOUND, "mango should be FOUND");

      require(mango.value == "orange", "mango should contain value 'orange'");

      // Missing key between existing ranges.
      auto carrot = reader.get("carrot");

      require(carrot.status == GetStatus::NOT_FOUND,
              "carrot should be NOT_FOUND");

      // Smaller than the smallest key in the entire SSTable.
      auto aardvark = reader.get("aardvark");

      require(aardvark.status == GetStatus::NOT_FOUND,
              "aardvark should be NOT_FOUND");

      // Larger than the largest key.
      auto zebra = reader.get("zebra");

      require(zebra.status == GetStatus::NOT_FOUND,
              "zebra should be NOT_FOUND");
    }

    std::cout << "PASS: SSTable Writer -> Reader round-trip" << std::endl;

    // ============================================================
    // ITERATOR TEST
    // ============================================================

    {
      SstableReader reader(path);

      auto it = reader.getIterator();

      std::vector<std::string> expected_keys = {"apple", "banana", "cat", "dog",
                                                "mango"};

      std::vector<std::string> actual_keys;

      while (it.valid()) {
        actual_keys.push_back(it.getKey());

        const auto &entry = it.getEntry();

        // Verify one normal PUT.
        if (it.getKey() == "apple") {
          require(entry.value == "red",
                  "Iterator returned wrong value for apple");

          require(entry.sequence == 1,
                  "Iterator returned wrong sequence for apple");

          require(entry.operation == OperationType::PUT,
                  "Iterator returned wrong operation for apple");
        }

        // Verify tombstones survive sequential reads too.
        if (it.getKey() == "cat") {
          require(entry.sequence == 3,
                  "Iterator returned wrong sequence for cat");

          require(entry.operation == OperationType::DELETE,
                  "Iterator should expose cat as DELETE");
        }

        it.next();
      }

      require(actual_keys == expected_keys,
              "SSTable Iterator did not return records in sorted order");

      std::cout << "PASS: SSTable Iterator walked all records in sorted order"
                << std::endl;
    }

    {
      const std::filesystem::path empty_path = "empty_sstable_test.sst";

      std::filesystem::remove(empty_path);

      {
        SstableBuilder builder(empty_path, 64, 10, 1);
        builder.finish();
      }

      {
        SstableReader reader(empty_path);

        auto it = reader.getIterator();

        require(!it.valid(), "Iterator over empty SSTable should be invalid");
      }

      std::filesystem::remove(empty_path);

      std::cout << "PASS: Empty SSTable Iterator is invalid" << std::endl;
    }
    // ============================================================
    // MEMTABLE TOMBSTONE -> SSTABLE INTEGRATION TEST
    // ============================================================

    {
      const std::filesystem::path tombstone_path =
          "sstable_tombstone_integration_test.sst";

      auto tombstone_temp_path = tombstone_path;
      tombstone_temp_path.replace_extension(".tmp");

      std::filesystem::remove(tombstone_path);
      std::filesystem::remove(tombstone_temp_path);

      // ----------------------------------------------------------
      // 1. Create a tombstone naturally inside a Memtable
      // ----------------------------------------------------------

      Memtable memtable(1024);

      auto put_result = memtable.put("A", "Harry", 10);
      require(put_result == Memtable::WriteResult::SUCCESS,
              "initial PUT should succeed");

      auto delete_result = memtable.remove("A", 20);
      require(delete_result == Memtable::WriteResult::SUCCESS,
              "DELETE should succeed");

      auto memtable_result = memtable.get("A");

      require(memtable_result.status == GetStatus::DELETED,
              "Memtable should contain tombstone before flush");

      memtable.freeze();

      // ----------------------------------------------------------
      // 2. Flush the Memtable into an SSTable using its Iterator
      // ----------------------------------------------------------

      {
        SstableBuilder builder(tombstone_path, 64, 10, memtable.entry_count());

        auto it = memtable.getIterator();

        while (it.valid()) {
          builder.add(it.key(), it.entry());
          it.next();
        }

        builder.finish();
      }

      // ----------------------------------------------------------
      // 3. Read the key back from disk
      // ----------------------------------------------------------

      {
        SstableReader reader(tombstone_path);

        auto result = reader.get("A");

        require(result.status == GetStatus::DELETED,
                "tombstone should survive Memtable -> SSTable");
      }

      std::filesystem::remove(tombstone_path);
      std::filesystem::remove(tombstone_temp_path);

      std::cout << "PASS: Memtable tombstone survived SSTable flush"
                << std::endl;
    }
    // ============================================================
    // 3. CORRUPTION TEST
    // ============================================================
    //
    // Header = 8 bytes.
    // First data block begins immediately afterward.
    // First 4 bytes of block = record_count.
    //
    // Therefore byte 12 is inside the first record.
    // Flip one bit without updating the block CRC.
    //

    {
      std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);

      require(file.is_open(), "could not reopen SSTable for corruption test");

      file.seekg(12);

      char byte = 0;
      file.read(&byte, 1);

      require(static_cast<bool>(file),
              "could not read byte for corruption test");

      byte ^= 0x01;

      file.seekp(12);
      file.write(&byte, 1);

      require(static_cast<bool>(file), "could not write corrupted byte");

      file.close();
    }

    bool corruption_detected = false;

    try {
      SstableReader corrupted_reader(path);

      // Constructor validates metadata, but data-block CRC
      // is checked only when that block is actually read.
      corrupted_reader.get("apple");

    } catch (const std::runtime_error &) {
      corruption_detected = true;
    }

    require(corruption_detected, "corrupted data block was not detected");

    std::cout << "PASS: SSTable block CRC detected corruption" << std::endl;

    // ============================================================
    // CLEANUP
    // ============================================================

    std::filesystem::remove(path);
    std::filesystem::remove(temp_path);

    std::cout << "PASS: SSTable reader integration tests" << std::endl;

    return 0;

  } catch (const std::exception &e) {

    std::filesystem::remove(path);
    std::filesystem::remove(temp_path);

    std::cerr << e.what() << std::endl;

    return 1;
  }
}
