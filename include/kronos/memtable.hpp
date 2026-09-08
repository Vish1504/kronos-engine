#pragma once
#include <iostream>
#include <map>
#include <string>
namespace kronos {
class Memtable {
public:
  enum class OperationType : uint8_t { DELETE = 0, PUT = 1 };
  enum class MemTableState : uint8_t {
    IMMUTABLE = 0,
    MUTABLE = 1
  }; // Can this Memtable be modified?
  enum class WriteResult : uint8_t {
    // write was accepted
    SUCCESS = 0,
    // this Memtable is frozen; Chronos should use the active Memtable
    IMMUTABLE = 1,
    // incoming record is older than the version already stored
    OLDER_SEQUENCE = 2
  };
  enum class GetStatus : uint8_t {
    FOUND = 0,
    DELETED = 1,
    NOT_FOUND = 2
  };             // What happened when I look for a key?
  struct Entry { // information sotred for each key
    std::string value;
    uint64_t sequence;
    OperationType operation;
  };
  struct GetResult {
    std::string value;
    GetStatus status;
  };
  explicit Memtable(size_t target_bytes)
      : memory_usage_(0), target_bytes_(target_bytes),
        state_(MemTableState::MUTABLE){

        };
  WriteResult put(const std::string &key, const std::string &value,
                  uint64_t sequence);
  WriteResult remove(const std::string &key, uint64_t sequence);
  GetResult get(const std::string &key) const; // Read only
  size_t entry_count() const; // for total count of enteries on the memtable
  size_t getMemory_usage() const;
  MemTableState GetState() const;
  bool freeze();
  bool would_exceed_target(const std::string &key,
                           const Entry &new_entry)
      const; // if true, freeze current Memtable and create new active Memtable

private:
  std::map<std::string, Entry> Mtable_;
  size_t memory_usage_;
  size_t target_bytes_;
  MemTableState state_;
};

} // namespace kronos