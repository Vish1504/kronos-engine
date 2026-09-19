#pragma once

#include "kronos/types.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace kronos {

/*
 * Compactor
 * ---------
 * Performs the actual physical merge of multiple SSTables.
 *
 * The Compactor does NOT decide:
 *   - when compaction should happen,
 *   - which levels should be compacted,
 *   - which SSTables should be selected,
 *   - or whether tombstones are safe to drop.
 *
 * Those decisions belong to higher-level compaction policy.
 *
 * Its job is simpler:
 *
 *   input SSTables
 *        ↓
 *   K-way merge
 *        ↓
 *   resolve multiple versions of the same key
 *        ↓
 *   optionally discard safe tombstones
 *        ↓
 *   produce a new SSTable
 *
 * Important invariant:
 * Every input SSTable must contain keys in strictly increasing
 * order, with no duplicate keys inside the same SSTable.
 */
class Compactor {

public:
  /*
   * Merge the given SSTables into a new SSTable.
   *
   * input_files
   *   Paths of all SSTables participating in this compaction.
   *
   * output_path
   *   Destination path for the compacted SSTable.
   *
   * block_size
   *   Target block size used by SstableBuilder.
   *
   * bits_per_key
   *   Bloom-filter configuration for the output SSTable.
   *
   * can_drop_tombstones
   *   false:
   *     DELETE records must remain in the output.
   *
   *   true:
   *     DELETE records may be omitted because higher-level
   *     compaction logic has already proven that doing so is safe.
   *
   * The Compactor does not make that safety decision itself.
   *
   * Returns:
   *   CompactionResult containing:
   *     - all input files that were consumed,
   *     - all output files that were produced.
   *
   * A successful compaction may produce zero output files if every
   * surviving record is a tombstone that can safely be dropped.
   */
  CompactionResult
  compact(const std::vector<std::filesystem::path> &input_files,
          const std::filesystem::path &output_path, size_t block_size,
          size_t bits_per_key, bool can_drop_tombstones = false);

private:
  /*
   * One item stored inside the merge heap.
   *
   * key
   *   The current key exposed by an SSTable iterator.
   *
   * iterator_index
   *   Identifies which iterator that key came from.
   *
   * We deliberately do not store the full InternalEntry here.
   * The actual value, sequence number and operation are still owned
   * by the iterator and can be accessed using iterator_index.
   */
  struct HeapItem {
    std::string key;
    size_t iterator_index;
  };

  /*
   * Comparator used by std::priority_queue.
   *
   * std::priority_queue is a max-heap by default.
   *
   * Returning:
   *
   *     a.key > b.key
   *
   * reverses the ordering so that the lexicographically smallest
   * key appears at heap.top().
   *
   * This gives the Compactor a min-heap over the current iterator keys.
   */
  struct HeapCompare {
    bool operator()(const HeapItem &a, const HeapItem &b) const {
      return a.key > b.key;
    }
  };
};

} // namespace kronos