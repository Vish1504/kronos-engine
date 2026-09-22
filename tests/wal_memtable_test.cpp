#include "kronos/memtable.hpp"
#include "kronos/wal.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

constexpr std::size_t WAL_HEADER_SIZE = sizeof(char) * 4 + sizeof(uint8_t);

/*
 * WAL record layout:
 *
 * [sequence:8]
 * [operation:1]
 * [keyLength:4]
 * [valueLength:4]
 * [key]
 * [value]
 * [crc32:4]
 */
constexpr std::size_t FIXED_RECORD_PREFIX_SIZE =
    sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t);

constexpr std::size_t CRC_SIZE = sizeof(uint32_t);

std::size_t recordSize(const std::string &key, const std::string &value) {
  return FIXED_RECORD_PREFIX_SIZE + key.size() + value.size() + CRC_SIZE;
}

void writeUint32At(const std::filesystem::path &path, std::uint64_t offset,
                   std::uint32_t value) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("Unable to open WAL for test mutation");
  }

  file.seekp(static_cast<std::streamoff>(offset));

  file.write(reinterpret_cast<const char *>(&value), sizeof(value));

  if (!file) {
    throw std::runtime_error("Unable to mutate WAL uint32 field");
  }
}

void flipByteAt(const std::filesystem::path &path, std::uint64_t offset) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("Unable to open WAL for corruption test");
  }

  file.seekg(static_cast<std::streamoff>(offset));

  char byte = 0;

  file.read(&byte, 1);

  if (!file) {
    throw std::runtime_error("Unable to read WAL corruption byte");
  }

  /*
   * Flip one bit without changing the length of the file.
   */
  byte ^= 0x01;

  file.seekp(static_cast<std::streamoff>(offset));

  file.write(&byte, 1);

  if (!file) {
    throw std::runtime_error("Unable to write WAL corruption byte");
  }
}

std::vector<uint8_t> readBytes(const std::filesystem::path &path,
                               std::uint64_t offset, std::size_t size) {
  std::ifstream file(path, std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("Unable to open WAL for reading");
  }

  file.seekg(static_cast<std::streamoff>(offset));

  std::vector<uint8_t> bytes(size);

  file.read(reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(size));

  if (!file) {
    throw std::runtime_error("Unable to read WAL bytes");
  }

  return bytes;
}

void writeBytes(const std::filesystem::path &path, std::uint64_t offset,
                const void *data, std::size_t size) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("Unable to open WAL for test rewrite");
  }

  file.seekp(static_cast<std::streamoff>(offset));

  file.write(static_cast<const char *>(data),
             static_cast<std::streamsize>(size));

  if (!file) {
    throw std::runtime_error("Unable to rewrite WAL bytes");
  }
}

/*
 * Change the sequence number of an existing record and then recompute
 * that record's CRC.
 *
 * Without repairing the CRC, recovery would correctly fail at the checksum
 * first and we would never exercise the sequence-order validation.
 */
void rewriteSequenceWithValidCrc(const std::filesystem::path &path,
                                 std::uint64_t record_start,
                                 const std::string &key,
                                 const std::string &value,
                                 std::uint64_t new_sequence) {

  const std::size_t full_record_size = recordSize(key, value);

  /*
   * CRC protects everything except the stored CRC itself.
   */
  const std::size_t crc_payload_size = full_record_size - CRC_SIZE;

  auto bytes = readBytes(path, record_start, crc_payload_size);

  /*
   * Sequence is the first field of the record.
   */
  std::memcpy(bytes.data(), &new_sequence, sizeof(new_sequence));

  uLong crc = crc32(0L, Z_NULL, 0);

  crc = crc32(crc, reinterpret_cast<const Bytef *>(bytes.data()), bytes.size());

  const uint32_t checksum = static_cast<uint32_t>(crc);

  writeBytes(path, record_start, bytes.data(), bytes.size());

  writeBytes(path, record_start + crc_payload_size, &checksum,
             sizeof(checksum));
}

