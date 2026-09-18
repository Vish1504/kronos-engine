#include <cstddef>
#include <kronos/sstable.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <zlib.h>

#include "kronos/bloom_filter.hpp"

/*
 * SSTABLE ON-DISK FORMAT
 *
 * [ HEADER ]
 * [ DATA BLOCK 1 ]
 * [ DATA BLOCK 2 ]
 * ...
 * [ SPARSE INDEX ]
 * [ FOOTER ]
 *
 * Writer: C++ objects -> serialized bytes -> disk
 * Reader: disk -> serialized bytes -> C++ objects
 *
 * All multi-byte integers are stored in little-endian format.
 *
 * A data block:
 * [record_count][records...][CRC32]
 *
 * Sparse index:
 * [entry_count][entries...][CRC32]
 *
 * Footer:
 * [index_offset][index_size][CRC32]
 */

constexpr char sstable_magic_[4] = {'K', 'S', 'S', 'T'};
constexpr uint32_t version = 1;

// Convert an unsigned integer into little-endian bytes for disk.
template <typename T> void encode_to_le(uint8_t arr[], T value) {
  static_assert(std::is_unsigned_v<T>);
  for (size_t i = 0; i < sizeof(T); i++) {
    arr[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
}

/*
POSIX write() may write fewer bytes than requested.
 Keep writing until the entire buffer reaches disk.
 */
void write_all(int fd, const void *data, size_t size) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  size_t written = 0;

  while (written < size) {
    ssize_t result = ::write(fd, bytes + written, size - written);

    if (result <= 0) {
      throw std::runtime_error("Failed to write SSTable data");
    }

    written += static_cast<size_t>(result);
  }
}

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

// Build into a .tmp file so an incomplete SSTable is never mistaken for a
// successfully finalized table.
kronos::SstableBuilder::SstableBuilder(const std::filesystem::path &path,
                                       size_t blockSize, size_t bitsPerKey)
    : sstable_path_(path), target_blockSize_(blockSize),
      bits_per_key_(bitsPerKey) {

  sstable_path_ = path;
  target_blockSize_ = blockSize;
  // Create a copy of the path and change .sst to .tmp
  auto temp_path = path;
  temp_path.replace_extension(".tmp");

  // now we open a file with the .tmp extension
  fd_ = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ == -1) {
    throw std::runtime_error(
        "Kronos Error: Failed to create temp SSTable file: " +
        std::string(std::strerror(errno)));
  }

  //   if writeHeader() fails, construction itself fails, and C++ will not call
  //   the destructor for an object that never finished constructing.
  try {
    writeHeader();
  } catch (...) {
    ::close(fd_);
    fd_ = -1;
    throw;
  }
}

kronos::SstableBuilder::~SstableBuilder() {
  if (fd_ != -1) {
    ::close(fd_);
  }
}

// Identify the file as a Kronos SSTable and record its format version.
void kronos::SstableBuilder::writeHeader() {
  write_all(fd_, sstable_magic_, sizeof(sstable_magic_));

  //   uint32_t version = 1;

  uint8_t version_bytes[4];
  encode_to_le(version_bytes, version);

  write_all(fd_, version_bytes, sizeof(version_bytes));
}

// Serialize one logical Memtable entry into the SSTable record format.
// [sequence:8][operation:1][key_len:4][value_len:4][key][value]
void kronos::SstableBuilder::add(std::string key, InternalEntry e) {
  bloom_keys_.push_back(key);
  uint8_t sequence_bytes[8];
  encode_to_le(sequence_bytes, e.sequence);

  // we cast it so the compiler treats it as a raw byte
  uint8_t operation_byte = static_cast<uint8_t>(e.operation);

  // key length - 64 bit to 32 bit
  uint32_t keyLength = static_cast<uint32_t>(key.size());
  // value length - 64 bit to 32 bit
  uint32_t valueLength = static_cast<uint32_t>(e.value.size());

  // Temporary RAM representation of exactly one serialized record.
  std::vector<uint8_t> record_bytes;

  uint8_t key_length_bytes[4];
  uint8_t value_length_bytes[4];

  encode_to_le(key_length_bytes, keyLength);
  encode_to_le(value_length_bytes, valueLength);

  record_bytes.insert(record_bytes.end(), sequence_bytes,
                      sequence_bytes + sizeof(e.sequence));
  record_bytes.push_back(operation_byte);
  record_bytes.insert(record_bytes.end(), key_length_bytes,
                      key_length_bytes + sizeof(keyLength));
  record_bytes.insert(record_bytes.end(), value_length_bytes,
                      value_length_bytes + sizeof(valueLength));
  for (char c : key) {
    record_bytes.push_back(static_cast<uint8_t>(c));
  }

  for (char c : e.value) {
    record_bytes.push_back(static_cast<uint8_t>(c));
  }

  if (current_block_.size() != 0 && sizeof(uint32_t) + current_block_.size() +
                                            record_bytes.size() +
                                            sizeof(uint32_t) >
                                        target_blockSize_) {

    flushCurrentBlock();
  }
  if (current_block_.size() == 0) {
    currentBlock_firstKey_ = key;
  }
  current_block_.insert(current_block_.end(), record_bytes.begin(),
                        record_bytes.end());
  currentBlock_recordCount_++;
}

