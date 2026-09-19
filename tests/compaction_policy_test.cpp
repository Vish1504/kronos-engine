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

      // -----------------------------------------------------
      // L0 files
      //
      // Combined range:
      // D..J
      // -----------------------------------------------------

      {"10.sst", 0, "D", "H"},
      {"11.sst", 0, "F", "J"},

      // -----------------------------------------------------
      // L1 files
      // -----------------------------------------------------

      {"20.sst", 1, "A", "C"}, // no overlap
      {"21.sst", 1, "D", "G"}, // overlap
      {"22.sst", 1, "H", "M"}, // overlap
      {"23.sst", 1, "N", "Z"}  // no overlap
  };

  // =========================================================
  // TEST 1
  //
  // Custom L0 threshold = 2.
  //
  // Two L0 SSTables should trigger compaction.
  // =========================================================

  CompactionPolicy policy(2);

  expect(policy.shouldCompact(files), "Two L0 files should trigger compaction");

  const auto plan = policy.pickCompaction(files);

  expect(plan.has_value(), "Compaction plan should exist");

  expect(plan->output_level == 1, "L0 compaction should output into L1");

  // Every L0 SSTable participates once the threshold fires.
  expect(planContains(*plan, "10.sst"), "Plan should include first L0 SSTable");

  expect(planContains(*plan, "11.sst"),
         "Plan should include second L0 SSTable");

  // Combined L0 range is D..J.
  //
  // A..C does not overlap.
  expect(!planContains(*plan, "20.sst"),
         "Non-overlapping L1 A..C should be skipped");

  // D..G overlaps D..J.
  expect(planContains(*plan, "21.sst"),
         "Overlapping L1 D..G should be included");

  // H..M overlaps D..J.
  expect(planContains(*plan, "22.sst"),
         "Overlapping L1 H..M should be included");

  // N..Z does not overlap D..J.
  expect(!planContains(*plan, "23.sst"),
         "Non-overlapping L1 N..Z should be skipped");

  /*
   * Chronos v1 has only L0 and L1.
   *
   * L1 is therefore the bottom level.
   *
   * Since the plan includes every L0 SSTable and every
   * overlapping L1 SSTable, no older overlapping version
   * can remain below this compaction.
   */
  expect(plan->can_drop_tombstones,
         "Tombstones should be droppable at the v1 bottom level");

  // =========================================================
  // TEST 2
  //
  // Custom threshold = 3.
  //
  // Only two L0 files currently exist, so compaction
  // should NOT happen.
  // =========================================================

  CompactionPolicy high_threshold(3);

  expect(!high_threshold.shouldCompact(files),
         "Two L0 files should not trigger threshold three");

  expect(!high_threshold.pickCompaction(files).has_value(),
         "No plan should exist before threshold");

  // =========================================================
  // TEST 3
  //
  // Default threshold.
  //
  // Chronos v1 defaults to 4 L0 files.
  // =========================================================

  CompactionPolicy default_policy;

  expect(!default_policy.shouldCompact(files),
         "Default threshold should not compact with only two L0 files");

  // =========================================================
  // TEST 4
  //
  // Configurable threshold.
  //
  // Proves that changing the constructor value actually
  // changes when compaction triggers.
  // =========================================================

  std::vector<SstableMetadata> six_l0_files{
      {"0.sst", 0, "A", "B"}, {"1.sst", 0, "C", "D"}, {"2.sst", 0, "E", "F"},
      {"3.sst", 0, "G", "H"}, {"4.sst", 0, "I", "J"}, {"5.sst", 0, "K", "L"},
  };

  CompactionPolicy custom_threshold(6);

  expect(custom_threshold.shouldCompact(six_l0_files),
         "Custom threshold six should compact at six L0 files");

  CompactionPolicy threshold_seven(7);

  expect(!threshold_seven.shouldCompact(six_l0_files),
         "Threshold seven should not compact with only six L0 files");

  std::cout << "PASS: configurable L0 file-count trigger\n";
  std::cout << "PASS: overlapping L1 SSTables selected\n";
  std::cout << "PASS: non-overlapping L1 SSTables skipped\n";
  std::cout << "PASS: v1 bottom-level tombstone safety\n";
  std::cout << "PASS: default L0 threshold\n";

  std::cout << "\nAll CompactionPolicy tests passed.\n";

  return 0;
}