void testNormalRecovery() {
  const std::filesystem::path wal_path = "tests/integration_test.wal";

  std::filesystem::remove(wal_path);

  // --------------------------------------------------
  // 1. Write some data
  // --------------------------------------------------

  {
    kronos::Wal wal(wal_path);

    wal.put("name", "Krishna");
    wal.put("language", "C++");
    wal.put("name", "Vishnu");
    wal.remove("language");
  }

  /*
   * Imagine the process crashed here.
   *
   * The old Memtable disappeared, but the WAL survived.
   */

  kronos::Memtable memtable(4096);

  // --------------------------------------------------
  // 2. Recover WAL
  // --------------------------------------------------

  {
    kronos::Wal wal(wal_path);

    const auto records = wal.recover();

    for (const auto &record : records) {
      if (record.operation == kronos::OperationType::PUT) {

        memtable.put(record.key, record.value, record.sequence);

      } else if (record.operation == kronos::OperationType::DELETE) {

        memtable.remove(record.key, record.sequence);
      }
    }
  }

  // --------------------------------------------------
  // 3. Verify reconstructed logical state
  // --------------------------------------------------

  auto name = memtable.get("name");

  assert(name.status == kronos::Memtable::GetStatus::FOUND);

  assert(name.value == "Vishnu");

  auto language = memtable.get("language");

  assert(language.status == kronos::Memtable::GetStatus::DELETED);

  std::cout << "PASS: WAL successfully rebuilt Memtable\n";

  std::filesystem::remove(wal_path);
}

void testIncompleteFinalRecordRepair() {
  const std::filesystem::path wal_path = "tests/wal_truncated_tail_test.wal";

  std::filesystem::remove(wal_path);

  {
    kronos::Wal wal(wal_path);

    wal.put("safe", "survives");
    wal.put("partial", "should-disappear");
  }

  const auto original_size = std::filesystem::file_size(wal_path);

  /*
   * Remove two bytes from the final record's CRC.
   *
   * This simulates a crash during the final WAL append.
   */
  assert(original_size > 2);

  std::filesystem::resize_file(wal_path, original_size - 2);

  {
    kronos::Wal wal(wal_path);

    const auto records = wal.recover();

    /*
     * First record survives.
     * Incomplete second record is discarded.
     */
    assert(records.size() == 1);

    assert(records[0].key == "safe");
    assert(records[0].value == "survives");
    assert(records[0].sequence == 0);
  }

  /*
   * Recovery should have physically repaired the WAL by truncating it to the
   * beginning of the incomplete second record.
   */
  const std::uintmax_t expected_size =
      WAL_HEADER_SIZE + recordSize("safe", "survives");

  assert(std::filesystem::file_size(wal_path) == expected_size);

  /*
   * Recovery must also be repeatable.
   *
   * Running recovery again should see the already-repaired WAL and produce
   * exactly the same surviving record.
   */
  {
    kronos::Wal wal(wal_path);

    const auto records = wal.recover();

    assert(records.size() == 1);
    assert(records[0].key == "safe");
    assert(records[0].sequence == 0);
  }

  std::cout << "PASS: incomplete WAL tail repaired safely\n";

  std::filesystem::remove(wal_path);
}

void testCrcCorruptionFailsRecovery() {
  const std::filesystem::path wal_path = "tests/wal_crc_corruption_test.wal";

  std::filesystem::remove(wal_path);

  {
    kronos::Wal wal(wal_path);
    wal.put("important", "value");
  }

  /*
   * First record starts immediately after the WAL header.
   *
   * Skip:
   *
   * sequence
   * operation
   * keyLength
   * valueLength
   *
   * and corrupt the first key byte.
   *
   * The record remains structurally complete, so recovery must NOT treat this
   * as an incomplete tail. CRC32 must detect it and fail open.
   */
  const std::uint64_t first_key_offset =
      WAL_HEADER_SIZE + FIXED_RECORD_PREFIX_SIZE;

  flipByteAt(wal_path, first_key_offset);

  bool corruption_detected = false;

  try {
    kronos::Wal wal(wal_path);
    (void)wal.recover();

  } catch (const std::runtime_error &error) {
    const std::string message = error.what();

    corruption_detected = message.find("CRC32 mismatch") != std::string::npos;
  }

  assert(corruption_detected);

  std::cout << "PASS: WAL CRC corruption rejected\n";

  std::filesystem::remove(wal_path);
}

