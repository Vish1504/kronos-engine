#include <kronos/wal.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

namespace fs = std::filesystem;

/*
 * WAL (Write-Ahead Log)
 * ---------------------
 *
 * A write is persisted to the WAL before it is applied to the Memtable.
 * If the process crashes and the in-memory Memtable disappears, the WAL
 * can be replayed during startup to reconstruct the lost state.
 *
 * File layout:
 *
 *   HEADER:
 *   [Magic][Version]
 *
 *   RECORD:
 *   [Sequence][Operation][Key Length][Value Length][Key][Value][CRC32]
 *
 * Field sizes:
 *
 *   Sequence     -> uint64_t
 *   Operation    -> uint8_t
 *   Key Length   -> uint32_t
 *   Value Length -> uint32_t
 *   Key          -> variable bytes
 *   Value        -> variable bytes
 *   CRC32        -> uint32_t
 *
 * Chronos v1 serializes integer fields using the host machine's native
 * byte order. A fixed byte order can be introduced later if cross-platform
 * WAL portability becomes a requirement.
 */

template <typename T>
void appendBytes(std::vector<uint8_t> &record, const T &value) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);

  record.insert(record.end(), bytes, bytes + sizeof(value));
}

/// making this helper private to wal.cpp
namespace {
void fsyncParentDirectory(const std::filesystem::path &path) {
  std::filesystem::path parent = path.parent_path();

  if (parent.empty()) {
    parent = ".";
  }

  int fd = ::open(parent.c_str(), O_RDONLY);

  if (fd == -1) {
    throw std::runtime_error("Failed to open WAL parent directory for fsync");
  }

  while (::fsync(fd) == -1) {
    if (errno == EINTR) {
      continue;
    }

    if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {
      ::close(fd);
      return;
    }

    ::close(fd);

    throw std::runtime_error("Failed to fsync WAL parent directory");
  }

  ::close(fd);
}

} // namespace

void kronos::Wal::ensureNextSequenceAtLeast(uint64_t minimum_next_sequence) {

  if (next_sequence_ < minimum_next_sequence) {
    next_sequence_ = minimum_next_sequence;
  }
}

kronos::Wal::Wal(const std::filesystem::path &pathWal) : path_(pathWal) {
  const bool isNewFile = !fs::exists(pathWal);

  /*
   * O_RDWR is required because the WAL is both replayed and appended to.
   *
   * O_CREAT creates the WAL if the database does not have one yet.
   */
  fd_ = ::open(pathWal.c_str(), O_RDWR | O_CREAT, 0644);

  if (fd_ == -1) {
    throw std::runtime_error("Failed to open WAL file");
  }

  try {
    if (isNewFile) {
      size_t totalWritten = 0;

      // Write WAL magic.
      while (totalWritten < sizeof(magic_)) {
        ssize_t result = ::write(
            fd_, reinterpret_cast<const uint8_t *>(magic_) + totalWritten,
            sizeof(magic_) - totalWritten);

        if (result == -1) {
          if (errno == EINTR) {
            continue;
          }

          throw std::runtime_error("Failed to write WAL magic");
        }

        if (result == 0) {
          throw std::runtime_error("Failed to write WAL magic");
        }

        totalWritten += static_cast<size_t>(result);
      }

      // Write WAL format version.
      totalWritten = 0;

      while (totalWritten < sizeof(version_)) {
        ssize_t result = ::write(
            fd_, reinterpret_cast<const uint8_t *>(&version_) + totalWritten,
            sizeof(version_) - totalWritten);

        if (result == -1) {
          if (errno == EINTR) {
            continue;
          }

          throw std::runtime_error("Failed to write WAL version");
        }

        if (result == 0) {
          throw std::runtime_error("Failed to write WAL version");
        }

        totalWritten += static_cast<size_t>(result);
      }

      /*
       * The complete WAL header [Magic][Version] must be durable before
       * the database begins accepting writes.
       */
      while (::fsync(fd_) == -1) {
        if (errno == EINTR) {
          continue;
        }

        throw std::runtime_error("Failed to fsync new WAL");
      }

      /*
       * Persist creation of the WAL directory entry.
       */
      fsyncParentDirectory(path_);

    } else {

      /*
       * Validate the existing WAL header before attempting recovery.
       */
      if (::lseek(fd_, 0, SEEK_SET) == -1) {
        throw std::runtime_error("Failed to seek WAL");
      }

      char magicRead[sizeof(magic_)];

      if (readUpTo(magicRead, sizeof(magicRead)) != sizeof(magicRead)) {
        throw std::runtime_error("Failed to read WAL magic");
      }

      if (std::memcmp(magicRead, magic_, sizeof(magic_)) != 0) {
        throw std::runtime_error("Invalid WAL magic");
      }

      uint8_t versionRead = 0;

      if (readUpTo(&versionRead, sizeof(versionRead)) != sizeof(versionRead)) {
        throw std::runtime_error("Failed to read WAL version");
      }

      if (versionRead != version_) {
        throw std::runtime_error("Unsupported WAL version");
      }
    }

    /*
     * New writes must always append after the existing WAL contents.
     */
    if (::lseek(fd_, 0, SEEK_END) == -1) {
      throw std::runtime_error("Failed to seek to end of WAL");
    }

  } catch (...) {

    ::close(fd_);
    fd_ = -1;

    throw;
  }
}

