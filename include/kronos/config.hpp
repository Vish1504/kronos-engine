#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace kronos {

class Config {

private:
  std::unordered_map<std::string, std::string> configurations_;

public:
  explicit Config(const std::filesystem::path &pathConfig);

  int getMemtableSize() const;

  bool getBloomFilterEnabled() const;

  std::string getLogPath() const; // Path for kronos.log

  /*
   * Number of L0 SSTables required before compaction begins.
   *
   * If the setting is not present in the config file,
   * Chronos uses the v1 default of 4.
   */
  size_t getL0CompactionTrigger() const;
};

} // namespace kronos