void testAbsurdLengthDoesNotAllocate() {
  const std::filesystem::path wal_path = "tests/wal_length_corruption_test.wal";

  std::filesystem::remove(wal_path);

  const std::string first_key = "safe";
  const std::string first_value = "value";

  {
    kronos::Wal wal(wal_path);

    wal.put(first_key, first_value);
    wal.put("victim", "payload");
  }

  /*
   * Locate record #2.
   */
  const std::uint64_t second_record_start =
      WAL_HEADER_SIZE + recordSize(first_key, first_value);

  /*
   * keyLength lives after:
   *
   * [sequence][operation]
   */
  const std::uint64_t key_length_offset =
      second_record_start + sizeof(uint64_t) + sizeof(uint8_t);

  /*
   * Pretend the final record claims a ~4 GiB key.
   *
   * Old behavior:
   *
   *     std::string key(UINT32_MAX, '\0');
   *
   * could attempt a gigantic allocation before discovering EOF.
   *
   * New behavior validates the declared length against the remaining physical
   * bytes first.
   */
  const uint32_t absurd_length = std::numeric_limits<uint32_t>::max();

  writeUint32At(wal_path, key_length_offset, absurd_length);

  {
    kronos::Wal wal(wal_path);

    const auto records = wal.recover();

    /*
     * The first fully valid record survives.
     *
     * The malformed/incomplete-looking final record is discarded without
     * attempting the absurd allocation.
     */
    assert(records.size() == 1);

    assert(records[0].key == first_key);
    assert(records[0].value == first_value);
    assert(records[0].sequence == 0);
  }

  const std::uintmax_t expected_size =
      WAL_HEADER_SIZE + recordSize(first_key, first_value);

  assert(std::filesystem::file_size(wal_path) == expected_size);

  std::cout << "PASS: absurd WAL length rejected before allocation\n";

  std::filesystem::remove(wal_path);
}

void testNonIncreasingSequenceFailsRecovery() {
  const std::filesystem::path wal_path =
      "tests/wal_sequence_corruption_test.wal";

  std::filesystem::remove(wal_path);

  const std::string first_key = "first";
  const std::string first_value = "one";

  const std::string second_key = "second";
  const std::string second_value = "two";

  {
    kronos::Wal wal(wal_path);

    wal.put(first_key, first_value);   // sequence 0
    wal.put(second_key, second_value); // sequence 1
  }

  const std::uint64_t second_record_start =
      WAL_HEADER_SIZE + recordSize(first_key, first_value);

  /*
   * Change record #2 from sequence 1 -> sequence 0.
   *
   * Then repair its CRC so recovery reaches the sequence validation rather
   * than failing earlier at checksum verification.
   */
  rewriteSequenceWithValidCrc(wal_path, second_record_start, second_key,
                              second_value, 0);

  bool sequence_corruption_detected = false;

  try {
    kronos::Wal wal(wal_path);
    (void)wal.recover();

  } catch (const std::runtime_error &error) {
    const std::string message = error.what();

    sequence_corruption_detected =
        message.find("non-increasing sequence number") != std::string::npos;
  }

  assert(sequence_corruption_detected);

  std::cout << "PASS: non-increasing WAL sequence rejected\n";

  std::filesystem::remove(wal_path);
}

} // namespace

int main() {
  std::cout << "Running WAL recovery tests...\n\n";

  testNormalRecovery();

  testIncompleteFinalRecordRepair();

  testCrcCorruptionFailsRecovery();

  testAbsurdLengthDoesNotAllocate();

  testNonIncreasingSequenceFailsRecovery();

  std::cout << "\nAll WAL recovery tests passed.\n";

  return 0;
}