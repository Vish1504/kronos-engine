#include "kronos/compactor.hpp"
#include "kronos/sstable.hpp"

#include <filesystem>
#include <memory>
#include <queue>
#include <stdexcept>
#include <vector>

namespace kronos {

/*
 * Perform a K-way merge over multiple immutable SSTables.
 *
 * The Compactor assumes that higher-level compaction policy has already:
 *
 *   - selected the correct input SSTables,
 *   - chosen where the resulting SSTable belongs,
 *   - determined whether tombstones are safe to discard.
 *
 * The Compactor itself only performs the physical merge.
 *
 * For every logical key:
 *
 *   1. Find every SSTable currently exposing that key.
 *   2. Choose the entry with the highest sequence number.
 *   3. Preserve the winner, unless it is a safely-droppable tombstone.
 *   4. Advance every iterator that contributed that key.
 *
 * Because every input SSTable is sorted, a min-heap allows us to efficiently
 * find the smallest key currently visible across all SSTables.
 */
CompactionResult
Compactor::compact(const std::vector<std::filesystem::path> &input_files,
                   const std::filesystem::path &output_path, size_t block_size,
                   size_t bits_per_key, bool can_drop_tombstones) {

  // -------------------------------------------------------------------------
  // Validate compaction request
  // -------------------------------------------------------------------------

  // A compaction without any input SSTables is meaningless.
  if (input_files.empty()) {
    throw std::invalid_argument(
        "Compaction requires at least one input SSTable");
  }

  // Never silently overwrite an already-existing SSTable.
  if (std::filesystem::exists(output_path)) {
    throw std::invalid_argument("Compaction output path already exists");
  }

  // The destination must also never be one of the source SSTables.
  for (const auto &input : input_files) {
    if (input == output_path) {
      throw std::invalid_argument(
          "Compaction output cannot overwrite an input SSTable");
    }
  }

  // -------------------------------------------------------------------------
  // Readers and Iterators
  // -------------------------------------------------------------------------

  /*
   * We need one SstableReader for every input SSTable.
   *
   * Each SstableReader::Iterator stores a pointer back to its Reader.
   * Therefore every Reader must:
   *
   *   - remain alive throughout the entire compaction,
   *   - remain at a stable memory address.
   *
   * vector<SstableReader> would be dangerous because vector reallocation
   * could move the Reader objects and invalidate the pointers stored by
   * existing Iterators.
   *
   * vector<unique_ptr<SstableReader>> avoids this:
   *
   *   vector owns the unique_ptr objects,
   *   unique_ptr owns the Readers,
   *   the Readers themselves remain at stable addresses.
   */
  std::vector<std::unique_ptr<SstableReader>> readers;

  // We already know how many Readers and Iterators we need, so reserving
  // avoids unnecessary vector reallocations.
  readers.reserve(input_files.size());

  /*
   * One Iterator per input SSTable.
   *
   * Each iterator exposes the current key/entry from its corresponding table.
   */
  std::vector<SstableReader::Iterator> iterators;
  iterators.reserve(input_files.size());

  /*
   * Min-heap containing the current key from every active iterator.
   *
   * Each HeapItem stores:
   *
   *   key            -> what key is currently visible
   *   iterator_index -> which iterator/SSTable produced it
   *
   * HeapCompare causes the lexicographically smallest key to appear
   * at heap.top().
   */
  std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCompare> heap;

  // -------------------------------------------------------------------------
  // Initialize Readers, Iterators and Heap
  // -------------------------------------------------------------------------

  /*
   * For every input SSTable:
   *
   *   1. Create the Reader.
   *   2. Store the Reader so its lifetime is secured.
   *   3. Create an Iterator from that stored Reader.
   *   4. Store the Iterator.
   *   5. If it contains data, add its first key to the heap.
   *
   * The ordering here matters:
   *
   *     own the Reader first,
   *     create the borrowing Iterator second.
   */
  for (size_t i = 0; i < input_files.size(); ++i) {

    auto reader = std::make_unique<SstableReader>(input_files[i]);

    readers.push_back(std::move(reader));

    auto iterator = readers.back()->getIterator();

    iterators.push_back(std::move(iterator));

    if (iterators.back().valid()) {
      heap.push({iterators.back().getKey(), iterators.size() - 1});
    }
  }

  // -------------------------------------------------------------------------
  // Lazy output SSTable
  // -------------------------------------------------------------------------

  /*
   * Do not create the output SSTable immediately.
   *
   * A valid compaction can produce zero records.
   *
   * Example:
   *
   *   A -> DELETE
   *   B -> DELETE
   *
   * If both tombstones are safe to discard, nothing survives.
   *
   * Chronos deliberately does not create empty SSTables, so the builder is
   * created only when the first record actually needs to be written.
   */
  std::unique_ptr<SstableBuilder> builder;

  // -------------------------------------------------------------------------
  // K-way merge
  // -------------------------------------------------------------------------

  /*
   * SSTable invariant:
   *
   * Every input SSTable contains keys in strictly increasing order and
   * contains no duplicate key inside the same SSTable.
   *
   * Therefore, once an iterator contributes current_key and is advanced,
   * that same iterator cannot expose current_key again.
   */
  while (!heap.empty()) {

    // The heap always exposes the smallest current key across all SSTables.
    const std::string current_key = heap.top().key;

    /*
     * More than one SSTable may currently expose the same logical key.
     *
     * Example:
     *
     *   Iterator 0 -> A seq 10
     *   Iterator 1 -> C seq 15
     *   Iterator 2 -> A seq 20
     *
     * Before writing A, we must collect every current version of A so that
     * the newest sequence number can be selected.
     */
    std::vector<size_t> same_key_iterators;

    /*
     * Remove every heap item whose key equals current_key.
     *
     * We store the iterator indices because the actual InternalEntry remains
     * inside the Iterator.
     */
    while (!heap.empty() && heap.top().key == current_key) {

      same_key_iterators.push_back(heap.top().iterator_index);

      heap.pop();
    }

    // -----------------------------------------------------------------------
    // Resolve multiple versions of the same key
    // -----------------------------------------------------------------------

    /*
     * Start by assuming the first version is the winner.
     *
     * We then compare sequence numbers from all SSTables exposing this key.
     */
    size_t winner_index = same_key_iterators[0];

    /*
     * Sequence numbers represent logical recency.
     *
     * The largest sequence number is the newest version and therefore wins,
     * regardless of whether the operation is PUT or DELETE.
     *
     * DELETE does not automatically outrank PUT.
     */
    for (size_t index : same_key_iterators) {

      const auto &candidate = iterators[index].getEntry();

      const auto &winner = iterators[winner_index].getEntry();

      if (candidate.sequence > winner.sequence) {
        winner_index = index;
      }
    }

    const auto &winner_entry = iterators[winner_index].getEntry();

    // -----------------------------------------------------------------------
    // Tombstone handling
    // -----------------------------------------------------------------------

    /*
     * PUT winners are always written.
     *
     * DELETE winners normally remain as tombstones because an older PUT for
     * this key may still exist in another SSTable outside this compaction.
     *
     * Tombstones are dropped only when higher-level policy has already proven
     * that no older version can later become visible.
     */
    bool should_write = true;

    if (winner_entry.operation == OperationType::DELETE &&
        can_drop_tombstones) {

      should_write = false;
    }

    // -----------------------------------------------------------------------
    // Write surviving version
    // -----------------------------------------------------------------------

    if (should_write) {

      /*
       * Create the output SSTable only when the first surviving record appears.
       *
       * This avoids creating an empty SSTable when every winner is a
       * safely-droppable tombstone.
       */
      if (!builder) {
        builder = std::make_unique<SstableBuilder>(output_path, block_size,
                                                   bits_per_key);
      }

      /*
       * Because current_key is processed in heap order, output keys are
       * generated in strictly increasing order.
       *
       * This satisfies the SstableBuilder ordering invariant.
       */
      builder->add(current_key, winner_entry);
    }

    // -----------------------------------------------------------------------
    // Advance every iterator that contributed current_key
    // -----------------------------------------------------------------------

    /*
     * We must advance ALL iterators that contained current_key,
     * not just the iterator containing the winning version.
     *
     * Once the logical value of current_key has been resolved, every current
     * version of that key has been consumed.
     */
    for (size_t index : same_key_iterators) {

      iterators[index].next();

      /*
       * If that SSTable still contains another record, expose its new current
       * key to the heap so it can participate in the next merge decision.
       */
      if (iterators[index].valid()) {
        heap.push({iterators[index].getKey(), index});
      }
    }
  }

  // -------------------------------------------------------------------------
  // Finalize compaction result
  // -------------------------------------------------------------------------

  /*
   * The Compactor reports both:
   *
   *   input_files  -> SSTables consumed by this compaction
   *   output_files -> SSTables actually produced
   *
   * output_files may legitimately be empty.
   */
  CompactionResult result{input_files, {}};

  /*
   * If the builder was never created, every surviving entry was discarded
   * safely and there is deliberately no replacement SSTable.
   *
   * Otherwise finalize the SSTable and report it as an output.
   */
  if (builder) {

    builder->finish();

    result.output_files.push_back(output_path);
  }

  return result;
}

} // namespace kronos