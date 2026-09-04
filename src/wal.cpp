#include <kronos/wal.hpp>

#include <iostream>
// #include <fstream>
#include <cerrno> //for EINTR
#include <cstring>
#include <fcntl.h> // open(), O_RDWR, O_CREAT
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unistd.h> // read(), write(), lseek(), fsync(), ftruncate(), close()
#include <utility>
#include <vector>
#include <zlib.h> // crc32()

namespace fs = std::filesystem;

/*
  WAL (Write-Ahead Log) notes
  ---------------------
  The WAL provides durability for Kronos writes.
  A written record must first be recorded durably in the WAL [D], and then later
  added to Memtable [R]. If Kronos crashes and the Memtable is lost, the WAL
  can later be replayed to reconstruct the lost in-memory state.

  WAL file layout:

    FILE HEADER:
    [Magic][Version] (Validation for magic bytes has been
  implemented as well)

    RECORD:
    [Sequence][Operation][Key Length][Value Length][Key][Value][CRC32]

  Current field sizes:
    Sequence     -> uint64_t (8 bytes)
    Operation    -> uint8_t  (1 byte)
    Key Length   -> uint32_t (4 bytes)
    Value Length -> uint32_t (4 bytes)
    Key          -> string variable
    Value        -> string variable
    CRC32        -> uint32_t (4 bytes)

  V1 currently serializes integers using the host machine's native byte order.
  A fixed byte order can be introduced later for cross-platform portability.

  The first WAL implementation used std::fstream.
  So I later moved to POSIX I/O because Kronos needed fsync() for an explicit
  durability boundary.

 */

kronos::Wal::Wal(const std::filesystem::path &pathWal) {
  bool isNewFile = !fs::exists(pathWal); // Check existence first
  /* Here we open the WAL for both reading (needed for validation/recovery) and
  writing (needed for appending new records).*/
  //   file_.open(pathWal, std::ios::binary | std::ios::out | std::ios::in);

  fd_ = ::open(pathWal.c_str(), O_RDWR | O_CREAT, 0644);

  if ((fd_) == -1) {
    throw std::runtime_error("Failed to open WAL file");
  }

  if (isNewFile) {
    if (::write(fd_, magic_, sizeof(magic_)) != sizeof(magic_)) {
      throw std::runtime_error("Failed to write WAL magic");
    }

    if (::write(fd_, &version_, sizeof(version_)) != sizeof(version_)) {
      throw std::runtime_error("Failed to write WAL version");
    }
  }

  // if this is an already existing file
  if (!isNewFile) {
    // magic bytes verification
    char magicRead[4];
    if (::lseek(fd_, 0, SEEK_SET) == -1) {
      throw std::runtime_error("Failed to seek WAL");
    }

    /*
        We must read all four magic bytes before comparing them.
        A shorter read means the WAL header is incomplete or invalid.
        */

    // if (!file_) {
    //   throw std::runtime_error("Failed to read WAL magic");
    // }
    ssize_t bytesRead = ::read(fd_, magicRead, sizeof(magicRead));

    if (bytesRead != sizeof(magicRead)) {
      throw std::runtime_error("Failed to read WAL magic");
    }

    // checking if magic_ & magicRead are the same
    //  Verify that the file contains the expected WAL magic.
    if (std::memcmp(magicRead, magic_, sizeof(magic_)) != 0) {
      throw std::runtime_error("Invalid WAL magic");
    }
  }

  // Manually move the write pointer to the end of the existing file
  //   file_.seekp(0, std::ios::end);
  if (::lseek(fd_, 0, SEEK_END) == -1) {
    throw std::runtime_error("Failed to seek to end of WAL");
  }
}

/* This is a function template because the same operation is needed for
 * different field types (uint64_t sequence, uint32_t lengths, Operation, etc.).
 */
