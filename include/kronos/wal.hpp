#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>


namespace kronos {
class Wal {
public:
  explicit Wal(const std::filesystem::path &pathWal);
  void put(const std::string& key, const std::string& value);

private:
  enum class Operation : uint8_t { DELETE = 0, PUT = 1 };
  const char magic_[4] = {'K', 'R', 'W', 'L'};
  uint8_t version_ = 1;
  std::fstream file_; // To read annd write the file without havinng to close it
                      // an open it againn
  uint64_t next_sequence_;
};
} // namespace kronos