// Finalize the current RAM block and append it to the SSTable file.
void kronos::SstableBuilder::flushCurrentBlock() {
  if (current_block_.size() == 0) {
    return;
  }

  // Get current position
  off_t current_pos = lseek(fd_, 0, SEEK_CUR);
  if (current_pos == (off_t)-1) {
    throw std::runtime_error("Error getting position");
  }
  uint64_t block_offset = static_cast<uint64_t>(current_pos);

  uint32_t currentBlock_recordCount_32 =
      static_cast<uint32_t>(currentBlock_recordCount_);
  uint8_t currentBlock_recordCount_bytes[4];
  encode_to_le(currentBlock_recordCount_bytes, currentBlock_recordCount_32);

  // Now we calculate CRC32 over [record_count][records]
  // Initialize the CRC register
  uLong crc = crc32(0L, Z_NULL, 0);

  // Update CRC with the block's record count.
  crc = crc32(crc,
              reinterpret_cast<const Bytef *>(currentBlock_recordCount_bytes),
              sizeof(currentBlock_recordCount_bytes));

  // Update CRC with all serialized records in the block.
  crc = crc32(crc, reinterpret_cast<const Bytef *>(current_block_.data()),
              static_cast<uInt>(current_block_.size()));

  // Converting checksum to uint32_t as zlib returns the checksum as uLong.
  uint32_t checksum_value = static_cast<uint32_t>(crc);
  uint8_t checksum_bytes[4];
  encode_to_le(checksum_bytes, checksum_value);
  /* Final layout for a block:
    [record_count - 4 bytes][serialized records][CRC32 - 4 bytes]
       */
  write_all(fd_, currentBlock_recordCount_bytes,
            sizeof(currentBlock_recordCount_bytes));

  write_all(fd_, current_block_.data(), current_block_.size());

  write_all(fd_, checksum_bytes, sizeof(checksum_bytes));

  uint32_t block_size =
      static_cast<uint32_t>(sizeof(currentBlock_recordCount_bytes) +
                            current_block_.size() + sizeof(checksum_bytes));

  // Record how the Reader can later locate this block without scanning the
  // file.
  SparseIndexEntry s;
  s.block_size = block_size;
  s.block_offset = block_offset;
  s.firstKey = currentBlock_firstKey_;

  sparse_index_.push_back(s);

  // Old block is safely on disk and indexed.
  // Now we reuse the buffer for the next block.
  current_block_.clear();
  currentBlock_recordCount_ = 0;
  currentBlock_firstKey_.clear();
}

// Serialize the in-memory sparse index after all data blocks are written.
//
// Each entry:
// [first_key_len][first_key][block_offset][block_size]
void kronos::SstableBuilder::writeBloomFilter() {
  std::vector<uint8_t> bloom_block;

  uint32_t bit_count = static_cast<uint32_t>(bloom_filter_->bitCount());

  uint8_t bit_count_bytes[4];
  encode_to_le(bit_count_bytes, bit_count);
  bloom_block.insert(bloom_block.end(), bit_count_bytes, bit_count_bytes + 4);

  uint32_t probe_count = static_cast<uint32_t>(bloom_filter_->probeCount());
  uint8_t probe_count_bytes[4];
  encode_to_le(probe_count_bytes, probe_count);
  bloom_block.insert(bloom_block.end(), probe_count_bytes,
                     probe_count_bytes + 4);

  // append the Bloom filter's packed bits
  const auto &bits = bloom_filter_->bits();
  bloom_block.insert(bloom_block.end(), bits.begin(), bits.end());

  // Now we calculate CRC32 over [entry_count][all index entries]
  // Initialize the CRC register
  uLong crc = crc32(0L, Z_NULL, 0);

  // Update CRC with the all index_bytes.
  crc = crc32(crc, reinterpret_cast<const Bytef *>(bloom_block.data()),
              static_cast<uInt>(bloom_block.size()));

  // Converting checksum to uint32_t as zlib returns the checksum as uLong.
  uint32_t bloom_block_checksum_value = static_cast<uint32_t>(crc);
  uint8_t bloom_block_checksum_bytes[4];
  encode_to_le(bloom_block_checksum_bytes, bloom_block_checksum_value);

  /* Final sparse index layout:
     [entry_count][all index entries][CRC32]
  */
  bloom_block.insert(bloom_block.end(), bloom_block_checksum_bytes,
                     bloom_block_checksum_bytes +
                         sizeof(bloom_block_checksum_bytes));

  // write to .tmp file
  write_all(fd_, bloom_block.data(), bloom_block.size());
}