kronos::Wal::~Wal() {
  if (fd_ != -1) {
    ::close(fd_);
  }
}

uint64_t kronos::Wal::put(const std::string &key, const std::string &value) {
  return writeRecord(OperationType::PUT, key, value);
}

uint64_t kronos::Wal::remove(const std::string &key) {
  return writeRecord(OperationType::DELETE, key, "");
}

uint64_t kronos::Wal::writeRecord(OperationType operation,
                                  const std::string &key,
                                  const std::string &value) {

  /*
   * UINT64_MAX cannot have a successor.
   *
   * Reject the write before touching the WAL rather than allowing
   * next_sequence_ to wrap back to zero.
   */
  if (next_sequence_ == std::numeric_limits<uint64_t>::max()) {
    throw std::overflow_error("WAL sequence space exhausted");
  }

  const uint64_t sequence = next_sequence_;

  /*
   * WAL lengths are encoded as uint32_t, so the strings must fit before
   * converting size_t to uint32_t.
   */
  if (key.size() > std::numeric_limits<uint32_t>::max() ||
      value.size() > std::numeric_limits<uint32_t>::max()) {

    throw std::runtime_error("Key or value too large for WAL record");
  }

  const uint32_t keyLength = static_cast<uint32_t>(key.size());

  const uint32_t valueLength = static_cast<uint32_t>(value.size());

  /*
   * The disk format defines Operation as exactly one byte.
   *
   * Serialize an explicit uint8_t rather than relying on sizeof(OperationType).
   */
  const uint8_t operationByte = static_cast<uint8_t>(operation);

  /*
   * Build:
   *
   * [Sequence]
   * [Operation]
   * [Key Length]
   * [Value Length]
   * [Key]
   * [Value]
   *
   * CRC32 is calculated over these exact bytes and appended afterward.
   */
  std::vector<uint8_t> record;

  appendBytes(record, sequence);
  appendBytes(record, operationByte);
  appendBytes(record, keyLength);
  appendBytes(record, valueLength);

  for (char c : key) {
    record.push_back(static_cast<uint8_t>(c));
  }

  for (char c : value) {
    record.push_back(static_cast<uint8_t>(c));
  }

  uLong crc = crc32(0L, Z_NULL, 0);

  crc =
      crc32(crc, reinterpret_cast<const Bytef *>(record.data()), record.size());

  const uint32_t checksum = static_cast<uint32_t>(crc);

  /*
   * Final record:
   *
   * [Sequence][Operation][Key Length][Value Length]
   * [Key][Value][CRC32]
   */
  appendBytes(record, checksum);

  size_t totalWritten = 0;

  while (totalWritten < record.size()) {

    ssize_t bytesWritten = ::write(fd_, record.data() + totalWritten,
                                   record.size() - totalWritten);

    if (bytesWritten == -1) {

      if (errno == EINTR) {
        continue;
      }

      throw std::runtime_error("Failed to write WAL record");
    }

    if (bytesWritten == 0) {
      throw std::runtime_error("Failed to make progress writing WAL record");
    }

    totalWritten += static_cast<size_t>(bytesWritten);
  }

  /*
   * A write is not acknowledged until the WAL record has crossed the
   * file-level durability boundary.
   */
  while (::fsync(fd_) == -1) {

    if (errno == EINTR) {
      continue;
    }

    throw std::runtime_error("Failed to fsync WAL");
  }

  next_sequence_ = sequence + 1;

  return sequence;
}

