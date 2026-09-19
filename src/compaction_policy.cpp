#include "kronos/compaction_policy.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace kronos {

/*
 * Create the v1 compaction policy.
 *
 * The L0 trigger is configurable because it is a tuning parameter.
 *
 * Chronos v1 itself has only:
 *
 *     L0 -> L1
 *
 * so L1 being the bottom level is an architectural property rather
 * than something the user configures.
 */
CompactionPolicy::CompactionPolicy(size_t l0_file_trigger)
    : l0_file_trigger_(l0_file_trigger) {

  if (l0_file_trigger_ == 0) {
    throw std::invalid_argument(
        "L0 compaction trigger must be greater than zero");
  }
}

/*
 * Return true when two inclusive key ranges overlap.
 *
 * Two ranges do NOT overlap when:
 *
 *     A ends before B begins
 *
 *          OR
 *
 *     B ends before A begins
 *
 * Negating that condition gives us overlap.
 *
 * Examples:
 *
 *     A..F and D..K -> overlap
 *     A..F and G..Z -> no overlap
 */
bool CompactionPolicy::rangesOverlap(const std::string &a_smallest,
                                     const std::string &a_largest,
                                     const std::string &b_smallest,
                                     const std::string &b_largest) {

  return !(a_largest < b_smallest || b_largest < a_smallest);
}

/*
 * Decide whether L0 has accumulated enough SSTables
 * to trigger compaction.
 *
 * Only L0 files count toward this threshold.
 */
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

/*
 * Build one concrete Chronos v1 compaction plan.
 *
 * v1 strategy:
 *
 *   1. Select ALL live L0 SSTables once the trigger is reached.
 *
 *   2. Compute the combined key-range envelope of those L0 files.
 *
 *   3. Include every L1 SSTable overlapping that range.
 *
 *   4. Merge everything into a new L1 SSTable.
 *
 * Example:
 *
 * L0:
 *     A..D
 *     F..H
 *     C..G
 *
 * Combined range:
 *     A..H
 *
 * L1:
 *     A..B   -> include
 *     D..F   -> include
 *     H..K   -> include
 *     M..Z   -> skip
 */
std::optional<CompactionPlan> CompactionPolicy::pickCompaction(
    const std::vector<SstableMetadata> &live_files) const {

  // -------------------------------------------------------------------------
  // Collect all L0 SSTables
  // -------------------------------------------------------------------------

  std::vector<SstableMetadata> l0_files;

  for (const auto &file : live_files) {

    if (file.level != 0) {
      continue;
    }

    // Every SSTable must describe a valid inclusive key range.
    if (file.smallest_key > file.largest_key) {
      throw std::runtime_error("Invalid L0 SSTable key range");
    }

    l0_files.push_back(file);
  }

  // Not enough L0 files yet -> no compaction job.
  if (l0_files.size() < l0_file_trigger_) {
    return std::nullopt;
  }

  // -------------------------------------------------------------------------
  // Compute the combined L0 key range
  // -------------------------------------------------------------------------

  /*
   * Chronos v1 deliberately compacts ALL L0 SSTables together.
   *
   * We therefore need the smallest key from any selected L0 file
   * and the largest key from any selected L0 file.
   */
  std::string smallest_key = l0_files[0].smallest_key;

  std::string largest_key = l0_files[0].largest_key;

  for (const auto &file : l0_files) {

    smallest_key = std::min(smallest_key, file.smallest_key);

    largest_key = std::max(largest_key, file.largest_key);
  }

  // -------------------------------------------------------------------------
  // Create the initial plan
  // -------------------------------------------------------------------------

  CompactionPlan plan;

  // Every L0 file participates.
  plan.input_files = l0_files;

  // Chronos v1 compacts L0 into L1.
  plan.output_level = 1;

  // -------------------------------------------------------------------------
  // Include overlapping L1 SSTables
  // -------------------------------------------------------------------------

  /*
   * L1 is intended to remain organized by non-overlapping key ranges.
   *
   * Therefore, before creating the new L1 SSTable, we must also consume
   * every existing L1 SSTable that overlaps the selected L0 range.
   *
   * Otherwise the newly-created L1 SSTable could overlap existing L1 files.
   */
  for (const auto &file : live_files) {

    if (file.level != 1) {
      continue;
    }

    if (file.smallest_key > file.largest_key) {
      throw std::runtime_error("Invalid L1 SSTable key range");
    }

    if (rangesOverlap(smallest_key, largest_key, file.smallest_key,
                      file.largest_key)) {

      plan.input_files.push_back(file);
    }
  }

  // -------------------------------------------------------------------------
  // Tombstone safety
  // -------------------------------------------------------------------------

  /*
   * L1 is the bottom level in Chronos v1.
   *
   * We are compacting:
   *
   *     - every L0 SSTable,
   *     - every overlapping L1 SSTable.
   *
   * Therefore no older overlapping version of a key can remain below
   * this compaction.
   *
   * Since the output is going to the bottom level, DELETE tombstones
   * may safely disappear.
   *
   * Writing this in terms of BOTTOM_LEVEL keeps the reason explicit,
   * even though both values are currently always 1.
   */
  plan.can_drop_tombstones = (plan.output_level == BOTTOM_LEVEL);

  // -------------------------------------------------------------------------
  // Deterministic plan ordering
  // -------------------------------------------------------------------------

  /*
   * Sorting is not required for compaction correctness.
   *
   * It makes plans deterministic, which is useful for:
   *
   *     - tests,
   *     - debugging,
   *     - logs,
   *     - reproducibility.
   *
   * Files are ordered first by level, then by path.
   */
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