// Serialize the in-memory sparse index after all data blocks are written.
//
// Each entry:
// [first_key_len][first_key][block_offset][block_size]
void kronos::SstableBuilder::writeSparseIndex() {

  uint32_t entry_count = static_cast<uint32_t>(sparse_index_.size());
  uint8_t entry_count_bytes[4];
  encode_to_le(entry_count_bytes, entry_count);

  // Build the complete serialized index in RAM so we can checksum it once.
  std::vector<uint8_t> index_bytes;
  index_bytes.insert(index_bytes.end(), entry_count_bytes,
                     entry_count_bytes + sizeof(entry_count_bytes));

  // Convert each SparseIndexEntry object into its disk representation.
  for (size_t i = 0; i < sparse_index_.size(); i++) {
    // first key length
    uint32_t firstKey_length =
        static_cast<uint32_t>(sparse_index_[i].firstKey.size());
    uint8_t firstKey_length_bytes[4];
    encode_to_le(firstKey_length_bytes, firstKey_length);
    index_bytes.insert(index_bytes.end(), firstKey_length_bytes,
                       firstKey_length_bytes + sizeof(firstKey_length_bytes));

    // first key
    for (char c : sparse_index_[i].firstKey) {
      index_bytes.push_back(static_cast<uint8_t>(c));
    }

    // block_offset
    uint64_t block_offset = sparse_index_[i].block_offset;
    uint8_t block_offset_bytes[8];
    encode_to_le(block_offset_bytes, block_offset);
    index_bytes.insert(index_bytes.end(), block_offset_bytes,
                       block_offset_bytes + sizeof(block_offset_bytes));

    // block_size
    uint32_t block_size = sparse_index_[i].block_size;
    uint8_t block_size_bytes[4];
    encode_to_le(block_size_bytes, block_size);
    index_bytes.insert(index_bytes.end(), block_size_bytes,
                       block_size_bytes + sizeof(block_size_bytes));
  }

  // Now we calculate CRC32 over [entry_count][all index entries]
  // Initialize the CRC register
  uLong crc = crc32(0L, Z_NULL, 0);

  // Update CRC with the all index_bytes.
  crc = crc32(crc, reinterpret_cast<const Bytef *>(index_bytes.data()),
              static_cast<uInt>(index_bytes.size()));

  // Converting checksum to uint32_t as zlib returns the checksum as uLong.
  uint32_t index_checksum_value = static_cast<uint32_t>(crc);
  uint8_t index_checksum_bytes[4];
  encode_to_le(index_checksum_bytes, index_checksum_value);

  /* Final sparse index layout:
     [entry_count][all index entries][CRC32]
  */
  index_bytes.insert(index_bytes.end(), index_checksum_bytes,
                     index_checksum_bytes + sizeof(index_checksum_bytes));

  // write to .tmp file
  write_all(fd_, index_bytes.data(), index_bytes.size());
}

void kronos::SstableBuilder::writeFooter(uint64_t bloom_offset,
                                         uint64_t bloom_size,
                                         uint64_t index_offset,
                                         uint64_t index_size) {
  uint8_t bloom_offset_bytes[8];
  encode_to_le(bloom_offset_bytes, bloom_offset);

  uint8_t bloom_size_bytes[8];
  encode_to_le(bloom_size_bytes, bloom_size);

  uint8_t index_offset_bytes[8];
  encode_to_le(index_offset_bytes, index_offset);

  uint8_t index_size_bytes[8];
  encode_to_le(index_size_bytes, index_size);

  // Now we calculate CRC32 over
  // [bloom_offset][bloom_size][index_offset][index_size] Initialize the CRC
  // register
  uLong crc = crc32(0L, Z_NULL, 0);

  // Update CRC with the all bloom_offset_bytes.
  crc = crc32(crc, reinterpret_cast<const Bytef *>(bloom_offset_bytes),
              sizeof(bloom_offset_bytes));

  // Update CRC with the all bloom_size_bytes.
  crc = crc32(crc, reinterpret_cast<const Bytef *>(bloom_size_bytes),
              sizeof(bloom_size_bytes));
  // Update CRC with the all index_offset_bytes.
  crc = crc32(crc, reinterpret_cast<const Bytef *>(index_offset_bytes),
              sizeof(index_offset_bytes));

  // Update CRC with the all index_size_bytes.
  crc = crc32(crc, reinterpret_cast<const Bytef *>(index_size_bytes),
              sizeof(index_size_bytes));

  // Converting checksum to uint32_t as zlib returns the checksum as uLong.
  uint32_t footer_checksum = static_cast<uint32_t>(crc);
  uint8_t footer_checksum_bytes[4];
  encode_to_le(footer_checksum_bytes, footer_checksum);

  write_all(fd_, bloom_offset_bytes, sizeof(bloom_offset_bytes));
  write_all(fd_, bloom_size_bytes, sizeof(bloom_size_bytes));
  write_all(fd_, index_offset_bytes, sizeof(index_offset_bytes));
  write_all(fd_, index_size_bytes, sizeof(index_size_bytes));
  write_all(fd_, footer_checksum_bytes, sizeof(footer_checksum_bytes));
}