/*
 * read() may legally return fewer bytes than requested.
 *
 * Keep reading until:
 *
 *   - the requested byte count has been obtained,
 *   - EOF is reached,
 *   - or a genuine read error occurs.
 */
size_t kronos::Wal::readUpTo(void *buffer, size_t bytesToRead) {

  size_t totalRead = 0;

  uint8_t *bytes = static_cast<uint8_t *>(buffer);

  while (totalRead < bytesToRead) {

    ssize_t result = ::read(fd_, bytes + totalRead, bytesToRead - totalRead);

    if (result > 0) {
      totalRead += static_cast<size_t>(result);
      continue;
    }

    if (result == 0) {
      break;
    }

    if (errno == EINTR) {
      continue;
    }

    throw std::runtime_error("Failed to read WAL");
  }

  return totalRead;
}

/*
 * WAL recovery policy
 * -------------------
 *
 * 1. Clean EOF between records:
 *      recovery succeeds.
 *
 * 2. EOF while reading the final record:
 *      keep all earlier valid records,
 *      truncate the incomplete tail,
 *      fsync the repair,
 *      and finish recovery successfully.
 *
 * 3. CRC mismatch or invalid operation:
 *      treat it as genuine corruption,
 *      do not silently truncate it,
 *      and fail recovery.
 */
