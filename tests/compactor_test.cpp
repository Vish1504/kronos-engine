#include "kronos/compactor.hpp"
#include "kronos/sstable.hpp"
#include "kronos/types.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace kronos;

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

// =========================================================
// TEST 1
//
// Normal compaction:
//   - K-way merge works
//   - newest sequence wins
//   - tombstones are preserved
// =========================================================

void testNormalCompaction() {

  const std::filesystem::path test_dir = "compactor_test_data_normal";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto sstable0_path = test_dir / "0.sst";
  const auto sstable1_path = test_dir / "1.sst";
  const auto sstable2_path = test_dir / "2.sst";
  const auto output_path = test_dir / "compacted.sst";

  constexpr size_t block_size = 64;
  constexpr size_t bits_per_key = 10;

  // -------------------------------------------------------
  // SSTable 0
  //
  // A -> PUT seq 10
  // D -> PUT seq 30
  // G -> PUT seq 50
  // -------------------------------------------------------

  {
    SstableBuilder builder(sstable0_path, block_size, bits_per_key);

    builder.add("A", InternalEntry{"A10", 10, OperationType::PUT});

    builder.add("D", InternalEntry{"D30", 30, OperationType::PUT});

    builder.add("G", InternalEntry{"G50", 50, OperationType::PUT});

    builder.finish();
  }

  // -------------------------------------------------------
  // SSTable 1
  //
  // B -> PUT seq 15
  // D -> DELETE seq 40
  // F -> PUT seq 45
  // -------------------------------------------------------

  {
    SstableBuilder builder(sstable1_path, block_size, bits_per_key);

    builder.add("B", InternalEntry{"B15", 15, OperationType::PUT});

    builder.add("D", InternalEntry{"", 40, OperationType::DELETE});

    builder.add("F", InternalEntry{"F45", 45, OperationType::PUT});

    builder.finish();
  }

  // -------------------------------------------------------
  // SSTable 2
  //
  // A -> PUT seq 20
  // C -> PUT seq 25
  // H -> PUT seq 60
  // -------------------------------------------------------

  {
    SstableBuilder builder(sstable2_path, block_size, bits_per_key);

    builder.add("A", InternalEntry{"A20", 20, OperationType::PUT});

    builder.add("C", InternalEntry{"C25", 25, OperationType::PUT});

    builder.add("H", InternalEntry{"H60", 60, OperationType::PUT});

    builder.finish();
  }

  Compactor compactor;

  std::vector<std::filesystem::path> input_files{sstable0_path, sstable1_path,
                                                 sstable2_path};

  // false:
  // this is normal compaction, so tombstones must survive.
  CompactionResult result = compactor.compact(input_files, output_path,
                                              block_size, bits_per_key, false);

  expect(result.input_files.size() == 3,
         "Normal compaction should report three input files");

  expect(result.output_files.size() == 1,
         "Normal compaction should produce one output SSTable");

  expect(std::filesystem::exists(output_path),
         "Normal compaction output SSTable should exist");

  struct ExpectedEntry {
    std::string key;
    uint64_t sequence;
    OperationType operation;
  };

  const std::vector<ExpectedEntry> expected{
      {"A", 20, OperationType::PUT}, {"B", 15, OperationType::PUT},
      {"C", 25, OperationType::PUT}, {"D", 40, OperationType::DELETE},
      {"F", 45, OperationType::PUT}, {"G", 50, OperationType::PUT},
      {"H", 60, OperationType::PUT},
  };

  SstableReader reader(output_path);

  auto iterator = reader.getIterator();

  size_t index = 0;

  while (iterator.valid()) {

    expect(index < expected.size(),
           "Normal compacted SSTable has too many records");

    const auto &entry = iterator.getEntry();
    const auto &expected_entry = expected[index];

    expect(iterator.getKey() == expected_entry.key,
           "Unexpected key in normal compacted SSTable");

    expect(entry.sequence == expected_entry.sequence,
           "Unexpected sequence in normal compacted SSTable");

    expect(entry.operation == expected_entry.operation,
           "Unexpected operation in normal compacted SSTable");

    iterator.next();
    ++index;
  }

  expect(index == expected.size(),
         "Normal compacted SSTable has too few records");

  std::filesystem::remove_all(test_dir);

  std::cout << "PASS: normal K-way compaction preserves tombstones\n";
}