template <typename T>
void appendBytes(std::vector<uint8_t> &record, const T &value) {

  /* we need to view "value" as individual bytes and append all its bytes to the
   serialized WAL record */
  const uint8_t *valueInBytes = reinterpret_cast<const uint8_t *>(&value);

  record.insert(record.end(),                // WHERE should I insert?
                valueInBytes,                // WHAT is the beginning?
                valueInBytes + sizeof(value) // WHERE is the end?
  );
}

void kronos::Wal::put(const std::string &key, const std::string &value) {
  // Here we build the serialized representation of: [Sequence][PUT][Key
  // Length][Value Length][Key][Value]

  uint64_t sequence = next_sequence_;
  Operation op = Operation::PUT;

  // .size() returns type size_t, which is 64-bit on a 64 bit machine
  /* Our WAL format stores lengths as uint32_t (4 bytes), so verify the sizes
    fit before converting them to uint32_t. */
  if (key.size() > std::numeric_limits<uint32_t>::max() ||
      value.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Key or value too large for WAL record");
  }

  // We explicitly convert size_t -> uint32_t
  uint32_t keyLength = static_cast<uint32_t>(key.size());
  uint32_t valueLength = static_cast<uint32_t>(value.size());

  /* We’re assembling the serialized WAL record using a temporary RAM buffer bu
   * so that we can run CRC32 over those exact bytes. */
  std::vector<uint8_t> record;

  appendBytes(record, sequence);
  appendBytes(record, op);
  appendBytes(record, keyLength);
  appendBytes(record, valueLength);
  for (char c : key) {
    record.push_back(static_cast<uint8_t>(c));
  }
  for (char c : value) {
    record.push_back(static_cast<uint8_t>(c));
  }

  // Now we calculate CRC32 over these exact serialized bytes.
  // Initialize the CRC register
  uLong crc = crc32(0L, Z_NULL, 0);

  // Compute the checksum over the current contents of record [R].
  crc =
      crc32(crc, reinterpret_cast<const Bytef *>(record.data()), record.size());

  // Converting checksum to uint32_t as zlib returns the checksum as uLong.
  uint32_t checksum_value = static_cast<uint32_t>(crc);

  /* Final layout:
  [Sequence][Operation][Key Length][Value Length][Key][Value][CRC32]
     */

  // Append the 4 checksum bytes to the serialized record [R]
  appendBytes(record, checksum_value);

  size_t totalWritten = 0;

  while (totalWritten < record.size()) {
    ssize_t bytesWritten = ::write(fd_, record.data() + totalWritten,
                                   record.size() - totalWritten);

    if (bytesWritten == -1) {
      if (errno == EINTR) { // If interrupted by OS signal
        continue; // If interrupted by OS signal, retry the remaining bytes
      }

      throw std::runtime_error("Failed to write WAL record");
    }

    totalWritten += static_cast<size_t>(bytesWritten);
  }

  while (::fsync(fd_) == -1) {
    if (errno == EINTR) {
      continue;
    }
    throw std::runtime_error("Failed to fsync WAL");
  }

  next_sequence_++;
}

kronos::Wal::~Wal() {
  if (fd_ != -1) {
    ::close(fd_);
  }
}

// //corrupted record
// /*  - keep all previously recovered records
//     - truncate WAL [D] back to the start of this incomplete record
//     - finish recovery successfully*/

// }

/*
  Recovery notes
  --------------
  Now imagine Kronos crashes. Everything in the Memtable[R] will eventually
  disappear because it's in the RAM. When Kronos starts again, all it has is
  this file: WAL[D]

   Recovery policy:
  1. Clean EOF between records:
     Recovery succeeds.

  2. EOF in the middle of the final record:
     Keep previously valid records,
     truncate the incomplete tail,
     fsync the repair,
     and finish recovery successfully.

  3. CRC mismatch / invalid operation:
     Treat it as genuine corruption,
     do NOT truncate automatically,
     and fail recovery.
*/

