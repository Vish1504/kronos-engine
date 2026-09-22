#pragma once

#include "kronos/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kronos {

class Wal {
public:
  struct RecoveredRecord {
    uint64_t sequence;
    OperationType operation;
    std::string key;
    std::string value;
  };

  explicit Wal(const std::filesystem::path &pathWal);
  ~Wal();

  uint64_t put(const std::string &key, const std::string &value);
  uint64_t remove(const std::string &key);

  std::vector<RecoveredRecord> recover();

  /*
   * Recovery may learn about older persisted sequence history from MANIFEST
   * even when those records are no longer present in the WAL.
   *
   * This raises the allocator floor without ever moving it backwards.
   */
  void ensureNextSequenceAtLeast(uint64_t minimum_next_sequence);
  void reclaimThrough(uint64_t checkpoint);

private:
  const char magic_[4] = {'K', 'R', 'W', 'L'};
  uint8_t version_ = 1;

  uint64_t next_sequence_ = 0;
  int fd_ = -1;
  std::filesystem::path path_;
  size_t readUpTo(void *buffer, size_t bytesToRead);

  uint64_t writeRecord(OperationType operation, const std::string &key,
                       const std::string &value);
};

} // namespace kronos