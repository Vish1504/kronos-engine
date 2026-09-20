#include "kronos/config.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace {

/*
 * Remove leading and trailing spaces/tabs from a config line.
 */
std::string trim(const std::string &str) {

  const auto first = str.find_first_not_of(" \t");

  if (first == std::string::npos) {
    return "";
  }

  const auto last = str.find_last_not_of(" \t");

  return str.substr(first, last - first + 1);
}

} // namespace

namespace kronos {

Config::Config(const std::filesystem::path &configPath) {

  std::ifstream file(configPath);

  if (!file.is_open()) {
    throw std::runtime_error("Failed to open configuration file");
  }

  std::string line;

  /*
   * Configuration format:
   *
   *     key=value
   *
   * Blank lines are ignored.
   */
  while (std::getline(file, line)) {

    line = trim(line);

    if (line.empty()) {
      continue;
    }

    const auto delimiterPos = line.find('=');

    if (delimiterPos == std::string::npos) {
      throw std::runtime_error("Missing '=' in configuration line");
    }

    const std::string key = trim(line.substr(0, delimiterPos));

    if (key.empty()) {
      throw std::runtime_error("Configuration key cannot be empty");
    }

    const std::string value = trim(line.substr(delimiterPos + 1));

    configurations_[key] = value;
  }
}

int Config::getMemtableSize() const {

  if (!configurations_.contains("memtable_size_mb")) {
    throw std::runtime_error(
        "memtable_size_mb key is missing in your config file");
  }

  const std::string &value = configurations_.at("memtable_size_mb");

  try {

    return std::stoi(value);

  } catch (const std::invalid_argument &) {

    throw std::runtime_error(
        "Invalid memtable_size_mb value: expected an integer");

  } catch (const std::out_of_range &) {

    throw std::runtime_error("memtable_size_mb value is out of range");
  }
}

bool Config::getBloomFilterEnabled() const {

  if (!configurations_.contains("bloom_filter")) {
    throw std::runtime_error("bloom_filter key is missing in your config file");
  }

  const std::string &value = configurations_.at("bloom_filter");

  if (value == "true") {
    return true;
  }

  if (value == "false") {
    return false;
  }

  throw std::runtime_error(
      "Invalid bloom_filter value: expected 'true' or 'false'");
}

std::string Config::getLogPath() const {

  if (!configurations_.contains("log_path")) {
    throw std::runtime_error("log_path key is missing in your config file");
  }

  const std::string &value = configurations_.at("log_path");

  if (value.empty()) {
    throw std::runtime_error("log_path cannot be empty");
  }

  return value;
}

/*
 * Return the configurable L0 compaction threshold.
 *
 * Unlike required settings such as log_path, this has a sensible
 * Chronos v1 default. Existing config files therefore do not need
 * to be modified just to keep working.
 */
size_t Config::getL0CompactionTrigger() const {

  constexpr size_t DEFAULT_TRIGGER = 4;

  const auto it = configurations_.find("l0_compaction_trigger");

  // Setting omitted -> use Chronos v1 default.
  if (it == configurations_.end()) {
    return DEFAULT_TRIGGER;
  }

  try {

    /*
     * stoull is used because this is a count and cannot
     * meaningfully be negative.
     */
    const unsigned long long value = std::stoull(it->second);

    if (value == 0) {
      throw std::runtime_error(
          "l0_compaction_trigger must be greater than zero");
    }

    return static_cast<size_t>(value);

  } catch (const std::invalid_argument &) {

    throw std::runtime_error(
        "Invalid l0_compaction_trigger: expected a positive integer");

  } catch (const std::out_of_range &) {

    throw std::runtime_error("l0_compaction_trigger value is out of range");
  }
}

} // namespace kronos