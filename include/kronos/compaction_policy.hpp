#pragma once

#include "kronos/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace kronos {

class CompactionPolicy {

public:
  // v1 policy:
  //
  //   L0 -> L1
  //
  // Compaction begins once L0 reaches l0_file_trigger.
  //
  // bottom_level tells us whether L1 is actually the
  // lowest level and therefore whether tombstones may
  // safely be removed.
  explicit CompactionPolicy(size_t l0_file_trigger = 4,
                            size_t bottom_level = 1);

  bool shouldCompact(const std::vector<SstableMetadata> &live_files) const;

  std::optional<CompactionPlan>
  pickCompaction(const std::vector<SstableMetadata> &live_files) const;

private:
  size_t l0_file_trigger_;
  size_t bottom_level_;

  static bool rangesOverlap(const std::string &a_smallest,
                            const std::string &a_largest,
                            const std::string &b_smallest,
                            const std::string &b_largest);
};

} // namespace kronos