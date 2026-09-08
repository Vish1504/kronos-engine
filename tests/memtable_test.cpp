#include "kronos/memtable.hpp"

#include <cassert>
#include <iostream>
#include <string>

void test_put_and_get() {
  kronos::Memtable memtable(1024);

  auto result = memtable.put("name", "Krishna", 1);

  assert(result == kronos::Memtable::WriteResult::SUCCESS);

  auto get_result = memtable.get("name");

  assert(get_result.status == kronos::Memtable::GetStatus::FOUND);
  assert(get_result.value == "Krishna");
  assert(memtable.entry_count() == 1);
  assert(memtable.getMemory_usage() > 0);

  std::cout << "PASS: put and get\n";
}

void test_newer_sequence_overwrites() {
  kronos::Memtable memtable(1024);

  memtable.put("name", "Krishna", 1);

  auto result = memtable.put("name", "Vishnu", 2);

  assert(result == kronos::Memtable::WriteResult::SUCCESS);

  auto get_result = memtable.get("name");

  assert(get_result.status == kronos::Memtable::GetStatus::FOUND);
  assert(get_result.value == "Vishnu");

  // Same key, so there should still only be one entry.
  assert(memtable.entry_count() == 1);

  std::cout << "PASS: newer sequence overwrites\n";
}

void test_older_and_equal_sequences_are_rejected() {
  kronos::Memtable memtable(1024);

  memtable.put("name", "Krishna", 10);

  auto older_result = memtable.put("name", "Old Value", 9);

  assert(older_result == kronos::Memtable::WriteResult::OLDER_SEQUENCE);

  auto equal_result = memtable.put("name", "Another Value", 10);

  assert(equal_result == kronos::Memtable::WriteResult::OLDER_SEQUENCE);

  // Neither rejected write should have changed the stored value.
  auto get_result = memtable.get("name");

  assert(get_result.status == kronos::Memtable::GetStatus::FOUND);
  assert(get_result.value == "Krishna");

  std::cout << "PASS: older/equal sequences rejected\n";
}

void test_remove_creates_tombstone() {
  kronos::Memtable memtable(1024);

  memtable.put("name", "Krishna", 1);

  auto result = memtable.remove("name", 2);

  assert(result == kronos::Memtable::WriteResult::SUCCESS);

  auto get_result = memtable.get("name");

  assert(get_result.status == kronos::Memtable::GetStatus::DELETED);

  // We did not erase the key from the Memtable.
  assert(memtable.entry_count() == 1);

  std::cout << "PASS: remove creates tombstone\n";
}

void test_remove_missing_key_creates_tombstone() {
  kronos::Memtable memtable(1024);

  auto result = memtable.remove("ghost", 5);

  assert(result == kronos::Memtable::WriteResult::SUCCESS);

  auto get_result = memtable.get("ghost");

  assert(get_result.status == kronos::Memtable::GetStatus::DELETED);
  assert(memtable.entry_count() == 1);

  std::cout << "PASS: missing-key remove creates tombstone\n";
}

void test_not_found() {
  kronos::Memtable memtable(1024);

  auto get_result = memtable.get("does-not-exist");

  assert(get_result.status == kronos::Memtable::GetStatus::NOT_FOUND);

  std::cout << "PASS: not found\n";
}

void test_freeze() {
  kronos::Memtable memtable(1024);

  memtable.put("name", "Krishna", 1);

  assert(memtable.GetState() == kronos::Memtable::MemTableState::MUTABLE);

  // First freeze should actually change the state.
  assert(memtable.freeze() == true);

  assert(memtable.GetState() == kronos::Memtable::MemTableState::IMMUTABLE);

  // Already frozen, so nothing changes.
  assert(memtable.freeze() == false);

  auto put_result = memtable.put("language", "C++", 2);

  assert(put_result == kronos::Memtable::WriteResult::IMMUTABLE);

  auto remove_result = memtable.remove("name", 3);

  assert(remove_result == kronos::Memtable::WriteResult::IMMUTABLE);

  // Reads must still work after freezing.
  auto get_result = memtable.get("name");

  assert(get_result.status == kronos::Memtable::GetStatus::FOUND);
  assert(get_result.value == "Krishna");

  std::cout << "PASS: freeze\n";
}

void test_memory_accounting_on_replacement() {
  kronos::Memtable memtable(10000);

  memtable.put("key", "small", 1);

  size_t small_usage = memtable.getMemory_usage();

  memtable.put(
      "key", "this is a significantly larger value than the previous value", 2);

  size_t large_usage = memtable.getMemory_usage();

  assert(large_usage > small_usage);

  memtable.put("key", "x", 3);

  size_t reduced_usage = memtable.getMemory_usage();

  assert(reduced_usage < large_usage);

  // Replacements still represent one key.
  assert(memtable.entry_count() == 1);

  std::cout << "PASS: memory accounting on replacement\n";
}

void test_would_exceed_target() {
  kronos::Memtable memtable(512);

  kronos::Memtable::Entry huge_entry{std::string(2000, 'x'), 1,
                                     kronos::Memtable::OperationType::PUT};

  assert(memtable.would_exceed_target("large-key", huge_entry));

  std::cout << "PASS: target prediction\n";
}

void test_iterator_order() {
  kronos::Memtable memtable(4096);

  memtable.put("zebra", "Z", 1);
  memtable.put("apple", "A", 2);
  memtable.put("mango", "M", 3);

  memtable.freeze();

  auto it = memtable.getIterator();

  assert(it.valid());
  assert(it.key() == "apple");
  assert(it.entry().value == "A");

  it.next();

  assert(it.valid());
  assert(it.key() == "mango");
  assert(it.entry().value == "M");

  it.next();

  assert(it.valid());
  assert(it.key() == "zebra");
  assert(it.entry().value == "Z");

  it.next();

  assert(!it.valid());

  std::cout << "PASS: iterator sorted order\n";
}

void test_empty_iterator() {
  kronos::Memtable memtable(1024);

  auto it = memtable.getIterator();

  assert(!it.valid());

  std::cout << "PASS: empty iterator\n";
}

int main() {
  std::cout << "Running Memtable tests...\n\n";

  test_put_and_get();
  test_newer_sequence_overwrites();
  test_older_and_equal_sequences_are_rejected();
  test_remove_creates_tombstone();
  test_remove_missing_key_creates_tombstone();
  test_not_found();
  test_freeze();
  test_memory_accounting_on_replacement();
  test_would_exceed_target();
  test_iterator_order();
  test_empty_iterator();

  std::cout << "\nAll Memtable tests passed.\n";

  return 0;
}