#pragma once

#include <cstdint>
#include <string>

namespace kronos {

enum class OperationType : uint8_t { DELETE = 0, PUT = 1 };

struct InternalEntry {
  std::string value;
  uint64_t sequence;
  OperationType operation;
};

enum class GetStatus : uint8_t {
  FOUND = 0,
  DELETED = 1,
  NOT_FOUND = 2
}; // What happened when I look for a key?

struct GetResult {
  std::string value;
  GetStatus status;
};
} // namespace kronos