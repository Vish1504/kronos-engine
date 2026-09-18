#pragma once

#include "kronos/types.hpp"

#include <filesystem>
#include <vector>

namespace kronos {

class Manifest {

public:
  explicit Manifest(
      std::filesystem::path manifest_path);

  // Reload authoritative state from disk.
  void load();

  const std::vector<SstableMetadata> &
  liveFiles() const noexcept;

  std::vector<SstableMetadata>
  filesAtLevel(size_t level) const;

  bool contains(
      const std::filesystem::path &path) const;

  // Atomically-ish commit one logical state transition:
  //
  // old files removed
  // new files added
  //
  // Full power-loss fsync hardening is Module 9.
  void applyEdit(
      const ManifestEdit &edit);

private:
  std::filesystem::path manifest_path_;

  std::vector<SstableMetadata> live_files_;

  std::filesystem::path
  temporaryPath() const;

  void persistSnapshot(
      const std::vector<SstableMetadata> &files) const;
};

} // namespace kronos