// Finalize the SSTable in strict order:
// final block -> bloom filter -> sparse index -> footer -> fsync -> rename .tmp
// to .sst
// Finalize the SSTable in strict order:
// final block -> bloom filter -> sparse index -> footer -> fsync -> rename .tmp
// to .sst
void kronos::SstableBuilder::finish() {

  // add() may have left one partially filled block in RAM.
  flushCurrentBlock();
  if (bloom_keys_.empty()) {
    throw std::invalid_argument("Cannot create an empty SSTable");
  }
  // ---------------------------------------------------------
  // BLOOM FILTER
  // ---------------------------------------------------------
  bloom_filter_.emplace(bits_per_key_, bloom_keys_.size());
  for (size_t i = 0; i < bloom_keys_.size(); i++) {
    bloom_filter_->add(bloom_keys_[i]);
  }
  // Current file position is where the Bloom filter will begin.
  off_t bloom_offset_pos = ::lseek(fd_, 0, SEEK_CUR);

  if (bloom_offset_pos == -1) {
    throw std::runtime_error("Unable to get Bloom filter offset position");
  }

  uint64_t bloom_offset = static_cast<uint64_t>(bloom_offset_pos);

  writeBloomFilter();

  // ---------------------------------------------------------
  // SPARSE INDEX
  // ---------------------------------------------------------

  // Bloom filter has finished.
  // The current position is now where the sparse index begins.
  off_t index_offset_pos = ::lseek(fd_, 0, SEEK_CUR);

  if (index_offset_pos == -1) {
    throw std::runtime_error("Unable to get index offset position");
  }

  uint64_t index_offset = static_cast<uint64_t>(index_offset_pos);

  // Since the index immediately follows the Bloom filter:
  // bloom_size = index start - bloom start.
  uint64_t bloom_size = index_offset - bloom_offset;

  writeSparseIndex();

  // Current position is now the end of the sparse index.
  off_t index_end_pos = ::lseek(fd_, 0, SEEK_CUR);

  if (index_end_pos == -1) {
    throw std::runtime_error("Unable to get index end position");
  }

  uint64_t index_end = static_cast<uint64_t>(index_end_pos);

  uint64_t index_size = index_end - index_offset;

  // ---------------------------------------------------------
  // FOOTER
  // ---------------------------------------------------------

  // TEMPORARILY leave this as-is.
  // We'll change writeFooter() next so it also receives
  // bloom_offset and bloom_size.
  writeFooter(bloom_offset, bloom_size, index_offset, index_size);

  // ---------------------------------------------------------
  // DURABILITY + ATOMIC FINALIZATION
  // ---------------------------------------------------------

  if (::fsync(fd_) == -1) {
    throw std::runtime_error("Failed to fsync SSTable");
  }

  ::close(fd_);
  fd_ = -1;

  auto temp_path = sstable_path_;
  temp_path.replace_extension(".tmp");

  // Atomic finalization boundary:
  // only the completed file receives the .sst extension.
  if (std::rename(temp_path.c_str(), sstable_path_.c_str()) != 0) {
    throw std::runtime_error(
        "Unable to rename file extension from .tmp to .sst");
  }
}

/*
 * SSTABLE READER
 *
 * Opening a table:
 * header -> footer -> sparse index
 *
 * Looking up a key:
 * sparse index in RAM
 * -> candidate data block on disk
 * -> block in RAM
 * -> exact record
 */

// Reader-side mirror of write_all(): keep reading until the buffer is full.
void read_exact(int fd, void *data, size_t size) {
  uint8_t *bytes = static_cast<uint8_t *>(data);

  size_t readQty = 0;

  while (readQty < size) {
    ssize_t result = ::read(fd, bytes + readQty, size - readQty);

    if (result <= 0) {
      throw std::runtime_error("Failed to read SSTable data");
    }

    readQty += static_cast<size_t>(result);
  }
}

// Reconstruct an unsigned integer from its little-endian disk representation.
template <typename T> T decode_from_le(const uint8_t arr[]) {
  static_assert(std::is_unsigned_v<T>);

  T value = 0;

  for (size_t i = 0; i < sizeof(T); i++) {
    value |= static_cast<T>(arr[i]) << (8 * i);
  }

  return value;
}
void kronos::SstableReader::readHeader() {
  char magic_buffer[4] = {0};
  // Read up to 4 bytes from the file descriptor
  read_exact(fd_, magic_buffer, 4);

  if (std::memcmp(magic_buffer, sstable_magic_, sizeof(sstable_magic_)) != 0) {
    throw std::runtime_error(
        "Invalid SSTable magic: file may be corrupted or not an SSTable");
  }
  uint8_t version_buffer[4] = {0};
  // Read the next 4 bytes from the file descriptor s
  read_exact(fd_, version_buffer, 4);
  uint32_t sstable_version = decode_from_le<uint32_t>(version_buffer);
  if (sstable_version != version) {
    throw std::runtime_error("Unsupported SSTable version");
  }
}

// The fixed-size footer lives at the end of the file and tells us
// where the variable-size sparse index begins.
kronos::SstableReader::FooterInfo kronos::SstableReader::readFooter() {

  // Footer layout:
  // [bloom_offset:u64]
  // [bloom_size:u64]
  // [index_offset:u64]
  // [index_size:u64]
  // [CRC32:u32]
  //
  // Total = 36 bytes

  constexpr size_t footer_size = 36;

  // Jump to the beginning of the footer.
  off_t footer_position =
      ::lseek(fd_, -static_cast<off_t>(footer_size), SEEK_END);

  if (footer_position == -1) {
    throw std::runtime_error("Unable to seek to SSTable footer");
  }

  uint8_t footer_bytes[footer_size];

  read_exact(fd_, footer_bytes, footer_size);

  // ---------------------------------------------------------
  // Decode footer fields
  // ---------------------------------------------------------

  uint64_t bloom_offset = decode_from_le<uint64_t>(footer_bytes);

  uint64_t bloom_size = decode_from_le<uint64_t>(footer_bytes + 8);

  uint64_t index_offset = decode_from_le<uint64_t>(footer_bytes + 16);

  uint64_t index_size = decode_from_le<uint64_t>(footer_bytes + 24);

  uint32_t stored_checksum = decode_from_le<uint32_t>(footer_bytes + 32);

  // ---------------------------------------------------------
  // Recalculate CRC over the metadata only.
  //
  // CRC covers:
  // [bloom_offset][bloom_size][index_offset][index_size]
  //
  // i.e. first 32 bytes.
  // ---------------------------------------------------------

  uLong crc = crc32(0L, Z_NULL, 0);

  crc = crc32(crc, reinterpret_cast<const Bytef *>(footer_bytes), 32);

  uint32_t calculated_checksum = static_cast<uint32_t>(crc);

  if (stored_checksum != calculated_checksum) {
    throw std::runtime_error("SSTable footer checksum mismatch");
  }

  return {bloom_offset, bloom_size, index_offset, index_size};
}

