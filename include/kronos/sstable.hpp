// #pragma once

// #include "kronos/bloom_filter.hpp"
// #include "kronos/types.hpp"

// #include <cstdint>
// #include <filesystem>
// #include <optional>
// #include <string>
// #include <vector>
// // SparseIndexEntry is shared by both SstableBuilder and SstableReader.
// /*
// SSTable format:

// [ HEADER ]

// [ DATA BLOCK ]
// [ DATA BLOCK ]
// ...
// [ DATA BLOCK ]

// [ BLOOM FILTER ]

// [ SPARSE INDEX ]

// [ FOOTER ]
// */
// namespace kronos {

// struct SparseIndexEntry {
//   std::string firstKey;

//   uint64_t block_offset;
//   uint32_t block_size;
// };

// class SstableBuilder {

// public:
//   explicit SstableBuilder(const std::filesystem::path &path, size_t
//   blockSize,
//                           size_t bitsPerKey);

//   ~SstableBuilder();

//   using Entry = InternalEntry;

//   void add(std::string key, InternalEntry e);

//   void finish();

// private:
//   std::filesystem::path sstable_path_;

//   size_t target_blockSize_;

//   size_t currentBlock_recordCount_ = 0;

//   std::string currentBlock_firstKey_;

//   std::vector<uint8_t> current_block_;

//   std::vector<SparseIndexEntry> sparse_index_;
//   size_t bits_per_key_;
//   std::vector<std::string> bloom_keys_;

//   int fd_ = -1;

//   // Writes magic + format version.
//   void writeHeader();

//   void flushCurrentBlock();

//   // Serializes the Bloom filter and writes:
//   //
//   // [bit_count]
//   // [probe_count]
//   // [packed bits]
//   // [CRC32]
//   void writeBloomFilter();

//   // Serializes sparse_index_ and writes:
//   //
//   // [entry_count]
//   // [index entries]
//   // [CRC32]
//   void writeSparseIndex();

//   // Writes:
//   //
//   // [bloom_offset]
//   // [bloom_size]
//   // [index_offset]
//   // [index_size]
//   // [CRC32]
//   void writeFooter(uint64_t bloom_offset, uint64_t bloom_size,
//                    uint64_t index_offset, uint64_t index_size);

//   // Bloom filter belonging to this SSTable.
//   std::optional<bloom_filter> bloom_filter_;
//   std::optional<std::string> last_key_;
// };

// class SstableReader {

// public:
//   class Iterator {

//   public:
//     bool valid() const;

//     using Entry = InternalEntry;

//     void next();

//     const Entry &getEntry() const;

//     const std::string &getKey() const;

//     explicit Iterator(const SstableReader *reader);

//   private:
//     const SstableReader *reader_; // Which SSTable?

//     size_t current_block_index_; // Which block?

//     std::vector<uint8_t> block_bytes_; // Current block loaded into RAM.

//     size_t cursor_; // Current position inside block.

//     std::string current_key_;

//     Entry current_entry_;

//     uint32_t record_count_;

//     uint32_t current_record_index_;

//     bool valid_;

//     void loadBlock(size_t block_index);

//     void parseCurrentRecord();
//   };

//   explicit SstableReader(const std::filesystem::path &path);

//   ~SstableReader();

//   Iterator getIterator() const;

//   using GetResult = kronos::GetResult;

//   GetResult get(const std::string &key) const;

// private:
//   int fd_ = -1;

//   std::vector<SparseIndexEntry> sparse_index_;

//   // Empty until the Bloom block has been loaded from disk.
//   std::optional<bloom_filter> bloom_filter_;

//   void readHeader();

//   struct FooterInfo {
//     uint64_t bloom_offset;
//     uint64_t bloom_size;

//     uint64_t index_offset;
//     uint64_t index_size;
//   };

//   FooterInfo readFooter();

//   // Reads:
//   //
//   // [bit_count]
//   // [probe_count]
//   // [packed bits]
//   // [CRC32]
//   //
//   // verifies the CRC and restores bloom_filter_.
//   void loadBloomFilter(uint64_t bloom_offset, uint64_t bloom_size);

