#pragma once

#include "types.hpp"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace kronos {

class Compactor {
public:
  CompactionResult
  compact(const std::vector<std::filesystem::path> &input_files,
          const std::filesystem::path &output_path, size_t block_size,
          size_t bits_per_key);

private:
  struct HeapItem {
    std::string key;
    size_t iterator_index;
  };

  struct HeapCompare {
    bool operator()(const HeapItem &a, const HeapItem &b) const {
      return a.key > b.key;
    }
  };
};

} // namespace kronos