// loadSparseIndex(offset, size)
//           ↓
// 1. Is the size even plausible?
//           ↓
// 2. Seek to index location.
//           ↓
// 3. Read the whole serialized index into RAM.
//           ↓
// 4. Separate:
//    [payload][CRC]
//           ↓
// 5. Recalculate CRC.
//    If mismatch → corruption.
//           ↓
// 6. cursor = 0
//           ↓
// 7. Read entry_count.
//           ↓
// 8. Repeat entry_count times:
//    read key length
//    read key
//    read block offset
//    read block size
//
//           ↓
// 9. Turn each disk representation into
//    SparseIndexEntry C++ objects.
//           ↓
// 10. Ensure no unexplained bytes remain.
//           ↓
// sparse_index_ ready
void kronos::SstableReader::loadBloomFilter(uint64_t bloom_offset,
                                            uint64_t bloom_size) {

  // Bloom block layout:
  //
  // [bit_count:u32]
  // [probe_count:u32]
  // [packed bits:N]
  // [CRC32:u32]

  // Minimum possible block:
  // 4 bytes bit_count
  // 4 bytes probe_count
  // 4 bytes CRC
  // = 12 bytes
  if (bloom_size < 12) {
    throw std::runtime_error("Invalid Bloom filter block size");
  }

  // Move to the beginning of the Bloom filter block.
  if (::lseek(fd_, static_cast<off_t>(bloom_offset), SEEK_SET) == -1) {
    throw std::runtime_error("Unable to seek to Bloom filter");
  }

  // Read the complete Bloom block.
  std::vector<uint8_t> bloom_bytes(bloom_size);

  read_exact(fd_, bloom_bytes.data(), bloom_bytes.size());

  // ---------------------------------------------------------
  // Decode metadata
  // ---------------------------------------------------------

  uint32_t bit_count = decode_from_le<uint32_t>(bloom_bytes.data());

  uint32_t probe_count = decode_from_le<uint32_t>(bloom_bytes.data() + 4);

  // ---------------------------------------------------------
  // Verify CRC32
  // ---------------------------------------------------------

  // Last 4 bytes contain the checksum written by
  // writeBloomFilter().
  size_t checksum_offset = bloom_bytes.size() - 4;

  uint32_t stored_checksum =
      decode_from_le<uint32_t>(bloom_bytes.data() + checksum_offset);

  // Calculate CRC over everything EXCEPT the stored CRC:
  //
  // [bit_count][probe_count][packed bits]
  uLong crc = crc32(0L, Z_NULL, 0);

  crc = crc32(crc, reinterpret_cast<const Bytef *>(bloom_bytes.data()),
              static_cast<uInt>(checksum_offset));

  uint32_t calculated_checksum = static_cast<uint32_t>(crc);

  if (stored_checksum != calculated_checksum) {
    throw std::runtime_error("Bloom filter checksum mismatch");
  }

  // ---------------------------------------------------------
  // Extract packed Bloom bits
  // ---------------------------------------------------------

  // First 8 bytes:
  // [bit_count][probe_count]
  //
  // Last 4 bytes:
  // [CRC32]
  //
  // Everything between them is bits_.

  auto bits_begin = bloom_bytes.begin() + 8;
  auto bits_end = bloom_bytes.end() - 4;

  std::vector<uint8_t> bits(bits_begin, bits_end);

  // ---------------------------------------------------------
  // Validate persisted state
  // ---------------------------------------------------------

  if (bit_count == 0 || probe_count == 0 || bits.empty()) {
    throw std::runtime_error("Invalid Bloom filter metadata");
  }

  // The number of packed bytes should correspond exactly to
  // ceil(bit_count / 8).
  size_t expected_byte_count = (static_cast<size_t>(bit_count) + 7) / 8;

  if (bits.size() != expected_byte_count) {
    throw std::runtime_error("Invalid Bloom filter bit count");
  }

  // ---------------------------------------------------------
  // Restore Bloom filter
  // ---------------------------------------------------------

  bloom_filter_.emplace(static_cast<size_t>(bit_count),
                        static_cast<size_t>(probe_count), std::move(bits));
}
void kronos::SstableReader::loadSparseIndex(uint64_t index_offset,
                                            uint64_t index_size) {
  // EVen an empty sparse index has 8 bytes
  if (index_size < 8) {
    throw std::runtime_error("Invalid SSTable sparse index size");
  }

  // Moves cursor to beginnning of sparse index
  if (::lseek(fd_, static_cast<off_t>(index_offset), SEEK_SET) == -1) {
    throw std::runtime_error("Failed to seek to SSTable sparse index");
  }

  // It's just a RAM buffer large enough to hold the serialized sparse index
  // exactly as it exists on disk.
  std::vector<uint8_t> index_bytes(static_cast<size_t>(index_size));

  // Now we copy the sparse index from disk into that vector.
  read_exact(fd_, index_bytes.data(), index_bytes.size());

  // This is the index (entry) without the CRC checksum value
  size_t payload_size = index_bytes.size() - sizeof(uint32_t);

  uLong crc = crc32(0L, Z_NULL, 0);

  // Calculate the checksum of the first payload_size bytes in index_bytes
  crc = crc32(crc, reinterpret_cast<const Bytef *>(index_bytes.data()),
              static_cast<uInt>(payload_size));

  // our sstable stores crc32 as 32 bit
  uint32_t calculated_crc = static_cast<uint32_t>(crc);

  // since we're on the RAM now, we cannot use ::lseek() to move the cursor
  const uint8_t *stored_crc_ptr = index_bytes.data() + payload_size;

  uint32_t stored_crc = decode_from_le<uint32_t>(stored_crc_ptr);

  if (calculated_crc != stored_crc) {
    throw std::runtime_error("Invalid SSTable sparse index CRC");
  }

  // CRC passed. It is now safe to interpret lengths and offsets.

  // cursor tracks our current byte while deserializing the payload.
  size_t cursor = 0;

  // Helper: make sure we never read past the CRC-protected payload.
  auto require_bytes = [&](size_t count) {
    if (cursor > payload_size || count > payload_size - cursor) {
      throw std::runtime_error("Corrupted SSTable sparse index");
    }
  };

  // --------------------------------------------------
  // 1. Read entry_count
  // --------------------------------------------------

  // our format starts with [entry_count] and is uint32_t type
  require_bytes(sizeof(uint32_t));

  // Index begins with the number of block descriptors that follow.
  uint32_t entry_count = decode_from_le<uint32_t>(index_bytes.data() + cursor);

  cursor += sizeof(uint32_t);

  // Minimum entry = key_len(4) + empty key(0) + offset(8) + size(4) = 16 bytes.
  // Reject an entry_count that physically cannot fit in this payload.
  size_t remaining_bytes = payload_size - cursor;
  // The smallest mininum smallest entry will be 16 bytes
  //
  //
  //  first_key_length     4
  //  first_key            0   ← technically empty
  //  block_offset         8
  //  block_size           4
  //  ----------------------
  //  minimum             16 bytes
  //
  //  remaining_bytes / 16 -> is the max number of entries possible
  if (entry_count > remaining_bytes / 16) {
    throw std::runtime_error("Invalid SSTable sparse index entry count");
  }

  sparse_index_.clear();
  sparse_index_.reserve(entry_count); // To optimize performance

  // --------------------------------------------------
  // 2. Parse every SparseIndexEntry
  // --------------------------------------------------
  // Deserialize each disk entry into a usable RAM object.
  for (uint32_t i = 0; i < entry_count; i++) {

    // ---- firstKey length ----

    require_bytes(sizeof(uint32_t));

    uint32_t first_key_length =
        decode_from_le<uint32_t>(index_bytes.data() + cursor);

    cursor += sizeof(uint32_t);

    // ---- firstKey ----

    require_bytes(first_key_length);

    std::string first_key(
        reinterpret_cast<const char *>(index_bytes.data() + cursor),
        first_key_length);

    cursor += first_key_length;

    // ---- block offset ----

    require_bytes(sizeof(uint64_t));

    uint64_t block_offset =
        decode_from_le<uint64_t>(index_bytes.data() + cursor);

    cursor += sizeof(uint64_t);

    // ---- block size ----

    require_bytes(sizeof(uint32_t));

    uint32_t block_size = decode_from_le<uint32_t>(index_bytes.data() + cursor);

    cursor += sizeof(uint32_t);

    // ---- reconstruct RAM object ----

    SparseIndexEntry entry;

    entry.firstKey = std::move(first_key);
    entry.block_offset = block_offset;
    entry.block_size = block_size;

    sparse_index_.push_back(std::move(entry));
  }

  // Writer should have produced exactly entry_count entries.
  // No unexplained bytes should remain before the CRC.
  if (cursor != payload_size) {
    throw std::runtime_error(
        "Unexpected trailing bytes in SSTable sparse index");
  }
}

