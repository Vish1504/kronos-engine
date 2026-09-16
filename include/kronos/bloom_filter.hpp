#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kronos {
class bloom_filter {
public:
  void add(const std::string &key);
  bloom_filter(const size_t &bits_per_key, const size_t &keyCount);
  bool mayContain(const std::string &key) const;
  size_t bitCount() const;
  size_t probeCount() const;
  const std::vector<uint8_t> &bits() const;
  bloom_filter(size_t bit_count, size_t probe_count, std::vector<uint8_t> bits);

private:
  size_t bit_count_;          // how many bits exist
  std::vector<uint8_t> bits_; // the actual packed bits
  size_t probe_count_;        // how many positions each key checks
  uint32_t murmurHash(const std::string &key, uint32_t seed) const;
};
} // namespace kronos