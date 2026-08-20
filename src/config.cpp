#include <fstream>
#include <iostream>
#include <kronos/config.hpp>
#include <stdexcept>
std::string trim(const std::string &str) {
  auto first = str.find_first_not_of(" \t");
  auto last = str.find_last_not_of(" \t");

  if (first == std::string::npos) {
    return "";
  }

  return str.substr(first, last - first + 1);
}
kronos::Config::Config(const std::filesystem::path &configPath) {

  std::ifstream file(configPath); // Open the configuration file for reading.

  // To verify if the file opened successfully
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open configuration file");
  }

  std::string line;

  // Read line by line
  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty()) { // incase of blankline
      continue;
    }
    // add it to unordered map
    auto delimiterPos = line.find('=');

    if (delimiterPos == std::string::npos) {
      throw std::runtime_error("Missing '=' ");
    }
    std::string key = trim(line.substr(0, delimiterPos));
    // if (key.empty()) {
    //   throw std::runtime_error("Configuration key cannot be empty");
    // }
    std::string value = trim(line.substr(delimiterPos + 1));
    configurations_[key] = value;
  }
}

int kronos::Config::getMemtableSize() {
  // To check if memtable_size_mb exists on the config file
  if (!configurations_.contains("memtable_size_mb")) {
    throw std::runtime_error(
        "memtable_size_mb key is missing in your config file");
  }
  std::string value = configurations_.at("memtable_size_mb");

  // convert the value to int
  try {
    int num = std::stoi(value);
    return num;
  } catch (const std::invalid_argument &e) {
    throw std::runtime_error(
        "Invalid input: No conversion could be performed.\n");
  } catch (const std::out_of_range &e) {
    throw std::runtime_error(
        "Overflow error: Value out of range for an int.\n");
  }
}

bool kronos::Config::getBloomFilterEnabled() {

  if (!configurations_.contains("bloom_filter")) {
    throw std::runtime_error("bloom_filter key is missing in your config file");
  }
  std::string value = configurations_.at("bloom_filter");
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  throw std::runtime_error(
      "Invalid bloom_filter value: expected 'true' or 'false'");
}

std::string kronos::Config::getLogPath() {
  if (!configurations_.contains("log_path")) {
    throw std::runtime_error("log_path key is missing in your config file");
  }

  std::string value = configurations_.at("log_path");
  if (value.empty()) {
    throw std::runtime_error("log_path cannot be empty");
  }
  return value;
}