/*
 * Point lookup:
 *
 * 1. Search the RAM sparse index for the candidate block.
 * 2. Jump directly to that block on disk.
 * 3. Read the block into RAM and verify its CRC.
 * 4. Sequentially scan its sorted records for the exact key.
 */
kronos::SstableReader::GetResult
kronos::SstableReader::get(const std::string &key) const {

  GetResult g = {.value = "", .status = GetStatus::NOT_FOUND};

  // Bloom filter can prove that the key definitely does not exist.
  if (bloom_filter_.has_value() && !bloom_filter_->mayContain(key)) {
    return g;
  }

  // No data blocks exist in this SSTable.
  if (sparse_index_.empty()) {
    return g;
  }

  if (key < sparse_index_[0].firstKey) {
    return g;
  }

  size_t target_id = 0;

  for (size_t i = 1; i < sparse_index_.size(); i++) {
    if (sparse_index_[i].firstKey <= key) {
      target_id = i;

    } else {
      break;
    }
  }

  // Sparse index gives us the exact disk region containing the candidate block.
  uint64_t target_block_offset = sparse_index_[target_id].block_offset;
  uint32_t target_block_size = sparse_index_[target_id].block_size;

  // Now we have our block.
  // Address:Block starts "block_offset" bytes from the beginning of the file

  if (::lseek(fd_, static_cast<off_t>(target_block_offset), SEEK_SET) == -1) {
    throw std::runtime_error("Failed to seek to SSTable data block");
  }

  std::vector<uint8_t> block_bytes(target_block_size);

  read_exact(fd_, block_bytes.data(), block_bytes.size());

  // This is the index (entry) without the CRC checksum value
  size_t payload_size = block_bytes.size() - sizeof(uint32_t);

  uLong crc = crc32(0L, Z_NULL, 0);

  crc = crc32(crc, reinterpret_cast<const Bytef *>(block_bytes.data()),
              static_cast<uInt>(payload_size));

  // Converting checksum to uint32_t as zlib returns the checksum as uLong.
  uint32_t checksum_value = static_cast<uint32_t>(crc);
  const uint8_t *stored_crc_ptr = block_bytes.data() + payload_size;

  uint32_t stored_crc = decode_from_le<uint32_t>(stored_crc_ptr);

  if (checksum_value != stored_crc) {
    throw std::runtime_error("Invalid SSTable block CRC");
  }

  // --------------------------------------------------
  // 4. Parse block
  // --------------------------------------------------

  size_t cursor = 0;

  auto require_bytes = [&](size_t count) {
    if (cursor > payload_size || count > payload_size - cursor) {
      throw std::runtime_error("Corrupted SSTable data block");
    }
  };

  // First 4 bytes = record_count.
  require_bytes(sizeof(uint32_t));

  uint32_t record_count = decode_from_le<uint32_t>(block_bytes.data() + cursor);

  cursor += sizeof(uint32_t);

  // Every record requires at least:
  // sequence      = 8
  // operation     = 1
  // key_length    = 4
  // value_length  = 4
  // -----------------
  // minimum       = 17 bytes

  size_t remaining_bytes = payload_size - cursor;

  if (record_count > remaining_bytes / 17) {
    throw std::runtime_error("Invalid SSTable record count");
  }

  // --------------------------------------------------
  // 5. Scan records inside candidate block
  // --------------------------------------------------

  for (uint32_t i = 0; i < record_count; i++) {

    // ---- sequence ----

    require_bytes(sizeof(uint64_t));

    uint64_t sequence = decode_from_le<uint64_t>(block_bytes.data() + cursor);

    cursor += sizeof(uint64_t);

    // Sequence is stored/read correctly,
    // but single-SSTable get() does not need it yet.
    (void)sequence;

    // ---- operation ----

    require_bytes(sizeof(uint8_t));

    uint8_t operation_byte = block_bytes[cursor];

    cursor += sizeof(uint8_t);

    // ---- key length ----

    require_bytes(sizeof(uint32_t));

    uint32_t key_length = decode_from_le<uint32_t>(block_bytes.data() + cursor);

    cursor += sizeof(uint32_t);

    // ---- value length ----

    require_bytes(sizeof(uint32_t));

    uint32_t value_length =
        decode_from_le<uint32_t>(block_bytes.data() + cursor);

    cursor += sizeof(uint32_t);

    // ---- key ----

    require_bytes(key_length);

    std::string record_key(
        reinterpret_cast<const char *>(block_bytes.data() + cursor),
        key_length);

    cursor += key_length;

    // ---- value ----

    require_bytes(value_length);

    std::string record_value(
        reinterpret_cast<const char *>(block_bytes.data() + cursor),
        value_length);

    cursor += value_length;

    // --------------------------------------------------
    // 6. Compare this record with requested key
    // --------------------------------------------------

    if (record_key == key) {

      if (operation_byte == static_cast<uint8_t>(OperationType::DELETE)) {

        return {.value = "", .status = GetStatus::DELETED};
      }

      if (operation_byte == static_cast<uint8_t>(OperationType::PUT)) {

        return {.value = std::move(record_value), .status = GetStatus::FOUND};
      }

      throw std::runtime_error("Invalid operation type in SSTable record");
    }

    // Records are sorted.
    // If we've already passed our target,
    // it cannot appear later in this block.
    if (record_key > key) {
      return g;
    }
  }
  if (cursor != payload_size) {
    throw std::runtime_error("Unexpected trailing bytes in SSTable data block");
  }

  // We scanned the candidate block and didn't find it.
  return g;
}