// =========================================================
// TEST 2
//
// Tombstone GC:
//
// All surviving records are DELETEs and higher-level logic
// explicitly says they are safe to remove.
//
// Expected:
//   - no output SSTable is created
//   - CompactionResult.output_files is empty
// =========================================================

void testAllTombstonesDropped() {

  const std::filesystem::path test_dir = "compactor_test_data_tombstone_gc";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto sstable0_path = test_dir / "0.sst";
  const auto sstable1_path = test_dir / "1.sst";
  const auto output_path = test_dir / "compacted.sst";

  constexpr size_t block_size = 64;
  constexpr size_t bits_per_key = 10;

  // -------------------------------------------------------
  // SSTable 0
  //
  // A -> DELETE seq 10
  // C -> DELETE seq 30
  // -------------------------------------------------------

  {
    SstableBuilder builder(sstable0_path, block_size, bits_per_key);

    builder.add("A", InternalEntry{"", 10, OperationType::DELETE});

    builder.add("C", InternalEntry{"", 30, OperationType::DELETE});

    builder.finish();
  }

  // -------------------------------------------------------
  // SSTable 1
  //
  // A -> DELETE seq 20
  // B -> DELETE seq 25
  //
  // For A, seq 20 wins.
  // But because tombstone dropping is allowed,
  // A is removed entirely.
  // -------------------------------------------------------

  {
    SstableBuilder builder(sstable1_path, block_size, bits_per_key);

    builder.add("A", InternalEntry{"", 20, OperationType::DELETE});

    builder.add("B", InternalEntry{"", 25, OperationType::DELETE});

    builder.finish();
  }

  Compactor compactor;

  std::vector<std::filesystem::path> input_files{sstable0_path, sstable1_path};

  // true:
  // higher-level compaction logic has proven that
  // tombstones are safe to discard.
  CompactionResult result = compactor.compact(input_files, output_path,
                                              block_size, bits_per_key, true);

  expect(result.input_files.size() == 2,
         "Tombstone GC should report both input files");

  expect(result.output_files.empty(),
         "All-tombstone compaction should produce no output files");

  expect(!std::filesystem::exists(output_path),
         "No empty SSTable should be created");

  std::filesystem::remove_all(test_dir);

  std::cout << "PASS: safe tombstone GC produces no empty SSTable\n";
}

// =========================================================
// TEST 3
//
// Lazy output creation:
//
// The first key is a DELETE and gets dropped,
// but a later PUT survives.
//
// This proves that:
//   - builder does not need to exist immediately
//   - builder is created only when a surviving record appears
// =========================================================

void testLazyOutputCreation() {

  const std::filesystem::path test_dir = "compactor_test_data_lazy_builder";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto input_path = test_dir / "0.sst";
  const auto output_path = test_dir / "compacted.sst";

  constexpr size_t block_size = 64;
  constexpr size_t bits_per_key = 10;

  {
    SstableBuilder builder(input_path, block_size, bits_per_key);

    // This tombstone should disappear.
    builder.add("A", InternalEntry{"", 10, OperationType::DELETE});

    // This PUT should survive and cause the output builder
    // to be created lazily.
    builder.add("B", InternalEntry{"B20", 20, OperationType::PUT});

    builder.finish();
  }

  Compactor compactor;

  CompactionResult result = compactor.compact({input_path}, output_path,
                                              block_size, bits_per_key, true);

  expect(result.output_files.size() == 1,
         "A surviving PUT should create one output SSTable");

  expect(std::filesystem::exists(output_path),
         "Lazy output SSTable should exist");

  SstableReader reader(output_path);

  auto iterator = reader.getIterator();

  expect(iterator.valid(), "Lazy output SSTable should contain a record");

  expect(iterator.getKey() == "B",
         "Dropped tombstone A should not appear in output");

  const auto &entry = iterator.getEntry();

  expect(entry.sequence == 20, "Surviving B should retain sequence 20");

  expect(entry.operation == OperationType::PUT,
         "Surviving B should remain a PUT");

  iterator.next();

  expect(!iterator.valid(), "Lazy output SSTable should contain only B");

  std::filesystem::remove_all(test_dir);

  std::cout
      << "PASS: output SSTable is created lazily after dropped tombstones\n";
}

} // namespace

int main() {

  std::cout << "Running Compactor tests...\n\n";

  testNormalCompaction();

  testAllTombstonesDropped();

  testLazyOutputCreation();

  std::cout << "\nAll Compactor tests passed.\n";

  return 0;
}