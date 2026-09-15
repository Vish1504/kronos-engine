#include "third_party/murmurhash3/MurmurHash3.h"
#include <cmath>
#include <kronos/bloom_filter.hpp>
#include <stdexcept>
namespace {

constexpr uint32_t bloom_filter_seed = 0xbc9f1d34;

}
kronos::bloom_filter::bloom_filter(const size_t &bits_per_key,
                                   const size_t &key_count) {
  if (key_count == 0 || bits_per_key == 0) {
    throw std::invalid_argument(
        "Bloom filter key count & bits per key must be greater than zero");
  }

  bit_count_ = bits_per_key * key_count; // how many bits will the filter need?
  size_t byte_count =
      (bit_count_ + 7) / 8; // how many bytes are needed to hold those bits?
  bits_.resize(byte_count, 0);

  // optimal number of hash functions (or "probes") to use
  probe_count_ = static_cast<size_t>(
      std::round(bits_per_key * 0.693)); // bits_per_key×ln(2)

  //   Too few probes → not enough information is captured.
  // Too many probes → we set too many bits and saturate the filter faster.
  // There is a sweet spot, approximately bits_per_key × 0.693.

  if (probe_count_ == 0) {
    probe_count_ = 1;
  }
}

void kronos::bloom_filter::add(const std::string &key) {
  uint32_t h = 0;

  MurmurHash3_x86_32(key.data(), static_cast<int>(key.size()),
                     bloom_filter_seed, &h);

  uint32_t delta = (h >> 17) | (h << 15);

  for (size_t i = 0; i < probe_count_; i++) {
    size_t position = h % bit_count_;
    size_t byte_index = position / 8;
    size_t bit_offset = position % 8;
    // set that bit to 1 without disturbing any of the existing bits
    bits_[byte_index] = bits_[byte_index] | (1 << bit_offset);
    h = h + delta;
  }
}

bool kronos::bloom_filter::mayContain(const std::string &key) const {
  uint32_t h = 0;
  MurmurHash3_x86_32(key.data(), static_cast<int>(key.size()),
                     bloom_filter_seed, &h);
  uint32_t delta = (h >> 17) | (h << 15);

  for (size_t i = 0; i < probe_count_; i++) {
    size_t position = h % bit_count_;
    size_t byte_index = position / 8;
    size_t bit_offset = position % 8;
    if ((bits_[byte_index] & (1 << bit_offset)) == 0) {
      return false;
    }
    h = h + delta;
  }

  return true;
}