#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kronos {

enum class OperationType : uint8_t { DELETE = 0, PUT = 1 };

struct InternalEntry {
  std::string value;
  uint64_t sequence;
  OperationType operation;
};

enum class GetStatus : uint8_t { FOUND = 0, DELETED = 1, NOT_FOUND = 2 };

struct GetResult {
  std::string value;
  GetStatus status;
};

// Metadata describing one SSTable that belongs to the
// current authoritative database state.
struct SstableMetadata {
  std::filesystem::path path;

  size_t level;

  std::string smallest_key;
  std::string largest_key;
};

// Result produced by the Compactor.
//
// IMPORTANT:
// Producing this result does NOT make the output authoritative.
// The MANIFEST must commit that state transition separately.
struct CompactionResult {
  std::vector<std::filesystem::path> input_files;
  std::vector<std::filesystem::path> output_files;
};

// One logical change to the authoritative database state.
//
// Example:
//
// remove:
//   10.sst
//   11.sst
//
// add:
//   20.sst
struct ManifestEdit {
  std::vector<std::filesystem::path> remove_files;
  std::vector<SstableMetadata> add_files;
  std::optional<uint64_t> persisted_through_;
};

// A compaction decision made by the CompactionPolicy.
//
// It says:
//   - which files participate
//   - which level the output belongs to
//   - whether tombstones are safe to discard
struct CompactionPlan {
  std::vector<SstableMetadata> input_files;

  size_t output_level;

  bool can_drop_tombstones;
};

} // namespace kronos