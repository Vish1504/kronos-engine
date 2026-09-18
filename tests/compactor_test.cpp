#include "kronos/compactor.hpp"
#include "kronos/sstable.hpp"
#include "kronos/types.hpp"

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

} // namespace

int main() {

  std::cout << "Running Compactor tests...\n\n";

  const std::filesystem::path test_dir = "compactor_test_data";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto sstable0_path = test_dir / "0.sst";
  const auto sstable1_path = test_dir / "1.sst";
  const auto sstable2_path = test_dir / "2.sst";
  const auto output_path = test_dir / "compacted.sst";

  constexpr size_t block_size = 64;
  constexpr size_t bits_per_key = 10;

  // ---------------------------------------------------------
  // SSTable 0
  //
  // A -> PUT seq 10
  // D -> PUT seq 30
  // G -> PUT seq 50
  // ---------------------------------------------------------

  {
    SstableBuilder builder(sstable0_path, block_size, bits_per_key);

    builder.add("A", InternalEntry{"A10", 10, OperationType::PUT});

    builder.add("D", InternalEntry{"D30", 30, OperationType::PUT});

    builder.add("G", InternalEntry{"G50", 50, OperationType::PUT});

    builder.finish();
  }

  // ---------------------------------------------------------
  // SSTable 1
  //
  // B -> PUT seq 15
  // D -> DELETE seq 40
  // F -> PUT seq 45
  // ---------------------------------------------------------

  {
    SstableBuilder builder(sstable1_path, block_size, bits_per_key);

    builder.add("B", InternalEntry{"B15", 15, OperationType::PUT});

    builder.add("D", InternalEntry{"", 40, OperationType::DELETE});

    builder.add("F", InternalEntry{"F45", 45, OperationType::PUT});

    builder.finish();
  }

  // ---------------------------------------------------------
  // SSTable 2
  //
  // A -> PUT seq 20
  // C -> PUT seq 25
  // H -> PUT seq 60
  // ---------------------------------------------------------

  {
    SstableBuilder builder(sstable2_path, block_size, bits_per_key);

    builder.add("A", InternalEntry{"A20", 20, OperationType::PUT});

    builder.add("C", InternalEntry{"C25", 25, OperationType::PUT});

    builder.add("H", InternalEntry{"H60", 60, OperationType::PUT});

    builder.finish();
  }

  // ---------------------------------------------------------
  // Run compaction
  // ---------------------------------------------------------

  Compactor compactor;

  std::vector<std::filesystem::path> input_files{sstable0_path, sstable1_path,
                                                 sstable2_path};

  CompactionResult result =
      compactor.compact(input_files, output_path, block_size, bits_per_key);

  // ---------------------------------------------------------
  // Verify CompactionResult
  // ---------------------------------------------------------

  expect(result.input_files.size() == 3,
         "CompactionResult should report three input files");

  expect(result.output_files.size() == 1,
         "CompactionResult should report one output file");

  expect(result.output_files[0] == output_path,
         "CompactionResult should report the compacted SSTable");

  expect(std::filesystem::exists(output_path),
         "Compacted SSTable should exist");

  // ---------------------------------------------------------
  // Expected compacted records
  //
  // A10 vs A20       -> A20 wins
  // B15              -> survives
  // C25              -> survives
  // D30 vs DELETE40  -> DELETE40 wins
  // F45              -> survives
  // G50              -> survives
  // H60              -> survives
  // ---------------------------------------------------------

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

  // ---------------------------------------------------------
  // Walk compacted SSTable
  // ---------------------------------------------------------

  SstableReader reader(output_path);

  auto iterator = reader.getIterator();

  size_t index = 0;

  while (iterator.valid()) {

    expect(index < expected.size(),
           "Compacted SSTable contains too many records");

    const auto &entry = iterator.getEntry();
    const auto &expected_entry = expected[index];

    expect(iterator.getKey() == expected_entry.key,
           "Unexpected key in compacted SSTable");

    expect(entry.sequence == expected_entry.sequence,
           "Unexpected sequence number in compacted SSTable");

    expect(entry.operation == expected_entry.operation,
           "Unexpected operation in compacted SSTable");

    iterator.next();
    ++index;
  }

  expect(index == expected.size(),
         "Compacted SSTable contains too few records");

  std::cout << "PASS: K-way SSTable compaction\n";
  std::cout << "PASS: newest sequence wins during compaction\n";
  std::cout << "PASS: tombstone survives normal compaction\n";
  std::cout << "PASS: compacted SSTable remains sorted\n";

  std::filesystem::remove_all(test_dir);

  std::cout << "\nAll Compactor tests passed.\n";

  return 0;
}