kronos::SstableReader::SstableReader(const std::filesystem::path &path) {

  fd_ = ::open(path.c_str(), O_RDONLY);

  if (fd_ == -1) {
    throw std::runtime_error("Failed to open SSTable for reading");
  }

  try {
    readHeader();

    FooterInfo footer = readFooter();
    loadBloomFilter(footer.bloom_offset, footer.bloom_size);
    loadSparseIndex(footer.index_offset, footer.index_size);

  } catch (...) {
    ::close(fd_);
    fd_ = -1;
    throw;
  }
}
kronos::SstableReader::~SstableReader() {
  if (fd_ != -1) {
    ::close(fd_);
  }
}

kronos::SstableReader::Iterator kronos::SstableReader::getIterator() const {
  return Iterator(this);
}

kronos::SstableReader::Iterator::Iterator(const SstableReader *reader)
    : reader_(reader), current_block_index_(0), cursor_(0), record_count_(0),
      current_record_index_(0), valid_(false) {

  if (reader_ == nullptr) {
    throw std::runtime_error("Iterator has no SSTable reader");
  }

  // Empty SSTable -> iterator immediately points to END.
  if (reader_->sparse_index_.empty()) {
    return;
  }

  // Start from the first physical data block.
  loadBlock(0);

  // Next step:
  parseCurrentRecord();
}