// This function exists because a read function does not guaranntee you get all
// the bytes in one call
size_t kronos::Wal::readUpTo(void *buffer, size_t bytesToRead) {
  size_t totalRead = 0;

  // We will consider the destination buffer as raw bytes so we can advance
  // through it byte by byte.
  uint8_t *bytes = static_cast<uint8_t *>(buffer);

  while (totalRead < bytesToRead) {
    ssize_t result = ::read(fd_, bytes + totalRead, bytesToRead - totalRead);

    if (result > 0) {
      totalRead += static_cast<size_t>(result);
      continue;
    }

    if (result == 0) {
      // EOF
      break;
    }

    if (errno == EINTR) // If interrupted by OS signal
    {
      continue;
    }

    throw std::runtime_error("Failed to read WAL");
  }

  return totalRead;
}

std::vector<kronos::Wal::RecoveredRecord> kronos::Wal::recover() {

  /*                           WAL [D]
                                  ↓
                              read bytes
                                  ↓
                              deserialize
                                  ↓
                              recover operation

  */

  std::vector<RecoveredRecord> records;
  // Records begin immediately after [Magic][Version].
  if (::lseek(fd_, sizeof(magic_) + sizeof(version_), SEEK_SET) == -1) {
    throw std::runtime_error("Failed to seek WAL for recovery");
  }

  while (true) { // Keep trying to recover records until we intentionally break.
    /*
      Remember where this record begins.

      If EOF occurs after the record has started but before it finishes,
      recovery can safely truncate the incomplete final tail back to here.
    */
    off_t recordStart = ::lseek(fd_, 0, SEEK_CUR);

    if (recordStart == -1) {
      throw std::runtime_error("Failed to determine WAL record position");
    }

    // ---------------------------------------------------------
    // 1. SEQUENCE
    // ---------------------------------------------------------

    uint64_t sequence;

    size_t sequenceBytes = readUpTo(&sequence, sizeof(sequence));

    if (sequenceBytes == 0) {
      // Clean EOF.
      // No new record even started.
      break;
    }

    if (sequenceBytes < sizeof(sequence)) {
      // A record started but was cut off - We will remove this incomplete final
      // tail.
      if (::ftruncate(fd_, recordStart) == -1) {
        throw std::runtime_error("Failed to truncate incomplete WAL record");
      }
      while (::fsync(fd_) == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("Failed to fsync WAL after truncation");
      }

      break;
    }

    /*
          Reconstruct the exact serialized record bytes so that CRC32 can be
          recalculated and compared with the stored checksum.
        */
    std::vector<uint8_t> recordBytes;

    appendBytes(recordBytes, sequence);

    // ---------------------------------------------------------
    // 2. OPERATION
    // ---------------------------------------------------------

    uint8_t operationByte;

    if (readUpTo(&operationByte, sizeof(operationByte)) !=
        sizeof(operationByte)) {

      if (::ftruncate(fd_, recordStart) == -1) {
        throw std::runtime_error("Failed to truncate incomplete WAL record");
      }
      while (::fsync(fd_) == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("Failed to fsync WAL after truncation");
      }

      break;
    }

    Operation operation;

    if (operationByte == static_cast<uint8_t>(Operation::PUT)) {
      operation = Operation::PUT;
    } else if (operationByte == static_cast<uint8_t>(Operation::DELETE)) {
      operation = Operation::DELETE;
    } else {
      // This is not an incomplete write.
      // The WAL contains an invalid operation value.
      /*
        The byte exists, but it does not represent a valid operation.
        This is treated as corruption rather than an incomplete tail.
      */
      throw std::runtime_error("WAL corruption detected: invalid operation");
    }

    appendBytes(recordBytes, operationByte);

    // ---------------------------------------------------------
    // 3. KEY LENGTH
    // ---------------------------------------------------------

    uint32_t keyLength;

    if (readUpTo(&keyLength, sizeof(keyLength)) != sizeof(keyLength)) {

      if (::ftruncate(fd_, recordStart) == -1) {
        throw std::runtime_error("Failed to truncate incomplete WAL record");
      }
      while (::fsync(fd_) == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("Failed to fsync WAL after truncation");
      }

      break;
    }

    appendBytes(recordBytes, keyLength);

    // ---------------------------------------------------------
    // 4. VALUE LENGTH
    // ---------------------------------------------------------

    uint32_t valueLength;

    if (readUpTo(&valueLength, sizeof(valueLength)) != sizeof(valueLength)) {

      if (::ftruncate(fd_, recordStart) == -1) {
        throw std::runtime_error("Failed to truncate incomplete WAL record");
      }
      while (::fsync(fd_) == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("Failed to fsync WAL after truncation");
      }

      break;
    }

    appendBytes(recordBytes, valueLength);

    // ---------------------------------------------------------
    // 5. KEY
    // ---------------------------------------------------------

    // we're creating a key strig of keyLength length filled with null
    // characters.
    std::string key(keyLength, '\0');

    if (keyLength > 0) {
      if (readUpTo(key.data(), keyLength) != keyLength) {

        if (::ftruncate(fd_, recordStart) == -1) {
          throw std::runtime_error("Failed to truncate incomplete WAL record");
        }
        while (::fsync(fd_) == -1) {
          if (errno == EINTR) {
            continue;
          }
          throw std::runtime_error("Failed to fsync WAL after truncation");
        }

        break;
      }

      for (char c : key) {
        recordBytes.push_back(static_cast<uint8_t>(c));
      }
    }

    // ---------------------------------------------------------
    // 6. VALUE
    // ---------------------------------------------------------

    std::string value(valueLength, '\0');

    if (valueLength > 0) {
      if (readUpTo(value.data(), valueLength) != valueLength) {

        if (::ftruncate(fd_, recordStart) == -1) {
          throw std::runtime_error("Failed to truncate incomplete WAL record");
        }

        while (::fsync(fd_) == -1) {
          if (errno == EINTR) {
            continue;
          }
          throw std::runtime_error("Failed to fsync WAL after truncation");
        }

        break;
      }

      for (char c : value) {
        recordBytes.push_back(static_cast<uint8_t>(c));
      }
    }

    // ---------------------------------------------------------
    // 7. STORED CRC32
    // ---------------------------------------------------------

    uint32_t storedChecksum;

    if (readUpTo(&storedChecksum, sizeof(storedChecksum)) !=
        sizeof(storedChecksum)) {

      if (::ftruncate(fd_, recordStart) == -1) {
        throw std::runtime_error("Failed to truncate incomplete WAL record");
      }

      while (::fsync(fd_) == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("Failed to fsync WAL after truncation");
      }

      break;
    }

    // ---------------------------------------------------------
    // 8. RECOMPUTE CRC32
    // ---------------------------------------------------------

    uLong crc = crc32(0L, Z_NULL, 0);

    crc = crc32(crc, reinterpret_cast<const Bytef *>(recordBytes.data()),
                recordBytes.size());

    uint32_t calculatedChecksum = static_cast<uint32_t>(crc);

    if (calculatedChecksum != storedChecksum) {
      // Genuine corruption.
      //
      // IMPORTANT:
      // Do NOT truncate automatically.
      // Normal recovery fails here.
      throw std::runtime_error("WAL corruption detected: CRC32 mismatch");
    }

    // ---------------------------------------------------------
    // 9. RECORD IS VALID
    // ---------------------------------------------------------

    // now adding that struct to records vector
    records.push_back(
        RecoveredRecord{sequence, operation, std::move(key), std::move(value)});

    // Restore next sequence number.
    next_sequence_ = sequence + 1;
  }

  // recover() moved the file cursor around.
  // put() expects new records to be appended at the end.
  if (::lseek(fd_, 0, SEEK_END) == -1) {
    throw std::runtime_error("Failed to seek to end of WAL after recovery");
  }

  return records;
}