std::vector<kronos::Wal::RecoveredRecord> kronos::Wal::recover() {
  std::vector<RecoveredRecord> records;
  std::optional<uint64_t> previous_sequence;

  /*
   * Snapshot the physical WAL size.
   *
   * Recovery runs while no concurrent writer is modifying the WAL, so this
   * remains stable for the duration of this recovery pass.
   *
   * We use it later to validate key/value lengths BEFORE allocating memory.
   */
  struct stat wal_stat {};

  if (::fstat(fd_, &wal_stat) == -1) {
    throw std::runtime_error("Failed to determine WAL size during recovery");
  }

  if (wal_stat.st_size < 0) {
    throw std::runtime_error("Invalid WAL size");
  }

  const uint64_t wal_file_size = static_cast<uint64_t>(wal_stat.st_size);

  /*
   * Helper for a legitimately incomplete final WAL record.
   *
   * Preserve every earlier validated record and remove only the unfinished
   * tail.
   */
  auto truncateIncompleteTail = [&](off_t record_start) {
    if (::ftruncate(fd_, record_start) == -1) {
      throw std::runtime_error("Failed to truncate incomplete WAL record");
    }

    while (::fsync(fd_) == -1) {
      if (errno == EINTR) {
        continue;
      }

      throw std::runtime_error("Failed to fsync WAL after truncation");
    }
  };

  /*
   * WAL records begin immediately after:
   *
   * [Magic][Version]
   */
  if (::lseek(fd_, sizeof(magic_) + sizeof(version_), SEEK_SET) == -1) {

    throw std::runtime_error("Failed to seek WAL for recovery");
  }

  while (true) {
    /*
     * If this record turns out to be an interrupted final write, this is the
     * exact position to which the WAL must be truncated.
     */
    const off_t recordStart = ::lseek(fd_, 0, SEEK_CUR);

    if (recordStart == -1) {
      throw std::runtime_error("Failed to determine WAL record position");
    }

    // ---------------------------------------------------------
    // 1. SEQUENCE
    // ---------------------------------------------------------

    uint64_t sequence = 0;

    const size_t sequenceBytes = readUpTo(&sequence, sizeof(sequence));

    if (sequenceBytes == 0) {
      // Clean EOF between records.
      break;
    }

    if (sequenceBytes != sizeof(sequence)) {
      truncateIncompleteTail(recordStart);
      break;
    }

    /*
     * Reconstruct exactly the serialized bytes protected by CRC32.
     *
     * IMPORTANT:
     *
     * The writer calculates CRC over:
     *
     * [sequence]
     * [operation]
     * [keyLength]
     * [valueLength]
     * [key]
     * [value]
     *
     * Recovery must rebuild EXACTLY those bytes in EXACTLY that order.
     */
    std::vector<uint8_t> recordBytes;

    appendBytes(recordBytes, sequence);

    // ---------------------------------------------------------
    // 2. OPERATION
    // ---------------------------------------------------------

    uint8_t operationByte = 0;

    if (readUpTo(&operationByte, sizeof(operationByte)) !=
        sizeof(operationByte)) {

      truncateIncompleteTail(recordStart);
      break;
    }

    OperationType operation;

    if (operationByte == static_cast<uint8_t>(OperationType::PUT)) {

      operation = OperationType::PUT;

    } else if (operationByte == static_cast<uint8_t>(OperationType::DELETE)) {

      operation = OperationType::DELETE;

    } else {
      /*
       * The byte physically exists but is not a legal Chronos operation.
       * This is corruption rather than an incomplete tail.
       */
      throw std::runtime_error("WAL corruption detected: invalid operation");
    }

    appendBytes(recordBytes, operationByte);

    // ---------------------------------------------------------
    // 3. KEY LENGTH
    // ---------------------------------------------------------

    uint32_t keyLength = 0;

    if (readUpTo(&keyLength, sizeof(keyLength)) != sizeof(keyLength)) {

      truncateIncompleteTail(recordStart);
      break;
    }

    appendBytes(recordBytes, keyLength);

    // ---------------------------------------------------------
    // 4. VALUE LENGTH
    // ---------------------------------------------------------

    uint32_t valueLength = 0;

    if (readUpTo(&valueLength, sizeof(valueLength)) != sizeof(valueLength)) {

      truncateIncompleteTail(recordStart);
      break;
    }

    appendBytes(recordBytes, valueLength);

    // ---------------------------------------------------------
    // 5. VALIDATE LENGTHS BEFORE ALLOCATION
    // ---------------------------------------------------------

    /*
     * The cursor is now immediately after:
     *
     * [sequence]
     * [operation]
     * [keyLength]
     * [valueLength]
     *
     * From here the record must still contain:
     *
     * [key]
     * [value]
     * [CRC32]
     */
    const off_t payloadStart = ::lseek(fd_, 0, SEEK_CUR);

    if (payloadStart == -1) {
      throw std::runtime_error("Failed to determine WAL payload position");
    }

    const uint64_t payloadPosition = static_cast<uint64_t>(payloadStart);

    if (payloadPosition > wal_file_size) {
      throw std::runtime_error("WAL recovery position exceeds file size");
    }

    const uint64_t bytesRemaining = wal_file_size - payloadPosition;

    /*
     * Promote both lengths before addition so the calculation itself cannot
     * overflow uint32_t.
     */
    const uint64_t requiredBytes = static_cast<uint64_t>(keyLength) +
                                   static_cast<uint64_t>(valueLength) +
                                   static_cast<uint64_t>(sizeof(uint32_t));

    if (requiredBytes > bytesRemaining) {
      /*
       * The final record header exists but its declared payload/CRC does not
       * physically fit in the file.
       *
       * This is consistent with a crash during the final WAL append.
       */
      truncateIncompleteTail(recordStart);
      break;
    }

    // ---------------------------------------------------------
    // 6. KEY
    // ---------------------------------------------------------

    std::string key(keyLength, '\0');

    if (keyLength > 0) {
      if (readUpTo(key.data(), keyLength) != keyLength) {

        truncateIncompleteTail(recordStart);
        break;
      }

      for (char c : key) {
        recordBytes.push_back(static_cast<uint8_t>(c));
      }
    }

    // ---------------------------------------------------------
    // 7. VALUE
    // ---------------------------------------------------------

    std::string value(valueLength, '\0');

    if (valueLength > 0) {
      if (readUpTo(value.data(), valueLength) != valueLength) {

        truncateIncompleteTail(recordStart);
        break;
      }

      for (char c : value) {
        recordBytes.push_back(static_cast<uint8_t>(c));
      }
    }

    // ---------------------------------------------------------
    // 8. STORED CRC32
    // ---------------------------------------------------------

    uint32_t storedChecksum = 0;

    if (readUpTo(&storedChecksum, sizeof(storedChecksum)) !=
        sizeof(storedChecksum)) {

      truncateIncompleteTail(recordStart);
      break;
    }

    // ---------------------------------------------------------
    // 9. VERIFY CRC32
    // ---------------------------------------------------------

    uLong crc = crc32(0L, Z_NULL, 0);

    crc = crc32(crc, reinterpret_cast<const Bytef *>(recordBytes.data()),
                recordBytes.size());

    const uint32_t calculatedChecksum = static_cast<uint32_t>(crc);

    if (calculatedChecksum != storedChecksum) {
      /*
       * A complete-looking record exists, but its contents do not match the
       * checksum written with it.
       *
       * Never silently truncate genuine corruption.
       */
      throw std::runtime_error("WAL corruption detected: CRC32 mismatch");
    }

    // ---------------------------------------------------------
    // 10. VALIDATE SEQUENCE HISTORY
    // ---------------------------------------------------------

    if (previous_sequence.has_value() && sequence <= *previous_sequence) {

      throw std::runtime_error("WAL corruption detected: non-increasing "
                               "sequence number; previous=" +
                               std::to_string(*previous_sequence) +
                               ", current=" + std::to_string(sequence));
    }

    /*
     * UINT64_MAX cannot have a valid successor.
     */
    if (sequence == std::numeric_limits<uint64_t>::max()) {

      throw std::overflow_error("WAL sequence space exhausted");
    }

    previous_sequence = sequence;

    // ---------------------------------------------------------
    // 11. VALID RECORD
    // ---------------------------------------------------------

    records.push_back(
        RecoveredRecord{sequence, operation, std::move(key), std::move(value)});

    /*
     * Restore the sequence allocator from the newest completely validated
     * record.
     */
    next_sequence_ = sequence + 1;
  }

  /*
   * Recovery moved the descriptor while reading.
   *
   * Future writes must append after the surviving WAL contents.
   */
  if (::lseek(fd_, 0, SEEK_END) == -1) {
    throw std::runtime_error("Failed to seek to end of WAL after recovery");
  }

  return records;
}

