#include "kronos/sstable.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
  const std::filesystem::path final_path = "test_table.sst";

  auto temp_path = final_path;
  temp_path.replace_extension(".tmp");

  // Clean up leftovers from an earlier test run.
  std::filesystem::remove(final_path);
  std::filesystem::remove(temp_path);

  {
    // Small block target deliberately chosen so that
    // several records may force more than one data block.
    kronos::SstableBuilder builder(final_path, 64);

    builder.add("apple",
                kronos::InternalEntry{"red", 1, kronos::OperationType::PUT});

    builder.add("banana",
                kronos::InternalEntry{"yellow", 2, kronos::OperationType::PUT});

    builder.add("cat",
                kronos::InternalEntry{"", 3, kronos::OperationType::DELETE});

    builder.add("dog",
                kronos::InternalEntry{"brown", 4, kronos::OperationType::PUT});

    builder.finish();
  }

  if (!std::filesystem::exists(final_path)) {
    throw std::runtime_error("FAIL: final .sst file was not created");
  }

  if (std::filesystem::exists(temp_path)) {
    throw std::runtime_error("FAIL: temporary .tmp file still exists");
  }

  if (std::filesystem::file_size(final_path) == 0) {
    throw std::runtime_error("FAIL: SSTable file is empty");
  }

  std::cout << "PASS: SSTable writer created finalized .sst file\n";

  // Remove test output.
  std::filesystem::remove(final_path);

  return 0;
}