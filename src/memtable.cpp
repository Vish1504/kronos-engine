#include <kronos/memtable.hpp>

// helper functions
size_t estimate_entry_size(const std::string &key,
                           const kronos::Memtable::Entry &entry) {

  const size_t estimated_node_overhead = 4 * sizeof(void *);

  return sizeof(std::string) + sizeof(kronos::Memtable::Entry) + key.size() +
         entry.value.size() + estimated_node_overhead;
}

bool kronos::Memtable::would_exceed_target(const std::string &key,
                                           const Entry &new_entry) const {
  size_t new_size = estimate_entry_size(key, new_entry);
  auto it = Mtable_.find(key);
  if (it != Mtable_.end()) { // if key exists
    if (new_entry.sequence <= it->second.sequence) {
      return false;
    }
    size_t old_size = estimate_entry_size(key, it->second);
    size_t projected_usage = memory_usage_ - old_size + new_size;
    return projected_usage > target_bytes_;
  }

  size_t projected_usage = memory_usage_ + new_size;

  return projected_usage > target_bytes_;
}

kronos::Memtable::WriteResult kronos::Memtable::put(const std::string &key,
                                                    const std::string &value,
                                                    uint64_t sequence) {
  Entry new_entry;
  // checking if mutable
  if (state_ != MemTableState::MUTABLE) {
    return WriteResult ::IMMUTABLE;
  }
  auto it = Mtable_.find(key);
  size_t old_size = 0;
  // Checking if the key exists
  if (it != Mtable_.end()) {
    // checking if it's an older sequence
    if (it->second.sequence >= sequence) {
      return WriteResult ::OLDER_SEQUENCE; // 2=Older sequence
    }
    old_size = estimate_entry_size(key, it->second);
  }
  new_entry.value = value;
  new_entry.sequence = sequence;
  new_entry.operation = OperationType ::PUT;

  //   estimate memory usage

  size_t new_size = estimate_entry_size(key, new_entry);
  memory_usage_ = memory_usage_ - old_size + new_size;

  Mtable_[key] = new_entry;
  return WriteResult ::SUCCESS;
}

kronos::Memtable::WriteResult kronos::Memtable::remove(const std::string &key,
                                                       uint64_t sequence) {
  // check if memtable is immutable
  if (state_ != MemTableState::MUTABLE) {
    return WriteResult ::IMMUTABLE;
  }

  // create entry object new_Entry
  Entry new_entry;
  size_t old_size = 0;
  // check if key exists in memtable
  auto it = Mtable_.find(key);

  if (it != Mtable_.end()) {
    if (it->second.sequence >= sequence) {
      return WriteResult ::OLDER_SEQUENCE;
    }
    old_size = estimate_entry_size(key, it->second);
  }

  new_entry.value = "";
  new_entry.sequence = sequence;
  new_entry.operation = OperationType::DELETE;
  size_t new_size = estimate_entry_size(key, new_entry);
  memory_usage_ = memory_usage_ - old_size + new_size;

  Mtable_[key] = new_entry;
  return WriteResult ::SUCCESS;
}