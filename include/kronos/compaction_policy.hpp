#pragma once

#include "kronos/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace kronos {

/*
 * CompactionPolicy
 * ----------------
 * Decides WHEN compaction should happen and WHICH SSTables
 * should participate.
 *
 * Chronos v1 uses a simple two-level layout:
 *
 *     Memtable
 *        ↓
 *       L0
 *        ↓
 *       L1
 *
 * L0 may contain overlapping SSTables.
 * L1 is the bottom level in v1.
 *
 * The policy does NOT perform the physical merge.
 * That responsibility belongs to Compactor.
 */
class CompactionPolicy {

public:
  /*
   * Default number of live L0 SSTables that triggers compaction.
   *
   * This value may later come from Chronos Config.
   */
  static constexpr size_t DEFAULT_L0_FILE_TRIGGER = 4;

  /*
   * Chronos v1 has only L0 and L1.
   *
   * L1 is therefore the bottom level.
   * This is an architectural property, not a user tuning option.
   */
  static constexpr size_t BOTTOM_LEVEL = 1;

  /*
   * Create a compaction policy.
   *
   * l0_file_trigger
   *   Number of live L0 SSTables required before compaction begins.
   *
   * Example:
   *
   *   trigger = 4
   *
   *   3 L0 files -> no compaction
   *   4 L0 files -> compaction
   *   5 L0 files -> compaction
   *
   * The constructor should reject a trigger of 0.
   */
  explicit CompactionPolicy(size_t l0_file_trigger = DEFAULT_L0_FILE_TRIGGER);

  /*
   * Return true when the current live-file set has reached
   * the L0 compaction threshold.
   *
   * Only live L0 SSTables count toward the trigger.
   */
  bool shouldCompact(const std::vector<SstableMetadata> &live_files) const;

  /*
   * Build a concrete compaction job from the current live SSTables.
   *
   * Chronos v1 policy:
   *
   *   1. Collect all live L0 SSTables.
   *   2. If their count is below the threshold, return nullopt.
   *   3. Compute the combined key range of all selected L0 files.
   *   4. Include every L1 SSTable overlapping that range.
   *   5. Output the compaction result into L1.
   *   6. Allow tombstone dropping because L1 is the bottom level
   *      and all relevant older versions for the range are included.
   */
  std::optional<CompactionPlan>
  pickCompaction(const std::vector<SstableMetadata> &live_files) const;

private:
  /*
   * Configurable threshold controlling when L0 compaction begins.
   */
  size_t l0_file_trigger_;

  /*
   * Return true when two inclusive key ranges overlap.
   *
   * Example:
   *
   *   A..F and D..K -> true
   *   A..F and G..Z -> false
   */
  static bool rangesOverlap(const std::string &a_smallest,
                            const std::string &a_largest,
                            const std::string &b_smallest,
                            const std::string &b_largest);
};

} // namespace kronos