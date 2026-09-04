#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace kronos {
class Wal {
public:
  enum class Operation : uint8_t { DELETE = 0, PUT = 1 };
  //   Read bytes out of WAL [D] → deserialize them in RAM [R] → create a
  //   RecoveredRecord struct [R] → add each struct to a vector [R].
  struct RecoveredRecord {
    uint64_t sequence;
    Operation operation;
    std::string key;
    std::string value;
  };
   explicit Wal(const std::filesystem::path &pathWal);
  ~Wal();
  void put(const std::string &key, const std::string &value);
  std::vector<RecoveredRecord> recover();

private:
  const char magic_[4] = {'K', 'R', 'W', 'L'};
  uint8_t version_ = 1;
  //   std::fstream file_; // To read and write the file without having to close
  //   it
  // an open it againn
  uint64_t next_sequence_ = 0;
  int fd_ = -1;
  size_t readUpTo(void *buffer, size_t bytesToRead);
};
} // namespace kronos