#include <cassert>
#include <iostream>
#include <stdexcept>

#include <kronos/bloom_filter.hpp>
/*
This isn't a robust statistical false-positive test. We're only using it as a
 sanity check right now. Later, if we're benchmarking the Bloom filter's
 claimed ~1% false-positive rate, we'll test thousands of absent keys and
 measure the actual rate rather than asserting that every absent key returns
 false
*/
int main() {

  // 10 bits per key, expecting 100 keys.
  kronos::bloom_filter filter(10, 100);

  // ---------------------------------------------------------
  // Test 1: A key that was added must never return false.
  // Bloom filters must not have false negatives.
  // ---------------------------------------------------------

  filter.add("apple");

  assert(filter.mayContain("apple"));

  // ---------------------------------------------------------
  // Test 2: Multiple inserted keys must all be found.
  // ---------------------------------------------------------

  filter.add("banana");
  filter.add("orange");
  filter.add("mango");

  assert(filter.mayContain("apple"));
  assert(filter.mayContain("banana"));
  assert(filter.mayContain("orange"));
  assert(filter.mayContain("mango"));

  // ---------------------------------------------------------
  // Test 3: A key that was never inserted should normally
  // return false.
  //
  // Note: Bloom filters CAN produce false positives, so this
  // is not a mathematical guarantee. With this filter size
  // and only a few inserted keys, however, this particular
  // lookup should overwhelmingly be false.
  // ---------------------------------------------------------

  assert(!filter.mayContain("watermelon"));

  // ---------------------------------------------------------
  // Test 4: Invalid construction should be rejected.
  // ---------------------------------------------------------

  bool caught_zero_keys = false;

  try {
    kronos::bloom_filter invalid_filter(10, 0);
  } catch (const std::invalid_argument &) {
    caught_zero_keys = true;
  }

  assert(caught_zero_keys);

  bool caught_zero_bits = false;

  try {
    kronos::bloom_filter invalid_filter(0, 100);
  } catch (const std::invalid_argument &) {
    caught_zero_bits = true;
  }

  assert(caught_zero_bits);

  std::cout << "Bloom filter tests passed.\n";

  return 0;
}