void kronos::Wal::reclaimThrough(uint64_t checkpoint) {

  /*
   * The engine must serialize WAL reclamation against foreground writes.
   *
   * reclaimThrough() therefore assumes no concurrent put()/remove() is
   * modifying this WAL while the rewrite is happening.
   */

  const uint64_t allocator_before_reclaim = next_sequence_;

  /*
   * Parse and validate the existing WAL.
   *
   * recover() also leaves the file descriptor positioned at the end.
   */
  const auto records = recover();

  /*
   * recover() derives next_sequence_ from the WAL itself.
   *
   * The MANIFEST may have previously raised the allocator beyond what the
   * physical WAL alone can prove, so reclamation must never move the allocator
   * backwards.
   */
  if (next_sequence_ < allocator_before_reclaim) {
    next_sequence_ = allocator_before_reclaim;
  }

  std::filesystem::path temp_path = path_;
  temp_path += ".tmp";

  int temp_fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

  if (temp_fd == -1) {
    throw std::runtime_error(
        "Failed to create temporary WAL during reclamation");
  }

  auto write_all = [](int fd, const uint8_t *data, size_t size,
                      const char *error_message) {
    size_t total_written = 0;

    while (total_written < size) {

      ssize_t result = ::write(fd, data + total_written, size - total_written);

      if (result == -1) {

        if (errno == EINTR) {
          continue;
        }

        throw std::runtime_error(error_message);
      }

      if (result == 0) {
        throw std::runtime_error(error_message);
      }

      total_written += static_cast<size_t>(result);
    }
  };

  try {

    /*
     * Every WAL, even an empty reclaimed WAL, still begins with its header.
     */
    write_all(temp_fd, reinterpret_cast<const uint8_t *>(magic_),
              sizeof(magic_), "Failed to write reclaimed WAL magic");

    write_all(temp_fd, reinterpret_cast<const uint8_t *>(&version_),
              sizeof(version_), "Failed to write reclaimed WAL version");

    /*
     * Records <= checkpoint are already represented by authoritative
     * SSTable state and may therefore be removed.
     *
     * Anything newer than the checkpoint must remain in the WAL.
     */
    for (const auto &record : records) {

      if (record.sequence <= checkpoint) {
        continue;
      }

      if (record.key.size() > std::numeric_limits<uint32_t>::max() ||
          record.value.size() > std::numeric_limits<uint32_t>::max()) {

        throw std::runtime_error(
            "Recovered WAL record is too large to rewrite");
      }

      const uint32_t key_length = static_cast<uint32_t>(record.key.size());

      const uint32_t value_length = static_cast<uint32_t>(record.value.size());

      const uint8_t operation_byte = static_cast<uint8_t>(record.operation);

      std::vector<uint8_t> serialized;

      appendBytes(serialized, record.sequence);
      appendBytes(serialized, operation_byte);
      appendBytes(serialized, key_length);
      appendBytes(serialized, value_length);

      for (char c : record.key) {
        serialized.push_back(static_cast<uint8_t>(c));
      }

      for (char c : record.value) {
        serialized.push_back(static_cast<uint8_t>(c));
      }

      uLong crc = crc32(0L, Z_NULL, 0);

      crc = crc32(crc, reinterpret_cast<const Bytef *>(serialized.data()),
                  serialized.size());

      const uint32_t checksum = static_cast<uint32_t>(crc);

      appendBytes(serialized, checksum);

      write_all(temp_fd, serialized.data(), serialized.size(),
                "Failed to write reclaimed WAL record");
    }

    /*
     * Make the replacement WAL durable before making it visible.
     */
    while (::fsync(temp_fd) == -1) {

      if (errno == EINTR) {
        continue;
      }

      throw std::runtime_error("Failed to fsync reclaimed WAL");
    }

    if (::close(temp_fd) == -1) {
      temp_fd = -1;

      throw std::runtime_error("Failed to close reclaimed WAL");
    }

    temp_fd = -1;

    /*
     * Atomic replacement:
     *
     * Before rename -> old WAL is authoritative.
     * After rename  -> reclaimed WAL is authoritative.
     *
     * Both contain enough information for recovery because MANIFEST was
     * checkpointed before reclamation was allowed to begin.
     */
    if (::rename(temp_path.c_str(), path_.c_str()) == -1) {

      throw std::runtime_error("Failed to replace WAL during reclamation");
    }

    /*
     * Open the newly-installed WAL before discarding the descriptor for
     * the old inode.
     */
    int new_fd = ::open(path_.c_str(), O_RDWR);

    if (new_fd == -1) {
      throw std::runtime_error("Failed to reopen WAL after reclamation");
    }

    if (::lseek(new_fd, 0, SEEK_END) == -1) {

      ::close(new_fd);

      throw std::runtime_error("Failed to seek reclaimed WAL");
    }

    const int old_fd = fd_;

    fd_ = new_fd;

    if (old_fd != -1) {
      ::close(old_fd);
    }

  } catch (...) {

    if (temp_fd != -1) {
      ::close(temp_fd);
    }

    std::error_code error;
    std::filesystem::remove(temp_path, error);

    throw;
  }
  /*
   * The replacement WAL is already installed and fd_ now refers to it.
   *
   * Persist the rename itself. If directory fsync reports a genuine failure,
   * subsequent engine operations will surface the maintenance error, while the
   * Wal object still points at the correct current inode.
   */
  fsyncParentDirectory(path_);
}