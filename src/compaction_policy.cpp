#include "kronos/compaction_policy.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace kronos {

CompactionPolicy::CompactionPolicy(size_t l0_file_trigger, size_t bottom_level)
    : l0_file_trigger_(l0_file_trigger), bottom_level_(bottom_level) {

  if (l0_file_trigger_ == 0) {
    throw std::invalid_argument(
        "L0 compaction trigger must be greater than zero");
  }

  if (bottom_level_ < 1) {
    throw std::invalid_argument("Bottom level must be at least 1");
  }
}

bool CompactionPolicy::rangesOverlap(const std::string &a_smallest,
                                     const std::string &a_largest,
                                     const std::string &b_smallest,
                                     const std::string &b_largest) {

  return !(a_largest < b_smallest || b_largest < a_smallest);
}

bool CompactionPolicy::shouldCompact(
    const std::vector<SstableMetadata> &live_files) const {

  size_t l0_count = 0;

  for (const auto &file : live_files) {

    if (file.level == 0) {
      ++l0_count;
    }
  }

  return l0_count >= l0_file_trigger_;
}

std::optional<CompactionPlan> CompactionPolicy::pickCompaction(
    const std::vector<SstableMetadata> &live_files) const {

  std::vector<SstableMetadata> l0_files;

  for (const auto &file : live_files) {

    if (file.level == 0) {

      if (file.smallest_key > file.largest_key) {

        throw std::runtime_error("Invalid L0 SSTable key range");
      }

      l0_files.push_back(file);
    }
  }

  if (l0_files.size() < l0_file_trigger_) {

    return std::nullopt;
  }

  // -------------------------------------------------------
  // v1 simplification:
  //
  // Compact ALL L0 SSTables together.
  // -------------------------------------------------------

  std::string smallest_key = l0_files[0].smallest_key;

  std::string largest_key = l0_files[0].largest_key;

  for (const auto &file : l0_files) {

    smallest_key = std::min(smallest_key, file.smallest_key);

    largest_key = std::max(largest_key, file.largest_key);
  }

  CompactionPlan plan;

  plan.input_files = l0_files;

  plan.output_level = 1;

  // -------------------------------------------------------
  // Include every L1 SSTable whose key range overlaps the
  // combined L0 range.
  //
  // Example:
  //
  // L0 range = D..H
  //
  // L1:
  //   A..C  -> skip
  //   D..J  -> include
  //   K..Z  -> skip
  // -------------------------------------------------------

  for (const auto &file : live_files) {

    if (file.level != 1) {
      continue;
    }

    if (rangesOverlap(smallest_key, largest_key, file.smallest_key,
                      file.largest_key)) {

      plan.input_files.push_back(file);
    }
  }

  // Tombstones may disappear only when L1 is genuinely
  // the bottom level.
  //
  // Since v1 compacts ALL L0 files plus every overlapping
  // L1 file, there is no older overlapping state left
  // outside the compaction when L1 is the bottom.
  plan.can_drop_tombstones = (bottom_level_ == 1);

  // Deterministic ordering is useful for tests/debugging.
  std::sort(plan.input_files.begin(), plan.input_files.end(),
            [](const SstableMetadata &a, const SstableMetadata &b) {
              if (a.level != b.level) {
                return a.level < b.level;
              }

              return a.path.string() < b.path.string();
            });

  return plan;
}

} // namespace kronos