//   void loadSparseIndex(uint64_t index_offset, uint64_t index_size);
// };

// } // namespace kronos

#pragma once

#include "kronos/bloom_filter.hpp"
#include "kronos/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
// SparseIndexEntry is shared by both SstableBuilder and SstableReader.
/*
SSTable format:

[ HEADER ]

[ DATA BLOCK ]
[ DATA BLOCK ]
...
[ DATA BLOCK ]

[ BLOOM FILTER ]

[ SPARSE INDEX ]

[ FOOTER ]
*/
namespace kronos {

struct SparseIndexEntry {
  std::string firstKey;

  uint64_t block_offset;
  uint32_t block_size;
};

class SstableBuilder {

public:
  explicit SstableBuilder(const std::filesystem::path &path, size_t blockSize,
                          size_t bitsPerKey);

  ~SstableBuilder();

  using Entry = InternalEntry;

  void add(std::string key, InternalEntry e);

  void finish();

private:
  std::filesystem::path sstable_path_;

  size_t target_blockSize_;

  size_t currentBlock_recordCount_ = 0;

  std::string currentBlock_firstKey_;

  std::vector<uint8_t> current_block_;

  std::vector<SparseIndexEntry> sparse_index_;
  size_t bits_per_key_;
  std::vector<std::string> bloom_keys_;

  int fd_ = -1;

  // Writes magic + format version.
  void writeHeader();

  void flushCurrentBlock();

  // Serializes the Bloom filter and writes:
  //
  // [bit_count]
  // [probe_count]
  // [packed bits]
  // [CRC32]
  void writeBloomFilter();

  // Serializes sparse_index_ and writes:
  //
  // [entry_count]
  // [index entries]
  // [CRC32]
  void writeSparseIndex();

  // Writes:
  //
  // [bloom_offset]
  // [bloom_size]
  // [index_offset]
  // [index_size]
  // [CRC32]
  void writeFooter(uint64_t bloom_offset, uint64_t bloom_size,
                   uint64_t index_offset, uint64_t index_size);

  // Bloom filter belonging to this SSTable.
  std::optional<bloom_filter> bloom_filter_;
  std::optional<std::string> last_key_;
};

class SstableReader {

public:
  class Iterator {

  public:
    bool valid() const;

    using Entry = InternalEntry;

    void next();

    const Entry &getEntry() const;

    const std::string &getKey() const;

    explicit Iterator(const SstableReader *reader);

  private:
    const SstableReader *reader_; // Which SSTable?

    size_t current_block_index_; // Which block?

    std::vector<uint8_t> block_bytes_; // Current block loaded into RAM.

    size_t cursor_; // Current position inside block.

    std::string current_key_;

    Entry current_entry_;

    uint32_t record_count_;

    uint32_t current_record_index_;

    bool valid_;

    void loadBlock(size_t block_index);

    void parseCurrentRecord();
  };

  explicit SstableReader(const std::filesystem::path &path);

  ~SstableReader();

  Iterator getIterator() const;

  using GetResult = kronos::GetResult;

  GetResult get(const std::string &key) const;

  // Internal engine lookup that preserves sequence number and operation.
  // Public get() remains the simpler FOUND/DELETED/NOT_FOUND API.
  std::optional<InternalEntry> lookupEntry(const std::string &key) const;

private:
  int fd_ = -1;

  std::vector<SparseIndexEntry> sparse_index_;

  // Empty until the Bloom block has been loaded from disk.
  std::optional<bloom_filter> bloom_filter_;

  void readHeader();

  struct FooterInfo {
    uint64_t bloom_offset;
    uint64_t bloom_size;

    uint64_t index_offset;
    uint64_t index_size;
  };

  FooterInfo readFooter();

  // Reads:
  //
  // [bit_count]
  // [probe_count]
  // [packed bits]
  // [CRC32]
  //
  // verifies the CRC and restores bloom_filter_.
  void loadBloomFilter(uint64_t bloom_offset, uint64_t bloom_size);

  void loadSparseIndex(uint64_t index_offset, uint64_t index_size);
};

} // namespace kronos