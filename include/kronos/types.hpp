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
} // namespace kronos