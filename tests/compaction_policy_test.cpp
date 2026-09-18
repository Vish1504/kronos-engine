#include "kronos/compaction_policy.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace kronos;

namespace {

void expect(bool condition, const std::string &message) {

  if (!condition) {

    std::cerr << "FAIL: " << message << '\n';

    std::exit(1);
  }
}

bool planContains(const CompactionPlan &plan,
                  const std::filesystem::path &path) {

  for (const auto &file : plan.input_files) {

    if (file.path == path) {
      return true;
    }
  }

  return false;
}

} // namespace

int main() {

  std::cout << "Running CompactionPolicy tests...\n\n";

  const std::vector<SstableMetadata> files{

      // L0 files.
      //
      // Combined range:
      // D..J

      {"10.sst", 0, "D", "H"},
      {"11.sst", 0, "F", "J"},

      // L1 files.

      {"20.sst", 1, "A", "C"}, // no overlap
      {"21.sst", 1, "D", "G"}, // overlap
      {"22.sst", 1, "H", "M"}, // overlap
      {"23.sst", 1, "N", "Z"}  // no overlap
  };

  // -------------------------------------------------------
  // L0 threshold = 2
  // L1 is bottom level
  // -------------------------------------------------------

  CompactionPolicy policy(2, 1);

  expect(policy.shouldCompact(files), "Two L0 files should trigger compaction");

  const auto plan = policy.pickCompaction(files);

  expect(plan.has_value(), "Compaction plan should exist");

  expect(plan->output_level == 1, "L0 compaction should output into L1");

  expect(planContains(*plan, "10.sst"), "Plan should include first L0 SSTable");

  expect(planContains(*plan, "11.sst"),
         "Plan should include second L0 SSTable");

  expect(!planContains(*plan, "20.sst"),
         "Non-overlapping L1 A..C should be skipped");

  expect(planContains(*plan, "21.sst"),
         "Overlapping L1 D..G should be included");

  expect(planContains(*plan, "22.sst"),
         "Overlapping L1 H..M should be included");

  expect(!planContains(*plan, "23.sst"),
         "Non-overlapping L1 N..Z should be skipped");

  expect(plan->can_drop_tombstones,
         "Tombstones may be dropped when L1 is bottom level");

  // -------------------------------------------------------
  // Threshold not reached
  // -------------------------------------------------------

  CompactionPolicy high_threshold(3, 1);

  expect(!high_threshold.shouldCompact(files),
         "Two L0 files should not trigger threshold three");

  expect(!high_threshold.pickCompaction(files).has_value(),
         "No plan should exist before threshold");

  // -------------------------------------------------------
  // If L1 is NOT the bottom level, tombstones must remain.
  // -------------------------------------------------------

  CompactionPolicy deeper_database(2, 2);

  const auto deeper_plan = deeper_database.pickCompaction(files);

  expect(deeper_plan.has_value(), "Compaction should still be selected");

  expect(!deeper_plan->can_drop_tombstones,
         "Tombstones must remain when lower levels may exist");

  std::cout << "PASS: L0 file-count trigger\n";

  std::cout << "PASS: overlapping L1 SSTables selected\n";

  std::cout << "PASS: non-overlapping L1 SSTables skipped\n";

  std::cout << "PASS: tombstone safety follows bottom-level context\n";

  std::cout << "\nAll CompactionPolicy tests passed.\n";

  return 0;
}