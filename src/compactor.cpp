#include "kronos/compactor.hpp"

#include "kronos/sstable.hpp"

#include <memory>
#include <queue>
#include <vector>

namespace kronos {

CompactionResult
Compactor::compact(const std::vector<std::filesystem::path> &input_files,
                   const std::filesystem::path &output_path, size_t block_size,
                   size_t bits_per_key) {

  std::vector<std::unique_ptr<SstableReader>> readers;
  std::vector<SstableReader::Iterator> iterators;

  std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCompare> heap;

  // Initialize Readers and Iterators
  for (size_t i = 0; i < input_files.size(); ++i) {

    auto reader = std::make_unique<SstableReader>(input_files[i]);

    readers.push_back(std::move(reader));

    auto iterator = readers.back()->getIterator();

    iterators.push_back(std::move(iterator));

    if (iterators.back().valid()) {
      heap.push({iterators.back().getKey(), iterators.size() - 1});
    }
  }

  // One output SSTable for this entire compaction.
  SstableBuilder builder(output_path, block_size, bits_per_key);

  while (!heap.empty()) {

    std::string current_key = heap.top().key;

    std::vector<size_t> same_key_iterators;

    while (!heap.empty() && heap.top().key == current_key) {
      same_key_iterators.push_back(heap.top().iterator_index);
      heap.pop();
    }

    // Assume the first version is the winner initially.
    size_t winner_index = same_key_iterators[0];

    // Find the newest version.
    for (size_t index : same_key_iterators) {

      const auto &candidate = iterators[index].getEntry();
      const auto &winner = iterators[winner_index].getEntry();

      if (candidate.sequence > winner.sequence) {
        winner_index = index;
      }
    }

    const auto &winner_entry = iterators[winner_index].getEntry();

    // Write the newest version.
    builder.add(current_key, winner_entry);

    // Advance every iterator that contributed this key.
    for (size_t index : same_key_iterators) {

      iterators[index].next();

      if (iterators[index].valid()) {
        heap.push({iterators[index].getKey(), index});
      }
    }
  }

  builder.finish();

  return {input_files, {output_path}};
}
} // namespace kronos