void kronos::SstableReader::Iterator::loadBlock(size_t block_index) {

  if (block_index >= reader_->sparse_index_.size()) {
    throw std::runtime_error("SSTable iterator block index out of range");
  }

  current_block_index_ = block_index;

  const SparseIndexEntry &block = reader_->sparse_index_[block_index];

  if (block.block_size < 8) {
    throw std::runtime_error("Invalid SSTable block size");
  }

  // Jump to this block on disk.
  if (::lseek(reader_->fd_, static_cast<off_t>(block.block_offset), SEEK_SET) ==
      -1) {
    throw std::runtime_error("Failed to seek to SSTable iterator block");
  }

  // Bring the entire block into RAM.
  block_bytes_.resize(block.block_size);

  read_exact(reader_->fd_, block_bytes_.data(), block_bytes_.size());

  // --------------------------------------------------
  // Verify block CRC before parsing anything inside it.
  // --------------------------------------------------

  size_t payload_size = block_bytes_.size() - sizeof(uint32_t);

  uLong crc = crc32(0L, Z_NULL, 0);

  crc = crc32(crc, reinterpret_cast<const Bytef *>(block_bytes_.data()),
              static_cast<uInt>(payload_size));

  uint32_t calculated_crc = static_cast<uint32_t>(crc);

  const uint8_t *stored_crc_ptr = block_bytes_.data() + payload_size;

  uint32_t stored_crc = decode_from_le<uint32_t>(stored_crc_ptr);

  if (calculated_crc != stored_crc) {
    throw std::runtime_error("Invalid SSTable block CRC");
  }

  // --------------------------------------------------
  // Prepare to walk this block.
  // --------------------------------------------------

  cursor_ = 0;

  if (payload_size < sizeof(uint32_t)) {
    throw std::runtime_error("Corrupted SSTable data block");
  }

  // Every block begins with record_count.
  record_count_ = decode_from_le<uint32_t>(block_bytes_.data());

  cursor_ += sizeof(uint32_t);

  current_record_index_ = 0;

  if (record_count_ == 0) {
    throw std::runtime_error("Indexed SSTable block contains no records");
  }
}

void kronos::SstableReader::Iterator::parseCurrentRecord() {

  size_t payload_size = block_bytes_.size() - sizeof(uint32_t);

  auto require_bytes = [&](size_t count) {
    if (cursor_ > payload_size || count > payload_size - cursor_) {
      throw std::runtime_error("Corrupted SSTable data block");
    }
  };

  // sequence
  require_bytes(sizeof(uint64_t));

  uint64_t sequence = decode_from_le<uint64_t>(block_bytes_.data() + cursor_);

  cursor_ += sizeof(uint64_t);

  // operation
  require_bytes(sizeof(uint8_t));

  uint8_t operation_byte = block_bytes_[cursor_];

  cursor_ += sizeof(uint8_t);

  // key length
  require_bytes(sizeof(uint32_t));

  uint32_t key_length = decode_from_le<uint32_t>(block_bytes_.data() + cursor_);

  cursor_ += sizeof(uint32_t);

  // value length
  require_bytes(sizeof(uint32_t));

  uint32_t value_length =
      decode_from_le<uint32_t>(block_bytes_.data() + cursor_);

  cursor_ += sizeof(uint32_t);

  // key
  require_bytes(key_length);

  current_key_ =
      std::string(reinterpret_cast<const char *>(block_bytes_.data() + cursor_),
                  key_length);

  cursor_ += key_length;

  // value
  require_bytes(value_length);

  std::string value(
      reinterpret_cast<const char *>(block_bytes_.data() + cursor_),
      value_length);

  cursor_ += value_length;

  current_entry_ = {.value = std::move(value),
                    .sequence = sequence,
                    .operation = static_cast<OperationType>(operation_byte)};

  valid_ = true;
}

bool kronos::SstableReader::Iterator::valid() const { return valid_; }

const std::string &kronos::SstableReader::Iterator::getKey() const {

  if (!valid_) {
    throw std::runtime_error("SSTable iterator is not valid");
  }

  return current_key_;
}

const kronos::SstableReader::Iterator::Entry &
kronos::SstableReader::Iterator::getEntry() const {

  if (!valid_) {
    throw std::runtime_error("SSTable iterator is not valid");
  }

  return current_entry_;
}

void kronos::SstableReader::Iterator::next() {

  // Already at the end of the SSTable.
  if (!valid_) {
    return;
  }

  // More records remain inside the current block.
  if (current_record_index_ + 1 < record_count_) {
    current_record_index_++;
    parseCurrentRecord();
    return;
  }

  // Current block is exhausted.
  size_t next_block_index = current_block_index_ + 1;

  // No more blocks -> end of SSTable.
  if (next_block_index >= reader_->sparse_index_.size()) {
    valid_ = false;
    return;
  }

  // Move to the next block and expose its first record.
  loadBlock(next_block_index);
  parseCurrentRecord();
}
