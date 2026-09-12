#include <cstddef>
#include <kronos/sstable.hpp>

#include <iostream>
#include <stdexcept>
#include <type_traits>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
constexpr char sstable_magic_[4] = {'K', 'S', 'S', 'T'};
constexpr uint32_t version = 1;

template <typename T> void encode_to_le(uint8_t arr[], T value) {
  static_assert(std::is_unsigned_v<T>);
  for (size_t i = 0; i < sizeof(T); i++) {
    arr[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
}
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

kronos::SstableBuilder::SstableBuilder(const std::filesystem::path &path,
                                       size_t blockSize) {

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

void kronos::SstableBuilder::writeHeader() {
  write_all(fd_, sstable_magic_, sizeof(sstable_magic_));

  //   uint32_t version = 1;

  uint8_t version_bytes[4];
  encode_to_le(version_bytes, version);

  write_all(fd_, version_bytes, sizeof(version_bytes));
}

void kronos::SstableBuilder::add(std::string key, InternalEntry e) {
  uint8_t sequence_bytes[8];
  encode_to_le(sequence_bytes, e.sequence);

  // we cast it so the compiler treats it as a raw byte
  uint8_t operation_byte = static_cast<uint8_t>(e.operation);

  // key length - 64 bit to 32 bit
  uint32_t keyLength = static_cast<uint32_t>(key.size());
  // value length - 64 bit to 32 bit
  uint32_t valueLength = static_cast<uint32_t>(e.value.size());

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

void kronos::SstableBuilder::writeSparseIndex() {

  uint32_t entry_count = static_cast<uint32_t>(sparse_index_.size());
  uint8_t entry_count_bytes[4];
  encode_to_le(entry_count_bytes, entry_count);
  std::vector<uint8_t> index_bytes;
  index_bytes.insert(index_bytes.end(), entry_count_bytes,
                     entry_count_bytes + sizeof(entry_count_bytes));
  //   serialize
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

void kronos::SstableBuilder::writeFooter(uint64_t index_offset,
                                         uint64_t index_size) {
  uint8_t index_offset_bytes[8];
  encode_to_le(index_offset_bytes, index_offset);

  uint8_t index_size_bytes[8];
  encode_to_le(index_size_bytes, index_size);

  // Now we calculate CRC32 over [index_offset][index_size]
  // Initialize the CRC register
  uLong crc = crc32(0L, Z_NULL, 0);

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

  write_all(fd_, index_offset_bytes, sizeof(index_offset_bytes));
  write_all(fd_, index_size_bytes, sizeof(index_size_bytes));
  write_all(fd_, footer_checksum_bytes, sizeof(footer_checksum_bytes));
}

void kronos::SstableBuilder::finish() {
  flushCurrentBlock();

  off_t index_offset_pos = ::lseek(fd_, 0, SEEK_CUR);

  if (index_offset_pos == -1) {
    throw std::runtime_error("Unable to get index offset position");
  }

  // off_t will be 64 bit only on a native 64 bit system
  uint64_t index_offset = static_cast<uint64_t>(index_offset_pos);

  writeSparseIndex();

  off_t index_end_pos = ::lseek(fd_, 0, SEEK_CUR);

  if (index_end_pos == -1) {
    throw std::runtime_error("Unable to get index end position");
  }

  // off_t will be 64 bit only on a native 64 bit system
  uint64_t index_end = static_cast<uint64_t>(index_end_pos);

  uint64_t index_size = index_end - index_offset;

  writeFooter(index_offset, index_size);

  if (::fsync(fd_) == -1) {
    throw std::runtime_error("Failed to fsync SSTable");
  }

  ::close(fd_);
  fd_ = -1;

  auto temp_path = sstable_path_;
  temp_path.replace_extension(".tmp");

  // Rename from the temporary path (.tmp) to the final .sst path
  if (std::rename(temp_path.c_str(), sstable_path_.c_str()) != 0) {
    // Handle rename error (e.g., check errno)
    throw std::runtime_error(
        "Unable to rename file extennsion from .tmp to .sst");
  }
}

//
// Reader class functions from here
//

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

kronos::SstableReader::FooterInfo kronos::SstableReader::readFooter() {
  off_t new_offset = ::lseek(fd_, -20, SEEK_END);

  if (new_offset == -1) {
    throw std::runtime_error("Failed to seek to SSTable footer");
  }

  uint8_t index_offset_buffer[8] = {0};
  // Read up to 8 bytes from the file descriptor
  read_exact(fd_, index_offset_buffer, sizeof(index_offset_buffer));

  uint8_t index_size_buffer[8] = {0};
  // Read next  8 bytes from the file descriptor
  read_exact(fd_, index_size_buffer, sizeof(index_size_buffer));

  uint8_t footer_crc_buffer[4] = {0};
  // Read next 4 bytes from the file descriptor
  read_exact(fd_, footer_crc_buffer, sizeof(footer_crc_buffer));

  //   uint64_t decoded_index_offset_buffer =
  //   decode_from_le(index_offset_buffer); uint64_t decoded_index_size_buffer =
  //   decode_from_le(index_size_buffer);

  uLong crc = crc32(0L, Z_NULL, 0);

  crc = crc32(crc, reinterpret_cast<const Bytef *>(index_offset_buffer),
              sizeof(index_offset_buffer));

  crc = crc32(crc, reinterpret_cast<const Bytef *>(index_size_buffer),
              sizeof(index_size_buffer));

  uint32_t calculated_crc = static_cast<uint32_t>(crc);

  uint32_t stored_crc = decode_from_le<uint32_t>(footer_crc_buffer);

  if (calculated_crc != stored_crc) {
    throw std::runtime_error("Invalid SSTable footer CRC");
  }

  uint64_t index_offset = decode_from_le<uint64_t>(index_offset_buffer);

  uint64_t index_size = decode_from_le<uint64_t>(index_size_buffer);
  return {index_offset, index_size};
}

void kronos::SstableReader::loadSparseIndex(uint64_t index_offset,
                                            uint64_t index_size) {
  if (index_size < 8) {
    throw std::runtime_error("Invalid SSTable sparse index size");
  }

  if (::lseek(fd_, static_cast<off_t>(index_offset), SEEK_SET) == -1) {
    throw std::runtime_error("Failed to seek to SSTable sparse index");
  }

  std::vector<uint8_t> index_bytes(static_cast<size_t>(index_size));

  read_exact(fd_, index_bytes.data(), index_bytes.size());

  size_t payload_size = index_bytes.size() - sizeof(uint32_t);

  uLong crc = crc32(0L, Z_NULL, 0);

  crc = crc32(crc, reinterpret_cast<const Bytef *>(index_bytes.data()),
              static_cast<uInt>(payload_size));

  uint32_t calculated_crc = static_cast<uint32_t>(crc);

  const uint8_t *stored_crc_ptr = index_bytes.data() + payload_size;

  uint32_t stored_crc = decode_from_le<uint32_t>(stored_crc_ptr);

  if (calculated_crc != stored_crc) {
    throw std::runtime_error("Invalid SSTable sparse index CRC");
  }

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

  require_bytes(sizeof(uint32_t));

  uint32_t entry_count = decode_from_le<uint32_t>(index_bytes.data() + cursor);

  cursor += sizeof(uint32_t);

  // Basic sanity check.
  // Smallest possible index entry:
  // 4 bytes key length
  // 0 bytes key
  // 8 bytes block offset
  // 4 bytes block size
  // = 16 bytes
  size_t remaining_bytes = payload_size - cursor;

  if (entry_count > remaining_bytes / 16) {
    throw std::runtime_error("Invalid SSTable sparse index entry count");
  }

  sparse_index_.clear();
  sparse_index_.reserve(entry_count);

  // --------------------------------------------------
  // 2. Parse every SparseIndexEntry
  // --------------------------------------------------

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

kronos::SstableReader::SstableReader(const std::filesystem::path &path) {

  fd_ = ::open(path.c_str(), O_RDONLY);

  if (fd_ == -1) {
    throw std::runtime_error("Failed to open SSTable for reading");
  }

  try {
    readHeader();

    FooterInfo footer = readFooter();

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
