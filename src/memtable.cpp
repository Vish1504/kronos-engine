#include <kronos/memtable.hpp>

// Estimates the memory occupied by one key-entry pair.
size_t estimate_entry_size(const std::string &key,
                           const kronos::Memtable::Entry &entry) {
  // Rough estimate for std::map node bookkeeping.
  const size_t estimated_node_overhead = 4 * sizeof(void *);

  return sizeof(std::string) + sizeof(kronos::Memtable::Entry) + key.size() +
         entry.value.size() + estimated_node_overhead;
}

bool kronos::Memtable::would_exceed_target(const std::string &key,
                                           const Entry &new_entry) const {
  size_t new_size = estimate_entry_size(key, new_entry);
  auto it = Mtable_.find(key);

  if (it != Mtable_.end()) {

    /* Kronos will not allow the new sequence to be lesser or equal to the
     sequence of exisiting key */
    if (new_entry.sequence <= it->second.sequence) {
      return false;
    }

    // Replacing a key removes the old entry's estimated cost.
    size_t old_size = estimate_entry_size(key, it->second);
    size_t projected_usage = memory_usage_ - old_size + new_size;

    return projected_usage > target_bytes_;
  }

  // A new key adds its entire estimated size.
  size_t projected_usage = memory_usage_ + new_size;

  return projected_usage > target_bytes_;
}

kronos::Memtable::WriteResult kronos::Memtable::put(const std::string &key,
                                                    const std::string &value,
                                                    uint64_t sequence) {

  // Frozen memtables must never be modified.
  if (state_ != MemTableState::MUTABLE) {
    return WriteResult::IMMUTABLE;
  }

  auto it = Mtable_.find(key);
  size_t old_size = 0;

  if (it != Mtable_.end()) {

    // Only a newer version may replace the current value.
    if (it->second.sequence >= sequence) {
      return WriteResult::OLDER_SEQUENCE;
    }

    old_size = estimate_entry_size(key, it->second);
  }

  Entry new_entry;
  new_entry.value = value;
  new_entry.sequence = sequence;
  new_entry.operation = OperationType::PUT;

  // Replace the old memory cost with the new one.
  size_t new_size = estimate_entry_size(key, new_entry);
  memory_usage_ = memory_usage_ - old_size + new_size;

  Mtable_[key] = new_entry;

  return WriteResult::SUCCESS;
}

kronos::Memtable::WriteResult kronos::Memtable::remove(const std::string &key,
                                                       uint64_t sequence) {

  // Frozen memtables must never be modified.
  if (state_ != MemTableState::MUTABLE) {
    return WriteResult::IMMUTABLE;
  }

  auto it = Mtable_.find(key);
  size_t old_size = 0;

  if (it != Mtable_.end()) {

    // Prevent an older delete from overwriting newer state.
    if (it->second.sequence >= sequence) {
      return WriteResult::OLDER_SEQUENCE;
    }

    old_size = estimate_entry_size(key, it->second);
  }
  // A delete is stored as a tombstone, not erased from the map.

  Entry new_entry;
  new_entry.value = "";
  new_entry.sequence = sequence;
  new_entry.operation = OperationType::DELETE;

  size_t new_size = estimate_entry_size(key, new_entry);
  memory_usage_ = memory_usage_ - old_size + new_size;

  Mtable_[key] = new_entry;

  return WriteResult::SUCCESS;
}

kronos::Memtable::GetResult
kronos::Memtable::get(const std::string &key) const {

  auto it = Mtable_.find(key);

  if (it != Mtable_.end()) {
    if (it->second.operation == OperationType ::DELETE) {
      return {it->second.value, GetStatus ::DELETED};
    }
    return {it->second.value, GetStatus ::FOUND};
  }

  return {"", GetStatus ::NOT_FOUND};
}

size_t kronos::Memtable::entry_count() const { return Mtable_.size(); }
size_t kronos::Memtable::getMemory_usage() const { return memory_usage_; }
kronos::Memtable::MemTableState kronos::Memtable::GetState() const {
  return state_;
}

bool kronos::Memtable::freeze(){
  if(state_==MemTableState::MUTABLE){
    state_=MemTableState::IMMUTABLE;
    return true;
  }

  return false;
}