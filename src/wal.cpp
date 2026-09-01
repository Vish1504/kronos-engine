#include <cstring>
#include <fstream>
#include <iostream>
#include <kronos/wal.hpp>
#include <limits>
#include <stdexcept>
#include <vector>

/*
  WAL (Write-Ahead Log) notes
  ---------------------
  The WAL provides durability for Chronos writes.
  A written record must first be recorded durably in the WAL [D], and then later
  added to Memtable [R]. If Chronos crashes and the Memtable is lost, the WAL
  can later be replayed to reconstruct the lost in-memory state.

  WAL file layout:

    FILE HEADER:
    [Magic][Version] (Validation for magic bytes, and version has been
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
    CRC32        -> uint32_t (4 bytes) [to be implemented]

  V1 currently serializes integers using the host machine's native byte order.
  A fixed byte order can be introduced later for cross-platform portability.
 */

kronos::Wal::Wal(const std::filesystem::path &pathWal) {
  bool isNewFile = !fs::exists(pathWal); // Check existence first

  if (isNewFile) {
    /* We're creating an empty file first, because a new WALstd::fstream opened
    with both ios::in and ios::out expects the file to already exist */
    std::ofstream create_file(pathWal, std::ios::binary | std::ios::out);
  } // create_file is automatically closed here (RAII).

  /* Here we open the WAL for both reading (needed for validation/recovery) and
  writing (needed for appending new records).*/
  file_.open(pathWal, std::ios::binary | std::ios::out | std::ios::in);

  if (!file_.is_open()) {
    throw std::runtime_error("Failed to open WAL file");
  }

  if (isNewFile) {
    // Every new WAL begins with magic bytes
    file_.write(magic_, sizeof(magic_));
    /* since version_ is of type uint8_t, and .write() allows only const char*,
     * we use reinterpret_cast to let it view version_'s memory as bytes. There
     * will be no change to the value. */
    file_.write(reinterpret_cast<const char *>(&version_), sizeof(version_));
  }

  if (!isNewFile) {
    // magic bytes verification
    char magicRead[4];
    file_.seekg(0, std::ios::beg);
    file_.read(magicRead, sizeof(magicRead));

    /* We have to check if all 4 magic bytes were read, else if there were only
     2 magicRead bytes, and magicRead assigned garbage values to the remaining
     2 bytes, that can be an issue for rare scenarios. */
    if (!file_) {
      throw std::runtime_error("Failed to read WAL magic");
    }

    // checking if magic_ & magicRead are the same
    //  Verify that the file contains the expected WAL magic.
    if (std::memcmp(magicRead, magic_, sizeof(magic_)) != 0) {
      throw std::runtime_error("Invalid WAL magic");
    }
  }

  // Manually move the write pointer to the end of the existing file
  file_.seekp(0, std::ios::end);
}

/* This is a function template because the same operation is needed for
 * different field types (uint64_t sequence, uint32_t lengths, Operation, etc.).
 */
template <typename T>
void appendBytes(std::vector<uint8_t> &record, const T &value) {

  // we need to view "value" as individual bytes and append all its bytes to the serialized WAL record
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
  // .size() returns type size_t, which is 64-bit on a 64 bit machinne

  /* Our WAL format stores lengths as uint32_t (4 bytes), so verify the sizes
    fit before converting them to uint32_t. */
  if (key.size() > std::numeric_limits<uint32_t>::max() ||
      value.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Key or value too large for WAL record");
  }

  // We explicitly convert size_t -> uint32_t
  uint32_t keyLength = static_cast<uint32_t>(key.size());
  uint32_t valueLength = static_cast<uint32_t>(value.size());

  /* We’re assembling the serialized WAL record using a temporary RAM buffer bu so that we can run CRC32 over those exact bytes. */
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
}