#pragma once
#include "kronos/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
/* SSTable format will be:
[ HEADER ]
[ DATA BLOCK ]
[ DATA BLOCK ]
 ...
 ...
[ DATA BLOCK ]
[ SPARSE INDEX ]
[ FOOTER ]
 */
namespace kronos {

// SparseIndexEntry will be shared by both SstableBuilder & SstableReader
struct SparseIndexEntry {
  std::string firstKey;
  uint64_t block_offset;
  uint32_t block_size;
};
class SstableBuilder {

public:
  explicit SstableBuilder(const std::filesystem::path &path, size_t blockSize);
  ~SstableBuilder();

  using Entry = InternalEntry;
  void add(std::string key, InternalEntry e);
  void finish();

private:
  //   const char sstable_magic_[4] = {'K', 'S', 'S', 'T'};
  std::filesystem::path sstable_path_;
  size_t target_blockSize_;
  size_t currentBlock_recordCount_ = 0;
  std::string currentBlock_firstKey_;
  std::vector<uint8_t> current_block_;

  std::vector<SparseIndexEntry> sparse_index_;
  int fd_ = -1;
  void writeHeader(); //  → writes magic + format version
  void flushCurrentBlock();
  // serializes sparse_index_ and writes (entry_count +
  // entries + CRC)
  void writeSparseIndex();
  void writeFooter(
      uint64_t index_offset,
      uint64_t index_size); // writes where the sparse index lives + footer CRC
};

class SstableReader {
public:
  explicit SstableReader(const std::filesystem::path &path);
  ~SstableReader();

  using GetResult = kronos::GetResult; // from types.hpp
  GetResult get(const std::string &key) const;

private:
  int fd_ = -1;
  std::vector<SparseIndexEntry> sparse_index_;
  void readHeader();
  struct FooterInfo {
    uint64_t index_offset;
    uint64_t index_size;
  };

  FooterInfo readFooter();
  void loadSparseIndex(uint64_t index_offset, uint64_t index_size);
};
} // namespace kronos
