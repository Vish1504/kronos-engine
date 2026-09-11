#include <cstddef>
#include <kronos/sstable.hpp>

#include <iostream>
#include <type_traits>

template <typename T> void encode_to_le(uint8_t arr[], T value) {
  static_assert(std::is_unsigned_v<T>);
  for (size_t i = 0; i < sizeof(T); i++) {
    arr[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
}
void write_all(int fd, const uint8_t *data, size_t size) {
  size_t written = 0;

  while (written < size) {
    ssize_t result = ::write(fd, data + written, size - written);

    if (result <= 0) {
      throw std::runtime_error("Failed to write SSTable data");
    }

    written += static_cast<size_t>(result);
  }
}

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

  uint32_t version = 1;

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

  if (current_block_.size() != 0 &&
      (current_block_.size() + record_bytes.size()) > target_blockSize_) {
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
